/*
 * PPU implementation for scanline timing, VRAM logic, and sprite/background rendering.
 */

#include "ppu.h"
#include "nes.h"
#include "cartridge.h"

#ifndef ENABLE_DEBUG_SERIAL
#define ENABLE_DEBUG_SERIAL false
#endif

#if ENABLE_DEBUG_SERIAL
#define PERF_INC(field) (++perfCounters.field)
#define PERF_ADD(field, value) (perfCounters.field += (value))
#define PERF_DEC_SAT(field)          \
    do                               \
    {                                \
        if (perfCounters.field > 0)  \
        {                            \
            perfCounters.field--;    \
        }                            \
    } while (0)
#else
#define PERF_INC(field) ((void)0)
#define PERF_ADD(field, value) ((void)0)
#define PERF_DEC_SAT(field) ((void)0)
#endif

#ifndef FORCE_BLACK_LEFT_BG_CLIP
#define FORCE_BLACK_LEFT_BG_CLIP 1
#endif

static const uint16_t DRAM_ATTR nesPalette[64] = {
    0x7BEF, 0x001F, 0x0017, 0x4157, 0x9010, 0xA804, 0xA880, 0x88A0,
    0x5180, 0x03C0, 0x0340, 0x02C0, 0x020B, 0x0000, 0x0000, 0x0000,
    0xBDF7, 0x03DF, 0x02DF, 0x6A3F, 0xD819, 0xE00B, 0xF9C0, 0xE2E2,
    0xABE0, 0x05C0, 0x0540, 0x0548, 0x0451, 0x0000, 0x0000, 0x0000,
    0xFFDF, 0x3DFF, 0x6C5F, 0x9BDF, 0xFBDF, 0xFAD3, 0xFBCB, 0xFD08,
    0xFDC0, 0xBFC3, 0x5ECA, 0x5FD3, 0x075B, 0x7BCF, 0x0000, 0x0000,
    0xFFFF, 0xA73F, 0xBDDF, 0xDDDF, 0xFDDF, 0xFD38, 0xF696, 0xFF15,
    0xFECF, 0xDFCF, 0xBFD7, 0xBFDB, 0x07FF, 0xFEDF, 0x0000, 0x0000,
};

static inline uint8_t IRAM_ATTR reverse8(uint8_t v) {
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

void PPU::connect(NES* n) {
    bus = n;
}

void PPU::setMemoryPointers(uint8_t* vramPtr, uint8_t* palettePtr, Cartridge* cartPtr, bool* mirrorVertPtr) {
    vramDirect = vramPtr;
    paletteDirect = palettePtr;
    cartDirect = cartPtr;
    chrDirect = cartPtr->getChrData();  
    mirrorVertical = mirrorVertPtr;
}

void PPU::reset() {
    
    ppuCtrl = 0;
    ppuMask = 0;
    ppuStatus = 0;
    oamAddr = 0;
    
    
    vramAddr = 0;
    tempAddr = 0;
    fineX = 0;
    writeToggle = false;
    dataBuffer = 0;
    
    
    savedScrollAddr = 0;
    savedFineX = 0;
    
    
    memset(oam, 0, sizeof(oam));
    
    
    memset(bgPaletteCache, 0, sizeof(bgPaletteCache));
    memset(spPaletteCache, 0, sizeof(spPaletteCache));
    
    
    invalidateTileCache();
    
    
    ppuCycle = 0;
    ppuScanline = 0;
    oddFrame = false;
    nmiPending = false;
    nmiOccurred = false;
    statusReadSuppressionWindow = 0;
    suppressNextVblank = false;
    suppressCurrentVblankNmi = false;
    sprite0StrikePending = false;
    sprite0StrikeCycle = 0;
    frameReady = false;
    sprite0HitThisFrame = false;
    sprite0CheckedThisLine = false;
    markAllTilesDirty();
    resetRenderPerfCounters();
    
    frameCount = 0;
}

void IRAM_ATTR PPU::beginVblank() {
    if (suppressNextVblank) {
        ppuStatus &= (uint8_t)~0x80;
        nmiOccurred = false;
        nmiPending = false;
        suppressNextVblank = false;
        return;
    }
    ppuStatus |= 0x80;
    nmiOccurred = true;
    if ((ppuCtrl & 0x80) && !suppressCurrentVblankNmi) {
        nmiPending = true;
    }
}

void IRAM_ATTR PPU::endVblank() {
    ppuStatus &= (uint8_t)~0x80;
    ppuStatus &= (uint8_t)~0x40;
    ppuStatus &= (uint8_t)~0x20;
    nmiOccurred = false;
    nmiPending = false;
    suppressCurrentVblankNmi = false;
    suppressNextVblank = false;
    statusReadSuppressionWindow = 0;
    sprite0StrikePending = false;
}

void IRAM_ATTR PPU::beginStatusReadSuppressionWindow(uint8_t mode) {
    statusReadSuppressionWindow = mode;
}

void IRAM_ATTR PPU::endStatusReadSuppressionWindow() {
    statusReadSuppressionWindow = 0;
}

void PPU::invalidateTileCache() {
    for (int i = 0; i < TILE_CACHE_SIZE; i++) {
        tileCache[i].valid = false;
    }
}

void PPU::clearTileDirtyMap() {
    memset(tileDirtyMap, 0, sizeof(tileDirtyMap));
    tileDirtyAll = false;
}

void PPU::markAllTilesDirty() {
    memset(tileDirtyMap, 1, sizeof(tileDirtyMap));
    tileDirtyAll = true;
}

void PPU::consumeTileDirtyMap(uint8_t* outMap, bool* outAll) {
    if (outMap) {
        memcpy(outMap, tileDirtyMap, sizeof(tileDirtyMap));
    }
    if (outAll) {
        *outAll = tileDirtyAll;
    }
    clearTileDirtyMap();
}

void PPU::resetRenderPerfCounters() {
    memset(&perfCounters, 0, sizeof(perfCounters));
}

void PPU::getRenderPerfCounters(RenderPerfCounters& out) const {
    out = perfCounters;
}

void IRAM_ATTR PPU::markTileDirty(int tx, int ty) {
    if ((unsigned)tx >= DIRTY_TILE_W || (unsigned)ty >= DIRTY_TILE_H) {
        return;
    }
    tileDirtyMap[ty * DIRTY_TILE_W + tx] = 1;
}

void IRAM_ATTR PPU::markTileDirtyRect(int tx, int ty, int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = tx;
    int y0 = ty;
    int x1 = tx + w - 1;
    int y1 = ty + h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= DIRTY_TILE_W) x1 = DIRTY_TILE_W - 1;
    if (y1 >= DIRTY_TILE_H) y1 = DIRTY_TILE_H - 1;
    for (int y = y0; y <= y1; ++y) {
        uint8_t* row = &tileDirtyMap[y * DIRTY_TILE_W];
        for (int x = x0; x <= x1; ++x) {
            row[x] = 1;
        }
    }
}

