/*
 * Cartridge implementation for loading ROM data and mapper-facing state.
 */

#include "cartridge.h"
#include "nes.h"

#ifndef MAPPER7_BUS_CONFLICT_MODE
#define MAPPER7_BUS_CONFLICT_MODE 0
#endif

Cartridge::Cartridge() : prg(nullptr), chr(nullptr), prgSize(0), chrSize(0), prgBankSelect(0) {
    memset(chrRam, 0, sizeof(chrRam));
    memset(sram, 0, sizeof(sram));
    memset(chrBankPtrs, 0, sizeof(chrBankPtrs));
    chrWindow = chrRam;  
    prgBank2Offset = 0;
    prgBank3Offset = 0;
    
    mmc3PrevA12 = false;
    mmc3IrqPending = false;
    mmc3LastObserveCpuCycle = 0;
    mmc3LastObserveCpuCycleValid = false;
}

Cartridge::~Cartridge() {
    if (prg) {
        free(prg);
        prg = nullptr;
    }
    if (chr) {
        free(chr);
        chr = nullptr;
    }
}

bool Cartridge::load(const char* path) {
    File f = SD.open(path);
    if (!f) {
        Serial.printf("Cartridge: Failed to open %s\n", path);
        return false;
    }

    
    uint8_t header[16];
    if (f.read(header, 16) != 16) {
        Serial.println("Cartridge: Failed to read header");
        f.close();
        return false;
    }

    
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
        Serial.println("Cartridge: Invalid iNES header");
        f.close();
        return false;
    }

    
    prgBanks = header[4];        
    chrBanks = header[5];        
    
    uint8_t flags6 = header[6];
    uint8_t flags7 = header[7];
    
    mirrorVertical = (flags6 & 0x01) != 0;
    hasBattery = (flags6 & 0x02) != 0;
    bool hasTrainer = (flags6 & 0x04) != 0;
    
    const bool nes20 = (header[0] == 'N' && header[1] == 'E' && header[2] == 'S' && header[3] == 0x1A &&
                        (flags7 & 0x0C) == 0x08);
    submapper = 0;
    if (nes20) {
        submapper = (header[8] >> 4) & 0x0F;
        const uint8_t mapperMsb = header[8] & 0x0F;
        mapper = ((flags6 >> 4) & 0x0F) | (flags7 & 0xF0);
        if (mapperMsb != 0) {
            Serial.printf("  NOTE: NES 2.0 mapper high nibble 0x%X ignored by this build\n", mapperMsb);
        }
        Serial.printf("  Header: NES 2.0 (submapper %u)\n", submapper);
    } else {
        bool dirtyHeader = false;
        for (int i = 8; i < 16; i++) {
            if (header[i] != 0) { dirtyHeader = true; break; }
        }

        if (dirtyHeader) {
            mapper = (flags6 >> 4) & 0x0F;
            Serial.println("  NOTE: Dirty iNES header detected, using low nibble mapper only");
        } else {
            mapper = ((flags6 >> 4) & 0x0F) | (flags7 & 0xF0);
        }
    }

    Serial.println("=== Cartridge Info ===");
    Serial.printf("  PRG ROM: %d x 16KB = %dKB\n", prgBanks, prgBanks * 16);
    Serial.printf("  CHR ROM: %d x 8KB = %dKB\n", chrBanks, chrBanks * 8);
    Serial.printf("  Mapper: %d\n", mapper);
    Serial.printf("  Mirror: %s\n", mirrorVertical ? "Vertical" : "Horizontal");
    Serial.printf("  Battery: %s\n", hasBattery ? "Yes" : "No");
    
    
    if (mapper != 0 && mapper != 1 && mapper != 2 && mapper != 3 && mapper != 4 &&
        mapper != 5 &&
        mapper != 7 && mapper != 9 && mapper != 10 && mapper != 11 &&
        mapper != 18 && mapper != 19 && mapper != 23 && mapper != 25 &&
        mapper != 24 && mapper != 26 && mapper != 32 && mapper != 33 &&
        mapper != 34 && mapper != 48 && mapper != 64 && mapper != 66 &&
        mapper != 69 && mapper != 70 && mapper != 71 && mapper != 79 &&
        mapper != 85 && mapper != 87 && mapper != 94 &&
        mapper != 118 && mapper != 119 && mapper != 163 &&
        mapper != 76 && mapper != 88 && mapper != 95 && mapper != 148 &&
        mapper != 154 && mapper != 206) {
        Serial.printf("  WARNING: Mapper %d not fully supported!\n", mapper);
    } else {
        const char* mapperName = "Unknown";
        switch (mapper) {
            case 0: mapperName = "NROM"; break;
            case 1: mapperName = "MMC1"; break;
            case 2: mapperName = "UxROM"; break;
            case 3: mapperName = "CNROM"; break;
            case 4: mapperName = "MMC3"; break;
            case 5: mapperName = "MMC5"; break;
            case 7: mapperName = "AxROM"; break;
            case 9: mapperName = "MMC2"; break;
            case 10: mapperName = "MMC4"; break;
            case 11: mapperName = "Color Dreams"; break;
            case 18: mapperName = "Jaleco SS8806"; break;
            case 19: mapperName = "Namco 163"; break;
            case 23: mapperName = "VRC2b/VRC4e"; break;
            case 24: mapperName = "VRC6a"; break;
            case 25: mapperName = "VRC2c/VRC4b"; break;
            case 26: mapperName = "VRC6b"; break;
            case 32: mapperName = "Irem G-101"; break;
            case 33: mapperName = "Taito TC0190"; break;
            case 34: mapperName = "BNROM / NINA-001"; break;
            case 48: mapperName = "Taito TC0190 + IRQ"; break;
            case 64: mapperName = "Tengen RAMBO-1"; break;
            case 66: mapperName = "GxROM"; break;
            case 69: mapperName = "Sunsoft FME-7"; break;
            case 70: mapperName = "Bandai 74161/32"; break;
            case 71: mapperName = "Camerica"; break;
            case 79: mapperName = "NINA-03/06"; break;
            case 85: mapperName = "VRC7"; break;
            case 87: mapperName = "J87"; break;
            case 94: mapperName = "UN1ROM"; break;
            case 118: mapperName = "TxSROM"; break;
            case 119: mapperName = "TQROM"; break;
            case 163: mapperName = "Nanjing 163"; break;
            case 76: mapperName = "Namcot-3446"; break;
            case 88: mapperName = "Namcot-3443"; break;
            case 95: mapperName = "Namcot-3425"; break;
            case 148: mapperName = "Sachen SA-008-A / Tengen 800008"; break;
            case 154: mapperName = "Namcot-3453"; break;
            case 206: mapperName = "DxROM / Namcot 108"; break;
        }
        Serial.printf("  Mapper Name: %s\n", mapperName);
    }

    
    if (hasTrainer) {
        Serial.println("  Trainer: Yes (skipping 512 bytes)");
        f.seek(16 + 512);
    }

    
    prgSize = prgBanks * 0x4000;  
    chrSize = chrBanks * 0x2000;  

    
    if (prg) {
        free(prg);
    }
    prg = (uint8_t*)malloc(prgSize);
    if (!prg) prg = (uint8_t*)ps_malloc(prgSize);
    if (!prg) {
        Serial.println("  ERROR: Failed to allocate PRG memory!");
        f.close();
        return false;
    }
    Serial.printf("  PRG buffer allocated: %d bytes\n", prgSize);

    
    memset(prg, 0xFF, prgSize);
    size_t prgRead = f.read(prg, prgSize);
    Serial.printf("  PRG loaded: %d bytes\n", prgRead);
    
    
    Serial.printf("  First 16 bytes: ");
    for (int i = 0; i < 16 && i < (int)prgRead; i++) {
        Serial.printf("%02X ", prg[i]);
    }
    Serial.println();
    
    
    if (prgRead >= 0x4000) {
        uint32_t lastBankStart = prgRead - 0x4000;
        Serial.printf("  Last bank starts at offset: 0x%X\n", lastBankStart);
        
        
        Serial.printf("  Last 16 bytes (vectors): ");
        for (uint32_t i = prgRead - 16; i < prgRead; i++) {
            Serial.printf("%02X ", prg[i]);
        }
        Serial.println();
        
        
        uint16_t nmi   = prg[prgRead - 6] | (prg[prgRead - 5] << 8);
        uint16_t reset = prg[prgRead - 4] | (prg[prgRead - 3] << 8);
        uint16_t irq   = prg[prgRead - 2] | (prg[prgRead - 1] << 8);
        Serial.printf("  Vectors: NMI=$%04X RESET=$%04X IRQ=$%04X\n", nmi, reset, irq);
    }

    
    if (chrBanks > 0) {
        if (chr) {
            free(chr);
        }
        chr = (uint8_t*)ps_malloc(chrSize);
        if (!chr) {
            chr = (uint8_t*)malloc(chrSize);
        }
        if (chr) {
            size_t chrRead = f.read(chr, chrSize);
            Serial.printf("  CHR loaded: %d bytes\n", chrRead);
            chrWindow = chr;  
        } else {
            Serial.println("  WARNING: Failed to allocate CHR ROM, using CHR RAM");
            chrBanks = 0;
            chrSize = 0x2000;
            chrWindow = chrRam;
        }
    } else {
        Serial.println("  CHR RAM: 8KB (no CHR ROM)");
        chrSize = 0x2000;
        chrWindow = chrRam;
    }
    
    
    prgBankSelect = 0;
    mmc1ShiftReg = 0x10;
    mmc1WriteCount = 0;
    mmc1Control = 0x0C;  
    mmc1ChrBank0 = 0;
    mmc1ChrBank1 = 0;
    mmc1PrgBank = 0;
    cnromChrBank = 0;
    mapper34ChrBank0 = 0;
    mapper34ChrBank1 = 1;
    mapper34NinaMode = (mapper == 34 && chrBanks > 1);
    mmc3BankSelect = 0;
    memset(mmc3Banks, 0, sizeof(mmc3Banks));
    if (mapper == 76 || mapper == 88 || mapper == 95 || mapper == 154 || mapper == 206) {
        mmc3Banks[7] = 1;
    }
    mmc3PrgMode = false;
    mmc3ChrMode = false;
    mmc3IrqLatch = 0;
    mmc3IrqCounter = 0;
    mmc3IrqEnabled = false;
    mmc3IrqReload = false;
    mmc3IrqPending = false;
    mmc3PrevA12 = false;
    mmc3A12LowCycles = 0;
    mmc3LastObserveCpuCycle = 0;
    mmc3LastObserveCpuCycleValid = false;
    mmc3ClockedThisScanline = false;
    colorDreamsPrgBank = 0;
    colorDreamsChrBank = 0;
    gxromPrgBank = 0;
    gxromChrBank = 0;
    mmc2PrgBank = 0;
    mmc2ChrFd0 = 0;
    mmc2ChrFe0 = 0;
    mmc2ChrFd1 = 0;
    mmc2ChrFe1 = 0;
    mmc2Latch0FE = false;
    mmc2Latch1FE = false;
    fme7Command = 0;
    memset(fme7ChrBanks, 0, sizeof(fme7ChrBanks));
    memset(fme7PrgBanks, 0, sizeof(fme7PrgBanks));
    fme7RamEnable = false;
    fme7RamSelect = false;
    fme7RamWriteEnable = false;
    fme7IrqEnable = false;
    fme7IrqCounterEnable = false;
    fme7IrqCounter = 0;
    prgBank6000Offset = 0;
    memset(vrcChrBanks, 0, sizeof(vrcChrBanks));
    memset(vrcChrHighBits, 0, sizeof(vrcChrHighBits));
    vrc6Prg16Bank = 0;
    vrcPrgBank8000 = 0;
    vrcPrgBankA000 = 0;
    vrcPrgBankC000 = 0;
    vrc6PpuCtrl = 0x20;
    vrcIrqLatch = 0;
    vrcIrqCounter = 0;
    vrcIrqEnable = false;
    vrcIrqEnableAfterAck = false;
    vrcIrqModeCycle = false;
    vrcIrqPrescaler = 341;
    vrcRegCmd = 0;
    vrcReg1Mask = 0;
    vrcReg2Mask = 0;
    vrc2Latch = 0;
    memset(mapper18PrgRegs, 0, sizeof(mapper18PrgRegs));
    mapper18PrgRegs[1] = 1;
    memset(mapper18ChrRegs, 0, sizeof(mapper18ChrRegs));
    mapper18IrqCounter = 0;
    mapper18IrqLatch = 0;
    mapper18IrqEnable = false;
    mapper18IrqSizeMask = 0xFFFF;
    mapper18RamEnable = true;
    mapper18RamWriteEnable = true;
    mapper19PrgRegs[0] = 0;
    mapper19PrgRegs[1] = 1;
    mapper19PrgRegs[2] = 2;
    for (int i = 0; i < 8; i++) {
        mapper19ChrRegs[i] = (uint8_t)i;
    }
    mapper19NtRegs[0] = 0xE0;
    mapper19NtRegs[1] = 0xE1;
    mapper19NtRegs[2] = 0xE0;
    mapper19NtRegs[3] = 0xE1;
    memset(mapper19InternalRam, 0, sizeof(mapper19InternalRam));
    mapper19AddrPort = 0;
    mapper19ChrRamCtl = 0x03;
    mapper19WramProtect = 0x4F;
    mapper19RegF000 = 0;
    mapper19IrqCounter = 0;
    mapper19IrqEnable = false;
    m163Reg5000 = 0;
    m163Reg5200 = 0;
    m163Mode5300 = 0;
    m163FeedbackBit = 0;
    m163FeedbackSetMode = false;
    m163PrevA13 = false;
    m163LatchedA9 = 0;
    mmc5PrgMode = 3;
    mmc5ChrMode = 3;
    mmc5ExRamMode = 0;
    mmc5NtMap = 0;
    mmc5FillTile = 0;
    mmc5FillAttr = 0;
    mmc5UpperChrBits = 0;
    mmc5PrgRegs[0] = 0x00;
    mmc5PrgRegs[1] = 0x80;
    mmc5PrgRegs[2] = 0x81;
    mmc5PrgRegs[3] = 0x82;
    mmc5PrgRegs[4] = 0xFF;
    memset(mmc5ChrRegsA, 0, sizeof(mmc5ChrRegsA));
    memset(mmc5ChrRegsB, 0, sizeof(mmc5ChrRegsB));
    memset(mmc5ExRam, 0, sizeof(mmc5ExRam));
    mmc5LastChrWriteSetB = false;
    mmc5IrqEnable = false;
    mmc5IrqTargetScanline = 0;
    mmc5ScanlineCounter = 0;
    mmc5InFrame = false;
    mapper32Prg0 = 0;
    mapper32Prg1 = 1;
    mapper32Ctrl = 0;
    for (int i = 0; i < 8; i++) mapper32ChrBanks[i] = (uint8_t)i;
    mapper33Regs[0] = 0;
    mapper33Regs[1] = 1;
    mapper33Regs[2] = 0;
    mapper33Regs[3] = 1;
    mapper33Regs[4] = 0;
    mapper33Regs[5] = 0;
    mapper33Regs[6] = 0;
    mapper33Regs[7] = 0;
    mapper33Mirror = 0;
    mapper48IrqEnable = false;
    mapper48IrqCounter = 0;
    mapper48IrqLatch = 0;
    mapper64BankSelect = 0;
    memset(mapper64Regs, 0, sizeof(mapper64Regs));
    mapper64Regs[7] = 1;
    mapper64PrgMode = false;
    mapper64ChrMode = false;
    mapper64Chr1kMode = false;
    mapper64IrqEnable = false;
    mapper64IrqReload = false;
    mapper64IrqCycleMode = false;
    mapper64IrqCounter = 0;
    mapper64IrqLatch = 0;
    mapper64IrqPrescaler = 0;
    mapper64PrevA12 = false;
    mapper64A12LowCycles = 0;
    mapper70Reg = 0;
    mapper79Reg = 0;
    mapper87Reg = 0;
    mapper148Reg = 0;
    configureVrcMapping();
    oneScreenMirror = (mapper == 7 || mapper == 154);
    oneScreenUpper = false;
    mapper7BusConflictMode = 1;
    mapper71MirroringMode = (mapper == 71 && nes20 && submapper == 1);
    if (mapper == 7) {
        if (MAPPER7_BUS_CONFLICT_MODE == 1) {
            mapper7BusConflictMode = 1;
        } else if (MAPPER7_BUS_CONFLICT_MODE == 2) {
            mapper7BusConflictMode = 2;
        } else {
            // Compatibility default: keep bus conflicts disabled unless
            // explicitly forced at build time.
            mapper7BusConflictMode = 1;
        }
        Serial.printf("  Mapper7 conflicts: %s\n", mapper7BusConflictMode == 2 ? "AND" : "None");

        // Some mapper 7 dumps only carry a valid RESET vector in the last 32KB bank.
        // Keep bank 0 by default, but fall back to last bank when bank 0 vector is clearly invalid.
        const uint32_t prg32Count = (prgSize >= 0x8000u) ? (prgSize / 0x8000u) : 1u;
        if (prg32Count > 1u) {
            const uint16_t reset0 = (uint16_t)prg[0x7FFCu] | ((uint16_t)prg[0x7FFDu] << 8);
            const uint32_t lastBase = (prg32Count - 1u) * 0x8000u;
            const uint16_t resetLast = (uint16_t)prg[lastBase + 0x7FFCu] | ((uint16_t)prg[lastBase + 0x7FFDu] << 8);
            const bool reset0Valid = (reset0 >= 0x8000u) && (reset0 != 0xFFFFu);
            const bool resetLastValid = (resetLast >= 0x8000u) && (resetLast != 0xFFFFu);
            if (!reset0Valid && resetLastValid) {
                prgBankSelect = (uint8_t)(prg32Count - 1u);
                Serial.printf("  Mapper7 startup bank fallback: %u (RESET=$%04X)\n", prgBankSelect, resetLast);
            }
        }
    }
    ppuRenderingDirty = true;
    
    updateBankCache();
    updateNtPtrs();

    f.close();
    Serial.println("======================\n");
    return true;
}

bool Cartridge::isVrc2Mode() const {
    if (mapper != 23 && mapper != 25) {
        return false;
    }

    // NES 2.0 submapper 3 explicitly requests VRC2 behavior.
    // iNES mapper 23/25 defaults remain VRC4-like for compatibility.
    return submapper == 3;
}

bool Cartridge::isVrc4Mode() const {
    return mapper == 23 || mapper == 24 || mapper == 25 || mapper == 26 || mapper == 85;
}

bool Cartridge::shouldEnforceVrc4WramControl() const {
    if (mapper != 23 && mapper != 25) {
        return false;
    }

    // For NES 2.0 VRC4 submappers, $9002 bit0 controls WRAM visibility.
    // Keep legacy iNES mapper-23/25 behavior permissive for compatibility.
    return submapper == 1 || submapper == 2;
}

void Cartridge::configureVrcMapping() {
    vrcReg1Mask = 0;
    vrcReg2Mask = 0;

    if (mapper == 23) {
        switch (submapper) {
            case 1: // VRC4f (A0,A1)
            case 3: // VRC2b (A0,A1)
                vrcReg1Mask = 0x01;
                vrcReg2Mask = 0x02;
                break;
            case 2: // VRC4e (A2,A3)
                vrcReg1Mask = 0x04;
                vrcReg2Mask = 0x08;
                break;
            default: // iNES combined behavior: (A0,A1) + (A2,A3)
                vrcReg1Mask = 0x05;
                vrcReg2Mask = 0x0A;
                break;
        }
    } else if (mapper == 25) {
        switch (submapper) {
            case 1: // VRC4b (A1,A0)
            case 3: // VRC2c (A1,A0)
                vrcReg1Mask = 0x02;
                vrcReg2Mask = 0x01;
                break;
            case 2: // VRC4d (A3,A2)
                vrcReg1Mask = 0x08;
                vrcReg2Mask = 0x04;
                break;
            default: // iNES combined behavior: (A1,A0) + (A3,A2)
                vrcReg1Mask = 0x0A;
                vrcReg2Mask = 0x05;
                break;
        }
    }
}

