/*
 * 6502 CPU implementation with opcode execution and bus access logic.
 */

#include "cpu6502.h"
#include "nes.h"

static const uint8_t DRAM_ATTR kCpuCycles[256] = {
     7,2,2,2,3,3,5,2,3,2,2,2,4,4,6,2,
     2,5,2,2,4,4,6,2,2,4,2,2,4,4,7,2,
     6,6,2,2,3,3,5,2,4,2,2,2,4,4,6,2,
     2,5,2,2,4,4,6,2,2,4,2,2,4,4,7,2,
     6,6,2,2,3,3,5,2,4,2,2,2,3,4,6,2,
     2,5,2,2,4,4,6,2,2,4,2,2,4,4,7,2,
     6,6,2,2,3,3,5,2,4,2,2,2,5,4,6,2,
     2,5,2,2,4,4,6,2,2,4,2,2,4,4,7,2,
     2,6,2,2,3,3,3,2,2,2,2,2,4,4,4,2,
     2,6,2,2,4,4,4,2,2,5,2,2,4,5,5,2,
     2,6,2,2,3,3,3,2,2,2,2,2,4,4,4,2,
     2,5,2,2,4,4,4,2,2,4,2,2,4,4,4,2,
     2,6,2,2,3,3,5,2,2,2,2,2,4,4,6,2,
     2,5,2,2,4,4,6,2,2,4,2,2,4,4,7,2,
     2,6,2,2,3,3,5,2,2,2,2,2,4,4,6,2,
     2,5,2,2,4,4,6,2,2,4,2,2,4,4,7,2
};

void IRAM_ATTR CPU6502::clock(int targetCycles) {
    cycles -= targetCycles;
    while (cycles < 0) {
        const uint8_t used = step();
        cycles += used;
    }
}

void CPU6502::runCycles(int targetCycles) {
    clock(targetCycles);
}

void CPU6502::connect(NES* n) {
    bus = n;
}

void CPU6502::reset() {
    cycles = 0;  
    totalCycles = 0;
    irqPending = false;
    irqDelay = 0;
    
    uint8_t lo = bus->cpuRead(0xFFFC);
    uint8_t hi = bus->cpuRead(0xFFFD);
    PC = (hi << 8) | lo;
    
    
    if (PC == 0x0000 || PC == 0xFFFF) {
        PC = 0x8000;
    }
    
    A = 0;
    X = 0;
    Y = 0;
    SP = 0xFD;
    P = 0x24;  
}

void IRAM_ATTR CPU6502::nmi() {
    irqPending = false;
    irqDelay = 0;
    
    pushWord(PC);
    push((P & 0xEF) | 0x20);  
    P |= FLAG_I;
    
    uint8_t lo = bus->cpuRead(0xFFFA);
    uint8_t hi = bus->cpuRead(0xFFFB);
    PC = (hi << 8) | lo;
    addStallCycles(7);
}

void IRAM_ATTR CPU6502::irq() {
    if (P & FLAG_I) {
        irqPending = true;
        return;
    }
    irqPending = false;
    irqDelay = 0;
    
    pushWord(PC);
    push((P & 0xEF) | 0x20);  
    P |= FLAG_I;
    
    uint8_t lo = bus->cpuRead(0xFFFE);
    uint8_t hi = bus->cpuRead(0xFFFF);
    PC = (hi << 8) | lo;
    addStallCycles(7);
}

void IRAM_ATTR CPU6502::addStallCycles(int extraCycles) {
    if (extraCycles <= 0) {
        return;
    }
    cycles += extraCycles;
    totalCycles += (uint64_t)extraCycles;
}

uint8_t IRAM_ATTR CPU6502::fetch() {
    uint8_t v = read(PC);
    PC++;
    return v;
}

uint16_t IRAM_ATTR CPU6502::fetchWord() {
    uint8_t lo = fetch();
    uint8_t hi = fetch();
    return (hi << 8) | lo;
}

uint8_t IRAM_ATTR CPU6502::read(uint16_t addr) {
    return bus->cpuRead(addr);
}

void IRAM_ATTR CPU6502::write(uint16_t addr, uint8_t val) {
    bus->cpuWrite(addr, val);
}

void IRAM_ATTR CPU6502::setFlag(uint8_t flag, bool set) {
    const uint8_t v = set ? flag : 0;
    P = (uint8_t)((P & (uint8_t)~flag) | v);
}