void IRAM_ATTR PPU::markDirtyByPpuAddr(uint16_t addr) {
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        
        markAllTilesDirty();
        return;
    }
    if (addr < 0x3F00) {
        
        uint16_t ntAddr = (addr - 0x2000) & 0x0FFF;
        uint16_t offs = ntAddr & 0x03FF;
        if (offs < 0x03C0) {
            int tx = offs & 0x1F;
            int ty = (offs >> 5) & 0x1F;
            if (ty < DIRTY_TILE_H) {
                markTileDirty(tx, ty);
            }
        } else {
            
            int attr = offs - 0x03C0;
            int ax = (attr & 0x07) * 4;
            int ay = ((attr >> 3) & 0x07) * 4;
            markTileDirtyRect(ax, ay, 4, 4);
        }
        return;
    }
    
    markAllTilesDirty();
}

uint8_t IRAM_ATTR PPU::regRead(uint8_t reg) {
    uint8_t result = 0;
    
    switch (reg) {
        case 2:
        {
            if (sprite0StrikePending && bus) {
                if (bus->cpu.getTotalCycles() >= sprite0StrikeCycle) {
                    ppuStatus |= 0x40;
                    sprite0StrikePending = false;
                }
            }
            
            
            
            
            const uint8_t oldStatus = ppuStatus;
            if (statusReadSuppressionWindow == 1) {
                // Reading $2002 in the vblank-set race window (scanline 241, dot 0)
                // clears the flag and suppresses NMI for this frame.
                suppressNextVblank = true;
                suppressCurrentVblankNmi = true;
                nmiPending = false;
                ppuStatus &= (uint8_t)~0x80;
                nmiOccurred = false;
            } else if (statusReadSuppressionWindow == 2) {
                // Reads in the couple of dots after vblank set can still suppress NMI.
                suppressCurrentVblankNmi = true;
                nmiPending = false;
            }

            result = oldStatus;
            ppuStatus &= 0x7F;    
            nmiOccurred = false;
            writeToggle = false;  
            break;
        }
            
        case 4: 
            
            
            result = oam[oamAddr];
            break;
            
        case 7: 
            if (cartDirect && cartDirect->needsPpuAddressObserve()) {
                cartDirect->observePpuAddress(vramAddr & 0x3FFF);
            }
            
            
            
            if (vramAddr < 0x3F00) {
                
                result = dataBuffer;
                dataBuffer = bus->ppuRead(vramAddr);
            } else {
                
                result = bus->ppuRead(vramAddr);
                dataBuffer = bus->ppuRead(vramAddr - 0x1000);
            }
            
            vramAddr += (ppuCtrl & 0x04) ? 32 : 1;
            vramAddr &= 0x3FFF;  
            break;
    }
    
    return result;
}

void IRAM_ATTR PPU::regWrite(uint8_t reg, uint8_t val) {
    switch (reg) {
        case 0: 
        {
            const bool oldNmiEnabled = (ppuCtrl & 0x80) != 0;
            if (ppuCtrl != val) {
                markAllTilesDirty();
            }
            ppuCtrl = val;
            
            tempAddr = (tempAddr & 0xF3FF) | ((val & 0x03) << 10);
            const bool newNmiEnabled = (ppuCtrl & 0x80) != 0;
            if (!oldNmiEnabled && newNmiEnabled && (ppuStatus & 0x80) && !suppressCurrentVblankNmi) {
                nmiPending = true;
            }
            break;
        }
            
        case 1: 
            if (ppuMask != val) {
                markAllTilesDirty();
            }
            ppuMask = val;
            break;
            
        case 3: 
            oamAddr = val;
            break;
            
        case 4: 
        {
            uint8_t idx = oamAddr;
            int sprite = idx >> 2;
            int base = sprite * 4;
            const int spriteHeight = (ppuCtrl & 0x20) ? 16 : 8;

            int oldY = (int)oam[base + 0] + 1;
            int oldX = (int)oam[base + 3];

            oam[oamAddr++] = val;

            int newY = (int)oam[base + 0] + 1;
            int newX = (int)oam[base + 3];

            auto markSpriteRect = [&](int x, int y)
            {
                if (x >= 256 || y >= 240)
                {
                    return;
                }
                if (x < 0)
                {
                    x = 0;
                }
                if (y < 0)
                {
                    y = 0;
                }
                int tx0 = x >> 3;
                int tx1 = (x + 7) >> 3;
                int ty0 = y >> 3;
                int ty1 = (y + spriteHeight - 1) >> 3;
                markTileDirtyRect(tx0, ty0, tx1 - tx0 + 1, ty1 - ty0 + 1);
            };

            markSpriteRect(oldX, oldY);
            markSpriteRect(newX, newY);
            break;
        }
            
        case 5: 
            if (!writeToggle) {
                
                
                
                fineX = val & 0x07;
                tempAddr = (tempAddr & 0xFFE0) | (val >> 3);
            } else {
                
                
                
                tempAddr = (tempAddr & 0x8C1F) | ((val & 0x07) << 12) | ((val & 0xF8) << 2);
            }
            writeToggle = !writeToggle;
            break;
            
        case 6: 
            if (!writeToggle) {
                
                tempAddr = (tempAddr & 0x00FF) | ((val & 0x3F) << 8);
            } else {
                
                tempAddr = (tempAddr & 0xFF00) | val;
                vramAddr = tempAddr;
                if (cartDirect && cartDirect->needsPpuAddressObserve()) {
                    cartDirect->observePpuAddress(vramAddr & 0x3FFF);
                }
            }
            writeToggle = !writeToggle;
            break;
            
        case 7: 
            if (cartDirect && cartDirect->needsPpuAddressObserve()) {
                cartDirect->observePpuAddress(vramAddr & 0x3FFF);
            }
            markDirtyByPpuAddr(vramAddr);
            bus->ppuWrite(vramAddr, val);
            vramAddr += (ppuCtrl & 0x04) ? 32 : 1;
            vramAddr &= 0x3FFF;
            break;
    }
}

