/*
 * PPU declarations for rendering state, timing, and register interfaces.
 */

#pragma once
#include <Arduino.h>
#include <stdint.h>

class NES;
class Cartridge;

struct TileCache {
    uint8_t pixels[8];     
    uint16_t tileAddr;     
    bool valid;            
};

class PPU {
public:
    struct RenderPerfCounters {
        uint32_t frames = 0;
        uint32_t bgLines = 0;
        uint32_t bgTiles = 0;
        uint32_t bgZeroTiles = 0;
        uint32_t bgFastFillTiles = 0;
        uint32_t bgFastOpaqueTiles = 0;
        uint32_t bgOpaquePixels = 0;
        uint32_t spriteLines = 0;
        uint32_t spriteCandidates = 0;
        uint32_t spriteFastPathSprites = 0;
        uint32_t spriteDrawnPixels = 0;
        uint32_t observeCalls = 0;
    };
    
    void connect(NES* nes);
    void reset();
    
    
    void setMemoryPointers(uint8_t* vramPtr, uint8_t* palettePtr, Cartridge* cartPtr, bool* mirrorVertPtr);
    
    
    uint8_t IRAM_ATTR regRead(uint8_t reg);
    void IRAM_ATTR regWrite(uint8_t reg, uint8_t val);
    
    
    void IRAM_ATTR oamDMA(uint8_t page, uint8_t* cpuRam);
    
    
    
    void render(uint16_t* fb);
    
    void IRAM_ATTR renderLine(int scanline, uint16_t* lineBuffer);
    
    
    bool isVBlank() const { return (ppuStatus & 0x80) != 0; }
    void setVBlank(bool v) { if (v) ppuStatus |= 0x80; else ppuStatus &= 0x7F; }
    bool nmiEnabled() const { return (ppuCtrl & 0x80) != 0; }
    void IRAM_ATTR beginVblank();
    void IRAM_ATTR endVblank();
    
    void clearSprite0Hit() { ppuStatus &= ~0x40; sprite0StrikePending = false; }
    void clearSpriteOverflow() { ppuStatus &= ~0x20; }
    
    
    uint8_t getPpuMask() const { return ppuMask; }
    uint8_t getPpuStatus() const { return ppuStatus; }
    uint8_t getPpuCtrl() const { return ppuCtrl; }
    
    
    
    void getSprite0YRange(int& startY, int& endY) const {
        int y = oam[0] + 1;  
        int height = (ppuCtrl & 0x20) ? 16 : 8;  
        startY = y;
        endY = y + height;
    }
    
    
    void getSprite0XRange(int& startX, int& endX) const {
        startX = oam[3];
        endX = startX + 8;
    }
    
    
    
    bool IRAM_ATTR checkSprite0HitFast(int scanline);
    
    
    void IRAM_ATTR initFrameForSprite0Check() {
        loadPaletteCache();
        if ((ppuMask & 0x18) != 0) {
            vramAddr = tempAddr;
        }
    }
    
    
    void IRAM_ATTR skipScanlineForScrollUpdate();
    
    
    bool IRAM_ATTR isNmiPending() const { return nmiPending; }
    void IRAM_ATTR clearNmiPending() { nmiPending = false; }
    void IRAM_ATTR beginStatusReadSuppressionWindow(uint8_t mode);
    void IRAM_ATTR endStatusReadSuppressionWindow();
    
    
    void IRAM_ATTR stepPPU();

    
    int ppuCycle = 0;        
    int ppuScanline = 0;     
    bool oddFrame = false;

    
    uint16_t* frameBuffer = nullptr;
    
    bool renderEnabled = true;

    
    bool sprite0HitThisFrame = false;

    bool sprite0CheckedThisLine = false;

    
    volatile bool frameReady = false;
    
    
    
