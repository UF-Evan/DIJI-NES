#include "nes.h"
#include <SD.h>

bool NES::loadROM(const char* path) {
    cpu.connect(this);
    ppu.connect(this);
    
    // ?? ROM ??
    strncpy(currentRomPath, path, sizeof(currentRomPath) - 1);
    currentRomPath[sizeof(currentRomPath) - 1] = '\0';
    
    return cart.load(path);
}

void NES::reset() {
    memset(ram, 0, sizeof(ram));
    memset(vram, 0, sizeof(vram));
    memset(palette, 0, sizeof(palette));
    controller[0] = controller[1] = 0;
    controllerLatch[0] = controllerLatch[1] = 0;
    controllerShift[0] = controllerShift[1] = 0;
    controllerStrobe = false;
    
    // ??????
    mirrorVertical = cart.getMirrorVertical();
    
    // ? Cartridge ?? VRAM ?????? NES??? MMC3 ????? IRQ?
    cart.setVramPointer(vram);
    cart.setNES(this);

    // ? PPU ??????????
    ppu.setMemoryPointers(vram, palette, &cart, &mirrorVertical);
    
    cpu.reset();
    ppu.reset();
    apu.reset();
}

/**
 * NES::step()
 * ----------------------------------------------------------------------------
 * ?? CPU ??????
 *   - ?? 1 ? CPU ??
 *   - ???? cpuCycles * 3 ? PPU cycle
 *   - ???????????
 *
 * ???
 *   - instruction-accurate
 *   - cycle-accurate?CPU:PPU = 1:3?
 *   - ?????????????
 */
uint8_t IRAM_ATTR NES::step() {
    // 1. ???? CPU ??
    uint8_t cpuCycles = cpu.step();
    cart.clockCpuCycles(cpuCycles);

    // 2. ? CPU ????? PPU ??
    int ppuCycles = cpuCycles * 3;

    // 3. ???? PPU??????????????
    while (ppuCycles > 0) {
        int dot = ppu.getCurrentDot();           // ??????? dot
        int remainInLine = 341 - dot;            // ??????? dot ?
        int step = (ppuCycles < remainInLine) ? ppuCycles : remainInLine;

        // ?? PPU step ???
        ppu.advanceCycles(step);

        // ?????? PPU ??
        ppuCycles -= step;

        // ????????????????? CPU ????????
        if (step >= remainInLine) {
            break;
        }
    }

    // 4. ?? NMI?VBlank + NMI ???
    if (ppu.isNmiPending()) {
        ppu.clearNmiPending();
        cpu.nmi();
    }

    // 5. ?? Mapper IRQ
    if (cart.irqPending()) {
        cart.acknowledgeIrq();
        cpu.irq();
    }

    // 6. ?? CPU ??
    return cpuCycles;
}

/**
 * NES::stepScanline()
 * ----------------------------------------------------------------------------
 * ??????????
 *
 * ?????????????? CPU ???
 *   - ?? 113 / 114 CPU ??????? 113.666...
 *   - ????? CPU ???????? PPU??3?
 *   - ?????????
 *       - NMI?VBlank?
 *       - Mapper IRQ?? MMC3?
 *
 * ???
 *   - ?????Sprite 0 Hit?MMC3 IRQ?
 *   - ? step() ???????????
 *
 * ???
 *   ? ????
 *   ? ??????????
 *   ? ???????????
 */
void IRAM_ATTR NES::stepScanline() {
    // =????? NES ??? 113.666...
    // ?? 113/114 ????
    int target = scanlineParity ? 114 : 113;
    int executed = 0;

    while (executed < target) {
        uint8_t c = cpu.step();
        cart.clockCpuCycles(c);
        executed += c;
        // ?? PPU?CPU ?? ?3?
        ppu.advanceCycles(c * 3);

        // ?? NMI
        if (ppu.isNmiPending()) {
            ppu.clearNmiPending();
            cpu.nmi();
        }

        // MMC3 IRQ ??
        if (cart.irqPending()) {
            cart.acknowledgeIrq();
            cpu.irq();
        }
    }

    // ????????????113/114 ???
    scanlineParity = !scanlineParity;
}


// void IRAM_ATTR NES::stepThreeScanlines() {
//     // Batch 3 scanlines to reduce call overhead (matches Anemoia pattern)
//     stepScanline();
//     stepScanline();
//     stepScanline();
// }

