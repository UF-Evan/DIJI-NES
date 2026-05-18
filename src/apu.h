/**
 * ============================================================================
 * APU - 音频处理单元 (Audio Processing Unit)
 * ============================================================================
 * 
 * NES 的 APU 是 Ricoh 2A03/2A07 芯片的一部分，负责生成游戏音乐和音效。
 * 
 * APU 包含 5 个声音通道:
 * 1. Pulse 1 (脉冲波 1): 方波发生器，可调占空比和扫频
 * 2. Pulse 2 (脉冲波 2): 与 Pulse 1 相同，但扫频稍有不同
 * 3. Triangle (三角波): 产生柔和的三角波，常用于低音和旋律
 * 4. Noise (噪声): 使用 LFSR 产生噪声，用于打击乐和音效
 * 5. DMC (增量调制): 播放采样音频 (本实现暂未支持)
 * 
 * 寄存器地址: $4000-$4017
 * 
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <cstddef>

class APU {
public:
    using DmcReadCallback = uint8_t(*)(void*, uint16_t);
    APU();
    ~APU();
    
    /**
     * 重置 APU 到初始状态
     * 清除所有通道的寄存器和计数器
     */
    void reset();
    
    // ========================================================================
    // CPU 接口
    // ========================================================================
    
    /**
     * 写入 APU 寄存器
     * @param addr  寄存器地址 ($4000-$4017)
     * @param val   要写入的值
     */
    void regWrite(uint16_t addr, uint8_t val);
    
    /**
     * 读取 APU 寄存器
     * @param addr  寄存器地址 (通常只读 $4015 状态寄存器)
     * @return      寄存器值
     */
    uint8_t regRead(uint16_t addr);
    
    /**
     * 时钟 APU (每 CPU 周期调用一次)
     * 更新所有通道的定时器和计数器
     */
    void clock();
    
    /**
     * 轻量级时钟：只更新周期计数（用于 CPU 同步）
     * 不生成音频样本，开销更低
     * @param cycles  CPU 周期数
     */
    void clockCycles(uint32_t cycles);
    
    /**
     * 生成音频样本（在音频任务中调用）
     * 根据累积的 CPU 周期生成对应数量的音频样本
     * @param outBuf   输出缓冲区
     * @param maxSamples  最大样本数
     * @return  实际生成的样本数
     */
    int generateSamples(int16_t* outBuf, int maxSamples);
    
    /**
     * 获取当前混合后的音频输出
     * @return  音频样本值 (约 -0.5 到 0.5 范围)
     */
    float getOutput();
    // Debug: 获取各通道的归一化输出 (0..1)
    void getMixComponents(float &p1, float &p2, float &tri, float &noi);
    
    /**
     * 设置音频采样率 (用于可能的重采样)
     * @param rate  采样率 (Hz)
     */
    void setSampleRate(int rate) { sampleRate = rate; }

    /**
     * 获取可用的音频样本数量
     */
    int samplesAvailable() const;
    
    /**
     * 读取音频样本到缓冲区
     * @param buf   目标缓冲区
     * @param count 要读取的样本数
     * @return      实际读取的样本数
     */
    int readSamples(int16_t* buf, int count);
    
    // Compatibility hooks used by newer NES core integration.
    void setDmcMemoryReadCallback(DmcReadCallback cb, void* ctx) { (void)cb; (void)ctx; }
    bool irqPending() const { return false; }
    
    /**
     * Save State 接口
     */
    void saveState(uint8_t* buf, size_t& offset) const;
    void loadState(const uint8_t* buf, size_t& offset);
    size_t getStateSize() const;