    void IRAM_ATTR advanceCycles(int ppuCycles);

    
    bool renderedThisFrame = false;
    
    
    int getCurrentScanline() const { return scanline; }
    int getCurrentDot() const { return dot; }
    
    
    void setScanline(int sl) { scanline = sl; }
    void setDot(int d) { dot = d; }
    
    
    bool isRendering() const { 
        return (ppuMask & 0x18) && (scanline < 240); 
    }
    
    
    uint32_t getFrameCount() const { return frameCount; }
    static constexpr int DIRTY_TILE_W = 32;
    static constexpr int DIRTY_TILE_H = 30;
    static constexpr int DIRTY_TILE_COUNT = DIRTY_TILE_W * DIRTY_TILE_H;
    void clearTileDirtyMap();
    void markAllTilesDirty();
    void consumeTileDirtyMap(uint8_t* outMap, bool* outAll);
    void resetRenderPerfCounters();
    void getRenderPerfCounters(RenderPerfCounters& out) const;

private:
    NES* bus = nullptr;
    
    
    uint8_t* vramDirect = nullptr;      
    uint8_t* paletteDirect = nullptr;   
    Cartridge* cartDirect = nullptr;    
    uint8_t* chrDirect = nullptr;       
    bool* mirrorVertical = nullptr;     
    
    
    uint8_t ppuCtrl = 0;      
                               
                               
                               
                               
                               
                               
                               
    
    uint8_t ppuMask = 0;      
                               
                               
                               
                               
                               
                               
    
    uint8_t ppuStatus = 0;    
                               
                               
                               
    
    uint8_t oamAddr = 0;      
    
    
    uint16_t vramAddr = 0;    
                               
                               
                               
                               
                               
    
    uint16_t tempAddr = 0;    
    uint8_t fineX = 0;        
    bool writeToggle = false; 
    uint8_t dataBuffer = 0;   
    
    
    
    uint8_t oam[256];
    
    
    
    uint16_t bgPaletteCache[16];    
    uint16_t spPaletteCache[16];    
    
    
    
    static const int TILE_CACHE_SIZE = 128;
    TileCache tileCache[TILE_CACHE_SIZE];
    
    
    uint8_t IRAM_ATTR fastChrRead(uint16_t addr);
    uint8_t IRAM_ATTR fastVramRead(uint16_t addr);
    uint8_t IRAM_ATTR fastPaletteRead(uint8_t addr);
    
    
    void IRAM_ATTR renderBackgroundLine(int scanline, uint16_t* lineBuffer);
    void IRAM_ATTR renderSpriteLine(int scanline, uint16_t* lineBuffer);
    
    
    void IRAM_ATTR loadPaletteCache();
    
    
    void IRAM_ATTR incrementY();
    
    
    void invalidateTileCache();
    void IRAM_ATTR markTileDirty(int tx, int ty);
    void IRAM_ATTR markTileDirtyRect(int tx, int ty, int w, int h);
    void IRAM_ATTR markDirtyByPpuAddr(uint16_t addr);
    
    
    uint32_t frameCount = 0;
    uint8_t tileDirtyMap[DIRTY_TILE_COUNT];
    bool tileDirtyAll = true;
    
    
    

    
    int scanline = 0;         
    int dot = 0;              
    
    bool nmiOccurred = false; 
    bool nmiPending = false;  
    uint8_t statusReadSuppressionWindow = 0; // 0=off, 1=pre-vblank race, 2=early-vblank race
    bool suppressNextVblank = false;
    bool suppressCurrentVblankNmi = false;
    
    
    bool sprite0HitPossible = false;  
    bool sprite0Rendered = false;     
    uint8_t bgPixelOpacity[256];      
    bool sprite0StrikePending = false;
    uint64_t sprite0StrikeCycle = 0;
    
    
    static const int MAX_SPRITES_PER_LINE = 8;
    uint8_t spriteIndicesPerLine[240][MAX_SPRITES_PER_LINE];
    uint8_t spriteCountPerLine[240];
    void IRAM_ATTR evaluateOAM();
    
    
    
    uint16_t savedScrollAddr = 0;     
    uint8_t savedFineX = 0;           
    RenderPerfCounters perfCounters;
    
public:
    
    void saveState(uint8_t* buf, size_t& offset) const;
    void loadState(const uint8_t* buf, size_t& offset);
    size_t getStateSize() const;
};