void IRAM_ATTR PPU::oamDMA(uint8_t page, uint8_t* cpuRam) {
    (void)cpuRam;
    uint16_t addr = page << 8;

    uint8_t oldOam[256];
    memcpy(oldOam, oam, sizeof(oldOam));

    const uint8_t start = oamAddr;
    
    for (int i = 0; i < 256; i++) {
        
        oam[(uint8_t)(start + i)] = bus->cpuRead(addr++);
    }
    oamAddr = (uint8_t)(start + 256);

    // OAM DMA edge quirk used by some software timing paths.
    uint16_t edgeAddr = (uint16_t)(page << 8);
    if ((start >> 2) & 0x01) {
        for (uint8_t i = 4; i < 8; ++i) {
            oam[i] = bus->cpuRead(edgeAddr++);
        }
        edgeAddr += 248;
        for (uint8_t i = 0; i < 4; ++i) {
            oam[i] = bus->cpuRead(edgeAddr++);
        }
    } else {
        for (uint8_t i = 0; i < 8; ++i) {
            oam[i] = bus->cpuRead(edgeAddr++);
        }
    }

    
    const int spriteHeight = (ppuCtrl & 0x20) ? 16 : 8;
    auto markSpriteRect = [&](int x, int y)
    {
        if (x >= 256 || y >= 240)
        {
            return;
        }
        if (x < 0)
        {
            x = 0;
        }
        if (y < 0)
        {
            y = 0;
        }
        int tx0 = x >> 3;
        int tx1 = (x + 7) >> 3;
        int ty0 = y >> 3;
        int ty1 = (y + spriteHeight - 1) >> 3;
        markTileDirtyRect(tx0, ty0, tx1 - tx0 + 1, ty1 - ty0 + 1);
    };

    for (int i = 0; i < 64; ++i) {
        const int base = i * 4;
        const uint8_t oldYb = oldOam[base + 0];
        const uint8_t oldTile = oldOam[base + 1];
        const uint8_t oldAttr = oldOam[base + 2];
        const uint8_t oldXb = oldOam[base + 3];

        const uint8_t newYb = oam[base + 0];
        const uint8_t newTile = oam[base + 1];
        const uint8_t newAttr = oam[base + 2];
        const uint8_t newXb = oam[base + 3];

        if (oldYb == newYb && oldTile == newTile && oldAttr == newAttr && oldXb == newXb) {
            continue;
        }

        markSpriteRect((int)oldXb, (int)oldYb + 1);
        markSpriteRect((int)newXb, (int)newYb + 1);
    }
}

uint8_t IRAM_ATTR PPU::fastChrRead(uint16_t addr) {
    
    return cartDirect->ppuRead(addr);
}

uint8_t IRAM_ATTR PPU::fastVramRead(uint16_t addr) {
    
    return cartDirect->readNameTable(addr);
}

uint8_t IRAM_ATTR PPU::fastPaletteRead(uint8_t addr) {
    addr &= 0x1F;  
    
    if ((addr & 0x13) == 0x10) {
        addr &= 0x0F;
    }
    return paletteDirect[addr];
}

void IRAM_ATTR PPU::loadPaletteCache() {
    
    uint8_t bgColorIdx = fastPaletteRead(0) & 0x3F;
    uint16_t bgColor = nesPalette[bgColorIdx];
    
    
    for (int i = 0; i < 16; i++) {
        if ((i & 0x03) == 0) {
            
            bgPaletteCache[i] = bgColor;
        } else {
            
            uint8_t palIdx = fastPaletteRead(i) & 0x3F;
            bgPaletteCache[i] = nesPalette[palIdx];
        }
    }
    
    
    for (int i = 0; i < 16; i++) {
        if ((i & 0x03) == 0) {
            
            spPaletteCache[i] = bgColor;
        } else {
            
            uint8_t palIdx = fastPaletteRead(0x10 + i) & 0x3F;
            spPaletteCache[i] = nesPalette[palIdx];
        }
    }

    
}

void IRAM_ATTR PPU::incrementY() {
    
    if (!(ppuMask & 0x18)) return;
    
    
    int fineY = (vramAddr >> 12) & 0x07;
    
    if (fineY < 7) {
        
        vramAddr += 0x1000;
        return;
    }
    
    
    vramAddr &= ~0x7000;  
    
    int coarseY = (vramAddr >> 5) & 0x1F;
    
    if (coarseY == 29) {
        
        coarseY = 0;
        vramAddr ^= 0x0800;  
    } else if (coarseY == 31) {
        
        coarseY = 0;
    } else {
        coarseY++;
    }
    
    vramAddr = (vramAddr & ~0x03E0) | (coarseY << 5);
}

void IRAM_ATTR PPU::skipScanlineForScrollUpdate() {
    
    if ((ppuMask & 0x18) != 0) {
        vramAddr = (vramAddr & ~0x041F) | (tempAddr & 0x041F);
    }
    
    incrementY();
}