/**
 * NES::clock(bool skipRender)
 * ----------------------------------------------------------------------------
 * ???????Frame-based ????
 *
 * ????????Anemoia ????
 *   - ????0?239 ???
 *       * ? 3 ?????113 + 114 + 114 CPU ??
 *       * ?????? MMC3 IRQ
 *       * ???? renderLine??? frameskip?
 *
 *   - Post-render ??240??113 CPU ??
 *
 *   - VBlank?241?260??
 *       * ?? VBlank ??
 *       * ?? NMI??????
 *       * ????? 2501 CPU ??
 *
 *   - Pre-render ??261??
 *       * ?? VBlank / Sprite 0 Hit
 *
 * ???
 *   - ?????????
 *   - ?? frameskip / ??
 *   - ? ESP32 ????? FPS
 *
 * ???
 *   ? ????
 *   ? ??????????
 *   ? ???????????????/?????
 *
 * ?????
 *   - MMC3 IRQ / Sprite 0 Hit???????
 *   - CPU/PPU cycle ????????
 */
void IRAM_ATTR NES::clock() {
    // ?????????????????????????
    // ?????????????????????????
    bool skipRender = frameskipEnabled && skipNextFrame;
    skipNextFrame = false;
    
    // ?? Sprite 0 ? Y ????????????
    int sprite0StartY = -1, sprite0EndY = -1;
    bool needSprite0Check = false;
    
    if (skipRender && ((ppu.getPpuMask() & 0x18) == 0x18)) {
        // ?? + ????????????? Sprite 0 Hit
        ppu.getSprite0YRange(sprite0StartY, sprite0EndY);
        // ?? Sprite 0 ???????????
        needSprite0Check = (sprite0StartY >= 0 && sprite0StartY < 240 && sprite0EndY > 0);
    }
    
    // ???????????? PPU ??????? + vramAddr = tempAddr?
    // MMC3 ?????????????? IRQ ? bank ??
    if (skipRender) {
        ppu.initFrameForSprite0Check();
    }
    
    // ????? 0-239 (? 3 ???)
    // ????: PPU?? ? IRQ??(?????) ? IRQ?? ? CPU??
    // ??? IRQ handler ? cpu.clock() ???????/bank ??
    // ????????? renderLine() ???
    for (int scanline = 0; scanline < 240; scanline += 3) {
        // ? 0
        if (!skipRender) {
            ppu.renderLine(scanline, ppu.frameBuffer + scanline * 256);
        } else {
            if (needSprite0Check && !(ppu.getPpuStatus() & 0x40) &&
                scanline >= sprite0StartY && scanline < sprite0EndY) {
                ppu.checkSprite0HitFast(scanline);
            }
            ppu.skipScanlineForScrollUpdate();
        }
        // MMC3 IRQ counter is clocked by PPU renderLine()/A12 observation path.
        // Avoid legacy double-clock here.
        if (cart.irqPending()) cpu.irq();
        cpu.clock(113);
        cart.clockCpuCycles(113);
        
        // ? 1
        if (!skipRender) {
            ppu.renderLine(scanline + 1, ppu.frameBuffer + (scanline + 1) * 256);
        } else {
            if (needSprite0Check && !(ppu.getPpuStatus() & 0x40) &&
                (scanline + 1) >= sprite0StartY && (scanline + 1) < sprite0EndY) {
                ppu.checkSprite0HitFast(scanline + 1);
            }
            ppu.skipScanlineForScrollUpdate();
        }
        if (cart.irqPending()) cpu.irq();
        cpu.clock(114);
        cart.clockCpuCycles(114);
        
        // ? 2
        if (!skipRender) {
            ppu.renderLine(scanline + 2, ppu.frameBuffer + (scanline + 2) * 256);
        } else {
            if (needSprite0Check && !(ppu.getPpuStatus() & 0x40) &&
                (scanline + 2) >= sprite0StartY && (scanline + 2) < sprite0EndY) {
                ppu.checkSprite0HitFast(scanline + 2);
            }
            ppu.skipScanlineForScrollUpdate();
        }
        if (cart.irqPending()) cpu.irq();
        cpu.clock(114);
        cart.clockCpuCycles(114);
    }
    
    // ??? 240: Post-render (113 CPU ??)
    cpu.clock(113);
    cart.clockCpuCycles(113);
    
    // ??? 241-260: VBlank
    ppu.setVBlank(true);
    if (ppu.nmiEnabled()) {
        cpu.nmi();
    }
    cpu.clock(2274);  // VBlank ??? CPU ?? (20 scanlines ? ~113.67)
    cart.clockCpuCycles(2274);
    
    // ??? 261: Pre-render
    ppu.setVBlank(false);
    ppu.clearSprite0Hit();  // ?? Sprite 0 Hit
    // Pre-render MMC3 IRQ counter is handled in PPU path as well.
    if (cart.irqPending()) cpu.irq();
    cpu.clock(114);
    cart.clockCpuCycles(114);
    
    // ???????
    ppu.frameReady = true;
    ppu.renderedThisFrame = !skipRender;
    
}