void Cartridge::updateBankCache() {
    if (!prg || prgSize < 0x4000) {
        prgBank0Offset = 0;
        prgBank1Offset = 0;
        prgBank2Offset = 0;
        prgBank3Offset = 0;
        updateChrBankCache();
        return;
    }
    
    switch (mapper) {
        case 0:  
            prgBank0Offset = 0;
            prgBank1Offset = (prgSize > 0x4000) ? 0x4000 : 0;
            break;
            
        case 1:  
            updateMmc1Banks();
            break;
        case 5:
            updateMmc5PrgBanks();
            break;
            
        case 2:  
            prgBank1Offset = prgSize - 0x4000;
            prgBank0Offset = (uint32_t)prgBankSelect * 0x4000;
            if (prgBank0Offset >= prgSize) prgBank0Offset = 0;
            break;
            
        case 3:  
            prgBank0Offset = 0;
            prgBank1Offset = (prgSize > 0x4000) ? 0x4000 : 0;
            break;

        case 87:
            prgBank0Offset = 0;
            prgBank1Offset = (prgSize > 0x4000) ? 0x4000 : 0;
            break;

        case 32:
            updateMapper32Banks();
            break;

        case 33:
        case 48:
            updateMapper33Banks();
            break;

        case 64:
            updateMapper64Banks();
            break;

        case 34: {
            uint32_t bankCount = (prgSize >= 0x8000) ? (prgSize / 0x8000) : 1;
            uint32_t bank = prgBankSelect;
            if (bankCount > 0) {
                bank %= bankCount;
            } else {
                bank = 0;
            }
            prgBank0Offset = bank * 0x8000;
            prgBank1Offset = prgBank0Offset + 0x4000;
            break;
        }
            
        case 4:  
            updateMmc3Banks();
            break;

        case 7: {  
            uint32_t bankCount = (prgSize >= 0x8000) ? (prgSize / 0x8000) : 1;
            uint32_t bank = prgBankSelect;
            if (bankCount > 0) {
                bank %= bankCount;
            } else {
                bank = 0;
            }
            prgBank0Offset = bank * 0x8000;
            prgBank1Offset = prgBank0Offset + 0x4000;
            break;
        }

        case 9: {
            const uint32_t prg8kBanks = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
            const uint32_t sw = (prg8kBanks > 0) ? (mmc2PrgBank % prg8kBanks) : 0;
            const uint32_t fixedA = (prg8kBanks >= 3) ? (prg8kBanks - 3) : 0;
            const uint32_t fixedC = (prg8kBanks >= 2) ? (prg8kBanks - 2) : 0;
            const uint32_t fixedE = (prg8kBanks >= 1) ? (prg8kBanks - 1) : 0;
            prgBank0Offset = sw * 0x2000;
            prgBank1Offset = fixedA * 0x2000;
            prgBank2Offset = fixedC * 0x2000;
            prgBank3Offset = fixedE * 0x2000;
            break;
        }

        case 10: {
            const uint32_t prg16kBanks = (prgSize >= 0x4000) ? (prgSize / 0x4000) : 1;
            uint32_t sw = mmc2PrgBank & 0x0F;
            if (prg16kBanks > 0) {
                sw %= prg16kBanks;
            } else {
                sw = 0;
            }
            prgBank0Offset = sw * 0x4000;
            prgBank1Offset = (prg16kBanks > 0 ? (prg16kBanks - 1) : 0) * 0x4000;
            break;
        }

        case 11: {
            uint32_t bankCount = (prgSize >= 0x8000) ? (prgSize / 0x8000) : 1;
            uint32_t bank = colorDreamsPrgBank;
            if (bankCount > 0) {
                bank %= bankCount;
            } else {
                bank = 0;
            }
            prgBank0Offset = bank * 0x8000;
            prgBank1Offset = prgBank0Offset + 0x4000;
            break;
        }

        case 66: {
            uint32_t bankCount = (prgSize >= 0x8000) ? (prgSize / 0x8000) : 1;
            uint32_t bank = gxromPrgBank;
            if (bankCount > 0) {
                bank %= bankCount;
            } else {
                bank = 0;
            }
            prgBank0Offset = bank * 0x8000;
            prgBank1Offset = prgBank0Offset + 0x4000;
            break;
        }

        case 70: {
            const uint32_t prg16Count = (prgSize >= 0x4000) ? (prgSize / 0x4000) : 1;
            uint32_t b = (mapper70Reg >> 4) & 0x0F;
            if (prg16Count > 0) b %= prg16Count; else b = 0;
            prgBank0Offset = b * 0x4000;
            prgBank1Offset = (prg16Count > 0 ? (prg16Count - 1) : 0) * 0x4000;
            break;
        }

        case 71: {
            uint32_t bankCount = (prgSize >= 0x4000) ? (prgSize / 0x4000) : 1;
            uint32_t bank = prgBankSelect;
            if (bankCount > 0) {
                bank %= bankCount;
            } else {
                bank = 0;
            }
            prgBank0Offset = bank * 0x4000;
            prgBank1Offset = (bankCount > 0 ? (bankCount - 1) : 0) * 0x4000;
            break;
        }

        case 79:
        case 148: {
            const uint32_t prg32Count = (prgSize >= 0x8000) ? (prgSize / 0x8000) : 1;
            uint32_t b = (mapper == 79) ? ((mapper79Reg >> 3) & 0x01) : ((mapper148Reg >> 3) & 0x01);
            if (prg32Count > 0) b %= prg32Count; else b = 0;
            prgBank0Offset = b * 0x8000;
            prgBank1Offset = prgBank0Offset + 0x4000;
            break;
        }

        case 94: {
            uint32_t bankCount = (prgSize >= 0x4000) ? (prgSize / 0x4000) : 1;
            uint32_t bank = prgBankSelect;
            if (bankCount > 0) {
                bank %= bankCount;
            } else {
                bank = 0;
            }
            prgBank0Offset = bank * 0x4000;
            prgBank1Offset = (bankCount > 0 ? (bankCount - 1) : 0) * 0x4000;
            break;
        }

        case 24:
        case 26: {
            const uint32_t prg16Count = (prgSize >= 0x4000) ? (prgSize / 0x4000) : 1;
            const uint32_t prg8Count = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
            uint32_t b16 = vrc6Prg16Bank;
            uint32_t bC = vrcPrgBankC000;
            if (prg16Count > 0) b16 %= prg16Count; else b16 = 0;
            if (prg8Count > 0) bC %= prg8Count; else bC = 0;
            prgBank0Offset = b16 * 0x4000;
            prgBank2Offset = bC * 0x2000;
            prgBank3Offset = (prg8Count > 0 ? (prg8Count - 1) : 0) * 0x2000;
            break;
        }

        case 23:
        case 25: {
            const uint32_t prg8Count = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
            uint32_t b0 = vrcPrgBank8000 & 0x1F;
            uint32_t b1 = vrcPrgBankA000 & 0x1F;
            if (prg8Count > 0) {
                b0 %= prg8Count;
                b1 %= prg8Count;
            } else {
                b0 = b1 = 0;
            }
            const uint32_t secondLast = (prg8Count > 1) ? (prg8Count - 2) : 0;
            const uint32_t last = (prg8Count > 0) ? (prg8Count - 1) : 0;
            if (isVrc2Mode()) {
                prgBank0Offset = b0 * 0x2000;
                prgBank2Offset = secondLast * 0x2000;
            } else if (vrcRegCmd & 0x02) {
                prgBank0Offset = secondLast * 0x2000;
                prgBank2Offset = b0 * 0x2000;
            } else {
                prgBank0Offset = b0 * 0x2000;
                prgBank2Offset = secondLast * 0x2000;
            }
            prgBank1Offset = b1 * 0x2000;
            prgBank3Offset = last * 0x2000;
            break;
        }

        case 85: {
            const uint32_t prg8Count = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
            uint32_t b0 = vrcPrgBank8000;
            uint32_t b1 = vrcPrgBankA000;
            uint32_t b2 = vrcPrgBankC000;
            if (prg8Count > 0) {
                b0 %= prg8Count;
                b1 %= prg8Count;
                b2 %= prg8Count;
            } else {
                b0 = b1 = b2 = 0;
            }
            prgBank0Offset = b0 * 0x2000;
            prgBank1Offset = b1 * 0x2000;
            prgBank2Offset = b2 * 0x2000;
            prgBank3Offset = (prg8Count > 0 ? (prg8Count - 1) : 0) * 0x2000;
            break;
        }

        case 118:
        case 119:
            updateMmc3Banks();
            break;
        case 76:
        case 88:
        case 95:
        case 154:
        case 206:
            updateNamco108Banks();
            break;

        case 163:
            updateMapper163Prg();
            break;

        case 69: {
            const uint32_t prg8kBanks = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
            uint32_t b0 = fme7PrgBanks[1] & 0x3F;
            uint32_t b1 = fme7PrgBanks[2] & 0x3F;
            uint32_t b2 = fme7PrgBanks[3] & 0x3F;
            if (prg8kBanks > 0) {
                b0 %= prg8kBanks;
                b1 %= prg8kBanks;
                b2 %= prg8kBanks;
            } else {
                b0 = b1 = b2 = 0;
            }
            prgBank0Offset = b0 * 0x2000;
            prgBank1Offset = b1 * 0x2000;
            prgBank2Offset = b2 * 0x2000;
            prgBank3Offset = (prg8kBanks > 0 ? (prg8kBanks - 1) : 0) * 0x2000;

            uint32_t b6000 = fme7PrgBanks[0] & 0x3F;
            if (prg8kBanks > 0) {
                b6000 %= prg8kBanks;
            } else {
                b6000 = 0;
            }
            prgBank6000Offset = b6000 * 0x2000;
            break;
        }

        case 18:
            updateMapper18Banks();
            break;

        case 19:
            updateMapper19PrgBanks();
            break;
    }
    
    
    updateChrBankCache();
}

void Cartridge::updateMmc1Banks() {
    
    switch (mmc1Control & 0x03) {
        case 0:
            oneScreenMirror = true;
            oneScreenUpper = false;  
            break;
        case 1:
            oneScreenMirror = true;
            oneScreenUpper = true;   
            break;
        case 2:
            oneScreenMirror = false;
            mirrorVertical = true;   
            break;
        case 3:
            oneScreenMirror = false;
            mirrorVertical = false;  
            break;
    }
    updateNtPtrs();
    
    
    uint8_t prgMode = (mmc1Control >> 2) & 0x03;
    uint8_t prgBank = mmc1PrgBank & 0x0F;
    
    switch (prgMode) {
        case 0:
        case 1:
            
            prgBank0Offset = ((prgBank & 0x0E) * 0x4000);
            prgBank1Offset = prgBank0Offset + 0x4000;
            break;
        case 2:
            
            prgBank0Offset = 0;
            prgBank1Offset = prgBank * 0x4000;
            break;
        case 3:
            
            prgBank0Offset = prgBank * 0x4000;
            prgBank1Offset = prgSize - 0x4000;
            break;
    }
    
    
    if (prgBank0Offset >= prgSize) prgBank0Offset = 0;
    if (prgBank1Offset >= prgSize) prgBank1Offset = prgSize - 0x4000;
}
void Cartridge::updateMmc3Banks() {
    
    uint8_t prgMask = prgBanks * 2 - 1;
    uint8_t prg0 = mmc3Banks[6] & prgMask;
    uint8_t prg1 = mmc3Banks[7] & prgMask;
    uint8_t prgSecondLast = (prgBanks * 2 - 2) & prgMask;
    uint8_t prgLast = prgMask;
    
    if (mmc3PrgMode) {
        
        prgBank0Offset = (uint32_t)prgSecondLast * 0x2000;
        prgBank2Offset = (uint32_t)prg0 * 0x2000;
    } else {
        
        prgBank0Offset = (uint32_t)prg0 * 0x2000;
        prgBank2Offset = (uint32_t)prgSecondLast * 0x2000;
    }
    prgBank1Offset = (uint32_t)prg1 * 0x2000;
    prgBank3Offset = (uint32_t)prgLast * 0x2000;
    if (mapper == 118) {
        updateMapper118Nametables();
    }
}

void Cartridge::updateNamco108Banks() {
    const uint32_t prg8kCount = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
    const uint32_t last = (prg8kCount > 0) ? (prg8kCount - 1) : 0;
    const uint32_t secondLast = (prg8kCount > 1) ? (prg8kCount - 2) : 0;
    uint32_t b0 = mmc3Banks[6] & 0x0F;
    uint32_t b1 = mmc3Banks[7] & 0x0F;
    if (prg8kCount > 0) {
        b0 %= prg8kCount;
        b1 %= prg8kCount;
    } else {
        b0 = 0;
        b1 = 0;
    }

    prgBank0Offset = b0 * 0x2000;
    prgBank1Offset = b1 * 0x2000;
    prgBank2Offset = secondLast * 0x2000;
    prgBank3Offset = last * 0x2000;
}

void Cartridge::updateMapper32Banks() {
    const uint32_t prg8kCount = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
    uint32_t b0 = mapper32Prg0;
    uint32_t b1 = mapper32Prg1;
    if (prg8kCount > 0) {
        b0 %= prg8kCount;
        b1 %= prg8kCount;
    } else {
        b0 = b1 = 0;
    }
    const uint32_t secondLast = (prg8kCount > 1) ? (prg8kCount - 2) : 0;
    const uint32_t last = (prg8kCount > 0) ? (prg8kCount - 1) : 0;
    const bool swap = (mapper32Ctrl & 0x02) != 0;
    if (!swap) {
        prgBank0Offset = b0 * 0x2000;
        prgBank2Offset = secondLast * 0x2000;
    } else {
        prgBank0Offset = secondLast * 0x2000;
        prgBank2Offset = b0 * 0x2000;
    }
    prgBank1Offset = b1 * 0x2000;
    prgBank3Offset = last * 0x2000;
}

void Cartridge::updateMapper33Banks() {
    const uint32_t prg8kCount = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
    uint32_t b0 = mapper33Regs[0];
    uint32_t b1 = mapper33Regs[1];
    if (prg8kCount > 0) {
        b0 %= prg8kCount;
        b1 %= prg8kCount;
    } else {
        b0 = b1 = 0;
    }
    const uint32_t secondLast = (prg8kCount > 1) ? (prg8kCount - 2) : 0;
    const uint32_t last = (prg8kCount > 0) ? (prg8kCount - 1) : 0;
    prgBank0Offset = b0 * 0x2000;
    prgBank1Offset = b1 * 0x2000;
    prgBank2Offset = secondLast * 0x2000;
    prgBank3Offset = last * 0x2000;
}

void Cartridge::updateMapper64Banks() {
    const uint32_t prg8kCount = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
    uint32_t b6 = mapper64Regs[6];
    uint32_t b7 = mapper64Regs[7];
    uint32_t bF = mapper64Regs[15];
    if (prg8kCount > 0) {
        b6 %= prg8kCount;
        b7 %= prg8kCount;
        bF %= prg8kCount;
    } else {
        b6 = b7 = bF = 0;
    }
    const uint32_t last = (prg8kCount > 0) ? (prg8kCount - 1) : 0;
    if (!mapper64PrgMode) {
        prgBank0Offset = b6 * 0x2000;
        prgBank2Offset = bF * 0x2000;
    } else {
        prgBank0Offset = bF * 0x2000;
        prgBank2Offset = b6 * 0x2000;
    }
    prgBank1Offset = b7 * 0x2000;
    prgBank3Offset = last * 0x2000;
}

void Cartridge::updateMapper18Banks() {
    const uint32_t prg8Count = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
    uint32_t b0 = mapper18PrgRegs[0];
    uint32_t b1 = mapper18PrgRegs[1];
    uint32_t b2 = mapper18PrgRegs[2];
    if (prg8Count > 0) {
        b0 %= prg8Count;
        b1 %= prg8Count;
        b2 %= prg8Count;
    } else {
        b0 = b1 = b2 = 0;
    }
    const uint32_t last = (prg8Count > 0) ? (prg8Count - 1) : 0;
    prgBank0Offset = b0 * 0x2000;
    prgBank1Offset = b1 * 0x2000;
    prgBank2Offset = b2 * 0x2000;
    prgBank3Offset = last * 0x2000;
}

void Cartridge::updateMapper19PrgBanks() {
    const uint32_t prg8Count = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
    uint32_t b0 = mapper19PrgRegs[0] & 0x3F;
    uint32_t b1 = mapper19PrgRegs[1] & 0x3F;
    uint32_t b2 = mapper19PrgRegs[2] & 0x3F;
    if (prg8Count > 0) {
        b0 %= prg8Count;
        b1 %= prg8Count;
        b2 %= prg8Count;
    } else {
        b0 = b1 = b2 = 0;
    }
    const uint32_t last = (prg8Count > 0) ? (prg8Count - 1) : 0;
    prgBank0Offset = b0 * 0x2000;
    prgBank1Offset = b1 * 0x2000;
    prgBank2Offset = b2 * 0x2000;
    prgBank3Offset = last * 0x2000;
}

void Cartridge::updateMapper118Nametables() {
    if (mapper != 118 || !vram) {
        return;
    }

    uint8_t* oldNtPtrs[4] = {ntPtrs[0], ntPtrs[1], ntPtrs[2], ntPtrs[3]};
    uint8_t sourceReg[4];

    if (mmc3ChrMode) {
        sourceReg[0] = mmc3Banks[2];
        sourceReg[1] = mmc3Banks[3];
        sourceReg[2] = mmc3Banks[4];
        sourceReg[3] = mmc3Banks[5];
    } else {
        sourceReg[0] = mmc3Banks[0];
        sourceReg[1] = (uint8_t)(mmc3Banks[0] + 1);
        sourceReg[2] = mmc3Banks[1];
        sourceReg[3] = (uint8_t)(mmc3Banks[1] + 1);
    }

    for (int i = 0; i < 4; i++) {
        const bool a10 = (sourceReg[i] & 0x80) != 0;
        ntPtrs[i] = vram + (a10 ? 0x400 : 0x000);
    }

    for (int i = 0; i < 4; ++i) {
        if (ntPtrs[i] != oldNtPtrs[i]) {
            ppuRenderingDirty = true;
            break;
        }
    }
}

void Cartridge::updateMapper95Nametables() {
    if (mapper != 95 || !vram) {
        return;
    }

    uint8_t* oldNtPtrs[4] = {ntPtrs[0], ntPtrs[1], ntPtrs[2], ntPtrs[3]};
    const bool top = (mmc3Banks[0] & 0x20) != 0;
    const bool bottom = (mmc3Banks[1] & 0x20) != 0;
    ntPtrs[0] = vram + (top ? 0x400 : 0x000);
    ntPtrs[1] = vram + (top ? 0x400 : 0x000);
    ntPtrs[2] = vram + (bottom ? 0x400 : 0x000);
    ntPtrs[3] = vram + (bottom ? 0x400 : 0x000);

    for (int i = 0; i < 4; ++i) {
        if (ntPtrs[i] != oldNtPtrs[i]) {
            ppuRenderingDirty = true;
            break;
        }
    }
}

void Cartridge::updateMapper163Prg() {
    uint8_t low = m163Reg5000 & 0x0F;
    uint8_t high = m163Reg5200 & 0x03;
    if (m163Mode5300 & 0x01) {
        low = (low & 0x0C) | ((low & 0x01) << 1) | ((low & 0x02) >> 1);
        high = (high & 0x02) | ((high & 0x01) ^ 0x01);
    }
    if ((m163Mode5300 & 0x04) == 0) {
        low = (low & 0x0C) | 0x03;
    }
    uint32_t bank = ((uint32_t)high << 4) | low;
    prgBank0Offset = bank * 0x8000;
    if (prgSize > 0) {
        prgBank0Offset %= prgSize;
    }
}

