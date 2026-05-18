#pragma once
#include "cpu6502.h"
#include "ppu.h"
#include "cartridge.h"
#include "apu.h"

class NES {
public:
    bool loadROM(const char* path);
    void reset();
    // ???? CPU ????? PPU??? CPU ???
    uint8_t IRAM_ATTR step();
    // Anemoia ????????????? (CPU + PPU + ?? + DMA)
    void IRAM_ATTR clock();
    // ??????????????? CPU ????? PPU ??
    void IRAM_ATTR stepScanline();
    void IRAM_ATTR stepThreeScanlines();
    void render(uint16_t* fb);
    
    // ?????? (?? DMA ??)
    void renderLine(int scanline, uint16_t* lineBuffer);

    // CPU ??
    uint8_t IRAM_ATTR cpuRead(uint16_t addr);
    void IRAM_ATTR cpuWrite(uint16_t addr, uint8_t val);
    
    // PPU ??
    uint8_t IRAM_ATTR ppuRead(uint16_t addr);
    void IRAM_ATTR ppuWrite(uint16_t addr, uint8_t val);

    // ???
    void setController(uint8_t id, uint8_t state);  // id: 0 ? 1
    // ????? (????????????)
    void endFrame();

    // Save State ??
    bool saveState(const char* path);
    bool loadState(const char* path);
    size_t getStateSize() const;
    
    // Save State ??? (??????)
    bool saveStateToMemory(uint8_t* buffer, size_t bufferSize);
    bool loadStateFromMemory(const uint8_t* buffer, size_t bufferSize);

    // ??????
    CPU6502 cpu;
    APU apu;
    PPU& getPPU() { return ppu; }
    Cartridge& getCart() { return cart; }
    
    // ???? ROM ?? (?????????)
    const char* getCurrentRomPath() const { return currentRomPath; }

private:
    PPU ppu;
    Cartridge cart;
    uint8_t ram[0x800];      // 2KB CPU RAM
    uint8_t vram[0x800];     // 2KB PPU VRAM (Nametables)
    uint8_t palette[0x20];   // 32 bytes Palette RAM
    bool mirrorVertical = false;  // ??????
    
    // ?????
    uint8_t controller[2] = {0, 0};     // ??????
    uint8_t controllerLatch[2] = {0, 0}; // ?????
    uint8_t controllerShift[2] = {0, 0}; // ?????
    bool controllerStrobe = false;       // Strobe ??
    
    // ?? ROM ??
    char currentRomPath[128] = {0};
    // ????????? 113/114 ????
    bool scanlineParity = false;
    
    bool frameskipEnabled = true;  // ???? (true=????, false=?????)
    
public:
    // ??????
    void setFrameskipEnabled(bool enabled) { frameskipEnabled = enabled; }
    bool getFrameskipEnabled() const { return frameskipEnabled; }
    void requestFrameSkip(bool skip) { skipNextFrame = skip; }

private:
    bool skipNextFrame = false;
};