/**
 * ??????? (?? DMA ????)
 */
void NES::renderLine(int scanline, uint16_t* lineBuffer) {
    ppu.renderLine(scanline, lineBuffer);
}

void NES::render(uint16_t* fb) {
    ppu.render(fb);
}

void NES::endFrame() {
    // ????????NMI ? PPU::advanceCycles() ???????
    // ???????????????????? NMI
}

void NES::setController(uint8_t id, uint8_t state) {
    if (id < 2) {
        controller[id] = state;
    }
}

// ==================== CPU ?? ====================
uint8_t IRAM_ATTR NES::cpuRead(uint16_t addr) {
    if (addr < 0x2000) {
        // $0000-$1FFF: 2KB RAM (?? 4 ?)
        return ram[addr & 0x07FF];
    }
    else if (addr < 0x4000) {
        // $2000-$3FFF: PPU ??? (8 ?????)
        return ppu.regRead(addr & 0x0007);
    }
    else if (addr == 0x4016) {
        // ??? 1
        if (controllerStrobe) {
            return 0x40 | (controller[0] & 0x01);
        }
        uint8_t bit = (controllerLatch[0] >> controllerShift[0]) & 0x01;
        if (controllerShift[0] < 8) controllerShift[0]++;
        return 0x40 | bit;
    }
    else if (addr == 0x4017) {
        // ??? 2
        if (controllerStrobe) {
            return 0x40 | (controller[1] & 0x01);
        }
        uint8_t bit = (controllerLatch[1] >> controllerShift[1]) & 0x01;
        if (controllerShift[1] < 8) controllerShift[1]++;
        return 0x40 | bit;
    }
    else if (addr < 0x4020) {
        // $4000-$401F: APU ??? I/O
        // ??? APU ?????
        return apu.regRead(addr);
    }
    else if (addr >= 0x4020) {
        // $6000-$FFFF: Cartridge (SRAM $6000-$7FFF + PRG ROM $8000-$FFFF)
        return cart.cpuRead(addr);
    }
    return 0;
}

void IRAM_ATTR NES::cpuWrite(uint16_t addr, uint8_t val) {
    if (addr < 0x2000) {
        // $0000-$1FFF: 2KB RAM (?? 4 ?)
        ram[addr & 0x07FF] = val;
    }
    else if (addr < 0x4000) {
        // $2000-$3FFF: PPU ??? (8 ?????)
        ppu.regWrite(addr & 0x0007, val);
    }
    else if (addr == 0x4014) {
        // OAM DMA
        ppu.oamDMA(val, ram);
    }
    else if (addr == 0x4016) {
        // ??? Strobe
        bool newStrobe = (val & 0x01) != 0;
        if (controllerStrobe && !newStrobe) {
            // Strobe ???: ????????
            controllerLatch[0] = controller[0];
            controllerLatch[1] = controller[1];
            controllerShift[0] = 0;
            controllerShift[1] = 0;
        }
        controllerStrobe = newStrobe;
    }
    else if (addr < 0x4020) {
        // $4000-$401F: APU ??? (??? APU)
        apu.regWrite(addr, val);
    }
    else if (addr >= 0x4020) {
        // $6000-$7FFF: SRAM ??, $8000+: Mapper ??
        cart.cpuWrite(addr, val);
    }
}