bool IRAM_ATTR PPU::checkSprite0HitFast(int scanline) {
    
    if (ppuStatus & 0x40) return true;
    
    
    if ((ppuMask & 0x18) != 0x18) return false;
    
    
    uint8_t sprite0Y = oam[0];
    uint8_t sprite0Tile = oam[1];
    uint8_t sprite0Attr = oam[2];
    uint8_t sprite0X = oam[3];
    
    
    if (sprite0Y >= 0xEF) return false;
    
    
    int spriteY = sprite0Y + 1;
    
    
    bool is8x16 = (ppuCtrl & 0x20) != 0;
    int spriteHeight = is8x16 ? 16 : 8;
    
    
    if (scanline < spriteY || scanline >= spriteY + spriteHeight) return false;
    
    
    if (sprite0X >= 255) return false;
    
    
    int startCheckX = sprite0X;
    int endCheckX = sprite0X + 8;
    if (startCheckX < 8 && !(ppuMask & 0x06)) {
        startCheckX = 8;
    }
    if (endCheckX > 255) endCheckX = 255;
    if (startCheckX >= endCheckX) return false;
    
    
    int spriteRow = scanline - spriteY;
    bool flipV = (sprite0Attr & 0x80) != 0;
    bool flipH = (sprite0Attr & 0x40) != 0;
    int patternRow = flipV ? (spriteHeight - 1 - spriteRow) : spriteRow;
    
    uint16_t spPatternBase = (ppuCtrl & 0x08) ? 0x1000 : 0x0000;
    uint16_t spTileAddr;
    
    if (is8x16) {
        uint16_t base = (sprite0Tile & 0x01) ? 0x1000 : 0x0000;
        uint8_t tile = sprite0Tile & 0xFE;
        if (patternRow >= 8) {
            tile++;
            patternRow -= 8;
        }
        spTileAddr = base + tile * 16 + patternRow;
    } else {
        spTileAddr = spPatternBase + sprite0Tile * 16 + patternRow;
    }
    
    uint8_t** chrPtrs_sp0 = cartDirect->chrBankPtrs;
    uint8_t** ntPtrs_sp0 = cartDirect->ntPtrs;
    const bool mapper5 = cartDirect->getMapper() == 5;
    const bool observeAddr = cartDirect->needsPpuAddressObserve();
    
    uint8_t spPatLo = mapper5 ? cartDirect->readMmc5SpPatternByte(spTileAddr)
                              : chrPtrs_sp0[(spTileAddr >> 10) & 7][spTileAddr & 0x3FF];
    uint16_t spTileAddr8 = spTileAddr + 8;
    uint8_t spPatHi = mapper5 ? cartDirect->readMmc5SpPatternByte(spTileAddr8)
                              : chrPtrs_sp0[(spTileAddr8 >> 10) & 7][spTileAddr8 & 0x3FF];
    if (observeAddr) {
        cartDirect->observePpuAddress(spTileAddr);
        cartDirect->observePpuAddress(spTileAddr8);
        PERF_ADD(observeCalls, 2);
    }
    
    
    if ((spPatLo | spPatHi) == 0) return false;
    
    
    uint8_t spPixels[8];
    if (flipH) {
        for (int i = 0; i < 8; i++) {
            spPixels[i] = ((spPatLo >> i) & 1) | (((spPatHi >> i) & 1) << 1);
        }
    } else {
        for (int i = 0; i < 8; i++) {
            spPixels[i] = ((spPatLo >> (7 - i)) & 1) | (((spPatHi >> (7 - i)) & 1) << 1);
        }
    }
    
    
    uint16_t bgPatternBase = (ppuCtrl & 0x10) ? 0x1000 : 0x0000;
    int coarseX = vramAddr & 0x001F;
    int coarseY = (vramAddr >> 5) & 0x1F;
    int fineYOffset = (vramAddr >> 12) & 0x07;
    int baseNt = (vramAddr >> 10) & 0x03;
    
    
    for (int bit = 0; bit < 8; bit++) {
        int sx = sprite0X + bit;
        if (sx < startCheckX || sx >= endCheckX) continue;
        if (spPixels[bit] == 0) continue;  
        
        
        int bgPixelX = sx + fineX;
        int bgTileX = coarseX + (bgPixelX >> 3);
        int nt = baseNt;
        if (bgTileX >= 32) {
            bgTileX -= 32;
            nt ^= 1;
        }
        int tileFineX = bgPixelX & 0x07;
        
        
        uint16_t ntAddr = (uint16_t)((nt << 10) | (coarseY * 32 + bgTileX));
        uint8_t tileIndex = mapper5 ? cartDirect->readNameTable(ntAddr)
                                    : ntPtrs_sp0[nt][coarseY * 32 + bgTileX];
        
        
        uint16_t bgPatAddr = bgPatternBase + tileIndex * 16 + fineYOffset;
        uint8_t bgPatLo = mapper5 ? cartDirect->readMmc5BgPatternByte(bgPatAddr)
                                  : chrPtrs_sp0[(bgPatAddr >> 10) & 7][bgPatAddr & 0x3FF];
        uint16_t bgPatAddr8 = bgPatAddr + 8;
        uint8_t bgPatHi = mapper5 ? cartDirect->readMmc5BgPatternByte(bgPatAddr8)
                                  : chrPtrs_sp0[(bgPatAddr8 >> 10) & 7][bgPatAddr8 & 0x3FF];
        if (observeAddr) {
            cartDirect->observePpuAddress(bgPatAddr);
            cartDirect->observePpuAddress(bgPatAddr8);
            PERF_ADD(observeCalls, 2);
        }
        
        
        int shift = 7 - tileFineX;
        uint8_t bgPx = ((bgPatLo >> shift) & 1) | (((bgPatHi >> shift) & 1) << 1);
        
        
        if (bgPx != 0) {
            if (sx < 255 && !(ppuStatus & 0x40) && !sprite0StrikePending) {
                if (bus) {
                    sprite0StrikeCycle = bus->cpu.getTotalCycles() + (uint64_t)(sx / 3);
                } else {
                    sprite0StrikeCycle = 0;
                }
                sprite0StrikePending = true;
            }
            return true;
        }
    }
    
    return false;
}

void IRAM_ATTR PPU::evaluateOAM() {
    memset(spriteCountPerLine, 0, 240);
    
    ppuStatus &= ~0x20;
    
    int spriteHeight = (ppuCtrl & 0x20) ? 16 : 8;
    
    
    // Pick the first 8 sprites per scanline in OAM order (lowest index first).
    // Rendering then iterates back-to-front so lower indices stay on top.
    for (int i = 0; i < 64; ++i) {
        uint8_t y = oam[i * 4];
        if (y >= 0xEF) continue;
        
        int spriteY = y + 1;
        int endY = spriteY + spriteHeight;
        if (endY > 240) endY = 240;
        
        for (int sl = spriteY; sl < endY; sl++) {
            if (spriteCountPerLine[sl] < MAX_SPRITES_PER_LINE) {
                spriteIndicesPerLine[sl][spriteCountPerLine[sl]++] = i;
            } else {
                ppuStatus |= 0x20;
            }
        }
    }
}