private:
    // 音频采样相关
    static constexpr int SAMPLE_BUF_SIZE = 2048;
    int16_t sampleBuf[SAMPLE_BUF_SIZE];
    volatile int sampleWritePos = 0;
    volatile int sampleReadPos = 0;
    float sampleAccum = 0.0f;  // 用于下采样的累加器
    int sampleCount = 0;       // 累加的样本数
    float cyclesPerSample = 0; // CPU cycles per audio sample
    float cycleAccum = 0;      // 周期累加器
    int sampleRate = 44100;   // 音频采样率
    volatile uint64_t cpuClock = 0;         // CPU 时钟计数器（主循环累加）
    volatile uint64_t lastSampledClock = 0; // 上次采样时的 CPU 时钟
    
    // 内部方法：执行一个 APU 时钟周期的所有定时器更新
    void clockInternal();
    
    // ========================================================================
    // 脉冲波通道 (Pulse Channel)
    // ========================================================================
    /**
     * 脉冲波 (方波) 发生器
     * 
     * 特性:
     * - 可选 4 种占空比: 12.5%, 25%, 50%, 75%
     * - 包络发生器: 控制音量衰减
     * - 扫频单元: 自动调整频率 (用于音效)
     * - 长度计数器: 控制声音持续时间
     * 
     * 寄存器:
     * - $4000/$4004: 占空比, 包络
     * - $4001/$4005: 扫频
     * - $4002/$4006: 定时器低 8 位
     * - $4003/$4007: 长度计数器, 定时器高 3 位
     */
    struct Pulse {
        bool enabled = false;           // 通道启用状态 (由 $4015 控制)
        
        // --- 波形控制 ---
        uint8_t duty = 0;               // 占空比选择 (0-3)
        
        // --- 长度计数器 ---
        bool lengthHalt = false;        // 停止长度计数 (=1: 声音不会自动停止)
        uint8_t lengthCounter = 0;      // 长度计数器值
        
        // --- 包络 (音量控制) ---
        bool constantVolume = false;    // 使用恒定音量 (=1) 或包络衰减 (=0)
        uint8_t volume = 0;             // 恒定音量值 / 包络周期
        bool envelopeStart = false;     // 包络重新开始标志
        uint8_t envelopeVolume = 0;     // 当前包络音量 (0-15)
        uint8_t envelopeDivider = 0;    // 包络分频器
        
        // --- 定时器 (频率控制) ---
        uint16_t timerPeriod = 0;       // 定时器周期 (11位, 决定频率)
        uint16_t timerValue = 0;        // 当前定时器值
        uint8_t sequencePos = 0;        // 波形序列位置 (0-7)
        
        // --- 扫频单元 ---
        bool sweepEnabled = false;      // 扫频启用
        uint8_t sweepPeriod = 0;        // 扫频周期
        bool sweepNegate = false;       // 负向扫频 (频率降低)
        uint8_t sweepShift = 0;         // 扫频移位量
        bool sweepReload = false;       // 扫频重载标志
        uint8_t sweepDivider = 0;       // 扫频分频器
        
        uint8_t output() const;                 // 获取当前输出值 (0-15)
        void clockTimer();                      // 时钟定时器
        void clockEnvelope();                   // 时钟包络
        void clockLength();                     // 时钟长度计数器
        void clockSweep(bool isChannel1);       // 时钟扫频单元
    } pulse[2];  // 两个脉冲波通道
    
    // ========================================================================
    // 三角波通道 (Triangle Channel)
    // ========================================================================
    /**
     * 三角波发生器
     * 
     * 特性:
     * - 输出 32 级三角波 (0-15-0 的序列)
     * - 没有音量控制 (始终全音量)
     * - 线性计数器: 控制声音持续时间
     * - 通常用于低音声部
     * 
     * 寄存器:
     * - $4008: 线性计数器
     * - $400A: 定时器低 8 位
     * - $400B: 长度计数器, 定时器高 3 位
     */
    struct Triangle {
        bool enabled = false;           // 通道启用状态
        
        // --- 线性计数器 ---
        bool controlFlag = false;       // 控制标志 (也用于长度计数器停止)
        uint8_t linearLoad = 0;         // 线性计数器加载值
        uint8_t linearCounter = 0;      // 当前线性计数器值
        bool linearReload = false;      // 线性计数器重载标志
        
        // --- 长度计数器 ---
        uint8_t lengthCounter = 0;      // 长度计数器值
        
        // --- 定时器 ---
        uint16_t timerPeriod = 0;       // 定时器周期
        uint16_t timerValue = 0;        // 当前定时器值
        uint8_t sequencePos = 0;        // 波形序列位置 (0-31)
        
        uint8_t output() const;         // 获取当前输出值 (0-15)
        void clockTimer();              // 时钟定时器
        void clockLinear();             // 时钟线性计数器
        void clockLength();             // 时钟长度计数器
    } triangle;
    
    // ========================================================================
    // 噪声通道 (Noise Channel)
    // ========================================================================
    /**
     * 噪声发生器
     * 
     * 特性:
     * - 使用 15 位线性反馈移位寄存器 (LFSR) 产生伪随机噪声
     * - 两种模式: 长周期 (32767 步) 或短周期 (93 步)
     * - 包络发生器控制音量
     * - 常用于打击乐 (鼓、镲等) 和爆炸音效
     * 
     * 寄存器:
     * - $400C: 包络
     * - $400E: 模式, 周期
     * - $400F: 长度计数器
     */
    struct Noise {
        bool enabled = false;           // 通道启用状态
        
        // --- 长度计数器 ---
        bool lengthHalt = false;        // 停止长度计数
        uint8_t lengthCounter = 0;      // 长度计数器值
        
        // --- 包络 ---
        bool constantVolume = false;    // 恒定音量模式
        uint8_t volume = 0;             // 音量/包络周期
        bool envelopeStart = false;     // 包络重新开始
        uint8_t envelopeVolume = 0;     // 当前包络音量
        uint8_t envelopeDivider = 0;    // 包络分频器
        
        // --- 噪声生成器 ---
        bool mode = false;              // 模式 (0: 长周期, 1: 短周期)
        uint16_t timerPeriod = 0;       // 定时器周期
        uint16_t timerValue = 0;        // 当前定时器值
        uint16_t shiftRegister = 1;     // 15位 LFSR (初始值必须非零)
        
        uint8_t output() const;         // 获取当前输出值 (0-15)
        void clockTimer();              // 时钟定时器
        void clockEnvelope();           // 时钟包络
        void clockLength();             // 时钟长度计数器
    } noise;
    
    // ========================================================================
    // 帧计数器 (Frame Counter)
    // ========================================================================
    /**
     * 帧计数器控制包络、长度计数器和扫频的时钟频率
     * 
     * 两种模式:
     * - 4 步模式: 每帧 4 次中断，约 240 Hz (可产生 IRQ)
     * - 5 步模式: 每帧 5 次中断，约 192 Hz (不产生 IRQ)
     */
    uint8_t frameMode = 0;              // 帧模式 (0: 4步, 1: 5步)
    bool frameIRQ = false;              // 帧 IRQ 标志
    bool inhibitIRQ = false;            // 禁止 IRQ
    uint16_t frameCounter = 0;          // 帧计数器
    
    void clockFrameCounter();           // 时钟帧计数器
    void quarterFrame();                // 1/4 帧: 包络和线性计数器
    void halfFrame();                   // 1/2 帧: 长度计数器和扫频
    
    // ========================================================================
    // 查找表
    // ========================================================================
    
    static const uint8_t lengthTable[32];   // 长度计数器加载值表
    static const uint16_t noiseTable[16];   // 噪声周期表 (NTSC)
    static const uint8_t dutyTable[4][8];   // 脉冲波占空比序列
    static const uint8_t triangleTable[32]; // 三角波序列
};