bool IRAM_ATTR CPU6502::getFlag(uint8_t flag) const {
    return (P & flag) != 0;
}

void IRAM_ATTR CPU6502::updateZNFlags(uint8_t val) {
    P = (uint8_t)((P & (uint8_t)~(FLAG_Z | FLAG_N)) | (val & FLAG_N) | (val == 0 ? FLAG_Z : 0));
}

uint16_t IRAM_ATTR CPU6502::addrZeroPage() {
    return fetch();
}

uint16_t IRAM_ATTR CPU6502::addrZeroPageX() {
    return (fetch() + X) & 0xFF;
}

uint16_t IRAM_ATTR CPU6502::addrZeroPageY() {
    return (fetch() + Y) & 0xFF;
}

uint16_t IRAM_ATTR CPU6502::addrAbsolute() {
    return fetchWord();
}

uint16_t IRAM_ATTR CPU6502::addrAbsoluteX() {
    return fetchWord() + X;
}

uint16_t IRAM_ATTR CPU6502::addrAbsoluteY() {
    return fetchWord() + Y;
}

uint16_t IRAM_ATTR CPU6502::addrIndirectX() {
    uint8_t zp = (fetch() + X) & 0xFF;
    uint8_t lo = read(zp);
    uint8_t hi = read((zp + 1) & 0xFF);
    return (hi << 8) | lo;
}

uint16_t IRAM_ATTR CPU6502::addrIndirectY() {
    uint8_t zp = fetch();
    uint8_t lo = read(zp);
    uint8_t hi = read((zp + 1) & 0xFF);
    return ((hi << 8) | lo) + Y;
}

void IRAM_ATTR CPU6502::push(uint8_t val) {
    write(0x100 + SP, val);
    SP--;
}

uint8_t IRAM_ATTR CPU6502::pop() {
    SP++;
    return read(0x100 + SP);
}

void IRAM_ATTR CPU6502::pushWord(uint16_t val) {
    push((val >> 8) & 0xFF);
    push(val & 0xFF);
}

uint16_t IRAM_ATTR CPU6502::popWord() {
    uint8_t lo = pop();
    uint8_t hi = pop();
    return (hi << 8) | lo;
}