void IRAM_ATTR PPU::renderBackgroundLine(int scanline, uint16_t* lineBuffer) {
    (void)scanline;
    PERF_INC(bgLines);
    const bool hasLineBuffer = (lineBuffer != nullptr);
    
    uint32_t* p = (uint32_t*)bgPixelOpacity;
    for (int i = 0; i < 256 / 4; i++) {
        p[i] = 0;
    }
    
    
    if (!(ppuMask & 0x08)) {
        
        if (hasLineBuffer) {
            uint16_t bgColor = bgPaletteCache[0];
            uint32_t bgColor32 = (bgColor << 16) | bgColor;
            uint32_t* lb32 = (uint32_t*)lineBuffer;
            for (int i = 0; i < 128; i++) { lb32[i] = bgColor32; }
        }
        return;
    }
    
    
    uint16_t patternBase = (ppuCtrl & 0x10) ? 0x1000 : 0x0000;
    
    
    
    uint8_t** ntPtrs = cartDirect->ntPtrs;
    uint8_t** chrPtrs = cartDirect->chrBankPtrs;
    const uint8_t mapperId = cartDirect->getMapper();
    const bool mapper5 = mapperId == 5;
    const bool mapper5ExtAttr = mapper5 && cartDirect->isMapper5ExtAttrMode();
    const bool observeAddr = cartDirect->needsPpuAddressObserve();
    const bool observeNtAt = observeAddr && mapperId == 163;
    
    
    int coarseX = vramAddr & 0x001F;
    int coarseY = (vramAddr >> 5) & 0x1F;
    int fineYOffset = (vramAddr >> 12) & 0x07;
    int baseNt = (vramAddr >> 10) & 0x03;
    int tileY = coarseY;
    
    
    uint16_t bgColor = bgPaletteCache[0];
    
    int screenX = 0;
    
    int curCoarseX = coarseX + (fineX >> 3);
    int curNt = baseNt;
    if (curCoarseX >= 32) { curCoarseX -= 32; curNt ^= 1; }
    
    for (int tileCount = 0; tileCount < 33 && screenX < 256; tileCount++) {
        PERF_INC(bgTiles);
        
        int ntIdx = curNt;
        uint16_t tileOffset = tileY * 32 + curCoarseX;
        uint16_t ntReadAddr = (uint16_t)((ntIdx << 10) | tileOffset);
        if (observeNtAt) {
            cartDirect->observePpuAddress((uint16_t)(0x2000 + ntReadAddr));
            PERF_INC(observeCalls);
        }
        uint8_t tileIndex = mapper5 ? cartDirect->readNameTable(ntReadAddr) : ntPtrs[ntIdx][tileOffset];
        
        uint16_t attrOffset = 0x3C0 + (tileY >> 2) * 8 + (curCoarseX >> 2);
        uint16_t atReadAddr = (uint16_t)((ntIdx << 10) | attrOffset);
        if (observeNtAt) {
            cartDirect->observePpuAddress((uint16_t)(0x2000 + atReadAddr));
            PERF_INC(observeCalls);
        }
        uint8_t paletteNum;
        if (mapper5ExtAttr) {
            paletteNum = cartDirect->mmc5ExtAttrByte(tileOffset) & 0x03;
        } else {
            uint8_t attrByte = mapper5 ? cartDirect->readNameTable(atReadAddr) : ntPtrs[ntIdx][attrOffset];
            int attrShift = ((tileY & 0x02) << 1) | (curCoarseX & 0x02);
            paletteNum = (attrByte >> attrShift) & 0x03;
        }
        
        
        uint16_t* srcPal = &bgPaletteCache[paletteNum << 2];
        uint16_t tilePal[4] = { bgColor, srcPal[1], srcPal[2], srcPal[3] };
        
        
        uint16_t patAddr = patternBase + tileIndex * 16 + fineYOffset;
        uint8_t patLo = mapper5 ? cartDirect->readMmc5BgPatternByte(patAddr)
                                : chrPtrs[(patAddr >> 10) & 7][patAddr & 0x3FF];
        uint16_t patAddr8 = patAddr + 8;
        uint8_t patHi = mapper5 ? cartDirect->readMmc5BgPatternByte(patAddr8)
                                : chrPtrs[(patAddr8 >> 10) & 7][patAddr8 & 0x3FF];
        if (observeAddr) {
            cartDirect->observePpuAddress(patAddr);
            cartDirect->observePpuAddress(patAddr8);
            PERF_ADD(observeCalls, 2);
        }
        
        int startBit = (tileCount == 0) ? (fineX & 7) : 0;
        
        if ((patLo | patHi) == 0) {
            PERF_INC(bgZeroTiles);
            
            int pixelsToRender = 8 - startBit;
            if (hasLineBuffer) {
                if (startBit == 0 && screenX + 8 <= 256) {
                    PERF_INC(bgFastFillTiles);
                    uint32_t bgColor32 = (bgColor << 16) | bgColor;
                    uint32_t* out32 = (uint32_t*)(lineBuffer + screenX);
                    out32[0] = bgColor32;
                    out32[1] = bgColor32;
                    out32[2] = bgColor32;
                    out32[3] = bgColor32;
                } else {
                    for (int k = 0; k < pixelsToRender && screenX + k < 256; k++) {
                        lineBuffer[screenX + k] = bgColor;
                    }
                }
            }
            screenX += pixelsToRender;
            if (screenX > 256) screenX = 256;
        } else {
            if (startBit == 0 && screenX + 8 <= 256 && hasLineBuffer) {
                PERF_INC(bgFastOpaqueTiles);
                const uint8_t p0 = (uint8_t)(((patLo >> 7) & 1) | (((patHi >> 7) & 1) << 1));
                const uint8_t p1 = (uint8_t)(((patLo >> 6) & 1) | (((patHi >> 6) & 1) << 1));
                const uint8_t p2 = (uint8_t)(((patLo >> 5) & 1) | (((patHi >> 5) & 1) << 1));
                const uint8_t p3 = (uint8_t)(((patLo >> 4) & 1) | (((patHi >> 4) & 1) << 1));
                const uint8_t p4 = (uint8_t)(((patLo >> 3) & 1) | (((patHi >> 3) & 1) << 1));
                const uint8_t p5 = (uint8_t)(((patLo >> 2) & 1) | (((patHi >> 2) & 1) << 1));
                const uint8_t p6 = (uint8_t)(((patLo >> 1) & 1) | (((patHi >> 1) & 1) << 1));
                const uint8_t p7 = (uint8_t)((patLo & 1) | ((patHi & 1) << 1));

                uint32_t* out32 = (uint32_t*)(lineBuffer + screenX);
                out32[0] = (uint32_t)tilePal[p0] | ((uint32_t)tilePal[p1] << 16);
                out32[1] = (uint32_t)tilePal[p2] | ((uint32_t)tilePal[p3] << 16);
                out32[2] = (uint32_t)tilePal[p4] | ((uint32_t)tilePal[p5] << 16);
                out32[3] = (uint32_t)tilePal[p6] | ((uint32_t)tilePal[p7] << 16);
                
                uint8_t* op = bgPixelOpacity + screenX;
                op[0] = (p0 != 0);
                op[1] = (p1 != 0);
                op[2] = (p2 != 0);
                op[3] = (p3 != 0);
                op[4] = (p4 != 0);
                op[5] = (p5 != 0);
                op[6] = (p6 != 0);
                op[7] = (p7 != 0);
                PERF_ADD(bgOpaquePixels,
                    (uint32_t)(p0 != 0) + (uint32_t)(p1 != 0) +
                    (uint32_t)(p2 != 0) + (uint32_t)(p3 != 0) +
                    (uint32_t)(p4 != 0) + (uint32_t)(p5 != 0) +
                    (uint32_t)(p6 != 0) + (uint32_t)(p7 != 0));
                screenX += 8;
            } else {
                
                for (int bit = startBit; bit < 8 && screenX < 256; bit++, screenX++) {
                    uint8_t px = (uint8_t)(((patLo >> (7 - bit)) & 1) | (((patHi >> (7 - bit)) & 1) << 1));
                    if (hasLineBuffer) {
                        lineBuffer[screenX] = tilePal[px];
                    }
                    bgPixelOpacity[screenX] = (px != 0);
                    if (px != 0) {
                        PERF_INC(bgOpaquePixels);
                    }
                }
            }
        }
        
        
        curCoarseX++;
        if (curCoarseX >= 32) {
            curCoarseX = 0;
            curNt ^= 1;
        }
    }

    
    if (!(ppuMask & 0x02)) {
        const uint16_t clippedLeftColor = FORCE_BLACK_LEFT_BG_CLIP ? 0x0000 : bgColor;
        for (int x = 0; x < 8; ++x) {
            if (bgPixelOpacity[x]) {
                PERF_DEC_SAT(bgOpaquePixels);
            }
        }
        if (hasLineBuffer) {
            for (int x = 0; x < 8; ++x) {
                lineBuffer[x] = clippedLeftColor;
                bgPixelOpacity[x] = 0;
            }
        } else {
            for (int x = 0; x < 8; ++x) {
                bgPixelOpacity[x] = 0;
            }
        }
    }

    
    if (hasLineBuffer && screenX < 256) {
        uint16_t* dst16 = lineBuffer + screenX;
        int remain = 256 - screenX;
        uint32_t bgColor32 = (bgColor << 16) | bgColor;
        uint32_t* dst32 = (uint32_t*)dst16;
        for (int i = 0; i < (remain >> 1); ++i) {
            dst32[i] = bgColor32;
        }
        if (remain & 1) {
            dst16[remain - 1] = bgColor;
        }
        memset(bgPixelOpacity + screenX, 0, (size_t)remain);
    }
}

