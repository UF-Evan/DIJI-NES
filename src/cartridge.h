/*
 * Cartridge abstraction and ROM metadata/state declarations.
 */

#pragma once
#include <Arduino.h>
#include <SD.h>

class NES;

class Cartridge {
public:
    Cartridge();
    ~Cartridge();
    
    bool load(const char* path);
    uint8_t IRAM_ATTR cpuRead(uint16_t addr);
    void IRAM_ATTR cpuWrite(uint16_t addr, uint8_t val);
    
    uint8_t IRAM_ATTR ppuRead(uint16_t addr);
    void ppuWrite(uint16_t addr, uint8_t val);
    
    
    void setVramPointer(uint8_t* vramPtr) { vram = vramPtr; updateNtPtrs(); if (mapper == 19) updateChrBankCache(); }
    
    
    uint8_t IRAM_ATTR readNameTable(uint16_t addr);
    void IRAM_ATTR writeNameTable(uint16_t addr, uint8_t val);
    
    
    uint8_t getPrgBanks() const { return prgBanks; }
    uint8_t getChrBanks() const { return chrBanks; }
    uint8_t getMapper() const { return mapper; }
    bool getMirrorVertical() const { return mirrorVertical; }
    bool hasChrRam() const { return chrBanks == 0; }
    bool hasSRAM() const { return hasBattery; }
    bool shouldDisableFrameskip() const { return mapper == 7; }
    
    
    uint8_t* getChrData() { return chrWindow; }
    
    
    void setMirrorVertical(bool v) { mirrorVertical = v; updateNtPtrs(); }
    
    
    bool irqPending() const { return mmc3IrqPending; }
    void acknowledgeIrq() { mmc3IrqPending = false; }
    bool usesMmc3ScanlineClock() const { return false; }
    bool needsPpuAddressObserve() const { return mapper == 4 || mapper == 9 || mapper == 10 || mapper == 64 || mapper == 118 || mapper == 119 || mapper == 163; }
    void IRAM_ATTR clockIrqCounter();  
    void IRAM_ATTR ppuScanline();      
    void IRAM_ATTR observePpuAddress(uint16_t ppuAddr);
    void IRAM_ATTR clockCpuCycles(uint32_t cpuCycles);
    void finalizeMmc3Scanline(bool renderingEnabled, uint8_t ppuCtrl);

    
    void setNES(NES* n) { nes = n; }

    
    bool consumePpuRenderingDirty()
    {
        bool dirty = ppuRenderingDirty;
        ppuRenderingDirty = false;
        return dirty;
    }
    
    
    uint8_t* getSRAM() { return sram; }
    
    
    void saveState(uint8_t* buf, size_t& offset) const;
    void loadState(const uint8_t* buf, size_t& offset);
    size_t getStateSize() const;

private:
    
    uint8_t prgBanks = 0;      
    uint8_t chrBanks = 0;      
    uint8_t mapper = 0;        
    uint8_t submapper = 0;
    bool mirrorVertical = false;  
    bool hasBattery = false;   
    bool oneScreenMirror = false; 
    bool oneScreenUpper = false;  
    
    
    uint8_t* prg = nullptr;    
    uint8_t* chr = nullptr;    
    uint8_t* vram = nullptr;   
    uint8_t chrRam[0x2000];    
    uint8_t* chrWindow = nullptr; 
    uint32_t prgSize = 0;
    uint32_t chrSize = 0;
    
    
    uint8_t sram[0x2000];      
    
    
    uint8_t prgBankSelect = 0;  
    uint32_t prgBank0Offset = 0; 
    uint32_t prgBank1Offset = 0; 
    uint32_t prgBank2Offset = 0; 
    uint32_t prgBank3Offset = 0; 
    
    
public:
    uint8_t* chrBankPtrs[8] = {nullptr}; 
    
    
    uint8_t* ntPtrs[4] = {nullptr};  
    void updateNtPtrs();  
private:

    
    uint8_t mmc1ShiftReg = 0x10;   
    uint8_t mmc1WriteCount = 0;    
    uint8_t mmc1Control = 0x0C;    
    uint8_t mmc1ChrBank0 = 0;      
    uint8_t mmc1ChrBank1 = 0;      
    uint8_t mmc1PrgBank = 0;       
    
    
    uint8_t cnromChrBank = 0;      
    uint8_t mapper34ChrBank0 = 0;
    uint8_t mapper34ChrBank1 = 1;
    bool mapper34NinaMode = false;
    uint8_t colorDreamsPrgBank = 0;
    uint8_t colorDreamsChrBank = 0;
    uint8_t gxromPrgBank = 0;
    uint8_t gxromChrBank = 0;
    uint8_t mmc2PrgBank = 0;
    uint8_t mmc2ChrFd0 = 0;
    uint8_t mmc2ChrFe0 = 0;
    uint8_t mmc2ChrFd1 = 0;
    uint8_t mmc2ChrFe1 = 0;
    bool mmc2Latch0FE = false;
    bool mmc2Latch1FE = false;