// ==================== PPU ?? ====================
uint8_t IRAM_ATTR NES::ppuRead(uint16_t addr) {
    addr &= 0x3FFF;  // PPU ????? 14 ?
    
    if (addr < 0x2000) {
        // $0000-$1FFF: CHR ROM/RAM (Pattern Tables)
        return cart.ppuRead(addr);
    }
    else if (addr < 0x3F00) {
        // $2000-$3EFF: Nametables (with mirroring)
        return cart.readNameTable(addr & 0x0FFF);
    }
    else {
        // $3F00-$3FFF: Palette RAM
        uint8_t palAddr = addr & 0x1F;
        // ??: $3F10/$3F14/$3F18/$3F1C ??? $3F00/$3F04/$3F08/$3F0C
        if ((palAddr & 0x13) == 0x10) {
            palAddr &= 0x0F;
        }
        return palette[palAddr];
    }
}

void IRAM_ATTR NES::ppuWrite(uint16_t addr, uint8_t val) {
    addr &= 0x3FFF;
    
    if (addr < 0x2000) {
        // $0000-$1FFF: CHR RAM (??? RAM)
        cart.ppuWrite(addr, val);
    }
    else if (addr < 0x3F00) {
        // $2000-$3EFF: Nametables
        cart.writeNameTable(addr & 0x0FFF, val);
    }
    else {
        // $3F00-$3FFF: Palette RAM
        uint8_t palAddr = addr & 0x1F;
        if ((palAddr & 0x13) == 0x10) {
            palAddr &= 0x0F;
        }
        palette[palAddr] = val;
    }
}

// ============================================================================
// Save State
// ============================================================================

// ????????????
static const uint32_t SAVESTATE_MAGIC = 0x4E455353;  // "NESS"
static const uint16_t SAVESTATE_VERSION = 1;

size_t NES::getStateSize() const {
    size_t size = 0;
    
    // ??
    size += sizeof(SAVESTATE_MAGIC);
    size += sizeof(SAVESTATE_VERSION);
    
    // CPU RAM
    size += sizeof(ram);
    
    // PPU VRAM ????
    size += sizeof(vram);
    size += sizeof(palette);
    size += sizeof(mirrorVertical);
    
    // ?????
    size += sizeof(controller);
    size += sizeof(controllerLatch);
    size += sizeof(controllerShift);
    size += sizeof(controllerStrobe);
    
    // CPU ??
    size += cpu.getStateSize();
    
    // PPU ??
    size += ppu.getStateSize();
    
    // APU ??
    size += apu.getStateSize();
    
    // Cartridge ??
    size += cart.getStateSize();
    
    return size;
}

bool NES::saveStateToMemory(uint8_t* buffer, size_t bufferSize) {
    size_t requiredSize = getStateSize();
    if (bufferSize < requiredSize) {
        Serial.printf("SaveState: Buffer too small (%d < %d)\n", bufferSize, requiredSize);
        return false;
    }
    
    size_t offset = 0;
    
    // ???????
    buffer[offset++] = (SAVESTATE_MAGIC >> 0) & 0xFF;
    buffer[offset++] = (SAVESTATE_MAGIC >> 8) & 0xFF;
    buffer[offset++] = (SAVESTATE_MAGIC >> 16) & 0xFF;
    buffer[offset++] = (SAVESTATE_MAGIC >> 24) & 0xFF;
    buffer[offset++] = (SAVESTATE_VERSION >> 0) & 0xFF;
    buffer[offset++] = (SAVESTATE_VERSION >> 8) & 0xFF;
    
    // CPU RAM
    memcpy(buffer + offset, ram, sizeof(ram));
    offset += sizeof(ram);
    
    // PPU VRAM ????
    memcpy(buffer + offset, vram, sizeof(vram));
    offset += sizeof(vram);
    memcpy(buffer + offset, palette, sizeof(palette));
    offset += sizeof(palette);
    buffer[offset++] = mirrorVertical ? 1 : 0;
    
    // ?????
    memcpy(buffer + offset, controller, sizeof(controller));
    offset += sizeof(controller);
    memcpy(buffer + offset, controllerLatch, sizeof(controllerLatch));
    offset += sizeof(controllerLatch);
    memcpy(buffer + offset, controllerShift, sizeof(controllerShift));
    offset += sizeof(controllerShift);
    buffer[offset++] = controllerStrobe ? 1 : 0;
    
    // CPU ??
    cpu.saveState(buffer, offset);
    
    // PPU ??
    ppu.saveState(buffer, offset);
    
    // APU ??
    apu.saveState(buffer, offset);
    
    // Cartridge ??
    cart.saveState(buffer, offset);
    
    Serial.printf("SaveState: Saved %d bytes\n", offset);
    return true;
}