void Cartridge::updateMmc5PrgBanks() {
    const uint32_t prg8Count = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
    auto regToBank = [&](uint8_t reg) -> uint32_t {
        uint32_t b = reg & 0x7F;
        if (prg8Count > 0) b %= prg8Count; else b = 0;
        return b;
    };

    switch (mmc5PrgMode & 0x03) {
        case 0: {
            uint32_t b = regToBank(mmc5PrgRegs[4]) & ~0x03u;
            prgBank0Offset = b * 0x2000;
            prgBank1Offset = prgBank0Offset + 0x2000;
            prgBank2Offset = prgBank0Offset + 0x4000;
            prgBank3Offset = prgBank0Offset + 0x6000;
            break;
        }
        case 1: {
            uint32_t b0 = regToBank(mmc5PrgRegs[2]) & ~0x01u;
            uint32_t b1 = regToBank(mmc5PrgRegs[4]) & ~0x01u;
            prgBank0Offset = b0 * 0x2000;
            prgBank1Offset = prgBank0Offset + 0x2000;
            prgBank2Offset = b1 * 0x2000;
            prgBank3Offset = prgBank2Offset + 0x2000;
            break;
        }
        case 2: {
            uint32_t b0 = regToBank(mmc5PrgRegs[2]) & ~0x01u;
            uint32_t b1 = regToBank(mmc5PrgRegs[3]);
            uint32_t b2 = regToBank(mmc5PrgRegs[4]);
            prgBank0Offset = b0 * 0x2000;
            prgBank1Offset = prgBank0Offset + 0x2000;
            prgBank2Offset = b1 * 0x2000;
            prgBank3Offset = b2 * 0x2000;
            break;
        }
        default: {
            prgBank0Offset = regToBank(mmc5PrgRegs[1]) * 0x2000;
            prgBank1Offset = regToBank(mmc5PrgRegs[2]) * 0x2000;
            prgBank2Offset = regToBank(mmc5PrgRegs[3]) * 0x2000;
            prgBank3Offset = regToBank(mmc5PrgRegs[4]) * 0x2000;
            break;
        }
    }
}

void Cartridge::updateMmc5ChrBanks() {
    if (!chr || chrSize == 0) {
        for (int i = 0; i < 8; i++) {
            mmc5BgChrPtrs[i] = chrRam + (i * 0x400);
            mmc5SpChrPtrs[i] = chrRam + (i * 0x400);
        }
        return;
    }

    const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
    auto toPtr = [&](uint16_t bank) -> uint8_t* {
        uint32_t b = bank;
        if (chr1kCount > 0) b %= chr1kCount; else b = 0;
        return chr + b * 0x400;
    };

    uint16_t baseA[8];
    uint16_t baseB[8];
    for (int i = 0; i < 8; i++) {
        baseA[i] = (uint16_t)(mmc5ChrRegsA[i] | (mmc5UpperChrBits << 8));
    }
    for (int i = 0; i < 8; i++) {
        baseB[i] = (uint16_t)(mmc5ChrRegsB[i & 3] | (mmc5UpperChrBits << 8));
    }

    const uint8_t mode = mmc5ChrMode & 0x03;
    if (mode == 0) {
        uint16_t bA = baseA[7] & ~0x07u;
        uint16_t bB = baseB[3] & ~0x07u;
        for (int i = 0; i < 8; i++) {
            mmc5SpChrPtrs[i] = toPtr((uint16_t)(bA + i));
            mmc5BgChrPtrs[i] = toPtr((uint16_t)(bB + i));
        }
    } else if (mode == 1) {
        uint16_t a0 = baseA[3] & ~0x03u;
        uint16_t a1 = baseA[7] & ~0x03u;
        uint16_t b0 = baseB[1] & ~0x03u;
        uint16_t b1 = baseB[3] & ~0x03u;
        for (int i = 0; i < 4; i++) {
            mmc5SpChrPtrs[i] = toPtr((uint16_t)(a0 + i));
            mmc5SpChrPtrs[i + 4] = toPtr((uint16_t)(a1 + i));
            mmc5BgChrPtrs[i] = toPtr((uint16_t)(b0 + i));
            mmc5BgChrPtrs[i + 4] = toPtr((uint16_t)(b1 + i));
        }
    } else if (mode == 2) {
        uint16_t a0 = baseA[1] & ~0x01u;
        uint16_t a1 = baseA[3] & ~0x01u;
        uint16_t a2 = baseA[5] & ~0x01u;
        uint16_t a3 = baseA[7] & ~0x01u;
        uint16_t b0 = baseB[0] & ~0x01u;
        uint16_t b1 = baseB[1] & ~0x01u;
        uint16_t b2 = baseB[2] & ~0x01u;
        uint16_t b3 = baseB[3] & ~0x01u;
        mmc5SpChrPtrs[0] = toPtr(a0); mmc5SpChrPtrs[1] = toPtr((uint16_t)(a0 + 1));
        mmc5SpChrPtrs[2] = toPtr(a1); mmc5SpChrPtrs[3] = toPtr((uint16_t)(a1 + 1));
        mmc5SpChrPtrs[4] = toPtr(a2); mmc5SpChrPtrs[5] = toPtr((uint16_t)(a2 + 1));
        mmc5SpChrPtrs[6] = toPtr(a3); mmc5SpChrPtrs[7] = toPtr((uint16_t)(a3 + 1));
        mmc5BgChrPtrs[0] = toPtr(b0); mmc5BgChrPtrs[1] = toPtr((uint16_t)(b0 + 1));
        mmc5BgChrPtrs[2] = toPtr(b1); mmc5BgChrPtrs[3] = toPtr((uint16_t)(b1 + 1));
        mmc5BgChrPtrs[4] = toPtr(b2); mmc5BgChrPtrs[5] = toPtr((uint16_t)(b2 + 1));
        mmc5BgChrPtrs[6] = toPtr(b3); mmc5BgChrPtrs[7] = toPtr((uint16_t)(b3 + 1));
    } else {
        for (int i = 0; i < 8; i++) {
            mmc5SpChrPtrs[i] = toPtr(baseA[i]);
            mmc5BgChrPtrs[i] = toPtr(baseB[i]);
        }
    }
}

void Cartridge::updateChrBankCache() {
    uint8_t* oldChrPtrs[8];
    memcpy(oldChrPtrs, chrBankPtrs, sizeof(oldChrPtrs));

    
    if (!chr || chrBanks == 0) {
        for (int i = 0; i < 8; i++) {
            chrBankPtrs[i] = chrRam + (i * 0x400);
        }
    } else {
        switch (mapper) {
            case 5: {
                updateMmc5ChrBanks();
                for (int i = 0; i < 8; i++) {
                    chrBankPtrs[i] = mmc5BgChrPtrs[i];
                }
                break;
            }
            case 4:
            case 118:
            case 119: {
                
                uint8_t chrMask = chrBanks * 8 - 1;
                uint8_t banks[8];
                
                if (mmc3ChrMode) {
                    
                    banks[0] = mmc3Banks[2];
                    banks[1] = mmc3Banks[3];
                    banks[2] = mmc3Banks[4];
                    banks[3] = mmc3Banks[5];
                    banks[4] = mmc3Banks[0] & 0xFE;
                    banks[5] = (mmc3Banks[0] & 0xFE) + 1;
                    banks[6] = mmc3Banks[1] & 0xFE;
                    banks[7] = (mmc3Banks[1] & 0xFE) + 1;
                } else {
                    
                    banks[0] = mmc3Banks[0] & 0xFE;
                    banks[1] = (mmc3Banks[0] & 0xFE) + 1;
                    banks[2] = mmc3Banks[1] & 0xFE;
                    banks[3] = (mmc3Banks[1] & 0xFE) + 1;
                    banks[4] = mmc3Banks[2];
                    banks[5] = mmc3Banks[3];
                    banks[6] = mmc3Banks[4];
                    banks[7] = mmc3Banks[5];
                }
                
                for (int i = 0; i < 8; i++) {
                    uint8_t bankValue = banks[i];
                    if (mapper == 118) {
                        bankValue &= 0x7F;
                    }

                    if (mapper == 119 && (bankValue & 0x40)) {
                        uint32_t ramBank = bankValue & 0x07;
                        chrBankPtrs[i] = chrRam + (ramBank * 0x400);
                        continue;
                    }

                    uint32_t offset = (uint32_t)(bankValue & chrMask) * 0x400;
                    if (offset >= chrSize) offset %= chrSize;
                    chrBankPtrs[i] = chr + offset;
                }
                break;
            }

            case 76:
            case 88:
            case 95:
            case 154:
            case 206: {
                const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
                auto setChrBank = [&](int slot, uint32_t bank) {
                    if (chr1kCount > 0) {
                        bank %= chr1kCount;
                    } else {
                        bank = 0;
                    }
                    chrBankPtrs[slot] = chr + bank * 0x400;
                };

                uint32_t banks[8] = {0};
                if (mapper == 76) {
                    banks[0] = (uint32_t)(mmc3Banks[2] & 0x3F) * 2u;
                    banks[1] = banks[0] + 1u;
                    banks[2] = (uint32_t)(mmc3Banks[3] & 0x3F) * 2u;
                    banks[3] = banks[2] + 1u;
                    banks[4] = (uint32_t)(mmc3Banks[4] & 0x3F) * 2u;
                    banks[5] = banks[4] + 1u;
                    banks[6] = (uint32_t)(mmc3Banks[5] & 0x3F) * 2u;
                    banks[7] = banks[6] + 1u;
                } else {
                    banks[0] = (uint32_t)(mmc3Banks[0] & 0x3E);
                    banks[1] = banks[0] + 1u;
                    banks[2] = (uint32_t)(mmc3Banks[1] & 0x3E);
                    banks[3] = banks[2] + 1u;
                    banks[4] = (uint32_t)(mmc3Banks[2] & 0x3F);
                    banks[5] = (uint32_t)(mmc3Banks[3] & 0x3F);
                    banks[6] = (uint32_t)(mmc3Banks[4] & 0x3F);
                    banks[7] = (uint32_t)(mmc3Banks[5] & 0x3F);

                    if (mapper == 88 || mapper == 154) {
                        banks[4] |= 0x40u;
                        banks[5] |= 0x40u;
                        banks[6] |= 0x40u;
                        banks[7] |= 0x40u;
                    }
                }

                for (int i = 0; i < 8; ++i) {
                    setChrBank(i, banks[i]);
                }

                if (mapper == 95) {
                    updateMapper95Nametables();
                }
                break;
            }

            case 9:
            case 10: {
                const uint32_t chr4kCount = (chrSize >= 0x1000) ? (chrSize / 0x1000) : 1;
                uint32_t bankLo = (mmc2Latch0FE ? mmc2ChrFe0 : mmc2ChrFd0);
                uint32_t bankHi = (mmc2Latch1FE ? mmc2ChrFe1 : mmc2ChrFd1);
                if (chr4kCount > 0) {
                    bankLo %= chr4kCount;
                    bankHi %= chr4kCount;
                } else {
                    bankLo = 0;
                    bankHi = 0;
                }
                const uint32_t baseLo = bankLo * 0x1000;
                const uint32_t baseHi = bankHi * 0x1000;
                for (int i = 0; i < 4; i++) {
                    chrBankPtrs[i] = chr + baseLo + (uint32_t)i * 0x400;
                    chrBankPtrs[i + 4] = chr + baseHi + (uint32_t)i * 0x400;
                }
                break;
            }

            case 69: {
                const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
                for (int i = 0; i < 8; i++) {
                    uint32_t bank = fme7ChrBanks[i];
                    if (chr1kCount > 0) {
                        bank %= chr1kCount;
                    } else {
                        bank = 0;
                    }
                    chrBankPtrs[i] = chr + bank * 0x400;
                }
                break;
            }

            case 18: {
                const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
                for (int i = 0; i < 8; i++) {
                    uint32_t bank = mapper18ChrRegs[i];
                    if (chr1kCount > 0) {
                        bank %= chr1kCount;
                    } else {
                        bank = 0;
                    }
                    chrBankPtrs[i] = chr + bank * 0x400;
                }
                break;
            }

            case 19: {
                const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
                for (int i = 0; i < 8; i++) {
                    const uint8_t reg = mapper19ChrRegs[i];
                    const bool ntAsChrEnabled = ((mapper19ChrRamCtl >> (i >> 2)) & 0x01) == 0;
                    if (ntAsChrEnabled && reg >= 0xE0) {
                        if (vram) {
                            chrBankPtrs[i] = vram + ((reg & 0x01) ? 0x400 : 0x000);
                        } else {
                            chrBankPtrs[i] = chrRam + ((reg & 0x01) ? 0x400 : 0x000);
                        }
                        continue;
                    }
                    uint32_t bank = reg;
                    if (chr1kCount > 0x100) {
                        const bool c = (mapper19RegF000 & 0x40) != 0;
                        const bool a12 = i >= 4;
                        const bool pin44 = c || a12;
                        bank |= (uint32_t)(pin44 ? 1 : 0) << 8;
                    }
                    if (chr1kCount > 0) {
                        bank %= chr1kCount;
                    } else {
                        bank = 0;
                    }
                    chrBankPtrs[i] = chr + bank * 0x400;
                }
                break;
            }

            case 23:
            case 25: {
                const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
                const bool vrc2 = isVrc2Mode();
                for (int i = 0; i < 8; i++) {
                    uint32_t bank = (uint32_t)vrcChrBanks[i];
                    if (!vrc2) {
                        bank |= ((uint32_t)(vrcChrHighBits[i] & 0x01) << 8);
                    }
                    if (chr1kCount > 0) {
                        bank %= chr1kCount;
                    } else {
                        bank = 0;
                    }
                    chrBankPtrs[i] = chr + bank * 0x400;
                }
                break;
            }

            case 24:
            case 26:
            case 85: {
                const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
                for (int i = 0; i < 8; i++) {
                    uint32_t bank = vrcChrBanks[i];
                    if (chr1kCount > 0) {
                        bank %= chr1kCount;
                    } else {
                        bank = 0;
                    }
                    chrBankPtrs[i] = chr + bank * 0x400;
                }
                break;
            }

            case 32: {
                const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
                for (int i = 0; i < 8; ++i) {
                    uint32_t bank = mapper32ChrBanks[i];
                    if (chr1kCount > 0) bank %= chr1kCount; else bank = 0;
                    chrBankPtrs[i] = chr + bank * 0x400;
                }
                break;
            }

            case 33:
            case 48: {
                const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
                uint32_t banks[8];
                banks[0] = (uint32_t)(mapper33Regs[2] & 0xFF) * 2u;
                banks[1] = banks[0] + 1u;
                banks[2] = (uint32_t)(mapper33Regs[3] & 0xFF) * 2u;
                banks[3] = banks[2] + 1u;
                banks[4] = mapper33Regs[4];
                banks[5] = mapper33Regs[5];
                banks[6] = mapper33Regs[6];
                banks[7] = mapper33Regs[7];
                for (int i = 0; i < 8; ++i) {
                    uint32_t bank = banks[i];
                    if (chr1kCount > 0) bank %= chr1kCount; else bank = 0;
                    chrBankPtrs[i] = chr + bank * 0x400;
                }
                break;
            }

            case 64: {
                const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
                uint32_t banks[8] = {0};
                const bool inv = mapper64ChrMode;
                const bool k1 = mapper64Chr1kMode;

                auto pairHi = [&](uint8_t reg) -> uint32_t { return (uint32_t)((reg & 0xFE) + 1); };

                if (!inv) {
                    banks[0] = mapper64Regs[0];
                    banks[1] = k1 ? mapper64Regs[8] : pairHi(mapper64Regs[0]);
                    banks[2] = mapper64Regs[1];
                    banks[3] = k1 ? mapper64Regs[9] : pairHi(mapper64Regs[1]);
                    banks[4] = mapper64Regs[2];
                    banks[5] = mapper64Regs[3];
                    banks[6] = mapper64Regs[4];
                    banks[7] = mapper64Regs[5];
                } else {
                    banks[0] = mapper64Regs[2];
                    banks[1] = mapper64Regs[3];
                    banks[2] = mapper64Regs[4];
                    banks[3] = mapper64Regs[5];
                    banks[4] = mapper64Regs[0];
                    banks[5] = k1 ? mapper64Regs[8] : pairHi(mapper64Regs[0]);
                    banks[6] = mapper64Regs[1];
                    banks[7] = k1 ? mapper64Regs[9] : pairHi(mapper64Regs[1]);
                }

                for (int i = 0; i < 8; ++i) {
                    uint32_t bank = banks[i];
                    if (chr1kCount > 0) bank %= chr1kCount; else bank = 0;
                    chrBankPtrs[i] = chr + bank * 0x400;
                }
                break;
            }

            case 70: {
                uint32_t bank = mapper70Reg & 0x0F;
                const uint32_t chr8kCount = (chrSize >= 0x2000) ? (chrSize / 0x2000) : 1;
                if (chr8kCount > 0) bank %= chr8kCount; else bank = 0;
                const uint32_t base = bank * 0x2000;
                for (int i = 0; i < 8; ++i) {
                    chrBankPtrs[i] = chr + base + (uint32_t)i * 0x400;
                }
                break;
            }

            case 79:
            case 148: {
                uint32_t bank = (mapper == 79 ? (mapper79Reg & 0x07) : (mapper148Reg & 0x07));
                const uint32_t chr8kCount = (chrSize >= 0x2000) ? (chrSize / 0x2000) : 1;
                if (chr8kCount > 0) bank %= chr8kCount; else bank = 0;
                const uint32_t base = bank * 0x2000;
                for (int i = 0; i < 8; ++i) {
                    chrBankPtrs[i] = chr + base + (uint32_t)i * 0x400;
                }
                break;
            }

            case 87: {
                uint32_t bank = mapper87Reg & 0x03;
                const uint32_t chr8kCount = (chrSize >= 0x2000) ? (chrSize / 0x2000) : 1;
                if (chr8kCount > 0) bank %= chr8kCount; else bank = 0;
                const uint32_t base = bank * 0x2000;
                for (int i = 0; i < 8; ++i) {
                    chrBankPtrs[i] = chr + base + (uint32_t)i * 0x400;
                }
                break;
            }
            
            case 1: {
                
                bool chr8kMode = (mmc1Control & 0x10) == 0;
                
                if (chr8kMode) {
                    uint32_t base = (uint32_t)(mmc1ChrBank0 >> 1) * 0x2000;
                    if (base >= chrSize) base %= chrSize;
                    for (int i = 0; i < 8; i++) {
                        uint32_t offset = base + i * 0x400;
                        if (offset >= chrSize) offset %= chrSize;
                        chrBankPtrs[i] = chr + offset;
                    }
                } else {
                    uint32_t base0 = (uint32_t)mmc1ChrBank0 * 0x1000;
                    uint32_t base1 = (uint32_t)mmc1ChrBank1 * 0x1000;
                    if (base0 >= chrSize) base0 %= chrSize;
                    if (base1 >= chrSize) base1 %= chrSize;
                    for (int i = 0; i < 4; i++) {
                        uint32_t off0 = base0 + i * 0x400;
                        uint32_t off1 = base1 + i * 0x400;
                        if (off0 >= chrSize) off0 %= chrSize;
                        if (off1 >= chrSize) off1 %= chrSize;
                        chrBankPtrs[i] = chr + off0;
                        chrBankPtrs[i + 4] = chr + off1;
                    }
                }
                break;
            }

            case 34: {
                if (mapper34NinaMode) {
                    const uint32_t chr4kCount = (chrSize >= 0x1000) ? (chrSize / 0x1000) : 1;
                    uint32_t bank0 = mapper34ChrBank0;
                    uint32_t bank1 = mapper34ChrBank1;
                    if (chr4kCount > 0) {
                        bank0 %= chr4kCount;
                        bank1 %= chr4kCount;
                    } else {
                        bank0 = 0;
                        bank1 = 0;
                    }
                    const uint32_t base0 = bank0 * 0x1000;
                    const uint32_t base1 = bank1 * 0x1000;
                    for (int i = 0; i < 4; i++) {
                        chrBankPtrs[i] = chr + base0 + (uint32_t)i * 0x400;
                        chrBankPtrs[i + 4] = chr + base1 + (uint32_t)i * 0x400;
                    }
                } else {
                    uint8_t* base = chrWindow ? chrWindow : chr;
                    for (int i = 0; i < 8; i++) {
                        chrBankPtrs[i] = base + (i * 0x400);
                    }
                }
                break;
            }
             
            default: {
                
                uint8_t* base = chrWindow ? chrWindow : chr;
                for (int i = 0; i < 8; i++) {
                    chrBankPtrs[i] = base + (i * 0x400);
                }
                break;
            }
        }
    }

    for (int i = 0; i < 8; ++i)
    {
        if (chrBankPtrs[i] != oldChrPtrs[i])
        {
            ppuRenderingDirty = true;
            break;
        }
    }
}