    uint8_t fme7Command = 0;
    uint8_t fme7ChrBanks[8] = {0};
    uint8_t fme7PrgBanks[4] = {0};
    bool fme7RamEnable = false;
    bool fme7RamSelect = false;
    bool fme7RamWriteEnable = false;
    bool fme7IrqEnable = false;
    bool fme7IrqCounterEnable = false;
    uint16_t fme7IrqCounter = 0;
    uint32_t prgBank6000Offset = 0;
    uint8_t vrcChrBanks[8] = {0};
    uint8_t vrcChrHighBits[8] = {0};
    uint8_t vrc6Prg16Bank = 0;
    uint8_t vrcPrgBank8000 = 0;
    uint8_t vrcPrgBankA000 = 0;
    uint8_t vrcPrgBankC000 = 0;
    uint8_t vrc6PpuCtrl = 0x20;
    uint8_t vrcIrqLatch = 0;
    uint8_t vrcIrqCounter = 0;
    bool vrcIrqEnable = false;
    bool vrcIrqEnableAfterAck = false;
    bool vrcIrqModeCycle = false;
    int16_t vrcIrqPrescaler = 341;
    uint8_t vrcRegCmd = 0;
    uint8_t vrcReg1Mask = 0;
    uint8_t vrcReg2Mask = 0;
    uint8_t vrc2Latch = 0;
    uint8_t mapper18PrgRegs[3] = {0, 1, 0};
    uint8_t mapper18ChrRegs[8] = {0};
    uint16_t mapper18IrqCounter = 0;
    uint16_t mapper18IrqLatch = 0;
    bool mapper18IrqEnable = false;
    uint16_t mapper18IrqSizeMask = 0xFFFF;
    bool mapper18RamEnable = true;
    bool mapper18RamWriteEnable = true;
    uint8_t mapper19PrgRegs[3] = {0, 1, 2};
    uint8_t mapper19ChrRegs[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t mapper19NtRegs[4] = {0xE0, 0xE1, 0xE0, 0xE1};
    uint8_t mapper19InternalRam[128] = {0};
    uint8_t mapper19AddrPort = 0;
    uint8_t mapper19ChrRamCtl = 0x03;
    uint8_t mapper19WramProtect = 0x4F;
    uint8_t mapper19RegF000 = 0;
    uint16_t mapper19IrqCounter = 0;
    bool mapper19IrqEnable = false;
    uint8_t m163Reg5000 = 0;
    uint8_t m163Reg5200 = 0;
    uint8_t m163Mode5300 = 0;
    uint8_t m163FeedbackBit = 0;
    bool m163FeedbackSetMode = false;
    bool m163PrevA13 = false;
    uint8_t m163LatchedA9 = 0;
    uint8_t mmc5PrgMode = 3;
    uint8_t mmc5ChrMode = 3;
    uint8_t mmc5ExRamMode = 0;
    uint8_t mmc5NtMap = 0;
    uint8_t mmc5FillTile = 0;
    uint8_t mmc5FillAttr = 0;
    uint8_t mmc5UpperChrBits = 0;
    uint8_t mmc5PrgRegs[5] = {0x00, 0x80, 0x81, 0x82, 0xFF}; // $5113-$5117
    uint8_t mmc5ChrRegsA[8] = {0};
    uint8_t mmc5ChrRegsB[4] = {0};
    uint8_t mmc5ExRam[0x400] = {0};
    uint8_t* mmc5BgChrPtrs[8] = {nullptr};
    uint8_t* mmc5SpChrPtrs[8] = {nullptr};
    bool mmc5LastChrWriteSetB = false;
    bool mmc5IrqEnable = false;
    uint8_t mmc5IrqTargetScanline = 0;
    uint8_t mmc5ScanlineCounter = 0;
    bool mmc5InFrame = false;

    uint8_t mapper32Prg0 = 0;
    uint8_t mapper32Prg1 = 1;
    uint8_t mapper32Ctrl = 0;
    uint8_t mapper32ChrBanks[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    uint8_t mapper33Regs[8] = {0, 1, 0, 1, 0, 0, 0, 0};
    uint8_t mapper33Mirror = 0;
    bool mapper48IrqEnable = false;
    uint8_t mapper48IrqCounter = 0;
    uint8_t mapper48IrqLatch = 0;

    uint8_t mapper64BankSelect = 0;
    uint8_t mapper64Regs[16] = {0};
    bool mapper64PrgMode = false;
    bool mapper64ChrMode = false;
    bool mapper64Chr1kMode = false;
    bool mapper64IrqEnable = false;
    bool mapper64IrqReload = false;
    bool mapper64IrqCycleMode = false;
    uint8_t mapper64IrqCounter = 0;
    uint8_t mapper64IrqLatch = 0;
    uint8_t mapper64IrqPrescaler = 0;
    bool mapper64PrevA12 = false;
    uint8_t mapper64A12LowCycles = 0;

    uint8_t mapper70Reg = 0;
    uint8_t mapper79Reg = 0;
    uint8_t mapper87Reg = 0;
    uint8_t mapper148Reg = 0;
    
    
    uint8_t mmc3BankSelect = 0;    
    uint8_t mmc3Banks[8] = {0};    
    bool mmc3PrgMode = false;      
    bool mmc3ChrMode = false;      
    uint8_t mmc3IrqLatch = 0;      
    uint8_t mmc3IrqCounter = 0;    
    bool mmc3IrqEnabled = false;   
    bool mmc3IrqReload = false;    
    bool mmc3IrqPending = false;   
    bool mmc3PrevA12 = false;      
    uint8_t mmc3A12LowCycles = 0;   // In PPU-cycle-like units; 9 ~= 3 CPU cycles.
    uint64_t mmc3LastObserveCpuCycle = 0;
    bool mmc3LastObserveCpuCycleValid = false;
    bool mmc3ClockedThisScanline = false;
    bool ppuRenderingDirty = true;
    uint8_t mapper7BusConflictMode = 1; // 1: no conflicts, 2: AND conflicts
    bool mapper71MirroringMode = false;

    
    NES* nes = nullptr;
    
    
    void updateBankCache();
    void updateMmc1Banks();
    void updateMmc3Banks();
    void updateNamco108Banks();
    void updateMapper32Banks();
    void updateMapper33Banks();
    void updateMapper64Banks();
    void updateMapper18Banks();
    void updateMapper19PrgBanks();
    void updateChrBankCache();
    void updateMapper118Nametables();
    void updateMapper95Nametables();
    
    
    uint8_t IRAM_ATTR cpuReadMapper0(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper1(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper2(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper3(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper34(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper4(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper7(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper11(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper66(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper9(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper10(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper18(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper19(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper23(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper25(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper32(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper33(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper64(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper70(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper79(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper87(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper148(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper69(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper118(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper119(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper206(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper24(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper26(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper71(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper85(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper94(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper163(uint16_t addr);
    uint8_t IRAM_ATTR cpuReadMapper5(uint16_t addr);
    
    void cpuWriteMapper1(uint16_t addr, uint8_t val);
    void cpuWriteMapper2(uint16_t addr, uint8_t val);
    void cpuWriteMapper3(uint16_t addr, uint8_t val);
    void cpuWriteMapper34(uint16_t addr, uint8_t val);
    void cpuWriteMapper4(uint16_t addr, uint8_t val);
    void cpuWriteMapper7(uint16_t addr, uint8_t val);
    void cpuWriteMapper11(uint16_t addr, uint8_t val);
    void cpuWriteMapper66(uint16_t addr, uint8_t val);
    void cpuWriteMapper9(uint16_t addr, uint8_t val);
    void cpuWriteMapper10(uint16_t addr, uint8_t val);
    void cpuWriteMapper18(uint16_t addr, uint8_t val);
    void cpuWriteMapper19(uint16_t addr, uint8_t val);
    void cpuWriteMapper23(uint16_t addr, uint8_t val);
    void cpuWriteMapper25(uint16_t addr, uint8_t val);
    void cpuWriteMapper32(uint16_t addr, uint8_t val);
    void cpuWriteMapper33(uint16_t addr, uint8_t val);
    void cpuWriteMapper48(uint16_t addr, uint8_t val);
    void cpuWriteMapper64(uint16_t addr, uint8_t val);
    void cpuWriteMapper70(uint16_t addr, uint8_t val);
    void cpuWriteMapper79(uint16_t addr, uint8_t val);
    void cpuWriteMapper87(uint16_t addr, uint8_t val);
    void cpuWriteMapper148(uint16_t addr, uint8_t val);
    void cpuWriteMapper69(uint16_t addr, uint8_t val);
    void cpuWriteMapper118(uint16_t addr, uint8_t val);
    void cpuWriteMapper119(uint16_t addr, uint8_t val);
    void cpuWriteMapper206(uint16_t addr, uint8_t val);
    void cpuWriteMapper24(uint16_t addr, uint8_t val);
    void cpuWriteMapper26(uint16_t addr, uint8_t val);
    void cpuWriteMapper71(uint16_t addr, uint8_t val);
    void cpuWriteMapper85(uint16_t addr, uint8_t val);
    void cpuWriteMapper94(uint16_t addr, uint8_t val);
    void cpuWriteMapper163(uint16_t addr, uint8_t val);
    void cpuWriteMapper5(uint16_t addr, uint8_t val);
    void clockVrcIrq(uint32_t cpuCycles);
    void updateMapper163Prg();
    void updateMmc5PrgBanks();
    void updateMmc5ChrBanks();
    uint8_t IRAM_ATTR readMmc5NameTable(uint16_t addr);
    void writeMmc5NameTable(uint16_t addr, uint8_t val);
    uint8_t IRAM_ATTR readMapper19NameTable(uint16_t addr);
    void writeMapper19NameTable(uint16_t addr, uint8_t val);
    uint8_t IRAM_ATTR readMmc5Chr(uint16_t addr, bool spriteFetch) const;
    void configureVrcMapping();
    bool isVrc2Mode() const;
    bool isVrc4Mode() const;
    bool shouldEnforceVrc4WramControl() const;
public:
    bool isMapper5ExtAttrMode() const { return mapper == 5 && mmc5ExRamMode == 1; }
    uint8_t mmc5ExtAttrByte(uint16_t tileOffset) const { return mmc5ExRam[tileOffset & 0x03FF]; }
    uint8_t IRAM_ATTR readMmc5BgPatternByte(uint16_t addr) const { return readMmc5Chr(addr, false); }
    uint8_t IRAM_ATTR readMmc5SpPatternByte(uint16_t addr) const { return readMmc5Chr(addr, true); }
    void notifyScanlineStart(int scanline, bool renderingEnabled);
    
    uint8_t IRAM_ATTR ppuReadMapper1(uint16_t addr);
    uint8_t IRAM_ATTR ppuReadMapper3(uint16_t addr);
    uint8_t IRAM_ATTR ppuReadMapper4(uint16_t addr);
    
    void ppuWriteMapper1(uint16_t addr, uint8_t val);
    void ppuWriteMapper4(uint16_t addr, uint8_t val);
    void ppuWriteMapper119(uint16_t addr, uint8_t val);
};