uint8_t IRAM_ATTR CPU6502::step() {
    if (irqPending && !(P & FLAG_I)) {
        if (irqDelay > 0) {
            irqDelay--;
        } else {
            pushWord(PC);
            push((P & 0xEF) | 0x20);
            P |= FLAG_I;
            uint8_t lo = read(0xFFFE);
            uint8_t hi = read(0xFFFF);
            PC = (uint16_t)(hi << 8) | lo;
            irqPending = false;
            totalCycles += 7;
            return 7;
        }
    }

    uint8_t opcode = fetch();
    uint8_t cycles = kCpuCycles[opcode];
    auto readAbsoluteIndexedWithPenalty = [&](uint8_t index) -> uint8_t {
        uint16_t base = fetchWord();
        uint16_t addr = (uint16_t)(base + index);
        if ((base & 0xFF00) != (addr & 0xFF00)) {
            cycles++;
        }
        return read(addr);
    };
    auto readIndirectYWithPenalty = [&]() -> uint8_t {
        uint8_t zp = fetch();
        uint8_t lo = read(zp);
        uint8_t hi = read((uint8_t)(zp + 1));
        uint16_t base = (uint16_t)(hi << 8) | lo;
        uint16_t addr = (uint16_t)(base + Y);
        if ((base & 0xFF00) != (addr & 0xFF00)) {
            cycles++;
        }
        return read(addr);
    };
    auto branchRelative = [&](bool take) {
        int8_t off = (int8_t)fetch();
        if (!take) {
            return;
        }
        uint16_t oldPC = PC;
        PC = (uint16_t)(PC + off);
        cycles++;
        if ((oldPC & 0xFF00) != (PC & 0xFF00)) {
            cycles++;
        }
    };

    switch (opcode) {

    
    case 0xA9: A = fetch(); updateZNFlags(A); break;                      
    case 0xA5: A = read(addrZeroPage()); updateZNFlags(A); break;         
    case 0xB5: A = read(addrZeroPageX()); updateZNFlags(A); break;        
    case 0xAD: A = read(addrAbsolute()); updateZNFlags(A); break;         
    case 0xBD: A = readAbsoluteIndexedWithPenalty(X); updateZNFlags(A); break;
    case 0xB9: A = readAbsoluteIndexedWithPenalty(Y); updateZNFlags(A); break;
    case 0xA1: A = read(addrIndirectX()); updateZNFlags(A); break;        
    case 0xB1: A = readIndirectYWithPenalty(); updateZNFlags(A); break;

    
    case 0xA2: X = fetch(); updateZNFlags(X); break;                      
    case 0xA6: X = read(addrZeroPage()); updateZNFlags(X); break;         
    case 0xB6: X = read(addrZeroPageY()); updateZNFlags(X); break;        
    case 0xAE: X = read(addrAbsolute()); updateZNFlags(X); break;         
    case 0xBE: X = readAbsoluteIndexedWithPenalty(Y); updateZNFlags(X); break;

    
    case 0xA0: Y = fetch(); updateZNFlags(Y); break;                      
    case 0xA4: Y = read(addrZeroPage()); updateZNFlags(Y); break;         
    case 0xB4: Y = read(addrZeroPageX()); updateZNFlags(Y); break;        
    case 0xAC: Y = read(addrAbsolute()); updateZNFlags(Y); break;         
    case 0xBC: Y = readAbsoluteIndexedWithPenalty(X); updateZNFlags(Y); break;

    
    case 0x85: write(addrZeroPage(), A); break;                           
    case 0x95: write(addrZeroPageX(), A); break;                          
    case 0x8D: write(addrAbsolute(), A); break;                           
    case 0x9D: write(addrAbsoluteX(), A); break;                          
    case 0x99: write(addrAbsoluteY(), A); break;                          
    case 0x81: write(addrIndirectX(), A); break;                          
    case 0x91: write(addrIndirectY(), A); break;                          

    
    case 0x86: write(addrZeroPage(), X); break;                           
    case 0x96: write(addrZeroPageY(), X); break;                          
    case 0x8E: write(addrAbsolute(), X); break;                           

    
    case 0x84: write(addrZeroPage(), Y); break;                           
    case 0x94: write(addrZeroPageX(), Y); break;                          
    case 0x8C: write(addrAbsolute(), Y); break;                           

    
    case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79: case 0x61: case 0x71: {
        uint8_t val;
        switch (opcode) {
            case 0x69: val = fetch(); break;
            case 0x65: val = read(addrZeroPage()); break;
            case 0x75: val = read(addrZeroPageX()); break;
            case 0x6D: val = read(addrAbsolute()); break;
            case 0x7D: val = readAbsoluteIndexedWithPenalty(X); break;
            case 0x79: val = readAbsoluteIndexedWithPenalty(Y); break;
            case 0x61: val = read(addrIndirectX()); break;
            case 0x71: val = readIndirectYWithPenalty(); break;
            default: val = 0; break;
        }
        uint16_t sum = (uint16_t)(A + val + ((P & FLAG_C) ? 1 : 0));
        setFlag(FLAG_C, sum > 0xFF);
        setFlag(FLAG_V, (~(A ^ val) & (A ^ sum) & 0x80) != 0);
        A = sum & 0xFF;
        updateZNFlags(A);
        break;
    }

    
    case 0xE9: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9: case 0xE1: case 0xF1: {
        uint8_t val;
        switch (opcode) {
            case 0xE9: val = fetch(); break;
            case 0xE5: val = read(addrZeroPage()); break;
            case 0xF5: val = read(addrZeroPageX()); break;
            case 0xED: val = read(addrAbsolute()); break;
            case 0xFD: val = readAbsoluteIndexedWithPenalty(X); break;
            case 0xF9: val = readAbsoluteIndexedWithPenalty(Y); break;
            case 0xE1: val = read(addrIndirectX()); break;
            case 0xF1: val = readIndirectYWithPenalty(); break;
            default: val = 0; break;
        }
        uint16_t diff = (uint16_t)(A - val - ((P & FLAG_C) ? 0 : 1));
        setFlag(FLAG_C, diff < 0x100);
        setFlag(FLAG_V, ((A ^ val) & (A ^ diff) & 0x80) != 0);
        A = diff & 0xFF;
        updateZNFlags(A);
        break;
    }

    
    case 0x29: A &= fetch(); updateZNFlags(A); break;                     
    case 0x25: A &= read(addrZeroPage()); updateZNFlags(A); break;        
    case 0x35: A &= read(addrZeroPageX()); updateZNFlags(A); break;       
    case 0x2D: A &= read(addrAbsolute()); updateZNFlags(A); break;        
    case 0x3D: A &= readAbsoluteIndexedWithPenalty(X); updateZNFlags(A); break;
    case 0x39: A &= readAbsoluteIndexedWithPenalty(Y); updateZNFlags(A); break;
    case 0x21: A &= read(addrIndirectX()); updateZNFlags(A); break;       
    case 0x31: A &= readIndirectYWithPenalty(); updateZNFlags(A); break;

    
    case 0x09: A |= fetch(); updateZNFlags(A); break;                     
    case 0x05: A |= read(addrZeroPage()); updateZNFlags(A); break;        
    case 0x15: A |= read(addrZeroPageX()); updateZNFlags(A); break;       
    case 0x0D: A |= read(addrAbsolute()); updateZNFlags(A); break;        
    case 0x1D: A |= readAbsoluteIndexedWithPenalty(X); updateZNFlags(A); break;
    case 0x19: A |= readAbsoluteIndexedWithPenalty(Y); updateZNFlags(A); break;
    case 0x01: A |= read(addrIndirectX()); updateZNFlags(A); break;       
    case 0x11: A |= readIndirectYWithPenalty(); updateZNFlags(A); break;

    
    case 0x49: A ^= fetch(); updateZNFlags(A); break;                     
    case 0x45: A ^= read(addrZeroPage()); updateZNFlags(A); break;        
    case 0x55: A ^= read(addrZeroPageX()); updateZNFlags(A); break;       
    case 0x4D: A ^= read(addrAbsolute()); updateZNFlags(A); break;        
    case 0x5D: A ^= readAbsoluteIndexedWithPenalty(X); updateZNFlags(A); break;
    case 0x59: A ^= readAbsoluteIndexedWithPenalty(Y); updateZNFlags(A); break;
    case 0x41: A ^= read(addrIndirectX()); updateZNFlags(A); break;       
    case 0x51: A ^= readIndirectYWithPenalty(); updateZNFlags(A); break;

    
    case 0x0A: { 
        setFlag(FLAG_C, (A & 0x80) != 0);
        A <<= 1;
        updateZNFlags(A);
        break;
    }
    case 0x06: case 0x16: case 0x0E: case 0x1E: {
        uint16_t addr;
        switch (opcode) {
            case 0x06: addr = addrZeroPage(); break;
            case 0x16: addr = addrZeroPageX(); break;
            case 0x0E: addr = addrAbsolute(); break;
            case 0x1E: addr = addrAbsoluteX(); break;
            default: addr = 0; break;
        }
        uint8_t val = read(addr);
        setFlag(FLAG_C, (val & 0x80) != 0);
        val <<= 1;
        write(addr, val);
        updateZNFlags(val);
        break;
    }

    
    case 0x4A: { 
        setFlag(FLAG_C, (A & 0x01) != 0);
        A >>= 1;
        updateZNFlags(A);
        break;
    }
    case 0x46: case 0x56: case 0x4E: case 0x5E: {
        uint16_t addr;
        switch (opcode) {
            case 0x46: addr = addrZeroPage(); break;
            case 0x56: addr = addrZeroPageX(); break;
            case 0x4E: addr = addrAbsolute(); break;
            case 0x5E: addr = addrAbsoluteX(); break;
            default: addr = 0; break;
        }
        uint8_t val = read(addr);
        setFlag(FLAG_C, (val & 0x01) != 0);
        val >>= 1;
        write(addr, val);
        updateZNFlags(val);
        break;
    }

    
    case 0x2A: { 
        uint8_t carry = (P & FLAG_C) ? 1 : 0;
        setFlag(FLAG_C, (A & 0x80) != 0);
        A = (A << 1) | carry;
        updateZNFlags(A);
        break;
    }
    case 0x26: case 0x36: case 0x2E: case 0x3E: {
        uint16_t addr;
        switch (opcode) {
            case 0x26: addr = addrZeroPage(); break;
            case 0x36: addr = addrZeroPageX(); break;
            case 0x2E: addr = addrAbsolute(); break;
            case 0x3E: addr = addrAbsoluteX(); break;
            default: addr = 0; break;
        }
        uint8_t val = read(addr);
        uint8_t carry = (P & FLAG_C) ? 1 : 0;
        setFlag(FLAG_C, (val & 0x80) != 0);
        val = (val << 1) | carry;
        write(addr, val);
        updateZNFlags(val);
        break;
    }

    
    case 0x6A: { 
        uint8_t carry = getFlag(FLAG_C) ? 0x80 : 0;
        setFlag(FLAG_C, (A & 0x01) != 0);
        A = (A >> 1) | carry;
        updateZNFlags(A);
        break;
    }
    case 0x66: case 0x76: case 0x6E: case 0x7E: {
        uint16_t addr;
        switch (opcode) {
            case 0x66: addr = addrZeroPage(); break;
            case 0x76: addr = addrZeroPageX(); break;
            case 0x6E: addr = addrAbsolute(); break;
            case 0x7E: addr = addrAbsoluteX(); break;
            default: addr = 0; break;
        }
        uint8_t val = read(addr);
        uint8_t carry = getFlag(FLAG_C) ? 0x80 : 0;
        setFlag(FLAG_C, (val & 0x01) != 0);
        val = (val >> 1) | carry;
        write(addr, val);
        updateZNFlags(val);
        break;
    }

    
    case 0xE6: case 0xF6: case 0xEE: case 0xFE: {
        uint16_t addr;
        switch (opcode) {
            case 0xE6: addr = addrZeroPage(); break;
            case 0xF6: addr = addrZeroPageX(); break;
            case 0xEE: addr = addrAbsolute(); break;
            case 0xFE: addr = addrAbsoluteX(); break;
            default: addr = 0; break;
        }
        uint8_t val = read(addr) + 1;
        write(addr, val);
        updateZNFlags(val);
        break;
    }

    
    case 0xC6: case 0xD6: case 0xCE: case 0xDE: {
        uint16_t addr;
        switch (opcode) {
            case 0xC6: addr = addrZeroPage(); break;
            case 0xD6: addr = addrZeroPageX(); break;
            case 0xCE: addr = addrAbsolute(); break;
            case 0xDE: addr = addrAbsoluteX(); break;
            default: addr = 0; break;
        }
        uint8_t val = read(addr) - 1;
        write(addr, val);
        updateZNFlags(val);
        break;
    }

    
    case 0xE8: X++; updateZNFlags(X); break;  
    case 0xC8: Y++; updateZNFlags(Y); break;  
    case 0xCA: X--; updateZNFlags(X); break;  
    case 0x88: Y--; updateZNFlags(Y); break;  

    
    case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xC1: case 0xD1: {
        uint8_t val;
        switch (opcode) {
            case 0xC9: val = fetch(); break;
            case 0xC5: val = read(addrZeroPage()); break;
            case 0xD5: val = read(addrZeroPageX()); break;
            case 0xCD: val = read(addrAbsolute()); break;
            case 0xDD: val = readAbsoluteIndexedWithPenalty(X); break;
            case 0xD9: val = readAbsoluteIndexedWithPenalty(Y); break;
            case 0xC1: val = read(addrIndirectX()); break;
            case 0xD1: val = readIndirectYWithPenalty(); break;
            default: val = 0; break;
        }
        setFlag(FLAG_C, A >= val);
        setFlag(FLAG_Z, A == val);
        setFlag(FLAG_N, ((A - val) & 0x80) != 0);
        break;
    }

    
    case 0xE0: { uint8_t v = fetch(); setFlag(FLAG_C, X >= v); setFlag(FLAG_Z, X == v); setFlag(FLAG_N, ((X - v) & 0x80) != 0); break; }
    case 0xE4: { uint8_t v = read(addrZeroPage()); setFlag(FLAG_C, X >= v); setFlag(FLAG_Z, X == v); setFlag(FLAG_N, ((X - v) & 0x80) != 0); break; }
    case 0xEC: { uint8_t v = read(addrAbsolute()); setFlag(FLAG_C, X >= v); setFlag(FLAG_Z, X == v); setFlag(FLAG_N, ((X - v) & 0x80) != 0); break; }

    
    case 0xC0: { uint8_t v = fetch(); setFlag(FLAG_C, Y >= v); setFlag(FLAG_Z, Y == v); setFlag(FLAG_N, ((Y - v) & 0x80) != 0); break; }
    case 0xC4: { uint8_t v = read(addrZeroPage()); setFlag(FLAG_C, Y >= v); setFlag(FLAG_Z, Y == v); setFlag(FLAG_N, ((Y - v) & 0x80) != 0); break; }
    case 0xCC: { uint8_t v = read(addrAbsolute()); setFlag(FLAG_C, Y >= v); setFlag(FLAG_Z, Y == v); setFlag(FLAG_N, ((Y - v) & 0x80) != 0); break; }

    
    case 0x24: case 0x2C: {
        uint8_t val = (opcode == 0x24) ? read(addrZeroPage()) : read(addrAbsolute());
        setFlag(FLAG_Z, (A & val) == 0);
        setFlag(FLAG_N, (val & 0x80) != 0);
        setFlag(FLAG_V, (val & 0x40) != 0);
        break;
    }

    
    case 0x10: branchRelative((P & FLAG_N) == 0); break;
    case 0x30: branchRelative((P & FLAG_N) != 0); break;
    case 0x50: branchRelative((P & FLAG_V) == 0); break;
    case 0x70: branchRelative((P & FLAG_V) != 0); break;
    case 0x90: branchRelative((P & FLAG_C) == 0); break;
    case 0xB0: branchRelative((P & FLAG_C) != 0); break;
    case 0xD0: branchRelative((P & FLAG_Z) == 0); break;
    case 0xF0: branchRelative((P & FLAG_Z) != 0); break;

    
    case 0x4C: PC = fetchWord(); break;  
    case 0x6C: { 
        uint16_t ptr = fetchWord();
        uint8_t lo = read(ptr);
        
        uint8_t hi = read((ptr & 0xFF00) | ((ptr + 1) & 0x00FF));
        PC = (hi << 8) | lo;
        break;
    }

    
    case 0x20: { 
        uint16_t addr = fetchWord();
        pushWord(PC - 1);
        PC = addr;
        break;
    }
    case 0x60: { 
        PC = popWord() + 1;
        break;
    }

    
    case 0x48: push(A); break;                                    
    case 0x68: A = pop(); updateZNFlags(A); break;                
    case 0x08: push(P | 0x10); break;                             
    case 0x28: {
        const uint8_t oldP = P;
        P = (pop() & 0xEF) | 0x20;
        if ((oldP & FLAG_I) && !(P & FLAG_I) && irqPending) {
            irqDelay = 1;
        }
        break;
    }

    
    case 0xAA: X = A; updateZNFlags(X); break;  
    case 0xA8: Y = A; updateZNFlags(Y); break;  
    case 0x8A: A = X; updateZNFlags(A); break;  
    case 0x98: A = Y; updateZNFlags(A); break;  
    case 0xBA: X = SP; updateZNFlags(X); break; 
    case 0x9A: SP = X; break;                   

    
    case 0x18: setFlag(FLAG_C, false); break;  
    case 0x38: setFlag(FLAG_C, true); break;   
    case 0x58: {
        const bool wasI = (P & FLAG_I) != 0;
        P &= (uint8_t)~FLAG_I;
        if (wasI && irqPending) {
            irqDelay = 1;
        }
        break;
    }
    case 0x78: setFlag(FLAG_I, true); break;   
    case 0xD8: setFlag(FLAG_D, false); break;  
    case 0xF8: setFlag(FLAG_D, true); break;   
    case 0xB8: setFlag(FLAG_V, false); break;  

    
    case 0x00: { 
        PC++;
        pushWord(PC);
        push(P | 0x10);  
        setFlag(FLAG_I, true);
        PC = read(0xFFFE) | (read(0xFFFF) << 8);
        break;
    }
    case 0x40: { 
        P = (pop() & 0xEF) | 0x20;
        PC = popWord();
        break;
    }

    
    case 0xEA: break;

    
    case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: break;  
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: fetch(); break;    
    case 0x04: case 0x44: case 0x64: fetch(); break;                           
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: fetch(); break; 
    case 0x0C: fetchWord(); break;                                              
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: fetchWord(); break; 

    default: {
        
        
        
        break;
    }
    }

    totalCycles += (uint64_t)cycles;
    return cycles;
}

size_t CPU6502::getStateSize() const {
    return sizeof(A) + sizeof(X) + sizeof(Y) + sizeof(SP) + sizeof(P) + sizeof(PC);
}

void CPU6502::saveState(uint8_t* buf, size_t& offset) const {
    buf[offset++] = A;
    buf[offset++] = X;
    buf[offset++] = Y;
    buf[offset++] = SP;
    buf[offset++] = P;
    buf[offset++] = PC & 0xFF;
    buf[offset++] = (PC >> 8) & 0xFF;
}

void CPU6502::loadState(const uint8_t* buf, size_t& offset) {
    A = buf[offset++];
    X = buf[offset++];
    Y = buf[offset++];
    SP = buf[offset++];
    P = buf[offset++];
    PC = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    totalCycles = 0;
    irqPending = false;
    irqDelay = 0;
}