void IRAM_ATTR PPU::renderSpriteLine(int scanline, uint16_t* lineBuffer) {
    
    if (!(ppuMask & 0x10)) return;
    PERF_INC(spriteLines);
    const bool hideLeftSprite = (ppuMask & 0x04) == 0;
    const bool hideLeftBg = (ppuMask & 0x02) == 0;
    
    
    uint16_t patternBase = (ppuCtrl & 0x08) ? 0x1000 : 0x0000;
    
    
    uint8_t** chrPtrs = cartDirect->chrBankPtrs;
    const bool mapper5 = cartDirect->getMapper() == 5;
    const bool observeAddr = cartDirect->needsPpuAddressObserve();
    
    
    bool is8x16 = (ppuCtrl & 0x20) != 0;
    int spriteHeight = is8x16 ? 16 : 8;
    
    
    bool checkSprite0Hit = ((ppuStatus & 0x40) == 0);  
    
    
    
    int count = spriteCountPerLine[scanline];
    PERF_ADD(spriteCandidates, (uint32_t)count);
    if (count == 0) {
        return;
    }
    
    // Draw high indices first so low OAM indices win sprite-to-sprite priority.
    for (int j = count - 1; j >= 0; --j) {
        int i = spriteIndicesPerLine[scanline][j];
        
        uint8_t y = oam[i * 4 + 0];
        uint8_t tileIndex = oam[i * 4 + 1];
        uint8_t attr = oam[i * 4 + 2];
        uint8_t x = oam[i * 4 + 3];
        
        int spriteY = y + 1;
        
        
        int paletteNum = attr & 0x03;
        bool flipH = (attr & 0x40) != 0;
        bool flipV = (attr & 0x80) != 0;
        bool behindBg = (attr & 0x20) != 0;
        
        int palOffset = paletteNum << 2;
        
        
        int row = scanline - spriteY;
        int patternRow = flipV ? (spriteHeight - 1 - row) : row;
        
        
        uint16_t tileAddr;
        if (is8x16) {
            
            uint16_t base = (tileIndex & 0x01) ? 0x1000 : 0x0000;
            uint8_t tile = tileIndex & 0xFE;  
            
            
            if (patternRow >= 8) {
                tile++;           
                patternRow -= 8;  
            }
            tileAddr = base + tile * 16 + patternRow;
        } else {
            
            tileAddr = patternBase + tileIndex * 16 + patternRow;
        }
        
        
        uint8_t patternLo = mapper5 ? cartDirect->readMmc5SpPatternByte(tileAddr)
                                    : chrPtrs[(tileAddr >> 10) & 7][tileAddr & 0x3FF];
        uint16_t tileAddr8 = tileAddr + 8;
        uint8_t patternHi = mapper5 ? cartDirect->readMmc5SpPatternByte(tileAddr8)
                                    : chrPtrs[(tileAddr8 >> 10) & 7][tileAddr8 & 0x3FF];
        if (observeAddr) {
            cartDirect->observePpuAddress(tileAddr);
            cartDirect->observePpuAddress(tileAddr8);
            PERF_ADD(observeCalls, 2);
        }
        
        
        if ((patternLo | patternHi) == 0) continue;
        
        
        uint16_t* spPal = &spPaletteCache[palOffset];

        uint8_t bitsLo = flipH ? patternLo : reverse8(patternLo);
        uint8_t bitsHi = flipH ? patternHi : reverse8(patternHi);

        if (i == 0 && checkSprite0Hit && (ppuMask & 0x08)) {
            uint8_t lo = bitsLo;
            uint8_t hi = bitsHi;
            int sx = x;
            for (int bit = 0; bit < 8 && sx < 255; ++bit, ++sx) {
                uint8_t px = (uint8_t)((lo & 0x01u) | ((hi & 0x01u) << 1));
                lo >>= 1;
                hi >>= 1;
                if (!px) {
                    continue;
                }
                if (sx < 8 && (hideLeftSprite || hideLeftBg)) {
                    continue;
                }
                const bool bgOpaque = (bgPixelOpacity[sx] != 0);
                if (bgOpaque) {
                    if (sx < 255 && !(ppuStatus & 0x40) && !sprite0StrikePending) {
                        if (bus) {
                            sprite0StrikeCycle = bus->cpu.getTotalCycles() + (uint64_t)(sx / 3);
                        } else {
                            sprite0StrikeCycle = 0;
                        }
                        sprite0StrikePending = true;
                    }
                    checkSprite0Hit = false;
                }
                if (lineBuffer && (!behindBg || !bgOpaque)) {
                    lineBuffer[sx] = spPal[px];
                    PERF_INC(spriteDrawnPixels);
                }
            }
            continue;
        }

        if (!lineBuffer) {
            continue;
        }

        if (!behindBg && !hideLeftSprite && x >= 8) {
            PERF_INC(spriteFastPathSprites);
            int sx = x;
            uint8_t lo = bitsLo;
            uint8_t hi = bitsHi;
            for (int bit = 0; bit < 8 && sx < 256; ++bit, ++sx) {
                uint8_t px = (uint8_t)((lo & 0x01u) | ((hi & 0x01u) << 1));
                lo >>= 1;
                hi >>= 1;
                if (px) {
                    lineBuffer[sx] = spPal[px];
                    PERF_INC(spriteDrawnPixels);
                }
            }
            continue;
        }

        int sx = x;
        uint8_t lo = bitsLo;
        uint8_t hi = bitsHi;
        for (int bit = 0; bit < 8 && sx < 256; ++bit, ++sx) {
            uint8_t px = (uint8_t)((lo & 0x01u) | ((hi & 0x01u) << 1));
            lo >>= 1;
            hi >>= 1;
            if (!px) {
                continue;
            }
            if (sx < 8 && hideLeftSprite) {
                continue;
            }
            if (behindBg && bgPixelOpacity[sx]) {
                continue;
            }
            lineBuffer[sx] = spPal[px];
            PERF_INC(spriteDrawnPixels);
        }
    }
}