uint8_t IRAM_ATTR Cartridge::cpuRead(uint16_t addr) {
    
    if (addr >= 0x6000 && addr < 0x8000) {
        if (mapper == 7) {
            // AxROM boards do not expose WRAM here; return open-bus-like value.
            return (uint8_t)(addr >> 8);
        }
        if (mapper == 5) {
            uint8_t bank = mmc5PrgRegs[0];
            if (bank & 0x80) {
                uint32_t prg8Count = (prgSize >= 0x2000) ? (prgSize / 0x2000) : 1;
                uint32_t b = bank & 0x7F;
                if (prg8Count > 0) b %= prg8Count; else b = 0;
                uint32_t idx = b * 0x2000 + (addr - 0x6000);
                if (prgSize) idx %= prgSize;
                return prg[idx];
            }
            return sram[((bank & 0x07) << 13) | (addr & 0x1FFF)];
        }
        if (mapper == 69) {
            if (fme7RamSelect) {
                return fme7RamEnable ? sram[addr & 0x1FFF] : 0xFF;
            }
            uint32_t idx = prgBank6000Offset + (uint32_t)(addr - 0x6000);
            if (prgSize) {
                idx %= prgSize;
            }
            return prg[idx];
        }
        if (mapper == 18) {
            if (!mapper18RamEnable) {
                return 0xFF;
            }
            return sram[addr & 0x1FFF];
        }
        if (mapper == 23 || mapper == 25) {
            if (isVrc2Mode()) {
                if (addr < 0x7000) {
                    return (uint8_t)(((addr >> 8) & 0xFE) | (vrc2Latch & 0x01));
                }
                return (uint8_t)(addr >> 8);
            }
            if (shouldEnforceVrc4WramControl() && (vrcRegCmd & 0x01) == 0) {
                return (uint8_t)(addr >> 8);
            }
            return sram[addr & 0x1FFF];
        }
        return sram[addr & 0x1FFF];
    }

    if (mapper == 163) {
        if (addr == 0x5100) {
            return m163FeedbackBit;
        }
        if (addr == 0x5500) {
            return (uint8_t)((m163Mode5300 & 0x04) | m163FeedbackBit);
        }
    }

    if (mapper == 19) {
        if (addr >= 0x4800 && addr < 0x5000) {
            const uint8_t ret = mapper19InternalRam[mapper19AddrPort & 0x7F];
            if (mapper19AddrPort & 0x80) {
                mapper19AddrPort = (uint8_t)((mapper19AddrPort & 0x80) | ((mapper19AddrPort + 1) & 0x7F));
            }
            return ret;
        }
        if (addr >= 0x5000 && addr < 0x5800) {
            return (uint8_t)(mapper19IrqCounter & 0xFF);
        }
        if (addr >= 0x5800 && addr < 0x6000) {
            return (uint8_t)(((mapper19IrqEnable ? 0x80 : 0x00) | ((mapper19IrqCounter >> 8) & 0x7F)));
        }
    }

    if (mapper == 5 && addr == 0x5204) {
        uint8_t inFrame = mmc5InFrame ? 0x40 : 0x00;
        uint8_t irq = mmc3IrqPending ? 0x80 : 0x00;
        mmc3IrqPending = false;
        return (uint8_t)(inFrame | irq);
    }
    if (mapper == 5 && addr >= 0x5C00 && addr <= 0x5FFF) {
        return mmc5ExRam[addr & 0x03FF];
    }
    
    if (addr < 0x8000 || !prg) {
        return 0;
    }

    switch (mapper) {
        case 0: return cpuReadMapper0(addr);
        case 1: return cpuReadMapper1(addr);
        case 2: return cpuReadMapper2(addr);
        case 3: return cpuReadMapper3(addr);
        case 34: return cpuReadMapper34(addr);
        case 4: return cpuReadMapper4(addr);
        case 5: return cpuReadMapper5(addr);
        case 7: return cpuReadMapper7(addr);
        case 9: return cpuReadMapper9(addr);
        case 10: return cpuReadMapper10(addr);
        case 18: return cpuReadMapper18(addr);
        case 19: return cpuReadMapper19(addr);
        case 23: return cpuReadMapper23(addr);
        case 25: return cpuReadMapper25(addr);
        case 11: return cpuReadMapper11(addr);
        case 32: return cpuReadMapper32(addr);
        case 33: return cpuReadMapper33(addr);
        case 48: return cpuReadMapper33(addr);
        case 64: return cpuReadMapper64(addr);
        case 24: return cpuReadMapper24(addr);
        case 26: return cpuReadMapper26(addr);
        case 66: return cpuReadMapper66(addr);
        case 69: return cpuReadMapper69(addr);
        case 70: return cpuReadMapper70(addr);
        case 71: return cpuReadMapper71(addr);
        case 79: return cpuReadMapper79(addr);
        case 85: return cpuReadMapper85(addr);
        case 87: return cpuReadMapper87(addr);
        case 94: return cpuReadMapper94(addr);
        case 118: return cpuReadMapper118(addr);
        case 119: return cpuReadMapper119(addr);
        case 148: return cpuReadMapper148(addr);
        case 76:
        case 88:
        case 95:
        case 154:
        case 206: return cpuReadMapper206(addr);
        case 163: return cpuReadMapper163(addr);
        default:
            
            uint32_t prgAddr = (addr - 0x8000);
            if (prgSize) prgAddr %= prgSize;
            return prg[prgAddr];
    }
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper0(uint16_t addr) {
    
    uint32_t base = (addr < 0xC000) ? prgBank0Offset : prgBank1Offset;
    return prg[base + (addr & 0x3FFF)];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper1(uint16_t addr) {
    
    if (addr < 0xC000) {
        uint32_t idx = prgBank0Offset + (addr - 0x8000);
        if (idx >= prgSize) idx %= prgSize;
        return prg[idx];
    } else {
        uint32_t idx = prgBank1Offset + (addr - 0xC000);
        if (idx >= prgSize) idx %= prgSize;
        return prg[idx];
    }
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper2(uint16_t addr) {
    
    uint32_t base = (addr < 0xC000) ? prgBank0Offset : prgBank1Offset;
    uint32_t off  = (addr < 0xC000) ? (uint32_t)(addr - 0x8000) : (uint32_t)(addr - 0xC000);
    uint32_t idx  = base + off;
    if (idx >= prgSize) idx %= prgSize;
    return prg[idx];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper3(uint16_t addr) {
    
    uint32_t base = (addr < 0xC000) ? prgBank0Offset : prgBank1Offset;
    return prg[base + (addr & 0x3FFF)];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper4(uint16_t addr) {
    
    uint32_t base;
    if (addr < 0xA000)      base = prgBank0Offset;
    else if (addr < 0xC000) base = prgBank1Offset;
    else if (addr < 0xE000) base = prgBank2Offset;
    else                     base = prgBank3Offset;
    return prg[base + (addr & 0x1FFF)];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper7(uint16_t addr) {
    uint32_t idx = prgBank0Offset + (uint32_t)(addr - 0x8000);
    if (prgSize) {
        idx %= prgSize;
    }
    return prg[idx];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper34(uint16_t addr) {
    uint32_t idx = prgBank0Offset + (uint32_t)(addr - 0x8000);
    if (prgSize) {
        idx %= prgSize;
    }
    return prg[idx];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper11(uint16_t addr) {
    uint32_t idx = prgBank0Offset + (uint32_t)(addr - 0x8000);
    if (prgSize) {
        idx %= prgSize;
    }
    return prg[idx];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper66(uint16_t addr) {
    uint32_t idx = prgBank0Offset + (uint32_t)(addr - 0x8000);
    if (prgSize) {
        idx %= prgSize;
    }
    return prg[idx];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper9(uint16_t addr) {
    uint32_t base;
    if (addr < 0xA000) base = prgBank0Offset;
    else if (addr < 0xC000) base = prgBank1Offset;
    else if (addr < 0xE000) base = prgBank2Offset;
    else base = prgBank3Offset;
    return prg[base + (addr & 0x1FFF)];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper5(uint16_t addr) {
    uint32_t base;
    if (addr < 0xA000)      base = prgBank0Offset;
    else if (addr < 0xC000) base = prgBank1Offset;
    else if (addr < 0xE000) base = prgBank2Offset;
    else                     base = prgBank3Offset;
    if (prgSize == 0) return 0;
    uint32_t idx = base + (addr & 0x1FFF);
    idx %= prgSize;
    return prg[idx];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper10(uint16_t addr) {
    uint32_t idx;
    if (addr < 0xC000) {
        idx = prgBank0Offset + (uint32_t)(addr - 0x8000);
    } else {
        idx = prgBank1Offset + (uint32_t)(addr - 0xC000);
    }
    if (prgSize) {
        idx %= prgSize;
    }
    return prg[idx];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper18(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper19(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper23(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper25(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper32(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper33(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper64(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper70(uint16_t addr) {
    return cpuReadMapper71(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper79(uint16_t addr) {
    return cpuReadMapper34(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper87(uint16_t addr) {
    return cpuReadMapper0(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper148(uint16_t addr) {
    return cpuReadMapper34(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper69(uint16_t addr) {
    uint32_t base;
    if (addr < 0xA000) base = prgBank0Offset;
    else if (addr < 0xC000) base = prgBank1Offset;
    else if (addr < 0xE000) base = prgBank2Offset;
    else base = prgBank3Offset;
    return prg[base + (addr & 0x1FFF)];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper118(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper119(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper206(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper24(uint16_t addr) {
    if (prgSize == 0) return 0;
    if (addr < 0xC000) {
        uint32_t idx = prgBank0Offset + (addr - 0x8000);
        idx %= prgSize;
        return prg[idx];
    }
    uint32_t base = (addr < 0xE000) ? prgBank2Offset : prgBank3Offset;
    uint32_t idx = base + (addr & 0x1FFF);
    idx %= prgSize;
    return prg[idx];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper26(uint16_t addr) {
    return cpuReadMapper24(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper71(uint16_t addr) {
    uint32_t idx;
    if (addr < 0xC000) {
        idx = prgBank0Offset + (uint32_t)(addr - 0x8000);
    } else {
        idx = prgBank1Offset + (uint32_t)(addr - 0xC000);
    }
    if (prgSize) {
        idx %= prgSize;
    }
    return prg[idx];
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper85(uint16_t addr) {
    return cpuReadMapper4(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper94(uint16_t addr) {
    return cpuReadMapper71(addr);
}

uint8_t IRAM_ATTR Cartridge::cpuReadMapper163(uint16_t addr) {
    if (prgSize == 0) return 0;
    uint32_t idx = prgBank0Offset + (uint32_t)(addr - 0x8000);
    idx %= prgSize;
    return prg[idx];
}

void IRAM_ATTR Cartridge::cpuWrite(uint16_t addr, uint8_t val) {
    if (mapper == 5 && addr >= 0x5000 && addr < 0x6000) {
        cpuWriteMapper5(addr, val);
        return;
    }
    if (mapper == 163 && addr >= 0x5000 && addr < 0x6000) {
        cpuWriteMapper163(addr, val);
        return;
    }
    if (mapper == 19 && addr >= 0x4800 && addr < 0x6000) {
        cpuWriteMapper19(addr, val);
        return;
    }
    if (mapper == 79 && addr >= 0x4100 && addr < 0x6000 && ((addr & 0x0100) == 0x0100)) {
        cpuWriteMapper79(addr, val);
        return;
    }
    if (mapper == 87 && addr >= 0x6000 && addr < 0x8000) {
        cpuWriteMapper87(addr, val);
        return;
    }
    
    if (addr >= 0x6000 && addr < 0x8000) {
        if (mapper == 7) {
            // AxROM: no writable WRAM in this range.
            return;
        }
        if (mapper == 34 && mapper34NinaMode && addr >= 0x7FFD) {
            cpuWriteMapper34(addr, val);
            return;
        }
        if (mapper == 5) {
            uint8_t bank = mmc5PrgRegs[0];
            if ((bank & 0x80) == 0) {
                sram[((bank & 0x07) << 13) | (addr & 0x1FFF)] = val;
            }
            return;
        }
        if (mapper == 69) {
            if (fme7RamSelect && fme7RamEnable && fme7RamWriteEnable) {
                sram[addr & 0x1FFF] = val;
            }
            return;
        }
        if (mapper == 18) {
            if (mapper18RamEnable && mapper18RamWriteEnable) {
                sram[addr & 0x1FFF] = val;
            }
            return;
        }
        if (mapper == 19) {
            if ((mapper19WramProtect & 0xF0) == 0x40) {
                const uint8_t page = (uint8_t)((addr - 0x6000) >> 11);
                const bool writeProtected = ((mapper19WramProtect >> page) & 0x01) != 0;
                if (!writeProtected) {
                    sram[addr & 0x1FFF] = val;
                }
            }
            return;
        }
        if (mapper == 23 || mapper == 25) {
            if (isVrc2Mode()) {
                if (addr < 0x7000) {
                    vrc2Latch = val & 0x01;
                }
                return;
            }
            if (shouldEnforceVrc4WramControl() && (vrcRegCmd & 0x01) == 0) {
                return;
            }
            sram[addr & 0x1FFF] = val;
            return;
        }
        sram[addr & 0x1FFF] = val;
        return;
    }
    
    if (addr < 0x8000) return;
    
    switch (mapper) {
        case 1: cpuWriteMapper1(addr, val); break;
        case 2: cpuWriteMapper2(addr, val); break;
        case 3: cpuWriteMapper3(addr, val); break;
        case 34: cpuWriteMapper34(addr, val); break;
        case 4: cpuWriteMapper4(addr, val); break;
        case 5: cpuWriteMapper5(addr, val); break;
        case 7: cpuWriteMapper7(addr, val); break;
        case 9: cpuWriteMapper9(addr, val); break;
        case 10: cpuWriteMapper10(addr, val); break;
        case 18: cpuWriteMapper18(addr, val); break;
        case 19: cpuWriteMapper19(addr, val); break;
        case 23: cpuWriteMapper23(addr, val); break;
        case 25: cpuWriteMapper25(addr, val); break;
        case 11: cpuWriteMapper11(addr, val); break;
        case 32: cpuWriteMapper32(addr, val); break;
        case 33: cpuWriteMapper33(addr, val); break;
        case 48:
            if (addr < 0xC000) cpuWriteMapper33(addr, val);
            else cpuWriteMapper48(addr, val);
            break;
        case 64: cpuWriteMapper64(addr, val); break;
        case 24: cpuWriteMapper24(addr, val); break;
        case 26: cpuWriteMapper26(addr, val); break;
        case 66: cpuWriteMapper66(addr, val); break;
        case 69: cpuWriteMapper69(addr, val); break;
        case 70: cpuWriteMapper70(addr, val); break;
        case 71: cpuWriteMapper71(addr, val); break;
        case 79: cpuWriteMapper79(addr, val); break;
        case 85: cpuWriteMapper85(addr, val); break;
        case 87: cpuWriteMapper87(addr, val); break;
        case 94: cpuWriteMapper94(addr, val); break;
        case 118: cpuWriteMapper118(addr, val); break;
        case 119: cpuWriteMapper119(addr, val); break;
        case 148: cpuWriteMapper148(addr, val); break;
        case 76:
        case 88:
        case 95:
        case 154:
        case 206: cpuWriteMapper206(addr, val); break;
        case 163: cpuWriteMapper163(addr, val); break;
    }
}

void Cartridge::cpuWriteMapper1(uint16_t addr, uint8_t val) {
    
    if (val & 0x80) {
        
        mmc1ShiftReg = 0x10;
        mmc1WriteCount = 0;
        mmc1Control |= 0x0C;  
        updateBankCache();
        return;
    }
    
    
    mmc1ShiftReg = ((val & 0x01) << 4) | (mmc1ShiftReg >> 1);
    mmc1WriteCount++;
    
    if (mmc1WriteCount == 5) {
        
        uint8_t regVal = mmc1ShiftReg & 0x1F;
        
        if (addr < 0xA000) {
            
            mmc1Control = regVal;
        } else if (addr < 0xC000) {
            
            mmc1ChrBank0 = regVal;
        } else if (addr < 0xE000) {
            
            mmc1ChrBank1 = regVal;
        } else {
            
            mmc1PrgBank = regVal;
        }
        
        mmc1ShiftReg = 0x10;
        mmc1WriteCount = 0;
        updateBankCache();
    }
}

void Cartridge::cpuWriteMapper2(uint16_t addr, uint8_t val) {
    
    uint8_t newBank = val & 0x0F;
    if (newBank >= prgBanks) {
        newBank = prgBanks - 1;
    }
    prgBankSelect = newBank;
    updateBankCache();
}

void Cartridge::cpuWriteMapper3(uint16_t addr, uint8_t val) {
    
    cnromChrBank = val & 0x03;
    if (chr && chrSize > 0x2000) {
        uint32_t offset = (uint32_t)cnromChrBank * 0x2000;
        if (offset < chrSize) {
            chrWindow = chr + offset;
        }
    }
    updateChrBankCache();  
}

void Cartridge::cpuWriteMapper34(uint16_t addr, uint8_t val) {
    if (mapper34NinaMode && addr >= 0x7FFD && addr <= 0x7FFF) {
        if (addr == 0x7FFD) {
            prgBankSelect = val & 0x0F;
            updateBankCache();
        } else if (addr == 0x7FFE) {
            mapper34ChrBank0 = val & 0x0F;
            updateChrBankCache();
        } else {
            mapper34ChrBank1 = val & 0x0F;
            updateChrBankCache();
        }
        return;
    }

    prgBankSelect = val & 0x0F;
    updateBankCache();
}

void Cartridge::cpuWriteMapper4(uint16_t addr, uint8_t val) {
    
    if (addr < 0xA000) {
        if (addr & 1) {
            
            uint8_t reg = mmc3BankSelect & 0x07;
            if (reg >= 6) {
                // MMC3 PRG registers only expose 6 bits.
                val &= 0x3F;
            }
            mmc3Banks[reg] = val;
            updateBankCache();
        } else {
            
            mmc3BankSelect = val;
            mmc3PrgMode = (val & 0x40) != 0;
            mmc3ChrMode = (val & 0x80) != 0;
            updateBankCache();
        }
    } else if (addr < 0xC000) {
        if (addr & 1) {
            
        } else {
            
            mirrorVertical = (val & 0x01) == 0;
            updateNtPtrs();
        }
    } else if (addr < 0xE000) {
        if (addr & 1) {
            // MMC3 $C001 sets reload flag; counter reload occurs on next qualified A12 rise.
            mmc3IrqReload = true;
            // Compatibility quirk: many boards/test ROMs expect the counter to be
            // effectively cleared here before the next qualified rise.
            mmc3IrqCounter = 0;
        } else {
            
            mmc3IrqLatch = val;
        }
    } else {
        if (addr & 1) {
            
            mmc3IrqEnabled = true;
        } else {
            
            mmc3IrqEnabled = false;
            mmc3IrqPending = false;
        }
    }
}

void Cartridge::cpuWriteMapper7(uint16_t addr, uint8_t val) {
    uint8_t writeVal = val;
    if (mapper7BusConflictMode == 2) {
        writeVal &= cpuReadMapper7(addr);
    }

    // Standard AxROM uses PPP in bits 0..2 (up to 256 KiB PRG).
    // Keep bit 3 only for oversized homebrew variants above 256 KiB.
    const uint8_t bankMask = (prgSize > 0x40000u) ? 0x0Fu : 0x07u;
    prgBankSelect = writeVal & bankMask;
    oneScreenMirror = true;
    oneScreenUpper = (writeVal & 0x10) != 0;
    updateBankCache();
    updateNtPtrs();
}

void Cartridge::cpuWriteMapper11(uint16_t addr, uint8_t val) {
    (void)addr;
    colorDreamsPrgBank = val & 0x03;
    colorDreamsChrBank = (val >> 4) & 0x0F;
    if (chr && chrSize >= 0x2000) {
        uint32_t chrBankCount = chrSize / 0x2000;
        uint32_t bank = colorDreamsChrBank;
        if (chrBankCount > 0) {
            bank %= chrBankCount;
        } else {
            bank = 0;
        }
        chrWindow = chr + bank * 0x2000;
    }
    updateBankCache();
}

void Cartridge::cpuWriteMapper66(uint16_t addr, uint8_t val) {
    (void)addr;
    gxromChrBank = val & 0x03;
    gxromPrgBank = (val >> 4) & 0x03;
    if (chr && chrSize >= 0x2000) {
        uint32_t chrBankCount = chrSize / 0x2000;
        uint32_t bank = gxromChrBank;
        if (chrBankCount > 0) {
            bank %= chrBankCount;
        } else {
            bank = 0;
        }
        chrWindow = chr + bank * 0x2000;
    }
    updateBankCache();
}

void Cartridge::cpuWriteMapper71(uint16_t addr, uint8_t val) {
    if (addr >= 0x9000 && addr <= 0x9FFF) {
        if (!mapper71MirroringMode) {
            mapper71MirroringMode = true;
        }
        if (mapper71MirroringMode) {
            oneScreenMirror = true;
            oneScreenUpper = (val & 0x10) != 0;
            updateNtPtrs();
        }
        return;
    }

    // For compatibility, treat all non-$9000 writes as bank-select writes.
    // This matches common iNES mapper 71 behavior across Camerica/Codemasters games.
    prgBankSelect = val & 0x0F;
    updateBankCache();
}

void Cartridge::cpuWriteMapper9(uint16_t addr, uint8_t val) {
    if (addr < 0xA000) return;
    if (addr < 0xB000) {
        mmc2PrgBank = val & 0x0F;
        updateBankCache();
    } else if (addr < 0xC000) {
        mmc2ChrFd0 = val & 0x1F;
        updateChrBankCache();
    } else if (addr < 0xD000) {
        mmc2ChrFe0 = val & 0x1F;
        updateChrBankCache();
    } else if (addr < 0xE000) {
        mmc2ChrFd1 = val & 0x1F;
        updateChrBankCache();
    } else if (addr < 0xF000) {
        mmc2ChrFe1 = val & 0x1F;
        updateChrBankCache();
    } else {
        oneScreenMirror = false;
        mirrorVertical = (val & 0x01) != 0;
        updateNtPtrs();
    }
}

void Cartridge::cpuWriteMapper5(uint16_t addr, uint8_t val) {
    switch (addr) {
        case 0x5100:
            mmc5PrgMode = val & 0x03;
            updateMmc5PrgBanks();
            break;
        case 0x5101:
            mmc5ChrMode = val & 0x03;
            updateChrBankCache();
            break;
        case 0x5104:
            mmc5ExRamMode = val & 0x03;
            break;
        case 0x5105:
            mmc5NtMap = val;
            break;
        case 0x5106:
            mmc5FillTile = val;
            break;
        case 0x5107:
            mmc5FillAttr = val;
            break;
        case 0x5113:
            mmc5PrgRegs[0] = val;
            break;
        case 0x5114:
        case 0x5115:
        case 0x5116:
        case 0x5117:
            mmc5PrgRegs[addr - 0x5113] = val;
            updateMmc5PrgBanks();
            break;
        case 0x5120:
        case 0x5121:
        case 0x5122:
        case 0x5123:
        case 0x5124:
        case 0x5125:
        case 0x5126:
        case 0x5127:
            mmc5ChrRegsA[addr - 0x5120] = val;
            mmc5LastChrWriteSetB = false;
            updateChrBankCache();
            break;
        case 0x5128:
        case 0x5129:
        case 0x512A:
        case 0x512B:
            mmc5ChrRegsB[addr - 0x5128] = val;
            mmc5LastChrWriteSetB = true;
            updateChrBankCache();
            break;
        case 0x5130:
            mmc5UpperChrBits = val & 0x03;
            updateChrBankCache();
            break;
        case 0x5203:
            mmc5IrqTargetScanline = val;
            break;
        case 0x5204:
            mmc5IrqEnable = (val & 0x80) != 0;
            if (!mmc5IrqEnable) {
                mmc3IrqPending = false;
            }
            break;
        default:
            if (addr >= 0x5C00 && addr <= 0x5FFF) {
                if (mmc5ExRamMode != 3) {
                    mmc5ExRam[addr & 0x03FF] = val;
                }
            }
            break;
    }
}

void Cartridge::cpuWriteMapper10(uint16_t addr, uint8_t val) {
    if (addr < 0xA000) return;
    if (addr < 0xB000) {
        mmc2PrgBank = val & 0x0F;
        updateBankCache();
    } else if (addr < 0xC000) {
        mmc2ChrFd0 = val & 0x1F;
        updateChrBankCache();
    } else if (addr < 0xD000) {
        mmc2ChrFe0 = val & 0x1F;
        updateChrBankCache();
    } else if (addr < 0xE000) {
        mmc2ChrFd1 = val & 0x1F;
        updateChrBankCache();
    } else if (addr < 0xF000) {
        mmc2ChrFe1 = val & 0x1F;
        updateChrBankCache();
    } else {
        oneScreenMirror = false;
        mirrorVertical = (val & 0x01) == 0;
        updateNtPtrs();
    }
}

void Cartridge::cpuWriteMapper32(uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return;
    if (addr < 0x9000) {
        mapper32Prg0 = val;
        updateBankCache();
    } else if (addr < 0xA000) {
        mapper32Ctrl = val;
        oneScreenMirror = false;
        mirrorVertical = (val & 0x01) == 0;
        updateNtPtrs();
        updateBankCache();
    } else if (addr < 0xB000) {
        mapper32Prg1 = val;
        updateBankCache();
    } else if (addr < 0xC000) {
        mapper32ChrBanks[addr & 0x07] = val;
        updateChrBankCache();
    }
}

void Cartridge::cpuWriteMapper33(uint16_t addr, uint8_t val) {
    uint16_t a = addr & 0xF003;
    switch (a) {
        case 0x8000:
            mapper33Regs[0] = val & 0x3F;
            if (mapper == 33) {
                mapper33Mirror = (val >> 6) & 0x01;
                oneScreenMirror = false;
                mirrorVertical = mapper33Mirror == 0;
                updateNtPtrs();
            }
            break;
        case 0x8001: mapper33Regs[1] = val & 0x3F; break;
        case 0x8002: mapper33Regs[2] = val; break;
        case 0x8003: mapper33Regs[3] = val; break;
        case 0xA000: mapper33Regs[4] = val; break;
        case 0xA001: mapper33Regs[5] = val; break;
        case 0xA002: mapper33Regs[6] = val; break;
        case 0xA003: mapper33Regs[7] = val; break;
        default: break;
    }
    updateBankCache();
}

void Cartridge::cpuWriteMapper48(uint16_t addr, uint8_t val) {
    uint16_t a = addr & 0xF003;
    switch (a) {
        case 0xC000:
            mapper48IrqLatch = val ^ 0xFF;
            break;
        case 0xC001:
            mapper48IrqCounter = mapper48IrqLatch;
            mmc3IrqPending = false;
            break;
        case 0xC002:
            mapper48IrqEnable = true;
            break;
        case 0xC003:
            mapper48IrqEnable = false;
            mmc3IrqPending = false;
            break;
        case 0xE000:
            oneScreenMirror = false;
            mirrorVertical = ((val >> 6) & 0x01) == 0;
            updateNtPtrs();
            break;
        default:
            break;
    }
}

void Cartridge::cpuWriteMapper64(uint16_t addr, uint8_t val) {
    switch (addr & 0xE001) {
        case 0x8000:
            mapper64BankSelect = val;
            mapper64PrgMode = (val & 0x40) != 0;
            mapper64ChrMode = (val & 0x80) != 0;
            mapper64Chr1kMode = (val & 0x20) != 0;
            updateBankCache();
            break;
        case 0x8001: {
            const uint8_t reg = mapper64BankSelect & 0x0F;
            mapper64Regs[reg] = val;
            updateBankCache();
            break;
        }
        case 0xA000:
            oneScreenMirror = false;
            mirrorVertical = (val & 0x01) == 0;
            updateNtPtrs();
            break;
        case 0xC000:
            mapper64IrqLatch = val;
            break;
        case 0xC001:
            mapper64IrqCycleMode = (val & 0x01) != 0;
            mapper64IrqReload = true;
            mapper64IrqPrescaler = 0;
            break;
        case 0xE000:
            mapper64IrqEnable = false;
            mmc3IrqPending = false;
            break;
        case 0xE001:
            mapper64IrqEnable = true;
            break;
        default:
            break;
    }
}

void Cartridge::cpuWriteMapper70(uint16_t addr, uint8_t val) {
    uint8_t writeVal = val & cpuReadMapper70(addr);
    mapper70Reg = writeVal;
    oneScreenMirror = false;
    mirrorVertical = (writeVal & 0x80) == 0;
    updateNtPtrs();
    updateBankCache();
}

void Cartridge::cpuWriteMapper79(uint16_t addr, uint8_t val) {
    (void)addr;
    mapper79Reg = val;
    updateBankCache();
}

void Cartridge::cpuWriteMapper87(uint16_t addr, uint8_t val) {
    (void)addr;
    mapper87Reg = (uint8_t)(((val & 0x01) << 1) | ((val >> 1) & 0x01));
    updateChrBankCache();
}

void Cartridge::cpuWriteMapper148(uint16_t addr, uint8_t val) {
    uint8_t writeVal = val & cpuReadMapper148(addr);
    mapper148Reg = writeVal;
    updateBankCache();
}

void Cartridge::cpuWriteMapper69(uint16_t addr, uint8_t val) {
    if (addr < 0x8000) {
        return;
    }

    if (addr < 0xA000) {
        fme7Command = val & 0x0F;
        return;
    }

    if (addr >= 0xC000) {
        return;
    }

    const uint8_t cmd = fme7Command & 0x0F;
    if (cmd <= 0x07) {
        fme7ChrBanks[cmd] = val;
        updateChrBankCache();
        return;
    }

    switch (cmd) {
        case 0x08:
            fme7RamEnable = (val & 0x80) != 0;
            fme7RamSelect = (val & 0x40) != 0;
            fme7RamWriteEnable = true;
            fme7PrgBanks[0] = val & 0x3F;
            updateBankCache();
            break;
        case 0x09:
        case 0x0A:
        case 0x0B:
            fme7PrgBanks[cmd - 0x08] = val & 0x3F;
            updateBankCache();
            break;
        case 0x0C:
            switch (val & 0x03) {
                case 0:
                    oneScreenMirror = false;
                    mirrorVertical = true;
                    break;
                case 1:
                    oneScreenMirror = false;
                    mirrorVertical = false;
                    break;
                case 2:
                    oneScreenMirror = true;
                    oneScreenUpper = false;
                    break;
                default:
                    oneScreenMirror = true;
                    oneScreenUpper = true;
                    break;
            }
            updateNtPtrs();
            break;
        case 0x0D:
            fme7IrqEnable = (val & 0x01) != 0;
            fme7IrqCounterEnable = (val & 0x80) != 0;
            mmc3IrqPending = false;
            break;
        case 0x0E:
            fme7IrqCounter = (fme7IrqCounter & 0xFF00) | val;
            break;
        case 0x0F:
            fme7IrqCounter = (fme7IrqCounter & 0x00FF) | ((uint16_t)val << 8);
            break;
        default:
            break;
    }
}

void Cartridge::cpuWriteMapper118(uint16_t addr, uint8_t val) {
    if (addr < 0x8000) {
        return;
    }

    if (addr < 0xA000) {
        if (addr & 1) {
            uint8_t reg = mmc3BankSelect & 0x07;
            mmc3Banks[reg] = val;
            updateBankCache();
        } else {
            mmc3BankSelect = val;
            mmc3PrgMode = (val & 0x40) != 0;
            mmc3ChrMode = (val & 0x80) != 0;
            updateBankCache();
        }
    } else if (addr < 0xC000) {
        if ((addr & 1) == 0) {
            // TxSROM ignores MMC3 mirroring register; CIRAM A10 comes from CHR bank bit 7.
            updateMapper118Nametables();
        }
    } else if (addr < 0xE000) {
        if (addr & 1) {
            // MMC3 $C001 sets reload flag; counter reload occurs on next qualified A12 rise.
            mmc3IrqReload = true;
            mmc3IrqCounter = 0;
        } else {
            mmc3IrqLatch = val;
        }
    } else {
        if (addr & 1) {
            mmc3IrqEnabled = true;
        } else {
            mmc3IrqEnabled = false;
            mmc3IrqPending = false;
        }
    }
}

void Cartridge::cpuWriteMapper119(uint16_t addr, uint8_t val) {
    cpuWriteMapper4(addr, val);
}

void Cartridge::cpuWriteMapper206(uint16_t addr, uint8_t val) {
    if (addr < 0x8000 || addr >= 0xA000) {
        return;
    }

    if ((addr & 1) == 0) {
        mmc3BankSelect = val & 0x07;
        if (mapper == 154) {
            oneScreenMirror = true;
            oneScreenUpper = (val & 0x40) != 0;
            updateNtPtrs();
        }
        return;
    }

    const uint8_t reg = mmc3BankSelect & 0x07;
    if (reg <= 5) {
        mmc3Banks[reg] = val & 0x3F;
    } else {
        mmc3Banks[reg] = val & 0x0F;
    }

    updateBankCache();
}

void Cartridge::cpuWriteMapper23(uint16_t addr, uint8_t val) {
    const bool vrc2 = isVrc2Mode();
    uint16_t a = (uint16_t)((addr & 0xF000) |
                            ((((addr & vrcReg2Mask) != 0) ? 1 : 0) << 1) |
                            (((addr & vrcReg1Mask) != 0) ? 1 : 0));
    if (a >= 0xB000 && a <= 0xE003) {
        const uint8_t index = (uint8_t)(((a >> 1) & 0x01) | ((a - 0xB000) >> 11));
        const bool highNibble = (a & 0x01) != 0;
        if (!highNibble) {
            vrcChrBanks[index] = (uint8_t)((vrcChrBanks[index] & 0xF0) | (val & 0x0F));
        } else {
            vrcChrBanks[index] = (uint8_t)((vrcChrBanks[index] & 0x0F) | ((val & 0x0F) << 4));
            if (!vrc2) {
                vrcChrHighBits[index] = (val & 0x10) ? 1 : 0;
            }
        }
        updateChrBankCache();
        return;
    }

    switch (a & 0xF003) {
        case 0x8000:
        case 0x8001:
        case 0x8002:
        case 0x8003:
            vrcPrgBank8000 = val & 0x1F;
            updateBankCache();
            break;
        case 0xA000:
        case 0xA001:
        case 0xA002:
        case 0xA003:
            vrcPrgBankA000 = val & 0x1F;
            updateBankCache();
            break;
        case 0x9000:
        case 0x9001:
        case 0x9002:
        case 0x9003:
            if (vrc2) {
                // VRC2 mirroring uses only bit 0; bit 1 is ignored.
                oneScreenMirror = false;
                mirrorVertical = (val & 0x01) == 0;
                updateNtPtrs();
            } else if ((a & 0xF003) == 0x9000) {
                // Compatibility quirk used by Wai Wai World on legacy mapper 23 dumps:
                // ignore $FF here to avoid invalid one-screen mirroring selection.
                if (val != 0xFF) {
                    switch (val & 0x03) {
                        case 0:
                            oneScreenMirror = false;
                            mirrorVertical = true;
                            break;
                        case 1:
                            oneScreenMirror = false;
                            mirrorVertical = false;
                            break;
                        case 2:
                            oneScreenMirror = true;
                            oneScreenUpper = false;
                            break;
                        default:
                            oneScreenMirror = true;
                            oneScreenUpper = true;
                            break;
                    }
                    updateNtPtrs();
                }
            } else if ((a & 0xF003) == 0x9002) {
                // VRC4 only: WRAM control (bit0) + PRG swap mode (bit1).
                vrcRegCmd = val;
                updateBankCache();
            } else {
                // $9001 is unused and $9003 is external select on VRC4.
            }
            break;
        case 0xF000:
            if (vrc2) {
                break;
            }
            mmc3IrqPending = false;
            vrcIrqLatch = (uint8_t)((vrcIrqLatch & 0xF0) | (val & 0x0F));
            break;
        case 0xF001:
            if (vrc2) {
                break;
            }
            mmc3IrqPending = false;
            vrcIrqLatch = (uint8_t)((vrcIrqLatch & 0x0F) | ((val & 0x0F) << 4));
            break;
        case 0xF002:
            if (vrc2) {
                break;
            }
            mmc3IrqPending = false;
            vrcIrqPrescaler = 341;
            vrcIrqModeCycle = (val & 0x04) != 0;
            vrcIrqEnable = (val & 0x02) != 0;
            vrcIrqEnableAfterAck = (val & 0x01) != 0;
            if (vrcIrqEnable) {
                vrcIrqCounter = vrcIrqLatch;
            }
            break;
        case 0xF003:
            if (vrc2) {
                break;
            }
            mmc3IrqPending = false;
            vrcIrqEnable = vrcIrqEnableAfterAck;
            break;
        default:
            break;
    }
}

void Cartridge::cpuWriteMapper25(uint16_t addr, uint8_t val) {
    cpuWriteMapper23(addr, val);
}

void Cartridge::cpuWriteMapper18(uint16_t addr, uint8_t val) {
    if ((addr & 0xF003) == 0x9002) {
        mapper18RamEnable = (val & 0x01) != 0;
        mapper18RamWriteEnable = (val & 0x02) != 0;
        return;
    }

    if (addr >= 0x8000 && addr < 0xA000) {
        const uint8_t index = (uint8_t)(((addr >> 1) & 0x01) | ((addr - 0x8000) >> 11));
        if (index < 3) {
            const bool highNibble = (addr & 0x01) != 0;
            if (!highNibble) {
                mapper18PrgRegs[index] = (uint8_t)((mapper18PrgRegs[index] & 0xF0) | (val & 0x0F));
            } else {
                mapper18PrgRegs[index] = (uint8_t)((mapper18PrgRegs[index] & 0x0F) | ((val & 0x0F) << 4));
            }
            updateBankCache();
        }
        return;
    }

    if (addr >= 0xA000 && addr < 0xE000) {
        const uint8_t index = (uint8_t)(((addr >> 1) & 0x01) | ((addr - 0xA000) >> 11));
        if (index < 8) {
            const bool highNibble = (addr & 0x01) != 0;
            if (!highNibble) {
                mapper18ChrRegs[index] = (uint8_t)((mapper18ChrRegs[index] & 0xF0) | (val & 0x0F));
            } else {
                mapper18ChrRegs[index] = (uint8_t)((mapper18ChrRegs[index] & 0x0F) | ((val & 0x0F) << 4));
            }
            updateChrBankCache();
        }
        return;
    }

    switch (addr & 0xF003) {
        case 0xE000:
            mapper18IrqLatch = (uint16_t)((mapper18IrqLatch & 0xFFF0) | ((uint16_t)val & 0x000F));
            break;
        case 0xE001:
            mapper18IrqLatch = (uint16_t)((mapper18IrqLatch & 0xFF0F) | (((uint16_t)val & 0x000F) << 4));
            break;
        case 0xE002:
            mapper18IrqLatch = (uint16_t)((mapper18IrqLatch & 0xF0FF) | (((uint16_t)val & 0x000F) << 8));
            break;
        case 0xE003:
            mapper18IrqLatch = (uint16_t)((mapper18IrqLatch & 0x0FFF) | (((uint16_t)val & 0x000F) << 12));
            break;
        case 0xF000:
            mapper18IrqCounter = mapper18IrqLatch;
            mmc3IrqPending = false;
            break;
        case 0xF001:
            mapper18IrqEnable = (val & 0x01) != 0;
            if (val & 0x08) {
                mapper18IrqSizeMask = 0x000F;
            } else if (val & 0x04) {
                mapper18IrqSizeMask = 0x00FF;
            } else if (val & 0x02) {
                mapper18IrqSizeMask = 0x0FFF;
            } else {
                mapper18IrqSizeMask = 0xFFFF;
            }
            mmc3IrqPending = false;
            break;
        case 0xF002:
            switch (val & 0x03) {
                case 0:
                    oneScreenMirror = false;
                    mirrorVertical = false;
                    break;
                case 1:
                    oneScreenMirror = false;
                    mirrorVertical = true;
                    break;
                case 2:
                    oneScreenMirror = true;
                    oneScreenUpper = false;
                    break;
                default:
                    oneScreenMirror = true;
                    oneScreenUpper = true;
                    break;
            }
            updateNtPtrs();
            break;
        default:
            break;
    }
}

void Cartridge::cpuWriteMapper19(uint16_t addr, uint8_t val) {
    const uint16_t a = addr & 0xF800;

    if (a == 0x4800) {
        mapper19InternalRam[mapper19AddrPort & 0x7F] = val;
        if (mapper19AddrPort & 0x80) {
            mapper19AddrPort = (uint8_t)((mapper19AddrPort & 0x80) | ((mapper19AddrPort + 1) & 0x7F));
        }
        return;
    }
    if (a == 0x5000) {
        mapper19IrqCounter = (uint16_t)((mapper19IrqCounter & 0x7F00) | val);
        mmc3IrqPending = false;
        return;
    }
    if (a == 0x5800) {
        mapper19IrqCounter = (uint16_t)((mapper19IrqCounter & 0x00FF) | (((uint16_t)val & 0x7F) << 8));
        mapper19IrqEnable = (val & 0x80) != 0;
        mmc3IrqPending = false;
        return;
    }
    if (a == 0xF800) {
        mapper19AddrPort = val;
        mapper19WramProtect = val;
        return;
    }

    if (a >= 0x8000 && a <= 0xB800) {
        const uint8_t index = (uint8_t)((a - 0x8000) >> 11);
        if (index < 8) {
            mapper19ChrRegs[index] = val;
            updateChrBankCache();
        }
        return;
    }

    if (a >= 0xC000 && a <= 0xD800) {
        const uint8_t index = (uint8_t)((a - 0xC000) >> 11);
        if (index < 4) {
            mapper19NtRegs[index] = val;
            ppuRenderingDirty = true;
        }
        return;
    }

    switch (a) {
        case 0xE000:
            mapper19PrgRegs[0] = val & 0x3F;
            updateBankCache();
            break;
        case 0xE800:
            mapper19PrgRegs[1] = val & 0x3F;
            mapper19ChrRamCtl = (uint8_t)((val >> 6) & 0x03);
            updateBankCache();
            break;
        case 0xF000:
            mapper19RegF000 = val;
            mapper19PrgRegs[2] = val & 0x3F;
            updateBankCache();
            break;
        default:
            break;
    }
}

void Cartridge::cpuWriteMapper24(uint16_t addr, uint8_t val) {
    uint16_t a = addr & 0xF003;
    switch (a & 0xF000) {
        case 0x8000:
            vrc6Prg16Bank = val & 0x0F;
            updateBankCache();
            break;
        case 0xB000:
            if (a == 0xB003) {
                vrc6PpuCtrl = val;
                const uint8_t m = val & 0x0C;
                if (m == 0x00) {
                    oneScreenMirror = false;
                    mirrorVertical = true;
                } else if (m == 0x04) {
                    oneScreenMirror = false;
                    mirrorVertical = false;
                } else if (m == 0x08) {
                    oneScreenMirror = true;
                    oneScreenUpper = false;
                } else {
                    oneScreenMirror = true;
                    oneScreenUpper = true;
                }
                updateNtPtrs();
            }
            break;
        case 0xC000:
            vrcPrgBankC000 = val & 0x1F;
            updateBankCache();
            break;
        case 0xD000:
            vrcChrBanks[a & 0x0003] = val;
            updateChrBankCache();
            break;
        case 0xE000:
            vrcChrBanks[4 + (a & 0x0003)] = val;
            updateChrBankCache();
            break;
        case 0xF000:
            if ((a & 0x0003) == 0) {
                vrcIrqLatch = val;
            } else if ((a & 0x0003) == 1) {
                vrcIrqModeCycle = (val & 0x04) != 0;
                vrcIrqEnable = (val & 0x02) != 0;
                vrcIrqEnableAfterAck = (val & 0x01) != 0;
                mmc3IrqPending = false;
                vrcIrqPrescaler = 341;
                if (vrcIrqEnable) {
                    vrcIrqCounter = vrcIrqLatch;
                }
            } else if ((a & 0x0003) == 2) {
                mmc3IrqPending = false;
                vrcIrqEnable = vrcIrqEnableAfterAck;
            }
            break;
        default:
            break;
    }
}

void Cartridge::cpuWriteMapper26(uint16_t addr, uint8_t val) {
    // Mapper 26 swaps A0 and A1 compared to mapper 24.
    uint16_t a = (addr & ~0x0003) | ((addr & 0x0001) << 1) | ((addr & 0x0002) >> 1);
    cpuWriteMapper24(a, val);
}

void Cartridge::cpuWriteMapper85(uint16_t addr, uint8_t val) {
    uint16_t a = addr & 0xF018;
    switch (a) {
        case 0x8000:
            vrcPrgBank8000 = val & 0x3F;
            updateBankCache();
            break;
        case 0x8008:
        case 0x8010:
            vrcPrgBankA000 = val & 0x3F;
            updateBankCache();
            break;
        case 0x9000:
            vrcPrgBankC000 = val & 0x3F;
            updateBankCache();
            break;
        case 0xA000:
            vrcChrBanks[0] = val;
            updateChrBankCache();
            break;
        case 0xA008:
        case 0xA010:
            vrcChrBanks[1] = val;
            updateChrBankCache();
            break;
        case 0xB000:
            vrcChrBanks[2] = val;
            updateChrBankCache();
            break;
        case 0xB008:
        case 0xB010:
            vrcChrBanks[3] = val;
            updateChrBankCache();
            break;
        case 0xC000:
            vrcChrBanks[4] = val;
            updateChrBankCache();
            break;
        case 0xC008:
        case 0xC010:
            vrcChrBanks[5] = val;
            updateChrBankCache();
            break;
        case 0xD000:
            vrcChrBanks[6] = val;
            updateChrBankCache();
            break;
        case 0xD008:
        case 0xD010:
            vrcChrBanks[7] = val;
            updateChrBankCache();
            break;
        case 0xE000: {
            switch (val & 0x03) {
                case 0:
                    oneScreenMirror = false;
                    mirrorVertical = true;
                    break;
                case 1:
                    oneScreenMirror = false;
                    mirrorVertical = false;
                    break;
                case 2:
                    oneScreenMirror = true;
                    oneScreenUpper = false;
                    break;
                default:
                    oneScreenMirror = true;
                    oneScreenUpper = true;
                    break;
            }
            updateNtPtrs();
            break;
        }
        case 0xE008:
        case 0xE010:
            vrcIrqLatch = val;
            break;
        case 0xF000:
            vrcIrqModeCycle = (val & 0x04) != 0;
            vrcIrqEnable = (val & 0x02) != 0;
            vrcIrqEnableAfterAck = (val & 0x01) != 0;
            mmc3IrqPending = false;
            vrcIrqPrescaler = 341;
            if (vrcIrqEnable) {
                vrcIrqCounter = vrcIrqLatch;
            }
            break;
        case 0xF008:
        case 0xF010:
            mmc3IrqPending = false;
            vrcIrqEnable = vrcIrqEnableAfterAck;
            break;
        default:
            break;
    }
}

void Cartridge::cpuWriteMapper94(uint16_t addr, uint8_t val) {
    prgBankSelect = (val >> 2) & 0x07;
    updateBankCache();
}

void Cartridge::cpuWriteMapper163(uint16_t addr, uint8_t val) {
    if (addr < 0x5000 || addr >= 0x6000) {
        return;
    }

    uint8_t data = val;
    if (m163Mode5300 & 0x01) {
        data = (data & 0xFC) | ((data & 0x01) << 1) | ((data & 0x02) >> 1);
    }

    switch (addr & 0xFF00) {
        case 0x5000:
            m163Reg5000 = data;
            updateMapper163Prg();
            break;
        case 0x5100:
            if (m163FeedbackSetMode) {
                m163FeedbackBit = data & 0x01;
            } else {
                m163FeedbackBit ^= (data & 0x01);
            }
            break;
        case 0x5200:
            m163Reg5200 = data;
            updateMapper163Prg();
            break;
        case 0x5300:
            m163Mode5300 = data & 0x07;
            updateMapper163Prg();
            break;
        default:
            if (addr == 0x5101) {
                m163FeedbackSetMode = (data & 0x01) != 0;
            }
            break;
    }
}

void Cartridge::updateNtPtrs() {
    uint8_t* oldNtPtrs[4] = {ntPtrs[0], ntPtrs[1], ntPtrs[2], ntPtrs[3]};

    if (!vram) return;
    if (mapper == 118) {
        updateMapper118Nametables();
        return;
    }
    if (mapper == 95) {
        updateMapper95Nametables();
        return;
    }
    if (oneScreenMirror) {
        uint8_t* base = vram + (oneScreenUpper ? 0x400 : 0x000);
        ntPtrs[0] = base;
        ntPtrs[1] = base;
        ntPtrs[2] = base;
        ntPtrs[3] = base;
    } else if (mirrorVertical) {
        
        ntPtrs[0] = vram;
        ntPtrs[1] = vram + 0x400;
        ntPtrs[2] = vram;
        ntPtrs[3] = vram + 0x400;
    } else {
        
        ntPtrs[0] = vram;
        ntPtrs[1] = vram;
        ntPtrs[2] = vram + 0x400;
        ntPtrs[3] = vram + 0x400;
    }

    for (int i = 0; i < 4; ++i)
    {
        if (ntPtrs[i] != oldNtPtrs[i])
        {
            ppuRenderingDirty = true;
            break;
        }
    }
}

uint8_t IRAM_ATTR Cartridge::ppuRead(uint16_t addr) {
    if (addr >= 0x2000) return 0;

    if (mapper == 5) {
        return readMmc5Chr(addr, false);
    }

    if (mapper == 163) {
        uint16_t chrAddr = addr & 0x1FFF;
        if (m163Reg5000 & 0x80) {
            chrAddr = (addr & 0x0FFF) | ((uint16_t)(m163LatchedA9 & 0x01) << 12);
        }
        return chrRam[chrAddr & 0x1FFF];
    }
    uint8_t val = chrBankPtrs[(addr >> 10) & 7][addr & 0x3FF];

    if (mapper == 9 || mapper == 10) {
        bool needUpdate = false;

        if (mapper == 9) {
            if (addr == 0x0FD8) {
                if (mmc2Latch0FE) {
                    mmc2Latch0FE = false;
                    needUpdate = true;
                }
            } else if (addr == 0x0FE8) {
                if (!mmc2Latch0FE) {
                    mmc2Latch0FE = true;
                    needUpdate = true;
                }
            } else if (addr >= 0x1FD8 && addr <= 0x1FDF) {
                if (mmc2Latch1FE) {
                    mmc2Latch1FE = false;
                    needUpdate = true;
                }
            } else if (addr >= 0x1FE8 && addr <= 0x1FEF) {
                if (!mmc2Latch1FE) {
                    mmc2Latch1FE = true;
                    needUpdate = true;
                }
            }
        } else {
            if (addr >= 0x0FD8 && addr <= 0x0FDF) {
                if (mmc2Latch0FE) {
                    mmc2Latch0FE = false;
                    needUpdate = true;
                }
            } else if (addr >= 0x0FE8 && addr <= 0x0FEF) {
                if (!mmc2Latch0FE) {
                    mmc2Latch0FE = true;
                    needUpdate = true;
                }
            } else if (addr >= 0x1FD8 && addr <= 0x1FDF) {
                if (mmc2Latch1FE) {
                    mmc2Latch1FE = false;
                    needUpdate = true;
                }
            } else if (addr >= 0x1FE8 && addr <= 0x1FEF) {
                if (!mmc2Latch1FE) {
                    mmc2Latch1FE = true;
                    needUpdate = true;
                }
            }
        }

        if (needUpdate) {
            updateChrBankCache();
        }
    }

    return val;
}

uint8_t IRAM_ATTR Cartridge::readNameTable(uint16_t addr) {
    if (mapper == 5) {
        return readMmc5NameTable(addr);
    }
    if (mapper == 19) {
        return readMapper19NameTable(addr);
    }
    
    uint8_t* p = ntPtrs[(addr >> 10) & 3];
    if (__builtin_expect(p != nullptr, 1)) {
        return p[addr & 0x3FF];
    }
    return 0;
}

void IRAM_ATTR Cartridge::writeNameTable(uint16_t addr, uint8_t val) {
    if (mapper == 5) {
        writeMmc5NameTable(addr, val);
        return;
    }
    if (mapper == 19) {
        writeMapper19NameTable(addr, val);
        return;
    }
    uint8_t* p = ntPtrs[(addr >> 10) & 3];
    if (__builtin_expect(p != nullptr, 1)) {
        p[addr & 0x3FF] = val;
    }
}

uint8_t IRAM_ATTR Cartridge::ppuReadMapper1(uint16_t addr) {
    
    return chrBankPtrs[(addr >> 10) & 7][addr & 0x3FF];
}

uint8_t IRAM_ATTR Cartridge::ppuReadMapper3(uint16_t addr) {
    
    return chrBankPtrs[(addr >> 10) & 7][addr & 0x3FF];
}

uint8_t IRAM_ATTR Cartridge::ppuReadMapper4(uint16_t addr) {
    
    
    return chrBankPtrs[(addr >> 10) & 7][addr & 0x3FF];
}

void Cartridge::ppuWrite(uint16_t addr, uint8_t val) {
    if (addr >= 0x2000) return;
    if (mapper == 5) {
        if (chrBanks == 0) {
            chrRam[addr & 0x1FFF] = val;
            updateChrBankCache();
        }
        return;
    }
    if (mapper == 163) {
        uint16_t chrAddr = addr & 0x1FFF;
        if (m163Reg5000 & 0x80) {
            chrAddr = (addr & 0x0FFF) | ((uint16_t)(m163LatchedA9 & 0x01) << 12);
        }
        chrRam[chrAddr & 0x1FFF] = val;
        return;
    }
    
    switch (mapper) {
        case 1:
            ppuWriteMapper1(addr, val);
            break;
        case 4:
            ppuWriteMapper4(addr, val);
            break;
        case 19: {
            uint8_t* bankPtr = chrBankPtrs[(addr >> 10) & 7];
            if (bankPtr >= chrRam && bankPtr < (chrRam + sizeof(chrRam))) {
                bankPtr[addr & 0x3FF] = val;
            } else if (vram && bankPtr >= vram && bankPtr < (vram + 0x800)) {
                bankPtr[addr & 0x3FF] = val;
            }
            break;
        }
        case 119:
            ppuWriteMapper119(addr, val);
            break;
        default:
            
            if (chrBanks == 0) {
                chrRam[addr & 0x1FFF] = val;
            }
            break;
    }
}

void Cartridge::ppuWriteMapper1(uint16_t addr, uint8_t val) {
    if (chrBanks == 0) {
        chrRam[addr & 0x1FFF] = val;
    }
}

void Cartridge::ppuWriteMapper4(uint16_t addr, uint8_t val) {
    if (chrBanks == 0) {
        chrRam[addr & 0x1FFF] = val;
    }
}

uint8_t IRAM_ATTR Cartridge::readMapper19NameTable(uint16_t addr) {
    if (!vram) {
        return 0;
    }
    addr &= 0x0FFF;
    const uint8_t nt = (addr >> 10) & 0x03;
    const uint16_t off = addr & 0x03FF;
    const uint8_t reg = mapper19NtRegs[nt];

    if (reg >= 0xE0) {
        const uint16_t base = (reg & 0x01) ? 0x400 : 0x000;
        return vram[(base + off) & 0x07FF];
    }

    if (chrBanks == 0 || !chr) {
        const uint32_t idx = ((uint32_t)reg << 10) | off;
        return chrRam[idx & 0x1FFF];
    }

    const uint32_t chr1kCount = (chrSize >= 0x400) ? (chrSize / 0x400) : 1;
    uint32_t bank = reg;
    if (chr1kCount > 0x100) {
        const bool d = (mapper19RegF000 & 0x80) != 0;
        bank |= (uint32_t)(d ? 1 : 0) << 8;
    }
    if (chr1kCount > 0) {
        bank %= chr1kCount;
    } else {
        bank = 0;
    }
    return chr[bank * 0x400 + off];
}

void Cartridge::writeMapper19NameTable(uint16_t addr, uint8_t val) {
    if (!vram) {
        return;
    }
    addr &= 0x0FFF;
    const uint8_t nt = (addr >> 10) & 0x03;
    const uint16_t off = addr & 0x03FF;
    const uint8_t reg = mapper19NtRegs[nt];

    if (reg >= 0xE0) {
        const uint16_t base = (reg & 0x01) ? 0x400 : 0x000;
        vram[(base + off) & 0x07FF] = val;
        return;
    }

    if (chrBanks == 0 || !chr) {
        const uint32_t idx = ((uint32_t)reg << 10) | off;
        chrRam[idx & 0x1FFF] = val;
    }
}

uint8_t IRAM_ATTR Cartridge::readMmc5NameTable(uint16_t addr) {
    addr &= 0x0FFF;
    uint8_t nt = (addr >> 10) & 0x03;
    uint8_t mode = (mmc5NtMap >> (nt * 2)) & 0x03;
    uint16_t off = addr & 0x03FF;

    if (mode == 0) {
        return vram[off];
    }
    if (mode == 1) {
        return vram[0x400 + off];
    }
    if (mode == 2) {
        return mmc5ExRam[off];
    }

    if (off < 0x3C0) {
        return mmc5FillTile;
    }
    uint8_t p = mmc5FillAttr & 0x03;
    return (uint8_t)(p | (p << 2) | (p << 4) | (p << 6));
}

void Cartridge::writeMmc5NameTable(uint16_t addr, uint8_t val) {
    addr &= 0x0FFF;
    uint8_t nt = (addr >> 10) & 0x03;
    uint8_t mode = (mmc5NtMap >> (nt * 2)) & 0x03;
    uint16_t off = addr & 0x03FF;

    if (mode == 0) {
        vram[off] = val;
    } else if (mode == 1) {
        vram[0x400 + off] = val;
    } else if (mode == 2) {
        if (mmc5ExRamMode != 3) {
            mmc5ExRam[off] = val;
        }
    }
}

void Cartridge::ppuWriteMapper119(uint16_t addr, uint8_t val) {
    uint8_t* bankPtr = chrBankPtrs[(addr >> 10) & 7];
    if (bankPtr >= chrRam && bankPtr < (chrRam + sizeof(chrRam))) {
        bankPtr[addr & 0x3FF] = val;
    } else if (chrBanks == 0) {
        chrRam[addr & 0x1FFF] = val;
    }
}

void IRAM_ATTR Cartridge::clockIrqCounter() {
    if (mapper != 4 && mapper != 118 && mapper != 119) return;
    
    if (mmc3IrqCounter == 0 || mmc3IrqReload) {
        mmc3IrqCounter = mmc3IrqLatch;
        mmc3IrqReload = false;
    } else {
        mmc3IrqCounter--;
    }
    
    if (mmc3IrqCounter == 0 && mmc3IrqEnabled) {
        mmc3IrqPending = true;
    }
}

void IRAM_ATTR Cartridge::ppuScanline() {
    if (mapper != 4 && mapper != 118 && mapper != 119) return;  
    
    
    if (mmc3IrqCounter == 0) {
        mmc3IrqCounter = mmc3IrqLatch;
    } else {
        mmc3IrqCounter--;
        if (mmc3IrqCounter == 0 && mmc3IrqEnabled) {
            mmc3IrqPending = true;
        }
    }
}

void IRAM_ATTR Cartridge::observePpuAddress(uint16_t ppuAddr) {
    if (!needsPpuAddressObserve()) {
        return;
    }

    if (mapper == 163) {
        const bool a13 = (ppuAddr & 0x2000) != 0;
        if (!m163PrevA13 && a13) {
            m163LatchedA9 = (ppuAddr >> 9) & 0x01;
        }
        m163PrevA13 = a13;
    }

    if (mapper == 9 || mapper == 10) {
        bool needUpdate = false;
        if (mapper == 9) {
            if (ppuAddr == 0x0FD8) {
                if (mmc2Latch0FE) {
                    mmc2Latch0FE = false;
                    needUpdate = true;
                }
            } else if (ppuAddr == 0x0FE8) {
                if (!mmc2Latch0FE) {
                    mmc2Latch0FE = true;
                    needUpdate = true;
                }
            } else if (ppuAddr >= 0x1FD8 && ppuAddr <= 0x1FDF) {
                if (mmc2Latch1FE) {
                    mmc2Latch1FE = false;
                    needUpdate = true;
                }
            } else if (ppuAddr >= 0x1FE8 && ppuAddr <= 0x1FEF) {
                if (!mmc2Latch1FE) {
                    mmc2Latch1FE = true;
                    needUpdate = true;
                }
            }
        } else {
            if (ppuAddr >= 0x0FD8 && ppuAddr <= 0x0FDF) {
                if (mmc2Latch0FE) {
                    mmc2Latch0FE = false;
                    needUpdate = true;
                }
            } else if (ppuAddr >= 0x0FE8 && ppuAddr <= 0x0FEF) {
                if (!mmc2Latch0FE) {
                    mmc2Latch0FE = true;
                    needUpdate = true;
                }
            } else if (ppuAddr >= 0x1FD8 && ppuAddr <= 0x1FDF) {
                if (mmc2Latch1FE) {
                    mmc2Latch1FE = false;
                    needUpdate = true;
                }
            } else if (ppuAddr >= 0x1FE8 && ppuAddr <= 0x1FEF) {
                if (!mmc2Latch1FE) {
                    mmc2Latch1FE = true;
                    needUpdate = true;
                }
            }
        }
        if (needUpdate) {
            updateChrBankCache();
        }
    }

    if (mapper == 4 || mapper == 118 || mapper == 119) {
        uint8_t lowTick = 2;
        if (nes) {
            const uint64_t nowCpu = nes->cpu.getTotalCycles();
            if (mmc3LastObserveCpuCycleValid) {
                const uint64_t dCpu = nowCpu - mmc3LastObserveCpuCycle;
                if (dCpu > 0) {
                    const uint64_t ppuTicks = dCpu * 3ULL;
                    lowTick = (uint8_t)((ppuTicks > 255ULL) ? 255ULL : ppuTicks);
                }
            }
            mmc3LastObserveCpuCycle = nowCpu;
            mmc3LastObserveCpuCycleValid = true;
        }

        const bool a12 = (ppuAddr & 0x1000) != 0;
        if (!a12) {
            const uint16_t sum = (uint16_t)mmc3A12LowCycles + (uint16_t)lowTick;
            if (sum > 255) {
                mmc3A12LowCycles = 255;
            } else {
                mmc3A12LowCycles = (uint8_t)sum;
            }
            mmc3PrevA12 = false;
            return;
        }

        // MMC3 clocks on A12 rise after it has been low long enough
        // (approximately 3 CPU cycles / 9 PPU clocks).
        if (!mmc3PrevA12 && mmc3A12LowCycles >= 9) {
            clockIrqCounter();
            mmc3ClockedThisScanline = true;
        }
        mmc3PrevA12 = true;
        mmc3A12LowCycles = 0;
    }

    if (mapper == 64 && !mapper64IrqCycleMode) {
        const bool a12 = (ppuAddr & 0x1000) != 0;
        if (!a12) {
            if (mapper64A12LowCycles < 255) {
                mapper64A12LowCycles++;
            }
            mapper64PrevA12 = false;
            return;
        }

        if (!mapper64PrevA12 && mapper64A12LowCycles >= 3) {
            if (mapper64IrqReload || mapper64IrqCounter == 0) {
                mapper64IrqCounter = mapper64IrqLatch;
                mapper64IrqReload = false;
            } else {
                mapper64IrqCounter--;
            }
            if (mapper64IrqCounter == 0 && mapper64IrqEnable) {
                mmc3IrqPending = true;
            }
        }

        mapper64PrevA12 = true;
        mapper64A12LowCycles = 0;
    }
}

void Cartridge::finalizeMmc3Scanline(bool renderingEnabled, uint8_t ppuCtrl) {
    if (!(mapper == 4 || mapper == 118 || mapper == 119)) {
        return;
    }

    if (!renderingEnabled) {
        mmc3ClockedThisScanline = false;
        return;
    }

    // The software line renderer can miss sprite-fetch-side A12 rises on lines
    // with few/zero visible sprites. Inject one fallback scanline clock in the
    // common configurations where hardware still yields one rise per line.
    const bool sprite8x8 = (ppuCtrl & 0x20) == 0;
    const bool sprite8x16 = (ppuCtrl & 0x20) != 0;
    const bool bgAt0000 = (ppuCtrl & 0x10) == 0;
    const bool spAt1000 = (ppuCtrl & 0x08) != 0;
    const bool fallback8x8 = sprite8x8 && bgAt0000 && spAt1000;
    // In 8x16 mode, $2000.3 is ignored for sprite fetch and scanlines with
    // fewer than 8 sprites still fetch dummy tile $FF from $1FE0-$1FFF.
    const bool fallback8x16 = sprite8x16 && bgAt0000;
    if (!mmc3ClockedThisScanline && (fallback8x8 || fallback8x16)) {
        clockIrqCounter();
    }
    mmc3ClockedThisScanline = false;
}

void IRAM_ATTR Cartridge::clockCpuCycles(uint32_t cpuCycles) {
    if (mapper == 64 && mapper64IrqEnable && mapper64IrqCycleMode) {
        while (cpuCycles > 0) {
            cpuCycles--;
            mapper64IrqPrescaler++;
            if (mapper64IrqPrescaler < 4) {
                continue;
            }
            mapper64IrqPrescaler = 0;
            if (mapper64IrqReload || mapper64IrqCounter == 0) {
                mapper64IrqCounter = mapper64IrqLatch;
                mapper64IrqReload = false;
            } else {
                mapper64IrqCounter--;
            }
            if (mapper64IrqCounter == 0) {
                mmc3IrqPending = true;
            }
        }
        return;
    }

    if (isVrc4Mode() && !isVrc2Mode() && vrcIrqEnable) {
        clockVrcIrq(cpuCycles);
    }

    if (mapper == 18 && mapper18IrqEnable) {
        uint32_t remaining = cpuCycles;
        while (remaining > 0) {
            const uint16_t masked = (uint16_t)(mapper18IrqCounter & mapper18IrqSizeMask);
            if (masked == 0) {
                mapper18IrqCounter = (uint16_t)((mapper18IrqCounter & ~mapper18IrqSizeMask) | mapper18IrqSizeMask);
                mmc3IrqPending = true;
                mapper18IrqEnable = false;
                break;
            }
            mapper18IrqCounter--;
            remaining--;
        }
    }

    if (mapper == 19 && mapper19IrqEnable) {
        uint32_t remaining = cpuCycles;
        while (remaining > 0) {
            if (mapper19IrqCounter >= 0x7FFF) {
                mapper19IrqCounter = 0x7FFF;
                mapper19IrqEnable = false;
                mmc3IrqPending = true;
                break;
            }
            mapper19IrqCounter++;
            remaining--;
        }
    }

    if (mapper == 69 && fme7IrqCounterEnable) {
        uint32_t remaining = cpuCycles;
        while (remaining > 0) {
            if (fme7IrqCounter == 0) {
                fme7IrqCounter = 0xFFFF;
                if (fme7IrqEnable) {
                    mmc3IrqPending = true;
                }
            } else {
                fme7IrqCounter--;
            }
            remaining--;
        }
    }
}

void Cartridge::clockVrcIrq(uint32_t cpuCycles) {
    while (cpuCycles > 0) {
        bool tick = false;
        if (vrcIrqModeCycle) {
            tick = true;
        } else {
            vrcIrqPrescaler -= 3;
            if (vrcIrqPrescaler <= 0) {
                vrcIrqPrescaler += 341;
                tick = true;
            }
        }

        if (tick) {
            if (vrcIrqCounter == 0xFF) {
                vrcIrqCounter = vrcIrqLatch;
                mmc3IrqPending = true;
            } else {
                vrcIrqCounter++;
            }
        }

        cpuCycles--;
    }
}

uint8_t IRAM_ATTR Cartridge::readMmc5Chr(uint16_t addr, bool spriteFetch) const {
    if (mapper != 5) {
        if (chrBanks == 0) {
            return chrRam[addr & 0x1FFF];
        }
        return chrBankPtrs[(addr >> 10) & 7][addr & 0x3FF];
    }
    uint8_t* const* banks = spriteFetch ? mmc5SpChrPtrs : mmc5BgChrPtrs;
    uint8_t* base = banks[(addr >> 10) & 7];
    if (!base) {
        return 0;
    }
    return base[addr & 0x3FF];
}

void Cartridge::notifyScanlineStart(int scanline, bool renderingEnabled) {
    if (mapper == 4 || mapper == 118 || mapper == 119) {
        (void)scanline;
        mmc3ClockedThisScanline = false;
    }
    if (mapper == 48) {
        if (renderingEnabled && scanline >= 0 && scanline < 240 && mapper48IrqEnable) {
            mapper48IrqCounter++;
            if (mapper48IrqCounter == 0) {
                mmc3IrqPending = true;
                mapper48IrqEnable = false;
            }
        }
    }
    if (mapper != 5) return;

    if (!renderingEnabled || scanline < 0 || scanline >= 240) {
        if (scanline <= 0) {
            mmc5ScanlineCounter = 0;
            mmc5InFrame = false;
        }
        return;
    }

    if (!mmc5InFrame) {
        mmc5InFrame = true;
        mmc5ScanlineCounter = 0;
    }

    if (mmc5ScanlineCounter == mmc5IrqTargetScanline && mmc5IrqEnable) {
        mmc3IrqPending = true;
    }

    if (mmc5ScanlineCounter < 255) {
        mmc5ScanlineCounter++;
    }

    if (scanline >= 239) {
        mmc5InFrame = false;
    }
}

size_t Cartridge::getStateSize() const {
    size_t size = 0;
    size += sizeof(mapper);
    size += sizeof(submapper);
    size += sizeof(mirrorVertical);
    size += sizeof(oneScreenMirror);
    size += sizeof(oneScreenUpper);
    size += sizeof(prgBankSelect);
    size += sizeof(mapper7BusConflictMode);
    size += sizeof(mapper71MirroringMode);
    
    
    size += sizeof(mmc1ShiftReg);
    size += sizeof(mmc1WriteCount);
    size += sizeof(mmc1Control);
    size += sizeof(mmc1ChrBank0);
    size += sizeof(mmc1ChrBank1);
    size += sizeof(mmc1PrgBank);
    
    
    size += sizeof(cnromChrBank);
    size += sizeof(mapper34ChrBank0);
    size += sizeof(mapper34ChrBank1);
    size += sizeof(mapper34NinaMode);
    size += sizeof(colorDreamsPrgBank);
    size += sizeof(colorDreamsChrBank);
    size += sizeof(gxromPrgBank);
    size += sizeof(gxromChrBank);
    size += sizeof(mmc2PrgBank);
    size += sizeof(mmc2ChrFd0);
    size += sizeof(mmc2ChrFe0);
    size += sizeof(mmc2ChrFd1);
    size += sizeof(mmc2ChrFe1);
    size += sizeof(mmc2Latch0FE);
    size += sizeof(mmc2Latch1FE);
    size += sizeof(fme7Command);
    size += sizeof(fme7ChrBanks);
    size += sizeof(fme7PrgBanks);
    size += sizeof(fme7RamEnable);
    size += sizeof(fme7RamSelect);
    size += sizeof(fme7RamWriteEnable);
    size += sizeof(fme7IrqEnable);
    size += sizeof(fme7IrqCounterEnable);
    size += sizeof(fme7IrqCounter);
    size += sizeof(prgBank6000Offset);
    size += sizeof(vrcChrBanks);
    size += sizeof(vrcChrHighBits);
    size += sizeof(vrc6Prg16Bank);
    size += sizeof(vrcPrgBank8000);
    size += sizeof(vrcPrgBankA000);
    size += sizeof(vrcPrgBankC000);
    size += sizeof(vrc6PpuCtrl);
    size += sizeof(vrcIrqLatch);
    size += sizeof(vrcIrqCounter);
    size += sizeof(vrcIrqEnable);
    size += sizeof(vrcIrqEnableAfterAck);
    size += sizeof(vrcIrqModeCycle);
    size += sizeof(vrcIrqPrescaler);
    size += sizeof(vrcRegCmd);
    size += sizeof(vrcReg1Mask);
    size += sizeof(vrcReg2Mask);
    size += sizeof(vrc2Latch);
    size += sizeof(mapper18PrgRegs);
    size += sizeof(mapper18ChrRegs);
    size += sizeof(mapper18IrqCounter);
    size += sizeof(mapper18IrqLatch);
    size += sizeof(mapper18IrqEnable);
    size += sizeof(mapper18IrqSizeMask);
    size += sizeof(mapper18RamEnable);
    size += sizeof(mapper18RamWriteEnable);
    size += sizeof(mapper19PrgRegs);
    size += sizeof(mapper19ChrRegs);
    size += sizeof(mapper19NtRegs);
    size += sizeof(mapper19InternalRam);
    size += sizeof(mapper19AddrPort);
    size += sizeof(mapper19ChrRamCtl);
    size += sizeof(mapper19WramProtect);
    size += sizeof(mapper19RegF000);
    size += sizeof(mapper19IrqCounter);
    size += sizeof(mapper19IrqEnable);
    size += sizeof(m163Reg5000);
    size += sizeof(m163Reg5200);
    size += sizeof(m163Mode5300);
    size += sizeof(m163FeedbackBit);
    size += sizeof(m163FeedbackSetMode);
    size += sizeof(m163PrevA13);
    size += sizeof(m163LatchedA9);
    size += sizeof(mmc5PrgMode);
    size += sizeof(mmc5ChrMode);
    size += sizeof(mmc5ExRamMode);
    size += sizeof(mmc5NtMap);
    size += sizeof(mmc5FillTile);
    size += sizeof(mmc5FillAttr);
    size += sizeof(mmc5UpperChrBits);
    size += sizeof(mmc5PrgRegs);
    size += sizeof(mmc5ChrRegsA);
    size += sizeof(mmc5ChrRegsB);
    size += sizeof(mmc5ExRam);
    size += sizeof(mmc5LastChrWriteSetB);
    size += sizeof(mmc5IrqEnable);
    size += sizeof(mmc5IrqTargetScanline);
    size += sizeof(mmc5ScanlineCounter);
    size += sizeof(mmc5InFrame);
    size += sizeof(mapper32Prg0);
    size += sizeof(mapper32Prg1);
    size += sizeof(mapper32Ctrl);
    size += sizeof(mapper32ChrBanks);
    size += sizeof(mapper33Regs);
    size += sizeof(mapper33Mirror);
    size += sizeof(mapper48IrqEnable);
    size += sizeof(mapper48IrqCounter);
    size += sizeof(mapper48IrqLatch);
    size += sizeof(mapper64BankSelect);
    size += sizeof(mapper64Regs);
    size += sizeof(mapper64PrgMode);
    size += sizeof(mapper64ChrMode);
    size += sizeof(mapper64Chr1kMode);
    size += sizeof(mapper64IrqEnable);
    size += sizeof(mapper64IrqReload);
    size += sizeof(mapper64IrqCycleMode);
    size += sizeof(mapper64IrqCounter);
    size += sizeof(mapper64IrqLatch);
    size += sizeof(mapper64IrqPrescaler);
    size += sizeof(mapper64PrevA12);
    size += sizeof(mapper64A12LowCycles);
    size += sizeof(mapper70Reg);
    size += sizeof(mapper79Reg);
    size += sizeof(mapper87Reg);
    size += sizeof(mapper148Reg);
    
    
    size += sizeof(mmc3BankSelect);
    size += sizeof(mmc3Banks);
    size += sizeof(mmc3PrgMode);
    size += sizeof(mmc3ChrMode);
    size += sizeof(mmc3IrqLatch);
    size += sizeof(mmc3IrqCounter);
    size += sizeof(mmc3IrqEnabled);
    size += sizeof(mmc3IrqReload);
    size += sizeof(mmc3IrqPending);
    size += sizeof(mmc3PrevA12);
    size += sizeof(mmc3A12LowCycles);
    size += sizeof(mmc3LastObserveCpuCycle);
    size += sizeof(mmc3LastObserveCpuCycleValid);
    
    
    size += sizeof(sram);
    
    
    if (chrBanks == 0) {
        size += sizeof(chrRam);
    }
    
    return size;
}

void Cartridge::saveState(uint8_t* buf, size_t& offset) const {
    
    buf[offset++] = mapper;
    buf[offset++] = submapper;
    buf[offset++] = mirrorVertical ? 1 : 0;
    buf[offset++] = oneScreenMirror ? 1 : 0;
    buf[offset++] = oneScreenUpper ? 1 : 0;
    buf[offset++] = prgBankSelect;
    buf[offset++] = mapper7BusConflictMode;
    buf[offset++] = mapper71MirroringMode ? 1 : 0;
    
    
    buf[offset++] = mmc1ShiftReg;
    buf[offset++] = mmc1WriteCount;
    buf[offset++] = mmc1Control;
    buf[offset++] = mmc1ChrBank0;
    buf[offset++] = mmc1ChrBank1;
    buf[offset++] = mmc1PrgBank;
    
    
    buf[offset++] = cnromChrBank;
    buf[offset++] = mapper34ChrBank0;
    buf[offset++] = mapper34ChrBank1;
    buf[offset++] = mapper34NinaMode ? 1 : 0;
    buf[offset++] = colorDreamsPrgBank;
    buf[offset++] = colorDreamsChrBank;
    buf[offset++] = gxromPrgBank;
    buf[offset++] = gxromChrBank;
    buf[offset++] = mmc2PrgBank;
    buf[offset++] = mmc2ChrFd0;
    buf[offset++] = mmc2ChrFe0;
    buf[offset++] = mmc2ChrFd1;
    buf[offset++] = mmc2ChrFe1;
    buf[offset++] = mmc2Latch0FE ? 1 : 0;
    buf[offset++] = mmc2Latch1FE ? 1 : 0;
    buf[offset++] = fme7Command;
    memcpy(buf + offset, fme7ChrBanks, sizeof(fme7ChrBanks));
    offset += sizeof(fme7ChrBanks);
    memcpy(buf + offset, fme7PrgBanks, sizeof(fme7PrgBanks));
    offset += sizeof(fme7PrgBanks);
    buf[offset++] = fme7RamEnable ? 1 : 0;
    buf[offset++] = fme7RamSelect ? 1 : 0;
    buf[offset++] = fme7RamWriteEnable ? 1 : 0;
    buf[offset++] = fme7IrqEnable ? 1 : 0;
    buf[offset++] = fme7IrqCounterEnable ? 1 : 0;
    buf[offset++] = (uint8_t)(fme7IrqCounter & 0xFF);
    buf[offset++] = (uint8_t)((fme7IrqCounter >> 8) & 0xFF);
    buf[offset++] = (uint8_t)(prgBank6000Offset & 0xFF);
    buf[offset++] = (uint8_t)((prgBank6000Offset >> 8) & 0xFF);
    buf[offset++] = (uint8_t)((prgBank6000Offset >> 16) & 0xFF);
    buf[offset++] = (uint8_t)((prgBank6000Offset >> 24) & 0xFF);
    memcpy(buf + offset, vrcChrBanks, sizeof(vrcChrBanks));
    offset += sizeof(vrcChrBanks);
    memcpy(buf + offset, vrcChrHighBits, sizeof(vrcChrHighBits));
    offset += sizeof(vrcChrHighBits);
    buf[offset++] = vrc6Prg16Bank;
    buf[offset++] = vrcPrgBank8000;
    buf[offset++] = vrcPrgBankA000;
    buf[offset++] = vrcPrgBankC000;
    buf[offset++] = vrc6PpuCtrl;
    buf[offset++] = vrcIrqLatch;
    buf[offset++] = vrcIrqCounter;
    buf[offset++] = vrcIrqEnable ? 1 : 0;
    buf[offset++] = vrcIrqEnableAfterAck ? 1 : 0;
    buf[offset++] = vrcIrqModeCycle ? 1 : 0;
    buf[offset++] = (uint8_t)(vrcIrqPrescaler & 0xFF);
    buf[offset++] = (uint8_t)((vrcIrqPrescaler >> 8) & 0xFF);
    buf[offset++] = vrcRegCmd;
    buf[offset++] = vrcReg1Mask;
    buf[offset++] = vrcReg2Mask;
    buf[offset++] = vrc2Latch;
    memcpy(buf + offset, mapper18PrgRegs, sizeof(mapper18PrgRegs));
    offset += sizeof(mapper18PrgRegs);
    memcpy(buf + offset, mapper18ChrRegs, sizeof(mapper18ChrRegs));
    offset += sizeof(mapper18ChrRegs);
    buf[offset++] = (uint8_t)(mapper18IrqCounter & 0xFF);
    buf[offset++] = (uint8_t)((mapper18IrqCounter >> 8) & 0xFF);
    buf[offset++] = (uint8_t)(mapper18IrqLatch & 0xFF);
    buf[offset++] = (uint8_t)((mapper18IrqLatch >> 8) & 0xFF);
    buf[offset++] = mapper18IrqEnable ? 1 : 0;
    buf[offset++] = (uint8_t)(mapper18IrqSizeMask & 0xFF);
    buf[offset++] = (uint8_t)((mapper18IrqSizeMask >> 8) & 0xFF);
    buf[offset++] = mapper18RamEnable ? 1 : 0;
    buf[offset++] = mapper18RamWriteEnable ? 1 : 0;
    memcpy(buf + offset, mapper19PrgRegs, sizeof(mapper19PrgRegs));
    offset += sizeof(mapper19PrgRegs);
    memcpy(buf + offset, mapper19ChrRegs, sizeof(mapper19ChrRegs));
    offset += sizeof(mapper19ChrRegs);
    memcpy(buf + offset, mapper19NtRegs, sizeof(mapper19NtRegs));
    offset += sizeof(mapper19NtRegs);
    memcpy(buf + offset, mapper19InternalRam, sizeof(mapper19InternalRam));
    offset += sizeof(mapper19InternalRam);
    buf[offset++] = mapper19AddrPort;
    buf[offset++] = mapper19ChrRamCtl;
    buf[offset++] = mapper19WramProtect;
    buf[offset++] = mapper19RegF000;
    buf[offset++] = (uint8_t)(mapper19IrqCounter & 0xFF);
    buf[offset++] = (uint8_t)((mapper19IrqCounter >> 8) & 0xFF);
    buf[offset++] = mapper19IrqEnable ? 1 : 0;
    buf[offset++] = m163Reg5000;
    buf[offset++] = m163Reg5200;
    buf[offset++] = m163Mode5300;
    buf[offset++] = m163FeedbackBit;
    buf[offset++] = m163FeedbackSetMode ? 1 : 0;
    buf[offset++] = m163PrevA13 ? 1 : 0;
    buf[offset++] = m163LatchedA9;
    buf[offset++] = mmc5PrgMode;
    buf[offset++] = mmc5ChrMode;
    buf[offset++] = mmc5ExRamMode;
    buf[offset++] = mmc5NtMap;
    buf[offset++] = mmc5FillTile;
    buf[offset++] = mmc5FillAttr;
    buf[offset++] = mmc5UpperChrBits;
    memcpy(buf + offset, mmc5PrgRegs, sizeof(mmc5PrgRegs));
    offset += sizeof(mmc5PrgRegs);
    memcpy(buf + offset, mmc5ChrRegsA, sizeof(mmc5ChrRegsA));
    offset += sizeof(mmc5ChrRegsA);
    memcpy(buf + offset, mmc5ChrRegsB, sizeof(mmc5ChrRegsB));
    offset += sizeof(mmc5ChrRegsB);
    memcpy(buf + offset, mmc5ExRam, sizeof(mmc5ExRam));
    offset += sizeof(mmc5ExRam);
    buf[offset++] = mmc5LastChrWriteSetB ? 1 : 0;
    buf[offset++] = mmc5IrqEnable ? 1 : 0;
    buf[offset++] = mmc5IrqTargetScanline;
    buf[offset++] = mmc5ScanlineCounter;
    buf[offset++] = mmc5InFrame ? 1 : 0;
    buf[offset++] = mapper32Prg0;
    buf[offset++] = mapper32Prg1;
    buf[offset++] = mapper32Ctrl;
    memcpy(buf + offset, mapper32ChrBanks, sizeof(mapper32ChrBanks));
    offset += sizeof(mapper32ChrBanks);
    memcpy(buf + offset, mapper33Regs, sizeof(mapper33Regs));
    offset += sizeof(mapper33Regs);
    buf[offset++] = mapper33Mirror;
    buf[offset++] = mapper48IrqEnable ? 1 : 0;
    buf[offset++] = mapper48IrqCounter;
    buf[offset++] = mapper48IrqLatch;
    buf[offset++] = mapper64BankSelect;
    memcpy(buf + offset, mapper64Regs, sizeof(mapper64Regs));
    offset += sizeof(mapper64Regs);
    buf[offset++] = mapper64PrgMode ? 1 : 0;
    buf[offset++] = mapper64ChrMode ? 1 : 0;
    buf[offset++] = mapper64Chr1kMode ? 1 : 0;
    buf[offset++] = mapper64IrqEnable ? 1 : 0;
    buf[offset++] = mapper64IrqReload ? 1 : 0;
    buf[offset++] = mapper64IrqCycleMode ? 1 : 0;
    buf[offset++] = mapper64IrqCounter;
    buf[offset++] = mapper64IrqLatch;
    buf[offset++] = mapper64IrqPrescaler;
    buf[offset++] = mapper64PrevA12 ? 1 : 0;
    buf[offset++] = mapper64A12LowCycles;
    buf[offset++] = mapper70Reg;
    buf[offset++] = mapper79Reg;
    buf[offset++] = mapper87Reg;
    buf[offset++] = mapper148Reg;
    
    
    buf[offset++] = mmc3BankSelect;
    memcpy(buf + offset, mmc3Banks, sizeof(mmc3Banks));
    offset += sizeof(mmc3Banks);
    buf[offset++] = mmc3PrgMode ? 1 : 0;
    buf[offset++] = mmc3ChrMode ? 1 : 0;
    buf[offset++] = mmc3IrqLatch;
    buf[offset++] = mmc3IrqCounter;
    buf[offset++] = mmc3IrqEnabled ? 1 : 0;
    buf[offset++] = mmc3IrqReload ? 1 : 0;
    buf[offset++] = mmc3IrqPending ? 1 : 0;
    buf[offset++] = mmc3PrevA12 ? 1 : 0;
    buf[offset++] = mmc3A12LowCycles;
    for (int i = 0; i < 8; ++i) {
        buf[offset++] = (uint8_t)((mmc3LastObserveCpuCycle >> (i * 8)) & 0xFF);
    }
    buf[offset++] = mmc3LastObserveCpuCycleValid ? 1 : 0;
    
    
    memcpy(buf + offset, sram, sizeof(sram));
    offset += sizeof(sram);
    
    
    if (chrBanks == 0) {
        memcpy(buf + offset, chrRam, sizeof(chrRam));
        offset += sizeof(chrRam);
    }
}

void Cartridge::loadState(const uint8_t* buf, size_t& offset) {
    
    mapper = buf[offset++];
    submapper = buf[offset++];
    mirrorVertical = buf[offset++] != 0;
    oneScreenMirror = buf[offset++] != 0;
    oneScreenUpper = buf[offset++] != 0;
    prgBankSelect = buf[offset++];
    mapper7BusConflictMode = buf[offset++];
    mapper71MirroringMode = buf[offset++] != 0;
    
    
    mmc1ShiftReg = buf[offset++];
    mmc1WriteCount = buf[offset++];
    mmc1Control = buf[offset++];
    mmc1ChrBank0 = buf[offset++];
    mmc1ChrBank1 = buf[offset++];
    mmc1PrgBank = buf[offset++];
    
    
    cnromChrBank = buf[offset++];
    mapper34ChrBank0 = buf[offset++];
    mapper34ChrBank1 = buf[offset++];
    mapper34NinaMode = buf[offset++] != 0;
    colorDreamsPrgBank = buf[offset++];
    colorDreamsChrBank = buf[offset++];
    gxromPrgBank = buf[offset++];
    gxromChrBank = buf[offset++];
    mmc2PrgBank = buf[offset++];
    mmc2ChrFd0 = buf[offset++];
    mmc2ChrFe0 = buf[offset++];
    mmc2ChrFd1 = buf[offset++];
    mmc2ChrFe1 = buf[offset++];
    mmc2Latch0FE = buf[offset++] != 0;
    mmc2Latch1FE = buf[offset++] != 0;
    fme7Command = buf[offset++];
    memcpy(fme7ChrBanks, buf + offset, sizeof(fme7ChrBanks));
    offset += sizeof(fme7ChrBanks);
    memcpy(fme7PrgBanks, buf + offset, sizeof(fme7PrgBanks));
    offset += sizeof(fme7PrgBanks);
    fme7RamEnable = buf[offset++] != 0;
    fme7RamSelect = buf[offset++] != 0;
    fme7RamWriteEnable = buf[offset++] != 0;
    fme7IrqEnable = buf[offset++] != 0;
    fme7IrqCounterEnable = buf[offset++] != 0;
    fme7IrqCounter = (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
    offset += 2;
    prgBank6000Offset = (uint32_t)buf[offset] |
                        ((uint32_t)buf[offset + 1] << 8) |
                        ((uint32_t)buf[offset + 2] << 16) |
                        ((uint32_t)buf[offset + 3] << 24);
    offset += 4;
    memcpy(vrcChrBanks, buf + offset, sizeof(vrcChrBanks));
    offset += sizeof(vrcChrBanks);
    memcpy(vrcChrHighBits, buf + offset, sizeof(vrcChrHighBits));
    offset += sizeof(vrcChrHighBits);
    vrc6Prg16Bank = buf[offset++];
    vrcPrgBank8000 = buf[offset++];
    vrcPrgBankA000 = buf[offset++];
    vrcPrgBankC000 = buf[offset++];
    vrc6PpuCtrl = buf[offset++];
    vrcIrqLatch = buf[offset++];
    vrcIrqCounter = buf[offset++];
    vrcIrqEnable = buf[offset++] != 0;
    vrcIrqEnableAfterAck = buf[offset++] != 0;
    vrcIrqModeCycle = buf[offset++] != 0;
    vrcIrqPrescaler = (int16_t)((uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8));
    offset += 2;
    vrcRegCmd = buf[offset++];
    vrcReg1Mask = buf[offset++];
    vrcReg2Mask = buf[offset++];
    vrc2Latch = buf[offset++];
    memcpy(mapper18PrgRegs, buf + offset, sizeof(mapper18PrgRegs));
    offset += sizeof(mapper18PrgRegs);
    memcpy(mapper18ChrRegs, buf + offset, sizeof(mapper18ChrRegs));
    offset += sizeof(mapper18ChrRegs);
    mapper18IrqCounter = (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
    offset += 2;
    mapper18IrqLatch = (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
    offset += 2;
    mapper18IrqEnable = buf[offset++] != 0;
    mapper18IrqSizeMask = (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
    offset += 2;
    mapper18RamEnable = buf[offset++] != 0;
    mapper18RamWriteEnable = buf[offset++] != 0;
    memcpy(mapper19PrgRegs, buf + offset, sizeof(mapper19PrgRegs));
    offset += sizeof(mapper19PrgRegs);
    memcpy(mapper19ChrRegs, buf + offset, sizeof(mapper19ChrRegs));
    offset += sizeof(mapper19ChrRegs);
    memcpy(mapper19NtRegs, buf + offset, sizeof(mapper19NtRegs));
    offset += sizeof(mapper19NtRegs);
    memcpy(mapper19InternalRam, buf + offset, sizeof(mapper19InternalRam));
    offset += sizeof(mapper19InternalRam);
    mapper19AddrPort = buf[offset++];
    mapper19ChrRamCtl = buf[offset++];
    mapper19WramProtect = buf[offset++];
    mapper19RegF000 = buf[offset++];
    mapper19IrqCounter = (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
    offset += 2;
    mapper19IrqEnable = buf[offset++] != 0;
    m163Reg5000 = buf[offset++];
    m163Reg5200 = buf[offset++];
    m163Mode5300 = buf[offset++];
    m163FeedbackBit = buf[offset++];
    m163FeedbackSetMode = buf[offset++] != 0;
    m163PrevA13 = buf[offset++] != 0;
    m163LatchedA9 = buf[offset++];
    mmc5PrgMode = buf[offset++];
    mmc5ChrMode = buf[offset++];
    mmc5ExRamMode = buf[offset++];
    mmc5NtMap = buf[offset++];
    mmc5FillTile = buf[offset++];
    mmc5FillAttr = buf[offset++];
    mmc5UpperChrBits = buf[offset++];
    memcpy(mmc5PrgRegs, buf + offset, sizeof(mmc5PrgRegs));
    offset += sizeof(mmc5PrgRegs);
    memcpy(mmc5ChrRegsA, buf + offset, sizeof(mmc5ChrRegsA));
    offset += sizeof(mmc5ChrRegsA);
    memcpy(mmc5ChrRegsB, buf + offset, sizeof(mmc5ChrRegsB));
    offset += sizeof(mmc5ChrRegsB);
    memcpy(mmc5ExRam, buf + offset, sizeof(mmc5ExRam));
    offset += sizeof(mmc5ExRam);
    mmc5LastChrWriteSetB = buf[offset++] != 0;
    mmc5IrqEnable = buf[offset++] != 0;
    mmc5IrqTargetScanline = buf[offset++];
    mmc5ScanlineCounter = buf[offset++];
    mmc5InFrame = buf[offset++] != 0;
    mapper32Prg0 = buf[offset++];
    mapper32Prg1 = buf[offset++];
    mapper32Ctrl = buf[offset++];
    memcpy(mapper32ChrBanks, buf + offset, sizeof(mapper32ChrBanks));
    offset += sizeof(mapper32ChrBanks);
    memcpy(mapper33Regs, buf + offset, sizeof(mapper33Regs));
    offset += sizeof(mapper33Regs);
    mapper33Mirror = buf[offset++];
    mapper48IrqEnable = buf[offset++] != 0;
    mapper48IrqCounter = buf[offset++];
    mapper48IrqLatch = buf[offset++];
    mapper64BankSelect = buf[offset++];
    memcpy(mapper64Regs, buf + offset, sizeof(mapper64Regs));
    offset += sizeof(mapper64Regs);
    mapper64PrgMode = buf[offset++] != 0;
    mapper64ChrMode = buf[offset++] != 0;
    mapper64Chr1kMode = buf[offset++] != 0;
    mapper64IrqEnable = buf[offset++] != 0;
    mapper64IrqReload = buf[offset++] != 0;
    mapper64IrqCycleMode = buf[offset++] != 0;
    mapper64IrqCounter = buf[offset++];
    mapper64IrqLatch = buf[offset++];
    mapper64IrqPrescaler = buf[offset++];
    mapper64PrevA12 = buf[offset++] != 0;
    mapper64A12LowCycles = buf[offset++];
    mapper70Reg = buf[offset++];
    mapper79Reg = buf[offset++];
    mapper87Reg = buf[offset++];
    mapper148Reg = buf[offset++];
    
    
    mmc3BankSelect = buf[offset++];
    memcpy(mmc3Banks, buf + offset, sizeof(mmc3Banks));
    offset += sizeof(mmc3Banks);
    mmc3PrgMode = buf[offset++] != 0;
    mmc3ChrMode = buf[offset++] != 0;
    mmc3IrqLatch = buf[offset++];
    mmc3IrqCounter = buf[offset++];
    mmc3IrqEnabled = buf[offset++] != 0;
    mmc3IrqReload = buf[offset++] != 0;
    mmc3IrqPending = buf[offset++] != 0;
    mmc3PrevA12 = buf[offset++] != 0;
    mmc3A12LowCycles = buf[offset++];
    mmc3LastObserveCpuCycle = 0;
    for (int i = 0; i < 8; ++i) {
        mmc3LastObserveCpuCycle |= ((uint64_t)buf[offset++]) << (i * 8);
    }
    mmc3LastObserveCpuCycleValid = buf[offset++] != 0;
    
    
    memcpy(sram, buf + offset, sizeof(sram));
    offset += sizeof(sram);
    
    
    if (chrBanks == 0) {
        memcpy(chrRam, buf + offset, sizeof(chrRam));
        offset += sizeof(chrRam);
    }

    configureVrcMapping();
    
    
    updateBankCache();
    
    
    if (mapper == 3 && chr && chrSize > 0x2000) {
        uint32_t chrOffset = (uint32_t)cnromChrBank * 0x2000;
        if (chrOffset < chrSize) {
            chrWindow = chr + chrOffset;
        }
    } else if ((mapper == 11 || mapper == 66) && chr && chrSize >= 0x2000) {
        uint32_t bank = (mapper == 11) ? colorDreamsChrBank : gxromChrBank;
        uint32_t chrBankCount = chrSize / 0x2000;
        if (chrBankCount > 0) {
            bank %= chrBankCount;
        } else {
            bank = 0;
        }
        chrWindow = chr + bank * 0x2000;
    } else if (chrBanks == 0) {
        chrWindow = chrRam;
    } else if (chr) {
        chrWindow = chr;
    }
    
    
    updateChrBankCache();
    updateNtPtrs();
}
