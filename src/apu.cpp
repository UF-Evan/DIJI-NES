/**
 * ============================================================================
 * APU 实现 - NES 音频处理单元
 * ============================================================================
 */

#include "apu.h"
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"

// I2S 音频缓冲区（类似 Anemoia）
#define APU_AUDIO_BUFFER_SIZE 128
// 临时大音量测试：保留原混音音色，去除直流偏置后使用软限幅减少破音。
#define APU_VOLUME_GAIN 5
static int16_t apu_audio_buffer[APU_AUDIO_BUFFER_SIZE * 2];  // stereo
static uint16_t apu_buffer_index = 0;

/**
 * 长度计数器查找表
 * 
 * 当写入 $4003/$4007/$400B/$400F 的高 5 位时，
 * 使用该表查找实际的长度计数器值。
 * 
 * 这些值决定了声音持续多少 "帧" (约 4ms 每帧)
 * 例如: 索引 0 → 10 帧 ≈ 40ms
 *       索引 1 → 254 帧 ≈ 1 秒
 */
const uint8_t APU::lengthTable[32] = {
    10, 254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
    12,  16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

/**
 * 噪声周期查找表 (NTSC 制式)
 * 
 * 决定噪声生成器的更新频率
 * 较小的值 = 较高的频率 = 更高音调的噪声
 * 较大的值 = 较低的频率 = 更低沉的噪声
 */
const uint16_t APU::noiseTable[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

/**
 * 脉冲波占空比序列
 * 
 * 每个序列有 8 个步骤，决定方波的形状:
 * - 12.5% (1/8):  _-______  尖锐的声音
 * - 25%   (2/8):  _--_____  标准音色
 * - 50%   (4/8):  _----___  饱满的声音
 * - 75%   (6/8):  -__-----  与 25% 音色相似但相位相反
 */
const uint8_t APU::dutyTable[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},  // 占空比 12.5% (1/8)
    {0, 1, 1, 0, 0, 0, 0, 0},  // 占空比 25%   (2/8)
    {0, 1, 1, 1, 1, 0, 0, 0},  // 占空比 50%   (4/8)
    {1, 0, 0, 1, 1, 1, 1, 1}   // 占空比 75%   (6/8，即 25% 取反)
};

/**
 * 三角波序列
 * 
 * 32 步的三角波形: 15→0→15
 * 产生柔和的声音，没有谐波失真
 */
const uint8_t APU::triangleTable[32] = {
    15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15
};

// ============================================================================
// 构造函数 / 析构函数
// ============================================================================

APU::APU() {
    reset();
}

APU::~APU() {
    // 无需特殊清理
}

void APU::setVolumeLevel(uint8_t level) {
    if (level > 5) level = 5;
    volumeLevel = level;
}

/**
 * 重置 APU 到上电状态
 * 清除所有通道的状态和计数器
 */
void APU::reset() {
    // 清零所有脉冲波通道
    memset(&pulse[0], 0, sizeof(Pulse));
    memset(&pulse[1], 0, sizeof(Pulse));
    
    // 清零三角波通道
    memset(&triangle, 0, sizeof(Triangle));
    
    // 清零噪声通道，注意 LFSR 需要初始化为非零值
    memset(&noise, 0, sizeof(Noise));
    noise.shiftRegister = 1;  // LFSR 初始值必须非零，否则无法工作
    
    // 重置帧计数器
    frameMode = 0;
    frameIRQ = false;
    inhibitIRQ = false;
    frameCounter = 0;
    cpuClock = 0;
    lastSampledClock = 0;
    
    // 重置采样缓冲区
    sampleWritePos = 0;
    sampleReadPos = 0;
    sampleAccum = 0.0f;
    sampleCount = 0;
    cycleAccum = 0.0f;
    // NES CPU ~1.789773 MHz, audio sample rate 44100 Hz
    // cycles per sample = 1789773 / 44100 ≈ 40.58
    cyclesPerSample = 1789773.0f / (float)sampleRate;
}

// ============================================================================
// 寄存器访问
// ============================================================================

/**
 * 写入 APU 寄存器
 * 
 * 寄存器映射:
 * $4000-$4003: 脉冲波 1
 * $4004-$4007: 脉冲波 2
 * $4008-$400B: 三角波
 * $400C-$400F: 噪声
 * $4010-$4013: DMC (未实现)
 * $4015: 状态/启用寄存器
 * $4017: 帧计数器
 */
void APU::regWrite(uint16_t addr, uint8_t val) {
    switch (addr) {
        // ======================== 脉冲波 1 ($4000-$4003) ========================
        
        case 0x4000:
            // $4000: DDLC VVVV
            // DD: 占空比选择 (0-3)
            // L: 长度计数器停止 / 包络循环
            // C: 恒定音量标志
            // VVVV: 音量 / 包络周期
            pulse[0].duty = (val >> 6) & 0x03;
            pulse[0].lengthHalt = (val & 0x20) != 0;
            pulse[0].constantVolume = (val & 0x10) != 0;
            pulse[0].volume = val & 0x0F;
            break;
            
        case 0x4001:
            // $4001: EPPP NSSS
            // E: 扫频启用
            // PPP: 扫频周期
            // N: 负向扫频 (频率降低)
            // SSS: 移位量
            pulse[0].sweepEnabled = (val & 0x80) != 0;
            pulse[0].sweepPeriod = (val >> 4) & 0x07;
            pulse[0].sweepNegate = (val & 0x08) != 0;
            pulse[0].sweepShift = val & 0x07;
            pulse[0].sweepReload = true;
            break;
            
        case 0x4002:
            // $4002: TTTT TTTT
            // 定时器周期低 8 位
            pulse[0].timerPeriod = (pulse[0].timerPeriod & 0x700) | val;
            break;
            
        case 0x4003:
            // $4003: LLLL LTTT
            // LLLLL: 长度计数器加载索引
            // TTT: 定时器周期高 3 位
            pulse[0].timerPeriod = (pulse[0].timerPeriod & 0x0FF) | ((val & 0x07) << 8);
            if (pulse[0].enabled) {
                pulse[0].lengthCounter = lengthTable[(val >> 3) & 0x1F];
            }
            pulse[0].sequencePos = 0;       // 重置波形位置
            pulse[0].envelopeStart = true;  // 重新开始包络
            break;
            
        // ======================== 脉冲波 2 ($4004-$4007) ========================
        // 与脉冲波 1 相同的寄存器布局
        
        case 0x4004:
            pulse[1].duty = (val >> 6) & 0x03;
            pulse[1].lengthHalt = (val & 0x20) != 0;
            pulse[1].constantVolume = (val & 0x10) != 0;
            pulse[1].volume = val & 0x0F;
            break;
            
        case 0x4005:
            pulse[1].sweepEnabled = (val & 0x80) != 0;
            pulse[1].sweepPeriod = (val >> 4) & 0x07;
            pulse[1].sweepNegate = (val & 0x08) != 0;
            pulse[1].sweepShift = val & 0x07;
            pulse[1].sweepReload = true;
            break;
            
        case 0x4006:
            pulse[1].timerPeriod = (pulse[1].timerPeriod & 0x700) | val;
            break;
            
        case 0x4007:
            pulse[1].timerPeriod = (pulse[1].timerPeriod & 0x0FF) | ((val & 0x07) << 8);
            if (pulse[1].enabled) {
                pulse[1].lengthCounter = lengthTable[(val >> 3) & 0x1F];
            }
            pulse[1].sequencePos = 0;
            pulse[1].envelopeStart = true;
            break;
            
        // ======================== 三角波 ($4008-$400B) ========================
        
        case 0x4008:
            // $4008: CRRR RRRR
            // C: 控制标志 (也影响长度计数器)
            // RRRRRRR: 线性计数器加载值
            triangle.controlFlag = (val & 0x80) != 0;
            triangle.linearLoad = val & 0x7F;
            break;
            
        case 0x400A:
            // $400A: TTTT TTTT
            // 定时器周期低 8 位
            triangle.timerPeriod = (triangle.timerPeriod & 0x700) | val;
            break;
            
        case 0x400B:
            // $400B: LLLL LTTT
            // LLLLL: 长度计数器索引
            // TTT: 定时器周期高 3 位
            triangle.timerPeriod = (triangle.timerPeriod & 0x0FF) | ((val & 0x07) << 8);
            if (triangle.enabled) {
                triangle.lengthCounter = lengthTable[(val >> 3) & 0x1F];
            }
            triangle.linearReload = true;  // 设置线性计数器重载标志
            break;
            
        // ======================== 噪声 ($400C-$400F) ========================
        
        case 0x400C:
            // $400C: --LC VVVV
            // L: 长度计数器停止 / 包络循环
            // C: 恒定音量标志
            // VVVV: 音量 / 包络周期
            noise.lengthHalt = (val & 0x20) != 0;
            noise.constantVolume = (val & 0x10) != 0;
            noise.volume = val & 0x0F;
            break;
            
        case 0x400E:
            // $400E: M--- PPPP
            // M: 模式 (0: 长周期, 1: 短周期)
            // PPPP: 周期索引 (查表)
            noise.mode = (val & 0x80) != 0;
            noise.timerPeriod = noiseTable[val & 0x0F];
            break;
            
        case 0x400F:
            // $400F: LLLL L---
            // LLLLL: 长度计数器索引
            if (noise.enabled) {
                noise.lengthCounter = lengthTable[(val >> 3) & 0x1F];
            }
            noise.envelopeStart = true;
            break;
            
        // ======================== 状态寄存器 ($4015) ========================
        
        case 0x4015:
            // $4015: ---D NT21
            // D: DMC 启用
            // N: 噪声启用
            // T: 三角波启用
            // 2: 脉冲波 2 启用
            // 1: 脉冲波 1 启用
            pulse[0].enabled = (val & 0x01) != 0;
            pulse[1].enabled = (val & 0x02) != 0;
            triangle.enabled = (val & 0x04) != 0;
            noise.enabled = (val & 0x08) != 0;
            
            // 禁用通道时清除长度计数器
            if (!pulse[0].enabled) pulse[0].lengthCounter = 0;
            if (!pulse[1].enabled) pulse[1].lengthCounter = 0;
            if (!triangle.enabled) triangle.lengthCounter = 0;
            if (!noise.enabled) noise.lengthCounter = 0;
            break;
            
        // ======================== 帧计数器 ($4017) ========================
        
        case 0x4017:
            // $4017: MI-- ----
            // M: 模式 (0: 4步, 1: 5步)
            // I: 禁止 IRQ
            frameMode = (val >> 7) & 0x01;
            inhibitIRQ = (val & 0x40) != 0;
            if (inhibitIRQ) {
                frameIRQ = false;  // 清除 IRQ 标志
            }
            frameCounter = 0;
            
            // 5 步模式下立即执行一次时钟
            if (frameMode == 1) {
                quarterFrame();
                halfFrame();
            }
            break;
    }
}

/**
 * 读取 APU 寄存器
 * 只有 $4015 状态寄存器是可读的
 */
uint8_t APU::regRead(uint16_t addr) {
    if (addr == 0x4015) {
        // $4015 读取: IF-D NT21
        // I: 帧 IRQ 标志
        // F: DMC IRQ 标志
        // D: DMC 活动中
        // N: 噪声长度 > 0
        // T: 三角波长度 > 0
        // 2: 脉冲波 2 长度 > 0
        // 1: 脉冲波 1 长度 > 0
        uint8_t status = 0;
        if (pulse[0].lengthCounter > 0) status |= 0x01;
        if (pulse[1].lengthCounter > 0) status |= 0x02;
        if (triangle.lengthCounter > 0) status |= 0x04;
        if (noise.lengthCounter > 0) status |= 0x08;
        if (frameIRQ) status |= 0x40;
        
        // 读取后清除帧 IRQ 标志
        frameIRQ = false;
        return status;
    }
    return 0;
}

// ============================================================================
// 时钟函数
// ============================================================================

/**
 * 轻量级时钟：只累加 CPU 周期计数
 * 在 NES::step() 中调用，不执行实际的音频处理
 * @param cycles  CPU 周期数
 */
void APU::clockCycles(uint32_t cycles) {
    cpuClock += cycles;
}

/**
 * 内部时钟：执行一个 APU 时钟周期的所有定时器更新
 * 在 generateSamples() 中调用
 */
void APU::clockInternal() {
    // 三角波通道每 CPU 周期都要时钟
    triangle.clockTimer();
    
    // 其他通道和帧计数器每 2 个 CPU 周期时钟一次
    if ((lastSampledClock & 1) == 0) {
        pulse[0].clockTimer();
        pulse[1].clockTimer();
        noise.clockTimer();
        clockFrameCounter();
    }
}

/**
 * 生成音频样本（在音频任务中调用）
 * 追赶到当前 cpuClock 并生成对应的音频样本
 * @param outBuf      输出缓冲区
 * @param maxSamples  最大样本数
 * @return  实际生成的样本数
 */
int APU::generateSamples(int16_t* outBuf, int maxSamples) {
    int samplesGenerated = 0;
    
    // 追赶到当前 CPU 时钟
    while (lastSampledClock < cpuClock && samplesGenerated < maxSamples) {
        // 执行一个 APU 时钟周期
        clockInternal();
        lastSampledClock++;
        
        // 采样率转换
        cycleAccum += 1.0f;
        float output = getOutput();
        sampleAccum += output;
        sampleCount++;
        
        if (cycleAccum >= cyclesPerSample) {
            cycleAccum -= cyclesPerSample;
            
            // 取平均值作为这个采样点的值
            float avgSample = (sampleCount > 0) ? (sampleAccum / sampleCount) : 0.0f;
            
            // 转换为 16-bit 整数
            int16_t sample16 = (int16_t)(avgSample * 32767.0f);
            outBuf[samplesGenerated++] = sample16;
            
            // 重置累加器
            sampleAccum = 0.0f;
            sampleCount = 0;
        }
    }
    
    return samplesGenerated;
}

/**
 * APU 主时钟 (每 CPU 周期调用一次)
 * 使用 Anemoia 方式: 当缓冲区满时直接写入 I2S，I2S 阻塞会自然控制 APU 速度
 */
void APU::clock() {
    cpuClock++;
    
    // 三角波通道每 CPU 周期都要时钟
    triangle.clockTimer();
    
    // 其他通道和帧计数器每 2 个 CPU 周期时钟一次
    if ((cpuClock & 1) == 0) {
        pulse[0].clockTimer();
        pulse[1].clockTimer();
        noise.clockTimer();
        clockFrameCounter();
    }
    
    // 采样率转换 (Anemoia 方式)
    // APU 时钟频率 = CPU / 2 ≈ 894886 Hz
    // 每 894886 / 44100 ≈ 20.29 个 APU 时钟产生一个样本
    static uint32_t pulse_hz = 0;
    constexpr uint32_t SAMPLE_RATE = 44100;
    constexpr uint32_t THRESHOLD = 894886;  // APU 频率 (CPU / 2)
    
    pulse_hz += SAMPLE_RATE;
    if (pulse_hz > THRESHOLD) {
        pulse_hz -= THRESHOLD;
        
        // 获取各通道输出并混合 (类似 Anemoia 的简化混合)。
        // 先保留 NES 通道原本的单极性混音形态，再用慢速 DC 跟踪居中，
        // 比把每个方波通道强行改成正负摆动更不容易刺耳。
        uint16_t mixed = 0;
        
        // Pulse 1
        if (pulse[0].lengthCounter > 0 && pulse[0].output() > 0) {
            mixed += pulse[0].volume;
        }
        // Pulse 2
        if (pulse[1].lengthCounter > 0 && pulse[1].output() > 0) {
            mixed += pulse[1].volume;
        }
        // Triangle
        mixed += triangle.output();
        // Noise
        if (noise.lengthCounter > 0 && !(noise.shiftRegister & 0x01)) {
            mixed += noise.volume;
        }
        
        if (mixed > 255) mixed = 255;
        int32_t rawSample = (int32_t)mixed << 8;
        static int32_t dcEstimate = 0;
        dcEstimate += (rawSample - dcEstimate) >> 10;

        int32_t sample = (rawSample - dcEstimate) * APU_VOLUME_GAIN;
        constexpr int32_t SOFT_LIMIT = 18000;
        constexpr int32_t HARD_LIMIT = 30000;
        if (sample > SOFT_LIMIT) {
            sample = SOFT_LIMIT + (sample - SOFT_LIMIT) / 6;
        } else if (sample < -SOFT_LIMIT) {
            sample = -SOFT_LIMIT + (sample + SOFT_LIMIT) / 6;
        }
        if (sample > HARD_LIMIT) sample = HARD_LIMIT;
        if (sample < -HARD_LIMIT) sample = -HARD_LIMIT;

        static int32_t filteredSample = 0;
        filteredSample += (sample - filteredSample) >> 1;
        sample = filteredSample;
        //sample = (sample * volumeLevel) / 5;
        // 使用音量映射表，每一级更精细控制
        static const float VOLUME_MAP[6] = {
            0.0f,   // 0: 静音
            0.05f,  // 1: 5%
            0.10f,  // 2: 10%
            0.30f,  // 3: 30%
            0.50f,  // 4: 50%
            1.00f   // 5: 100%
		};
		sample = (int32_t)(sample * VOLUME_MAP[volumeLevel]);
        
        // 写入立体声缓冲区 (左右声道相同)
        uint16_t index = apu_buffer_index << 1;
        apu_audio_buffer[index] = (int16_t)sample;
        apu_audio_buffer[index + 1] = (int16_t)sample;
        
        apu_buffer_index++;
        if (apu_buffer_index >= APU_AUDIO_BUFFER_SIZE) {
            apu_buffer_index = 0;
            
            // 缓冲区满，写入 I2S (portMAX_DELAY 会阻塞直到 DMA 完成)
            // 这自然地控制了 APU 的时钟速度，与音频采样率同步
            static size_t dummy;
            i2s_write(I2S_NUM_0, apu_audio_buffer, sizeof(apu_audio_buffer), &dummy, portMAX_DELAY);
        }
    }
}

/**
 * 帧计数器时钟
 * 
 * 帧计数器控制包络和长度计数器的更新频率
 * 4 步模式: 产生约 240 Hz 的更新频率
 * 5 步模式: 产生约 192 Hz 的更新频率
 */
void APU::clockFrameCounter() {
    frameCounter++;
    
    if (frameMode == 0) {
        // 4 步模式 (产生 IRQ)
        // 周期: 3729, 7457, 11186, 14915 (APU 周期)
        switch (frameCounter) {
            case 3729:
                // 第 1 步: 包络和线性计数器
                quarterFrame();
                break;
            case 7457:
                // 第 2 步: 包络、线性计数器、长度计数器、扫频
                quarterFrame();
                halfFrame();
                break;
            case 11186:
                // 第 3 步: 包络和线性计数器
                quarterFrame();
                break;
            case 14915:
                // 第 4 步: 同上 + 可能产生 IRQ
                quarterFrame();
                halfFrame();
                if (!inhibitIRQ) frameIRQ = true;
                frameCounter = 0;  // 重置计数器
                break;
        }
    } else {
        // 5 步模式 (不产生 IRQ)
        switch (frameCounter) {
            case 3729:
                quarterFrame();
                break;
            case 7457:
                quarterFrame();
                halfFrame();
                break;
            case 11186:
                quarterFrame();
                break;
            case 14915:
                // 第 4 步什么都不做
                break;
            case 18641:
                // 第 5 步
                quarterFrame();
                halfFrame();
                frameCounter = 0;
                break;
        }
    }
}

/**
 * 四分之一帧: 更新包络和线性计数器
 * 频率约 240 Hz (4步模式) 或 192 Hz (5步模式)
 */
void APU::quarterFrame() {
    pulse[0].clockEnvelope();
    pulse[1].clockEnvelope();
    triangle.clockLinear();
    noise.clockEnvelope();
}

/**
 * 二分之一帧: 更新长度计数器和扫频
 * 频率约 120 Hz (4步模式) 或 96 Hz (5步模式)
 */
void APU::halfFrame() {
    pulse[0].clockLength();
    pulse[1].clockLength();
    triangle.clockLength();
    noise.clockLength();
    
    pulse[0].clockSweep(true);   // 脉冲波 1
    pulse[1].clockSweep(false);  // 脉冲波 2 (扫频计算略有不同)
}

// ============================================================================
// 音频输出
// ============================================================================

/**
 * 获取当前混合后的音频输出
 * 
 * NES 使用非线性混合器，但这里使用简化的线性近似
 * 
 * @return 混合后的音频样本 (约 -0.5 到 0.5)
 */
float APU::getOutput() {
    // 获取各通道原始输出 (0-15)，归一化到 0-1
    float p1 = pulse[0].output() / 15.0f;
    float p2 = pulse[1].output() / 15.0f;
    float tri = triangle.output() / 15.0f;
    float noi = noise.output() / 15.0f;
    
    // 使用 NES 混合器的近似公式
    // 原版使用查找表实现非线性混合
    float pulseOut = 0.00752f * (p1 + p2) * 15.0f;
    float tndOut = 0.00851f * tri * 15.0f + 0.00494f * noi * 15.0f;
    
    // 调整输出范围
    return (pulseOut + tndOut) * 2.0f - 0.5f;
}

void APU::getMixComponents(float &p1, float &p2, float &tri, float &noi) {
    p1 = pulse[0].output() / 15.0f;
    p2 = pulse[1].output() / 15.0f;
    tri = triangle.output() / 15.0f;
    noi = noise.output() / 15.0f;
}

// ============================================================================
// 脉冲波通道实现
// ============================================================================

/**
 * 获取脉冲波通道的当前输出值
 * 
 * 输出条件检查:
 * 1. 通道必须启用
 * 2. 长度计数器必须 > 0
 * 3. 定时器周期必须 >= 8 (避免超声波和杂音)
 * 4. 当前波形位置必须为高电平
 * 
 * @return 输出值 0-15
 */
uint8_t APU::Pulse::output() const {
    if (!enabled) return 0;
    if (lengthCounter == 0) return 0;
    if (timerPeriod < 8) return 0;  // 周期太小会产生杂音
    if (dutyTable[duty][sequencePos] == 0) return 0;  // 当前位置是低电平
    
    // 返回音量值
    if (constantVolume) {
        return volume;  // 恒定音量模式
    }
    return envelopeVolume;  // 包络衰减模式
}

/**
 * 时钟脉冲波定时器
 * 定时器控制波形的频率
 */
void APU::Pulse::clockTimer() {
    if (timerValue == 0) {
        // 定时器归零，重置并前进波形位置
        timerValue = timerPeriod;
        sequencePos = (sequencePos + 1) & 0x07;  // 0-7 循环
    } else {
        timerValue--;
    }
}

/**
 * 时钟包络发生器
 * 包络用于产生音量衰减效果 (如钢琴音色)
 */
void APU::Pulse::clockEnvelope() {
    if (envelopeStart) {
        // 收到重新开始信号，重置包络
        envelopeStart = false;
        envelopeVolume = 15;  // 从最大音量开始
        envelopeDivider = volume;  // 使用 volume 作为周期
    } else {
        // 正常时钟
        if (envelopeDivider == 0) {
            envelopeDivider = volume;  // 重置分频器
            if (envelopeVolume > 0) {
                // 音量衰减
                envelopeVolume--;
            } else if (lengthHalt) {
                // 循环模式: 回到最大音量
                envelopeVolume = 15;
            }
        } else {
            envelopeDivider--;
        }
    }
}


/**
 * 时钟长度计数器
 * 长度计数器控制声音的持续时间
 */
void APU::Pulse::clockLength() {
    // lengthHalt = 1 时停止计数 (声音持续播放)
    if (!lengthHalt && lengthCounter > 0) {
        lengthCounter--;
    }
}

/**
 * 时钟扫频单元
 * 扫频可以自动改变音调 (用于音效如激光或下落)
 * 
 * @param isChannel1  是否是脉冲波通道 1 (通道 1 的负向扫频略有不同)
 */
void APU::Pulse::clockSweep(bool isChannel1) {
    if (sweepReload) {
        // 收到重载信号
        if (sweepEnabled && sweepDivider == 0) {
            // 执行一次扫频
            uint16_t delta = timerPeriod >> sweepShift;
            if (sweepNegate) {
                // 负向扫频: 降低频率 (升高音调)
                timerPeriod -= delta;
                // 通道 1 额外减 1 (硬件怪癖)
                if (isChannel1) timerPeriod--;
            } else {
                // 正向扫频: 升高频率 (降低音调)
                timerPeriod += delta;
            }
        }
        sweepDivider = sweepPeriod;
        sweepReload = false;
    } else if (sweepDivider > 0) {
        sweepDivider--;
    } else {
        // 分频器归零，执行扫频
        sweepDivider = sweepPeriod;
        if (sweepEnabled && sweepShift > 0 && timerPeriod >= 8) {
            uint16_t delta = timerPeriod >> sweepShift;
            if (sweepNegate) {
                timerPeriod -= delta;
                if (isChannel1) timerPeriod--;
            } else if (timerPeriod + delta < 0x800) {
                // 只有当结果不会溢出时才增加
                timerPeriod += delta;
            }
        }
    }
}

// ============================================================================
// 三角波通道实现
// ============================================================================

/**
 * 获取三角波通道的当前输出值
 * 
 * 三角波没有音量控制，输出固定振幅的三角波
 */
uint8_t APU::Triangle::output() const {
    if (!enabled) return 0;
    if (lengthCounter == 0) return 0;
    if (linearCounter == 0) return 0;
    if (timerPeriod < 2) return 0;  // 过滤超声波 (避免失真)
    
    return triangleTable[sequencePos];
}

/**
 * 时钟三角波定时器
 * 三角波每 CPU 周期时钟，频率是其他通道的 2 倍
 */
void APU::Triangle::clockTimer() {
    if (timerValue == 0) {
        timerValue = timerPeriod;
        // 只有当两个计数器都 > 0 时才更新波形位置
        if (lengthCounter > 0 && linearCounter > 0) {
            sequencePos = (sequencePos + 1) & 0x1F;  // 0-31 循环
        }
    } else {
        timerValue--;
    }
}

/**
 * 时钟线性计数器
 * 线性计数器提供比长度计数器更细粒度的时间控制
 */
void APU::Triangle::clockLinear() {
    if (linearReload) {
        // 重载线性计数器
        linearCounter = linearLoad;
    } else if (linearCounter > 0) {
        linearCounter--;
    }
    
    // 如果控制标志清零，清除重载标志
    if (!controlFlag) {
        linearReload = false;
    }
}

/**
 * 时钟长度计数器
 */
void APU::Triangle::clockLength() {
    if (!controlFlag && lengthCounter > 0) {
        lengthCounter--;
    }
}

// ============================================================================
// 噪声通道实现
// ============================================================================

/**
 * 获取噪声通道的当前输出值
 * 
 * LFSR 的 bit 0 决定输出是静音还是有声
 */
uint8_t APU::Noise::output() const {
    if (!enabled) return 0;
    if (lengthCounter == 0) return 0;
    if ((shiftRegister & 0x01) != 0) return 0;  // bit 0 = 1 时静音
    
    if (constantVolume) {
        return volume;
    }
    return envelopeVolume;
}

/**
 * 时钟噪声定时器
 * 更新 LFSR (线性反馈移位寄存器) 产生伪随机序列
 */
void APU::Noise::clockTimer() {
    if (timerValue == 0) {
        timerValue = timerPeriod;
        
        // LFSR 反馈计算
        // 正常模式: bit 0 XOR bit 1
        // 短周期模式: bit 0 XOR bit 6
        uint8_t bit = mode ? 6 : 1;
        uint16_t feedback = (shiftRegister & 0x01) ^ ((shiftRegister >> bit) & 0x01);
        
        // 移位并将反馈放入最高位 (bit 14)
        shiftRegister = (shiftRegister >> 1) | (feedback << 14);
    } else {
        timerValue--;
    }
}

/**
 * 时钟包络发生器
 * 与脉冲波的包络相同
 */
void APU::Noise::clockEnvelope() {
    if (envelopeStart) {
        envelopeStart = false;
        envelopeVolume = 15;
        envelopeDivider = volume;
    } else {
        if (envelopeDivider == 0) {
            envelopeDivider = volume;
            if (envelopeVolume > 0) {
                envelopeVolume--;
            } else if (lengthHalt) {
                envelopeVolume = 15;  // 循环模式
            }
        } else {
            envelopeDivider--;
        }
    }
}

/**
 * 时钟长度计数器
 */
void APU::Noise::clockLength() {
    if (!lengthHalt && lengthCounter > 0) {
        lengthCounter--;
    }
}

// ============================================================================
// 音频采样缓冲区接口
// ============================================================================

/**
 * 获取缓冲区中可用的样本数量
 */
int APU::samplesAvailable() const {
    int available = sampleWritePos - sampleReadPos;
    if (available < 0) {
        available += SAMPLE_BUF_SIZE;
    }
    return available;
}

/**
 * 从缓冲区读取音频样本
 * @param buf   目标缓冲区
 * @param count 要读取的样本数
 * @return      实际读取的样本数
 */
int APU::readSamples(int16_t* buf, int count) {
    int read = 0;
    while (read < count && sampleReadPos != sampleWritePos) {
        buf[read++] = sampleBuf[sampleReadPos];
        sampleReadPos = (sampleReadPos + 1) % SAMPLE_BUF_SIZE;
    }
    return read;
}

// ============================================================================
// Save State
// ============================================================================

size_t APU::getStateSize() const {
    size_t size = 0;
    // Pulse channels x2
    size += 2 * (1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 1 + 1 + 1 + 1 + 1 + 1 + 1);
    // Triangle channel
    size += 1 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 1;
    // Noise channel
    size += 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 2;
    // Frame counter
    size += 1 + 1 + 1 + 2;
    return size;
}

void APU::saveState(uint8_t* buf, size_t& offset) const {
    // Pulse channel 1
    buf[offset++] = pulse[0].enabled ? 1 : 0;
    buf[offset++] = pulse[0].duty;
    buf[offset++] = pulse[0].lengthHalt ? 1 : 0;
    buf[offset++] = pulse[0].lengthCounter;
    buf[offset++] = pulse[0].constantVolume ? 1 : 0;
    buf[offset++] = pulse[0].volume;
    buf[offset++] = pulse[0].envelopeStart ? 1 : 0;
    buf[offset++] = pulse[0].envelopeVolume;
    buf[offset++] = pulse[0].envelopeDivider;
    buf[offset++] = pulse[0].timerPeriod & 0xFF;
    buf[offset++] = (pulse[0].timerPeriod >> 8) & 0xFF;
    buf[offset++] = pulse[0].timerValue & 0xFF;
    buf[offset++] = (pulse[0].timerValue >> 8) & 0xFF;
    buf[offset++] = pulse[0].sequencePos;
    buf[offset++] = pulse[0].sweepEnabled ? 1 : 0;
    buf[offset++] = pulse[0].sweepPeriod;
    buf[offset++] = pulse[0].sweepNegate ? 1 : 0;
    buf[offset++] = pulse[0].sweepShift;
    buf[offset++] = pulse[0].sweepReload ? 1 : 0;
    buf[offset++] = pulse[0].sweepDivider;
    
    // Pulse channel 2
    buf[offset++] = pulse[1].enabled ? 1 : 0;
    buf[offset++] = pulse[1].duty;
    buf[offset++] = pulse[1].lengthHalt ? 1 : 0;
    buf[offset++] = pulse[1].lengthCounter;
    buf[offset++] = pulse[1].constantVolume ? 1 : 0;
    buf[offset++] = pulse[1].volume;
    buf[offset++] = pulse[1].envelopeStart ? 1 : 0;
    buf[offset++] = pulse[1].envelopeVolume;
    buf[offset++] = pulse[1].envelopeDivider;
    buf[offset++] = pulse[1].timerPeriod & 0xFF;
    buf[offset++] = (pulse[1].timerPeriod >> 8) & 0xFF;
    buf[offset++] = pulse[1].timerValue & 0xFF;
    buf[offset++] = (pulse[1].timerValue >> 8) & 0xFF;
    buf[offset++] = pulse[1].sequencePos;
    buf[offset++] = pulse[1].sweepEnabled ? 1 : 0;
    buf[offset++] = pulse[1].sweepPeriod;
    buf[offset++] = pulse[1].sweepNegate ? 1 : 0;
    buf[offset++] = pulse[1].sweepShift;
    buf[offset++] = pulse[1].sweepReload ? 1 : 0;
    buf[offset++] = pulse[1].sweepDivider;
    
    // Triangle channel
    buf[offset++] = triangle.enabled ? 1 : 0;
    buf[offset++] = triangle.controlFlag ? 1 : 0;
    buf[offset++] = triangle.linearLoad;
    buf[offset++] = triangle.linearCounter;
    buf[offset++] = triangle.linearReload ? 1 : 0;
    buf[offset++] = triangle.lengthCounter;
    buf[offset++] = triangle.timerPeriod & 0xFF;
    buf[offset++] = (triangle.timerPeriod >> 8) & 0xFF;
    buf[offset++] = triangle.timerValue & 0xFF;
    buf[offset++] = (triangle.timerValue >> 8) & 0xFF;
    buf[offset++] = triangle.sequencePos;
    
    // Noise channel
    buf[offset++] = noise.enabled ? 1 : 0;
    buf[offset++] = noise.lengthHalt ? 1 : 0;
    buf[offset++] = noise.lengthCounter;
    buf[offset++] = noise.constantVolume ? 1 : 0;
    buf[offset++] = noise.volume;
    buf[offset++] = noise.envelopeStart ? 1 : 0;
    buf[offset++] = noise.envelopeVolume;
    buf[offset++] = noise.envelopeDivider;
    buf[offset++] = noise.mode ? 1 : 0;
    buf[offset++] = noise.timerPeriod & 0xFF;
    buf[offset++] = (noise.timerPeriod >> 8) & 0xFF;
    buf[offset++] = noise.timerValue & 0xFF;
    buf[offset++] = (noise.timerValue >> 8) & 0xFF;
    buf[offset++] = noise.shiftRegister & 0xFF;
    buf[offset++] = (noise.shiftRegister >> 8) & 0xFF;
    
    // Frame counter
    buf[offset++] = frameMode;
    buf[offset++] = frameIRQ ? 1 : 0;
    buf[offset++] = inhibitIRQ ? 1 : 0;
    buf[offset++] = frameCounter & 0xFF;
    buf[offset++] = (frameCounter >> 8) & 0xFF;
}

void APU::loadState(const uint8_t* buf, size_t& offset) {
    // Pulse channel 1
    pulse[0].enabled = buf[offset++] != 0;
    pulse[0].duty = buf[offset++];
    pulse[0].lengthHalt = buf[offset++] != 0;
    pulse[0].lengthCounter = buf[offset++];
    pulse[0].constantVolume = buf[offset++] != 0;
    pulse[0].volume = buf[offset++];
    pulse[0].envelopeStart = buf[offset++] != 0;
    pulse[0].envelopeVolume = buf[offset++];
    pulse[0].envelopeDivider = buf[offset++];
    pulse[0].timerPeriod = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    pulse[0].timerValue = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    pulse[0].sequencePos = buf[offset++];
    pulse[0].sweepEnabled = buf[offset++] != 0;
    pulse[0].sweepPeriod = buf[offset++];
    pulse[0].sweepNegate = buf[offset++] != 0;
    pulse[0].sweepShift = buf[offset++];
    pulse[0].sweepReload = buf[offset++] != 0;
    pulse[0].sweepDivider = buf[offset++];
    
    // Pulse channel 2
    pulse[1].enabled = buf[offset++] != 0;
    pulse[1].duty = buf[offset++];
    pulse[1].lengthHalt = buf[offset++] != 0;
    pulse[1].lengthCounter = buf[offset++];
    pulse[1].constantVolume = buf[offset++] != 0;
    pulse[1].volume = buf[offset++];
    pulse[1].envelopeStart = buf[offset++] != 0;
    pulse[1].envelopeVolume = buf[offset++];
    pulse[1].envelopeDivider = buf[offset++];
    pulse[1].timerPeriod = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    pulse[1].timerValue = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    pulse[1].sequencePos = buf[offset++];
    pulse[1].sweepEnabled = buf[offset++] != 0;
    pulse[1].sweepPeriod = buf[offset++];
    pulse[1].sweepNegate = buf[offset++] != 0;
    pulse[1].sweepShift = buf[offset++];
    pulse[1].sweepReload = buf[offset++] != 0;
    pulse[1].sweepDivider = buf[offset++];
    
    // Triangle channel
    triangle.enabled = buf[offset++] != 0;
    triangle.controlFlag = buf[offset++] != 0;
    triangle.linearLoad = buf[offset++];
    triangle.linearCounter = buf[offset++];
    triangle.linearReload = buf[offset++] != 0;
    triangle.lengthCounter = buf[offset++];
    triangle.timerPeriod = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    triangle.timerValue = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    triangle.sequencePos = buf[offset++];
    
    // Noise channel
    noise.enabled = buf[offset++] != 0;
    noise.lengthHalt = buf[offset++] != 0;
    noise.lengthCounter = buf[offset++];
    noise.constantVolume = buf[offset++] != 0;
    noise.volume = buf[offset++];
    noise.envelopeStart = buf[offset++] != 0;
    noise.envelopeVolume = buf[offset++];
    noise.envelopeDivider = buf[offset++];
    noise.mode = buf[offset++] != 0;
    noise.timerPeriod = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    noise.timerValue = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    noise.shiftRegister = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
    
    // Frame counter
    frameMode = buf[offset++];
    frameIRQ = buf[offset++] != 0;
    inhibitIRQ = buf[offset++] != 0;
    frameCounter = buf[offset] | (buf[offset + 1] << 8);
    offset += 2;
}