void IRAM_ATTR PPU::renderLine(int scanline, uint16_t* lineBuffer) {
    if (cartDirect && cartDirect->consumePpuRenderingDirty()) {
        markAllTilesDirty();
    }
    
    bool renderingEnabled = (ppuMask & 0x18) != 0;
    if (cartDirect) {
        cartDirect->notifyScanlineStart(scanline, renderingEnabled);
    }
    
    if (scanline == 0) {
        PERF_INC(frames);
        
        loadPaletteCache();
        
        if (renderingEnabled) {
            vramAddr = tempAddr;
        }
        
        evaluateOAM();
    } else {
        
        if (renderingEnabled) {
            vramAddr = (vramAddr & ~0x041F) | (tempAddr & 0x041F);
        }
    }
    
    
    
    
    
    renderBackgroundLine(scanline, lineBuffer);
    
    
    renderSpriteLine(scanline, lineBuffer);
    if (cartDirect) {
        cartDirect->finalizeMmc3Scanline(renderingEnabled, ppuCtrl);
    }
    
    
    incrementY();
}

void PPU::render(uint16_t* fb) {
    if (cartDirect && cartDirect->consumePpuRenderingDirty()) {
        markAllTilesDirty();
    }

    
    loadPaletteCache();
    
    
    uint16_t bgColor = bgPaletteCache[0];
    
    
    uint32_t bgColor32 = (bgColor << 16) | bgColor;
    uint32_t* fb32 = (uint32_t*)fb;
    int count32 = (256 * 240) / 2;
    for (int i = 0; i < count32; i++) {
        fb32[i] = bgColor32;
    }
    
    
    ppuStatus &= ~0x40;
    
    
    for (int sl = 0; sl < 240; sl++) {
        uint16_t* lineBuffer = fb + sl * 256;
        
        
        renderBackgroundLine(sl, lineBuffer);
        
        
        renderSpriteLine(sl, lineBuffer);
    }
    
    
    ppuStatus |= 0x80;
    
    frameCount++;
}

void IRAM_ATTR PPU::stepPPU() {
    
    if (scanline < 240) {
        
        if (dot == 0) {
            
            uint8_t sprite0Y = oam[0];
            sprite0HitPossible = (sprite0Y < 0xEF) && (sprite0Y + 1 <= scanline) && 
                                  (scanline < sprite0Y + 1 + ((ppuCtrl & 0x20) ? 16 : 8));
            sprite0Rendered = false;
        }
        
        
        
        if (sprite0HitPossible && dot >= 2 && dot <= 254 && !sprite0Rendered) {
            
            if ((ppuMask & 0x18) == 0x18) {  
                
                uint8_t sprite0X = oam[3];
                if (dot >= sprite0X + 1 && dot < sprite0X + 9) {
                    
                    ppuStatus |= 0x40;  
                    sprite0Rendered = true;
                }
            }
        }
    }
    
    
    if (scanline == 241 && dot == 1) {
        beginVblank();
    }
    
    
    if (scanline == 261) {
        if (dot == 1) {
            endVblank();
        }
        
        
        if ((ppuMask & 0x18) && dot >= 280 && dot <= 304) {
            
            
            
            vramAddr = (vramAddr & 0x041F) | (tempAddr & 0x7BE0);
        }
    }
    
    
    dot++;
    
    
    if (dot > 340) {
        dot = 0;
        scanline++;
        
        
        if (scanline <= 240 && (ppuMask & 0x18)) {
            
            vramAddr = (vramAddr & 0xFBE0) | (tempAddr & 0x041F);
        }
        
        
        if (scanline > 261) {
            scanline = 0;
            frameCount++;
            oddFrame = !oddFrame;
            
            
            if (oddFrame && (ppuMask & 0x18)) {
                dot = 1;
            }
        }
    }
}