bool NES::loadStateFromMemory(const uint8_t* buffer, size_t bufferSize) {
    if (bufferSize < 6) {
        Serial.println("LoadState: Buffer too small for header");
        return false;
    }
    
    size_t offset = 0;
    
    // ????
    uint32_t magic = buffer[offset] | (buffer[offset + 1] << 8) | 
                     (buffer[offset + 2] << 16) | (buffer[offset + 3] << 24);
    offset += 4;
    
    if (magic != SAVESTATE_MAGIC) {
        Serial.printf("LoadState: Invalid magic (0x%08X)\n", magic);
        return false;
    }
    
    // ????
    uint16_t version = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    if (version != SAVESTATE_VERSION) {
        Serial.printf("LoadState: Version mismatch (%d != %d)\n", version, SAVESTATE_VERSION);
        return false;
    }
    
    // CPU RAM
    memcpy(ram, buffer + offset, sizeof(ram));
    offset += sizeof(ram);
    
    // PPU VRAM ????
    memcpy(vram, buffer + offset, sizeof(vram));
    offset += sizeof(vram);
    memcpy(palette, buffer + offset, sizeof(palette));
    offset += sizeof(palette);
    mirrorVertical = buffer[offset++] != 0;
    
    // ?????
    memcpy(controller, buffer + offset, sizeof(controller));
    offset += sizeof(controller);
    memcpy(controllerLatch, buffer + offset, sizeof(controllerLatch));
    offset += sizeof(controllerLatch);
    memcpy(controllerShift, buffer + offset, sizeof(controllerShift));
    offset += sizeof(controllerShift);
    controllerStrobe = buffer[offset++] != 0;
    
    // CPU ??
    cpu.loadState(buffer, offset);
    
    // PPU ??
    ppu.loadState(buffer, offset);
    
    // APU ??
    apu.loadState(buffer, offset);
    
    // Cartridge ??
    cart.loadState(buffer, offset);
    
    // ???? PPU ????
    // ?? Cartridge ???? VRAM ??? NES
    cart.setVramPointer(vram);
    cart.setNES(this);
    ppu.setMemoryPointers(vram, palette, &cart, &mirrorVertical);
    
    Serial.printf("LoadState: Loaded %d bytes\n", offset);
    return true;
}

bool NES::saveState(const char* path) {
    // ?????
    size_t stateSize = getStateSize();
    uint8_t* buffer = (uint8_t*)ps_malloc(stateSize);
    if (!buffer) {
        buffer = (uint8_t*)malloc(stateSize);
    }
    if (!buffer) {
        Serial.println("SaveState: Failed to allocate buffer");
        return false;
    }
    
    // ?????
    bool success = saveStateToMemory(buffer, stateSize);
    if (!success) {
        free(buffer);
        return false;
    }
    
    // ????
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("SaveState: Failed to create file %s\n", path);
        free(buffer);
        return false;
    }
    
    size_t written = f.write(buffer, stateSize);
    f.close();
    free(buffer);
    
    if (written != stateSize) {
        Serial.printf("SaveState: Write error (%d != %d)\n", written, stateSize);
        return false;
    }
    
    Serial.printf("SaveState: Saved to %s\n", path);
    return true;
}

bool NES::loadState(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("LoadState: File not found %s\n", path);
        return false;
    }
    
    size_t fileSize = f.size();
    
    // ?????
    uint8_t* buffer = (uint8_t*)ps_malloc(fileSize);
    if (!buffer) {
        buffer = (uint8_t*)malloc(fileSize);
    }
    if (!buffer) {
        Serial.println("LoadState: Failed to allocate buffer");
        f.close();
        return false;
    }
    
    // ????
    size_t bytesRead = f.read(buffer, fileSize);
    f.close();
    
    if (bytesRead != fileSize) {
        Serial.printf("LoadState: Read error (%d != %d)\n", bytesRead, fileSize);
        free(buffer);
        return false;
    }
    
    // ????
    bool success = loadStateFromMemory(buffer, fileSize);
    free(buffer);
    
    if (success) {
        Serial.printf("LoadState: Loaded from %s\n", path);
    }
    
    return success;
}
