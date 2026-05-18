/*
 * 6502 CPU core declarations, register state, and execution interfaces.
 */

#pragma once
#include <Arduino.h>

class NES;

class CPU6502 {
public:
    
    uint8_t A = 0;      
    uint8_t X = 0;      
    uint8_t Y = 0;      
    uint8_t SP = 0xFD;  
    uint8_t P = 0x24;   
    uint16_t PC = 0x8000; 
    
    
    int cycles = 0;
    uint64_t totalCycles = 0;

    
    void reset();
    
    uint8_t IRAM_ATTR step();
    
    void IRAM_ATTR clock(int targetCycles);
    
    void runCycles(int cycles);
    void connect(NES* nes);
    void IRAM_ATTR nmi();   
    void IRAM_ATTR irq();   
    void IRAM_ATTR addStallCycles(int extraCycles);

    
    uint16_t getPC() const { return PC; }
    uint8_t getA() const { return A; }
    uint8_t getX() const { return X; }
    uint8_t getY() const { return Y; }
    uint8_t getSP() const { return SP; }
    uint8_t getP() const { return P; }
    uint64_t getTotalCycles() const { return totalCycles; }
    
    
    void saveState(uint8_t* buf, size_t& offset) const;
    void loadState(const uint8_t* buf, size_t& offset);
    size_t getStateSize() const;

private:
    NES* bus;
    bool irqPending = false;
    uint8_t irqDelay = 0;

    
    uint8_t IRAM_ATTR fetch();
    uint16_t IRAM_ATTR fetchWord();
    uint8_t IRAM_ATTR read(uint16_t addr);
    void IRAM_ATTR write(uint16_t addr, uint8_t val);
    
    
    void IRAM_ATTR setFlag(uint8_t flag, bool set);
    bool IRAM_ATTR getFlag(uint8_t flag) const;
    void IRAM_ATTR updateZNFlags(uint8_t val);

    
    uint16_t IRAM_ATTR addrZeroPage();
    uint16_t IRAM_ATTR addrZeroPageX();
    uint16_t IRAM_ATTR addrZeroPageY();
    uint16_t IRAM_ATTR addrAbsolute();
    uint16_t IRAM_ATTR addrAbsoluteX();
    uint16_t IRAM_ATTR addrAbsoluteY();
    uint16_t IRAM_ATTR addrIndirectX();  
    uint16_t IRAM_ATTR addrIndirectY();  

    
    void IRAM_ATTR push(uint8_t val);
    uint8_t IRAM_ATTR pop();
    void IRAM_ATTR pushWord(uint16_t val);
    uint16_t IRAM_ATTR popWord();

    
    static constexpr uint8_t FLAG_C = 0x01; 
    static constexpr uint8_t FLAG_Z = 0x02; 
    static constexpr uint8_t FLAG_I = 0x04; 
    static constexpr uint8_t FLAG_D = 0x08; 
    static constexpr uint8_t FLAG_B = 0x10; 
    static constexpr uint8_t FLAG_V = 0x40; 
    static constexpr uint8_t FLAG_N = 0x80; 
};