void IRAM_ATTR PPU::advanceCycles(int cycles) {
    while (cycles > 0) {

        
        
        
        int toLineEnd = 341 - ppuCycle;
        int step = (cycles < toLineEnd) ? cycles : toLineEnd;

        int oldCycle = ppuCycle;
        ppuCycle += step;
        cycles -= step;

        
        
        
        if (ppuScanline < 240 && frameBuffer) {
            if (oldCycle < 1 && ppuCycle >= 1) {

                
                if (ppuScanline == 0) {
                    loadPaletteCache();
                    
                    renderedThisFrame = false;
                }

                uint16_t* line = frameBuffer + ppuScanline * 256;

                
                
                uint16_t bgColor = bgPaletteCache[0];
                uint32_t bg32 = (bgColor << 16) | bgColor;
                uint32_t* p32 = (uint32_t*)line;
                for (int i = 0; i < 128; i++) p32[i] = bg32;

                
                if (renderEnabled) {
                    
                    if (ppuMask & 0x08) {
                        renderBackgroundLine(ppuScanline, line);
                    }

                    
                    if (ppuMask & 0x10) {
                        renderSpriteLine(ppuScanline, line);
                    }
                    
                    renderedThisFrame = true;
                } else {
                    
                    
                }
            }
            
            
            if (oldCycle < 256 && ppuCycle >= 256 && (ppuMask & 0x18)) {
                
                if ((vramAddr & 0x7000) != 0x7000) {
                    vramAddr += 0x1000;
                } else {
                    
                    vramAddr &= ~0x7000;
                    int coarseY = (vramAddr & 0x03E0) >> 5;
                    if (coarseY == 29) {
                        
                        coarseY = 0;
                        vramAddr ^= 0x0800;  
                    } else if (coarseY == 31) {
                        
                        coarseY = 0;
                    } else {
                        coarseY++;
                    }
                    vramAddr = (vramAddr & ~0x03E0) | (coarseY << 5);
                }
            }
            
            
            if (oldCycle < 257 && ppuCycle >= 257 && (ppuMask & 0x18)) {
                
                vramAddr = (vramAddr & ~0x041F) | (tempAddr & 0x041F);
            }
        }

        
        
        
        if (ppuScanline == 241) {
            if (oldCycle < 1 && ppuCycle >= 1) {
                beginVblank();
                frameReady = true;
                sprite0HitThisFrame = false;
            }
        }

        
        
        
        if (ppuScanline == 261) {
            if (oldCycle < 1 && ppuCycle >= 1) {
                endVblank();
            }
            
            
            
            if (ppuCycle >= 280 && ppuCycle <= 304 && (ppuMask & 0x18)) {
                
                vramAddr = (vramAddr & ~0x7BE0) | (tempAddr & 0x7BE0);
            }
        }

        
        
        
        if (ppuCycle >= 341) {
            ppuCycle = 0;
            ppuScanline++;

            
            if (ppuScanline >= 262) {
                ppuScanline = 0;
                frameCount++;
                oddFrame = !oddFrame;

                
                if (oddFrame && (ppuMask & 0x08)) {
                    ppuCycle = 1;
                }
            }
        }
    }
}

size_t PPU::getStateSize() const {
    size_t size = 0;
    
    size += sizeof(ppuCtrl);
    size += sizeof(ppuMask);
    size += sizeof(ppuStatus);
    size += sizeof(oamAddr);
    
    size += sizeof(vramAddr);
    size += sizeof(tempAddr);
    size += sizeof(fineX);
    size += sizeof(writeToggle);
    size += sizeof(dataBuffer);
    
    size += sizeof(oam);
    
    size += sizeof(scanline);
    size += sizeof(dot);
    size += sizeof(oddFrame);
    size += sizeof(nmiOccurred);
    size += sizeof(nmiPending);
    size += sizeof(statusReadSuppressionWindow);
    size += sizeof(suppressNextVblank);
    size += sizeof(suppressCurrentVblankNmi);
    size += sizeof(frameCount);
    
    size += sizeof(sprite0HitPossible);
    size += sizeof(sprite0Rendered);
    return size;
}

void PPU::saveState(uint8_t* buf, size_t& offset) const {
    
    buf[offset++] = ppuCtrl;
    buf[offset++] = ppuMask;
    buf[offset++] = ppuStatus;
    buf[offset++] = oamAddr;
    
    
    buf[offset++] = vramAddr & 0xFF;
    buf[offset++] = (vramAddr >> 8) & 0xFF;
    buf[offset++] = tempAddr & 0xFF;
    buf[offset++] = (tempAddr >> 8) & 0xFF;
    buf[offset++] = fineX;
    buf[offset++] = writeToggle ? 1 : 0;
    buf[offset++] = dataBuffer;
    
    
    memcpy(buf + offset, oam, sizeof(oam));
    offset += sizeof(oam);
    
    
    buf[offset++] = scanline & 0xFF;
    buf[offset++] = (scanline >> 8) & 0xFF;
    buf[offset++] = dot & 0xFF;
    buf[offset++] = (dot >> 8) & 0xFF;
    buf[offset++] = oddFrame ? 1 : 0;
    buf[offset++] = nmiOccurred ? 1 : 0;
    buf[offset++] = nmiPending ? 1 : 0;
    buf[offset++] = statusReadSuppressionWindow;
    buf[offset++] = suppressNextVblank ? 1 : 0;
    buf[offset++] = suppressCurrentVblankNmi ? 1 : 0;
    
    
    buf[offset++] = frameCount & 0xFF;
    buf[offset++] = (frameCount >> 8) & 0xFF;
    buf[offset++] = (frameCount >> 16) & 0xFF;
    buf[offset++] = (frameCount >> 24) & 0xFF;
    
    
    buf[offset++] = sprite0HitPossible ? 1 : 0;
    buf[offset++] = sprite0Rendered ? 1 : 0;
}

void PPU::loadState(const uint8_t* buf, size_t& offset) {
    
    ppuCtrl = buf[offset++];
    ppuMask = buf[offset++];
    ppuStatus = buf[offset++];
    oamAddr = buf[offset++];
    
    
    vramAddr = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    tempAddr = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    fineX = buf[offset++];
    writeToggle = buf[offset++] != 0;
    dataBuffer = buf[offset++];
    
    
    memcpy(oam, buf + offset, sizeof(oam));
    offset += sizeof(oam);
    
    
    scanline = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    dot = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    oddFrame = buf[offset++] != 0;
    nmiOccurred = buf[offset++] != 0;
    nmiPending = buf[offset++] != 0;
    statusReadSuppressionWindow = buf[offset++];
    suppressNextVblank = buf[offset++] != 0;
    suppressCurrentVblankNmi = buf[offset++] != 0;
    
    
    frameCount = buf[offset] | (buf[offset + 1] << 8) | 
                 (buf[offset + 2] << 16) | (buf[offset + 3] << 24);
    offset += 4;
    
    
    sprite0HitPossible = buf[offset++] != 0;
    sprite0Rendered = buf[offset++] != 0;
    
    
    loadPaletteCache();
    invalidateTileCache();
}
