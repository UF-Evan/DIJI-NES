#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <String.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nes.h"
#include "lgfx_conf.h"
#include "driver/i2s.h"
#include "esp_err.h"
#include "esp_timer.h"

// 串口调试开关
#ifndef ENABLE_DEBUG_SERIAL
#define ENABLE_DEBUG_SERIAL false
#endif

// ================ 菜单颜色配置 (高级灰色调) ================
#define MENU_BG_COLOR       0x2104  // 深灰背景 (RGB: 32, 32, 32)
#define MENU_HEADER_COLOR   0x4A69  // 中灰标题背景 (RGB: 72, 77, 72)
#define MENU_TEXT_COLOR     0xC618  // 浅灰文字 (RGB: 192, 192, 192)
#define MENU_HIGHLIGHT_BG   0xFDE0  // 选中项背景 (RGB: 255, 255, 0)
#define MENU_ARROW_COLOR    0xAD75  // 箭头颜色 (RGB: 168, 174, 168)
#define MENU_HINT_COLOR     0x7BCF  // 提示文字颜色 (RGB: 120, 120, 120)
#define MENU_TITLE_COLOR    0xE71C  // 标题文字 (RGB: 224, 224, 224)
#define MENU_BORDER_COLOR   0x52AA  // 边框颜色 (RGB: 80, 85, 80)
#define PAUSE_OVERLAY_COLOR 0x18C3  // 暂停遮罩 (深色半透明效果)

// ================ 菜单状态 ================
enum AppState {
    STATE_MENU,     // 主菜单
    STATE_PLAYING,  // 游戏中
    STATE_PAUSED    // 暂停菜单
};

static AppState currentState = STATE_MENU;
static std::vector<String> romList;       // ROM 文件列表
static int selectedIndex = 0;             // 当前选中的游戏索引
static int scrollOffset = 0;              // 滚动偏移
static const int ITEMS_PER_PAGE = 8;      // 每页显示的游戏数量
static int pauseMenuIndex = 0;            // 暂停菜单选项索引

// ROM 文件名可能包含 UTF-8 中文；默认 Font0 不含中文字形。
static const lgfx::IFont* MENU_ROM_FONT = &fonts::efontCN_16;
static const int MENU_ROM_NAME_MAX_WIDTH = 238;

// 按键防抖
static unsigned long lastButtonTime = 0;
static const unsigned long BUTTON_DEBOUNCE = 200;  // 200ms防抖

#if ENABLE_DEBUG_SERIAL
#define FPS_PRINT(...) Serial.printf(__VA_ARGS__)
#else
#define FPS_PRINT(...) ((void)0)
#endif

// ================ PIN定义 ================
// SD卡引脚
#define SD_CS_PIN     42
#define SD_SCLK_PIN   40
#define SD_MISO_PIN   39
#define SD_MOSI_PIN   41
#define SD_FREQ       10000000  // 10 MHz

// 游戏控制器按键
#define A_BUTTON      48
#define B_BUTTON      47
#define LEFT_BUTTON   8
#define RIGHT_BUTTON  18
#define UP_BUTTON     17
#define DOWN_BUTTON   3
#define START_BUTTON  15
#define SELECT_BUTTON 16

// I2S / APU -> MAX98357A (I2S DAC)
#define I2S_BCLK_PIN 5
#define I2S_LRCLK_PIN 4
#define I2S_DATA_PIN 6

// 音频参数
constexpr int AUDIO_SAMPLE_RATE = 44100;
constexpr int I2S_NUM = 0;

// ================ 全局变量 ================
NES nes;
LGFX tft;

// 屏幕参数
constexpr int SCREEN_WIDTH  = 256;
constexpr int SCREEN_HEIGHT = 240;
constexpr int TFT_OFFSET_X  = (320 - SCREEN_WIDTH) / 2; // 横向居中
constexpr int OVERSCAN_CROP_X = 4;  // 隐藏 LCD 上可见的横向卷轴边缘接缝
constexpr int DISPLAY_WIDTH = SCREEN_WIDTH - OVERSCAN_CROP_X * 2;
// 每个块的行数（用 DMA 一次推多行以减少 setAddrWindow/wait 开销）
// 8 行 = 30 次 DMA/帧，60 行 = 4 次 DMA/帧，120 行 = 2 次 DMA/帧
constexpr int BLOCK_LINES = 60;  // 增大到 60 行，240/60=4 次 DMA 每帧
constexpr int DISPLAY_BLOCK_LINES = (OVERSCAN_CROP_X > 0) ? 16 : BLOCK_LINES;

// FPS 统计变量
static uint32_t last_emulation_us = 0;  // 最近一次仿真帧耗时（微秒）
static uint32_t fps_count = 0;          // 已完成的仿真帧计数
static uint32_t fps_last_ms = 0;        // 上次打印 FPS 的时间戳
static uint32_t last_dma_us = 0;        // DMA 传输耗时
static uint32_t game_start_ms = 0;      // 当前游戏启动时间，用于启动失败保护
static uint32_t last_rendered_ms = 0;   // 最近一次成功入队渲染帧的时间

// Separate SPI bus for SD so it cannot reconfigure/conflict with the TFT SPI bus.
// If your TFT_eSPI setup uses HSPI, keep SD on FSPI.
SPIClass sdSPI(FSPI);

// 双缓冲：用于无撕裂推屏
static uint16_t* frame_buf[2] = {nullptr, nullptr};
static uint16_t* display_crop_buf = nullptr;
static volatile uint8_t render_buf_idx = 0;
// 记录最后一次被显示的缓冲索引（用于在跳帧时复用上一帧以避免闪烁）
static volatile uint8_t last_displayed_idx = 0;

static QueueHandle_t frame_queue = nullptr;

static void initializeAudio();
static void apu_task(void* arg);
static void muteAudio();

static bool gameJustEntered = false;
// 游戏暂停状态 - APU 任务使用
static volatile bool gameRunning = false;
static bool sdCardAvailable = false;  // SD 卡是否可用

// 帧同步
const uint32_t FRAME_TIME_US = 16667;  // ~60 FPS (1000000 / 60)
const int CPU_CYCLES_PER_FRAME = 29780; // NES: 1.79MHz / 60fps ≈ 29780 cycles

// 抽帧开关: true=启用抽帧(性能优先), false=每帧都渲染(画面优先)
// SMB1 等游戏会用隔帧闪烁表现受伤/无敌，使用奇数周期跳帧避免锁相。
static bool ENABLE_FRAMESKIP = true;
static uint64_t next_frame_us = 0;
static uint8_t force_render_frames = 0;
static uint8_t consecutive_skipped_frames = 0;
static uint8_t frameskip_phase = 0;

struct ButtonState {
    uint8_t A = 0;
    uint8_t B = 0;
    uint8_t LEFT = 0;
    uint8_t RIGHT = 0;
    uint8_t UP = 0;
    uint8_t DOWN = 0;
    uint8_t START = 0;
    uint8_t SELECT = 0;
} buttons;

// ================ 函数前向声明 ================
void updateButtons();
void runFrame();
void scanROMFiles();
void drawMainMenu();
void drawMenuList();
void drawPauseMenu();
void handleMenuInput();
void handlePauseInput();
bool loadSelectedROM();
void returnToMainMenu();
void clearScreenForGame();
bool tryInitSD();  // 尝试初始化 SD 卡

static void resetFrameScheduler(uint8_t forceRenderFrames = 2) {
    next_frame_us = 0;
    force_render_frames = forceRenderFrames;
    consecutive_skipped_frames = 0;
    frameskip_phase = 0;
    nes.requestFrameSkip(false);
}

// 从 ROM 路径生成 Save State 路径 (将 .nes 替换为 .sav)
static void getSaveStatePath(char* savePath, size_t maxLen) {
    const char* romPath = nes.getCurrentRomPath();
    strncpy(savePath, romPath, maxLen - 1);
    savePath[maxLen - 1] = '\0';
    
    // 查找最后一个 '.' 并替换扩展名
    char* dot = strrchr(savePath, '.');
    if (dot && (dot - savePath + 5) < (int)maxLen) {
        strcpy(dot, ".sav");
    } else {
        // 没有扩展名，直接追加
        strncat(savePath, ".sav", maxLen - strlen(savePath) - 1);
    }
}

// ================ 初始化函数 ================
void initializeSerial() {
#if ENABLE_DEBUG_SERIAL
    Serial.begin(115200);
    delay(500);
    
#else
    (void)0;
#endif
}

void initializeScreen() {
    tft.init();
    tft.setRotation(3);  // 横屏模式
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(0, 0);
    
    // 分配双缓冲（用于无撕裂推屏）
    for (int i = 0; i < 2; i++) {
        frame_buf[i] = (uint16_t*)heap_caps_malloc(
            SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
        );
        if (frame_buf[i]) {
            memset(frame_buf[i], 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
        }
    }

    display_crop_buf = (uint16_t*)heap_caps_malloc(
        DISPLAY_WIDTH * DISPLAY_BLOCK_LINES * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
    );
}

// 在 loop() 中调用：检查帧完成并入队
static bool tryEnqueueFrame() {
    PPU& ppu = nes.getPPU();
    if (!ppu.frameReady) return false;

    // 如果本帧没有渲染（跳帧），不入队，直接清除标志
    if (!ppu.renderedThisFrame) {
        ppu.frameReady = false;
        return false;
    }

    uint8_t send_idx = render_buf_idx;
    // 如果本帧有实际渲染，发送当前渲染缓冲并切换到另一个用于下一帧渲染
    if (xQueueSend(frame_queue, &send_idx, 0) == pdTRUE) {
        // 切换到另一缓冲用于下一帧渲染
        render_buf_idx = 1 - render_buf_idx;
        ppu.frameBuffer = frame_buf[render_buf_idx];
        ppu.frameReady = false;
        last_rendered_ms = millis();
        return true;
    }
    return false;
}

// display_task：只负责 DMA 推送已渲染的帧缓冲
static void display_task(void* arg) {
    uint8_t buf_idx;
    for (;;) {
        // 等待已渲染的缓冲索引
        if (xQueueReceive(frame_queue, &buf_idx, portMAX_DELAY) != pdTRUE)
            continue;
        
        uint32_t t0 = micros();
        uint16_t* buf = frame_buf[buf_idx];
        
        tft.startWrite();
        // 分块 DMA 推送（不渲染，直接推送已渲染的缓冲）
        for (int baseY = 0; baseY < SCREEN_HEIGHT; baseY += DISPLAY_BLOCK_LINES) {
            int h = SCREEN_HEIGHT - baseY;
            if (h > DISPLAY_BLOCK_LINES) h = DISPLAY_BLOCK_LINES;
            if (display_crop_buf) {
                for (int row = 0; row < h; row++) {
                    memcpy(display_crop_buf + row * DISPLAY_WIDTH,
                           buf + (baseY + row) * SCREEN_WIDTH + OVERSCAN_CROP_X,
                           DISPLAY_WIDTH * sizeof(uint16_t));
                }
                tft.setAddrWindow(TFT_OFFSET_X + OVERSCAN_CROP_X, baseY, DISPLAY_WIDTH, h);
                tft.pushPixelsDMA(display_crop_buf, DISPLAY_WIDTH * h);
                tft.waitDMA();
            } else {
                for (int row = 0; row < h; row++) {
                    tft.setAddrWindow(TFT_OFFSET_X + OVERSCAN_CROP_X, baseY + row, DISPLAY_WIDTH, 1);
                    tft.pushPixelsDMA(buf + (baseY + row) * SCREEN_WIDTH + OVERSCAN_CROP_X, DISPLAY_WIDTH);
                    tft.waitDMA();
                }
            }
        }
        tft.endWrite();
        // 记录最后一次显示的缓冲索引
        last_displayed_idx = buf_idx;
        last_dma_us = micros() - t0;

        // DMA 推屏会在 CPU0 上连续占用较久；明确让出一小段时间，
        // 避免稳定 60FPS 场景下 IDLE0 长时间得不到运行而触发 task WDT。
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// 行级帧调度（方法1使用）
void runFrame() {
    const int SCANLINES_PER_FRAME = 262;
    for (int i = 0; i < SCANLINES_PER_FRAME; ++i) {
        nes.stepScanline();
    }
}

void initializeButtons() {
    // 需要根据实际硬件布局调整这些引脚
    pinMode(A_BUTTON, INPUT_PULLUP);
    pinMode(B_BUTTON, INPUT_PULLUP);
    pinMode(LEFT_BUTTON, INPUT_PULLUP);
    pinMode(RIGHT_BUTTON, INPUT_PULLUP);
    pinMode(UP_BUTTON, INPUT_PULLUP);
    pinMode(DOWN_BUTTON, INPUT_PULLUP);
    pinMode(START_BUTTON, INPUT_PULLUP);
    pinMode(SELECT_BUTTON, INPUT_PULLUP);
}

void updateButtons() {
    // 直接读取按键状态 (INPUT_PULLUP: 按下=0, 松开=1)
    buttons.A      = !digitalRead(A_BUTTON);
    buttons.B      = !digitalRead(B_BUTTON);
    buttons.LEFT   = !digitalRead(LEFT_BUTTON);
    buttons.RIGHT  = !digitalRead(RIGHT_BUTTON);
    buttons.UP     = !digitalRead(UP_BUTTON);
    buttons.DOWN   = !digitalRead(DOWN_BUTTON);
    buttons.START  = !digitalRead(START_BUTTON);
    buttons.SELECT = !digitalRead(SELECT_BUTTON);
}

// ================ 清除屏幕边缘，进入游戏前调用 ================
void clearScreenForGame() {
    // 屏幕 320x240, 游戏区域居中显示，左右 overscan 裁边保持黑色
    
    // 先停止 DMA，确保不会覆盖我们的清屏操作
    tft.waitDMA();
    
    // 直接清除左右边缘区域（这些区域 DMA 不会触碰）
    tft.startWrite();
    tft.fillRect(0, 0, TFT_OFFSET_X + OVERSCAN_CROP_X, 240, TFT_BLACK);
    tft.fillRect(TFT_OFFSET_X + OVERSCAN_CROP_X, 0, DISPLAY_WIDTH, 240, TFT_BLACK);
    tft.fillRect(TFT_OFFSET_X + SCREEN_WIDTH - OVERSCAN_CROP_X, 0,
                 TFT_OFFSET_X + OVERSCAN_CROP_X, 240, TFT_BLACK);
    tft.endWrite();
    
    // 同时清空帧缓冲区，防止 DMA 任务推送旧数据
    if (frame_buf[0]) {
        memset(frame_buf[0], 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
    }
    if (frame_buf[1]) {
        memset(frame_buf[1], 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
    }
    
    // 清空帧队列中的待处理帧
    uint8_t dummy;
    while (xQueueReceive(frame_queue, &dummy, 0) == pdTRUE) {
        // 清空队列
    }
}

bool tryInitSD() {
    // 使用独立 SPI 总线初始化 SD
    sdSPI.begin(SD_SCLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

    if (!SD.begin(SD_CS_PIN, sdSPI, SD_FREQ)) {
        Serial.println("SD card init failed or not inserted");
        sdCardAvailable = false;
        return false;
    }
    
    Serial.println("SD card initialized");
    sdCardAvailable = true;
    return true;
}

void initializeSD() {
    tryInitSD();
}

// ================ ROM 文件扫描 ================
void scanROMFiles() {
    romList.clear();
    
    File root = SD.open("/");
    if (!root) {
        Serial.println("Failed to open root directory");
        return;
    }
    
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        
        if (!entry.isDirectory()) {
            String filename = entry.name();
            
            // 跳过 macOS 元数据文件 (以 ._ 开头)
            String basename = filename;
            int lastSlash = filename.lastIndexOf('/');
            if (lastSlash >= 0) {
                basename = filename.substring(lastSlash + 1);
            }
            if (basename.startsWith("._")) {
                entry.close();
                continue;
            }
            
            // 检查是否为 .nes 文件
            if (filename.endsWith(".nes") || filename.endsWith(".NES") || 
                filename.endsWith(".Nes")) {
                // 确保路径以 / 开头
                if (!filename.startsWith("/")) {
                    filename = "/" + filename;
                }
                romList.push_back(filename);
                Serial.printf("Found ROM: %s\n", filename.c_str());
            }
        }
        entry.close();
    }
    root.close();
    
    Serial.printf("Total ROMs found: %d\n", romList.size());
    
    // 排序 ROM 列表
    std::sort(romList.begin(), romList.end());
}

static int nextUtf8CharIndex(const String& text, int index) {
    const int length = text.length();
    if (index >= length) return length;

    uint8_t lead = (uint8_t)text[index];
    int charBytes = 1;
    if ((lead & 0xE0) == 0xC0) {
        charBytes = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        charBytes = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        charBytes = 4;
    }

    if (index + charBytes > length) return length;
    for (int i = 1; i < charBytes; i++) {
        if (((uint8_t)text[index + i] & 0xC0) != 0x80) return index + 1;
    }
    return index + charBytes;
}

static String trimTextToPixelWidth(const String& text, int maxWidth, const lgfx::IFont* font) {
    if (tft.textWidth(text, font) <= maxWidth) return text;

    const String ellipsis = "...";
    int ellipsisWidth = tft.textWidth(ellipsis, font);
    if (ellipsisWidth >= maxWidth) return "";

    String result;
    for (int i = 0; i < text.length();) {
        int next = nextUtf8CharIndex(text, i);
        String candidate = result + text.substring(i, next) + ellipsis;
        if (tft.textWidth(candidate, font) > maxWidth) break;

        result += text.substring(i, next);
        i = next;
    }

    return result + ellipsis;
}

static String getROMDisplayName(const String& romPath) {
    String displayName = romPath;
    int lastSlash = displayName.lastIndexOf('/');
    if (lastSlash >= 0) {
        displayName = displayName.substring(lastSlash + 1);
    }

    int dotPos = displayName.lastIndexOf('.');
    if (dotPos > 0) {
        displayName = displayName.substring(0, dotPos);
    }

    return trimTextToPixelWidth(displayName, MENU_ROM_NAME_MAX_WIDTH, MENU_ROM_FONT);
}

static void drawROMDisplayName(const String& romPath, int x, int y, uint16_t color) {
    tft.setTextSize(1);
    tft.setTextColor(color);
    tft.drawString(getROMDisplayName(romPath), x, y, MENU_ROM_FONT);
}

// ================ 主菜单绘制 ================
void drawMainMenu() {
    tft.fillScreen(MENU_BG_COLOR);
    
    // ===== 头部标题 =====
    tft.fillRect(0, 0, 320, 36, MENU_HEADER_COLOR);
    tft.drawRect(0, 0, 320, 36, MENU_BORDER_COLOR);
    
    // 绘制 DiJi-NES 标题
    tft.setTextColor(MENU_TITLE_COLOR);
    tft.setTextSize(2);
    tft.setCursor(105, 10);
    tft.print("DIJI-NES");
    
    // 装饰线
    tft.drawFastHLine(20, 34, 280, MENU_BORDER_COLOR);
    
    // ===== 游戏列表区域 =====
    int listStartY = 44;
    int itemHeight = 20;
    int listWidth = 280;
    int listX = 20;
    
    // 绘制列表边框
    tft.drawRect(listX - 2, listStartY - 2, listWidth + 4, ITEMS_PER_PAGE * itemHeight + 4, MENU_BORDER_COLOR);
    
    if (romList.empty()) {
        tft.setTextColor(MENU_HINT_COLOR);
        tft.setTextSize(1);
        if (!sdCardAvailable) {
            // SD 卡未插入
            tft.setCursor(60, listStartY + 40);
            tft.print("No SD card detected");
            tft.setCursor(40, listStartY + 60);
            tft.print("Please insert SD card with");
            tft.setCursor(40, listStartY + 75);
            tft.print(".nes ROM files");
            tft.setCursor(50, listStartY + 100);
            tft.setTextColor(MENU_ARROW_COLOR);
            tft.print("Press A to retry");
        } else {
            // SD 卡已插入但没有 ROM
            tft.setCursor(80, listStartY + 60);
            tft.print("No ROM files found on SD card");
            tft.setCursor(90, listStartY + 80);
            tft.print("Please add .nes files");
        }
    } else {
        // 计算分页信息
        int totalPages = (romList.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
        int currentPage = scrollOffset / ITEMS_PER_PAGE + 1;
        
        tft.setTextSize(1);
        
        for (int i = 0; i < ITEMS_PER_PAGE; i++) {
            int romIndex = scrollOffset + i;
            if (romIndex >= (int)romList.size()) break;
            
            int itemY = listStartY + i * itemHeight;
            
            // 高亮选中项
            if (romIndex == selectedIndex) {
                tft.fillRect(listX, itemY, listWidth, itemHeight - 1, MENU_HIGHLIGHT_BG);
                
                // 右侧箭头指示器
                tft.setTextColor(MENU_ARROW_COLOR);
                tft.setCursor(listX + listWidth - 20, itemY + 6);
                tft.print("<");
                
                // 左侧也加箭头
                tft.setCursor(listX + 4, itemY + 6);
                tft.print(">");
                
                tft.setTextColor(MENU_TITLE_COLOR);
            } else {
                tft.setTextColor(MENU_TEXT_COLOR);
            }
            
            drawROMDisplayName(romList[romIndex], listX + 18, itemY + 2,
                               romIndex == selectedIndex ? MENU_TITLE_COLOR : MENU_TEXT_COLOR);
        }
        
        // 分页信息
        tft.setTextColor(MENU_HINT_COLOR);
        tft.setCursor(270, listStartY + ITEMS_PER_PAGE * itemHeight + 6);
        char pageInfo[16];
        snprintf(pageInfo, sizeof(pageInfo), "%d/%d", currentPage, totalPages);
        tft.print(pageInfo);
    }
    
    // ===== 底部操作提示 =====
    int hintY = 210;
    tft.fillRect(0, hintY, 320, 30, MENU_HEADER_COLOR);
    tft.drawFastHLine(0, hintY, 320, MENU_BORDER_COLOR);
    
    tft.setTextColor(MENU_HINT_COLOR);
    tft.setTextSize(1);
    tft.setCursor(60, hintY + 10);
    tft.print("UP/DOWN: Select    START: Play Game");
}

// ================ 暂停菜单绘制 ================
void drawPauseMenu() {
    // 在游戏画面上绘制半透明遮罩
    // 由于硬件限制，我们用条纹效果模拟半透明
    for (int y = 0; y < 240; y += 2) {
        tft.drawFastHLine(TFT_OFFSET_X, y, SCREEN_WIDTH, PAUSE_OVERLAY_COLOR);
    }
    
    // 暂停菜单框
    int menuWidth = 160;
    int menuHeight = 140;  // 增加高度以容纳更多选项
    int menuX = (320 - menuWidth) / 2;
    int menuY = (240 - menuHeight) / 2;
    
    // 背景
    tft.fillRect(menuX, menuY, menuWidth, menuHeight, MENU_BG_COLOR);
    tft.drawRect(menuX, menuY, menuWidth, menuHeight, MENU_BORDER_COLOR);
    tft.drawRect(menuX + 1, menuY + 1, menuWidth - 2, menuHeight - 2, MENU_BORDER_COLOR);
    
    // 标题
    tft.setTextColor(MENU_TITLE_COLOR);
    tft.setTextSize(2);
    tft.setCursor(menuX + 40, menuY + 10);
    tft.print("PAUSED");
    
    // 分隔线
    tft.drawFastHLine(menuX + 10, menuY + 32, menuWidth - 20, MENU_BORDER_COLOR);
    
    // 菜单选项 - 添加 Save/Load State
    const char* options[] = {"Continue", "Save State", "Load State", "Exit to Menu"};
    int optionCount = 4;
    tft.setTextSize(1);
    
    for (int i = 0; i < optionCount; i++) {
        int optY = menuY + 42 + i * 20;
        
        if (i == pauseMenuIndex) {
            tft.fillRect(menuX + 10, optY - 2, menuWidth - 20, 18, MENU_HIGHLIGHT_BG);
            tft.setTextColor(MENU_ARROW_COLOR);
            tft.setCursor(menuX + 20, optY + 3);
            tft.print("> ");
            tft.setTextColor(MENU_TITLE_COLOR);
        } else {
            tft.setTextColor(MENU_TEXT_COLOR);
            tft.setCursor(menuX + 20, optY + 3);
            tft.print("  ");
        }
        
        tft.print(options[i]);
    }
    
    // 操作提示
    tft.setTextColor(MENU_HINT_COLOR);
    tft.setCursor(menuX + 15, menuY + menuHeight - 12);
    tft.print("UP/DOWN: Select  A: OK");
}

// ================ 菜单输入处理 ================
void handleMenuInput() {
    unsigned long now = millis();
    if (now - lastButtonTime < BUTTON_DEBOUNCE) return;
    
    updateButtons();
    
    if (romList.empty()) {
        // 无 ROM 或无 SD 卡时，A 键重试 SD 初始化
        if (buttons.A) {
            lastButtonTime = now;
            SD.end();
            delay(100);
            if (tryInitSD()) {
                scanROMFiles();
            }
            drawMainMenu();
        }
        return;
    }
    
    bool buttonPressed = false;
    
    // 上移选择
    if (buttons.UP) {
        if (selectedIndex > 0) {
            selectedIndex--;
            // 调整滚动
            if (selectedIndex < scrollOffset) {
                scrollOffset = selectedIndex;
            }
            buttonPressed = true;
        }
    }
    
    // 下移选择
    if (buttons.DOWN) {
        if (selectedIndex < (int)romList.size() - 1) {
            selectedIndex++;
            // 调整滚动
            if (selectedIndex >= scrollOffset + ITEMS_PER_PAGE) {
                scrollOffset = selectedIndex - ITEMS_PER_PAGE + 1;
            }
            buttonPressed = true;
        }
    }
    
    // 确认选择
    if (buttons.START || buttons.A) {
        if (loadSelectedROM()) {
            currentState = STATE_PLAYING;
        }
        buttonPressed = true;
    }
    
    if (buttonPressed) {
        lastButtonTime = now;
        // 使用局部刷新减少闪烁
        drawMenuList();
    }
}

// ================ 绘制菜单列表区域（局部刷新） ================
void drawMenuList() {
    int listStartY = 44;
    int itemHeight = 20;
    int listWidth = 280;
    int listX = 20;
    
    // 清除列表区域
    tft.fillRect(listX, listStartY, listWidth, ITEMS_PER_PAGE * itemHeight, MENU_BG_COLOR);
    
    if (romList.empty()) return;
    
    tft.setTextSize(1);
    
    for (int i = 0; i < ITEMS_PER_PAGE; i++) {
        int romIndex = scrollOffset + i;
        if (romIndex >= (int)romList.size()) break;
        
        int itemY = listStartY + i * itemHeight;
        
        // 高亮选中项
        if (romIndex == selectedIndex) {
            tft.fillRect(listX, itemY, listWidth, itemHeight - 1, MENU_HIGHLIGHT_BG);
            
            // 右侧箭头指示器
            tft.setTextColor(MENU_ARROW_COLOR);
            tft.setCursor(listX + listWidth - 20, itemY + 6);
            tft.print("<");
            
            // 左侧也加箭头
            tft.setCursor(listX + 4, itemY + 6);
            tft.print(">");
            
            tft.setTextColor(MENU_TITLE_COLOR);
        } else {
            tft.setTextColor(MENU_TEXT_COLOR);
        }
        
        drawROMDisplayName(romList[romIndex], listX + 18, itemY + 2,
                           romIndex == selectedIndex ? MENU_TITLE_COLOR : MENU_TEXT_COLOR);
    }
    
    // 更新分页信息
    int totalPages = (romList.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    int currentPage = scrollOffset / ITEMS_PER_PAGE + 1;
    tft.setTextColor(MENU_HINT_COLOR);
    tft.fillRect(260, listStartY + ITEMS_PER_PAGE * itemHeight + 2, 50, 14, MENU_BG_COLOR);
    tft.setCursor(270, listStartY + ITEMS_PER_PAGE * itemHeight + 6);
    char pageInfo[16];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", currentPage, totalPages);
    tft.print(pageInfo);
}

// ================ 暂停输入处理 ================
void handlePauseInput() {
    unsigned long now = millis();
    if (now - lastButtonTime < BUTTON_DEBOUNCE) return;
    
    updateButtons();
    
    bool buttonPressed = false;
    
    if (buttons.UP) {
        if (pauseMenuIndex > 0) {
            pauseMenuIndex--;
            buttonPressed = true;
        }
    }
    
    if (buttons.DOWN) {
        if (pauseMenuIndex < 3) {  // 现在有4个选项 (0-3)
            pauseMenuIndex++;
            buttonPressed = true;
        }
    }
    
    if (buttons.A || buttons.START) {
        // 等待按键释放
        delay(100);
        while (digitalRead(A_BUTTON) == LOW || digitalRead(START_BUTTON) == LOW) {
            delay(10);
        }
        delay(50);
        
        if (pauseMenuIndex == 0) {
            // Continue - 清屏后继续游戏
            clearScreenForGame();
            resetFrameScheduler(3);
            gameRunning = true;  // 恢复音频
            currentState = STATE_PLAYING;
        } else if (pauseMenuIndex == 1) {
            // Save State
            tft.fillScreen(TFT_BLACK);
            tft.setTextColor(MENU_TITLE_COLOR);
            tft.setTextSize(2);
            tft.setCursor(80, 110);
            tft.print("Saving...");
            
            char savePath[128];
            getSaveStatePath(savePath, sizeof(savePath));
            if (nes.saveState(savePath)) {
                tft.fillScreen(TFT_BLACK);
                tft.setTextColor(0x07E0);  // 绿色成功提示
                tft.setTextSize(2);
                tft.setCursor(60, 110);
                tft.print("State Saved!");
                delay(1000);
            } else {
                tft.fillScreen(TFT_BLACK);
                tft.setTextColor(0xF800);  // 红色错误提示
                tft.setTextSize(2);
                tft.setCursor(60, 110);
                tft.print("Save Failed!");
                delay(1500);
            }
            
            // 返回游戏
            clearScreenForGame();
            resetFrameScheduler(3);
            gameRunning = true;
            currentState = STATE_PLAYING;
        } else if (pauseMenuIndex == 2) {
            // Load State
            tft.fillScreen(TFT_BLACK);
            tft.setTextColor(MENU_TITLE_COLOR);
            tft.setTextSize(2);
            tft.setCursor(80, 110);
            tft.print("Loading...");
            
            char savePath[128];
            getSaveStatePath(savePath, sizeof(savePath));
            if (nes.loadState(savePath)) {
                tft.fillScreen(TFT_BLACK);
                tft.setTextColor(0x07E0);  // 绿色成功提示
                tft.setTextSize(2);
                tft.setCursor(60, 110);
                tft.print("State Loaded!");
                delay(1000);
            } else {
                tft.fillScreen(TFT_BLACK);
                tft.setTextColor(0xF800);  // 红色错误提示
                tft.setTextSize(2);
                tft.setCursor(60, 110);
                tft.print("Load Failed!");
                tft.setTextColor(MENU_HINT_COLOR);
                tft.setTextSize(1);
                tft.setCursor(50, 140);
                tft.print("No save state found");
                delay(1500);
            }
            
            // 返回游戏
            clearScreenForGame();
            resetFrameScheduler(3);
            gameRunning = true;
            currentState = STATE_PLAYING;
        } else {
            // Exit to Menu
            returnToMainMenu();
        }
        return;  // 直接返回，不需要后续处理
    }
    
    // B 按钮也返回游戏
    if (buttons.B) {
        delay(100);
        while (digitalRead(B_BUTTON) == LOW) {
            delay(10);
        }
        delay(50);
        clearScreenForGame();
        resetFrameScheduler(3);
        gameRunning = true;  // 恢复音频
        currentState = STATE_PLAYING;
        return;
    }
    
    if (buttonPressed) {
        lastButtonTime = now;
        if (currentState == STATE_PAUSED) {
            drawPauseMenu();
        }
    }
}

// ================ 加载选中的ROM ================
bool loadSelectedROM() {
    if (selectedIndex < 0 || selectedIndex >= (int)romList.size()) {
        return false;
    }
    
    const char* romPath = romList[selectedIndex].c_str();
    Serial.printf("Loading ROM: %s\n", romPath);
    
    // 显示加载中
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(MENU_TITLE_COLOR);
    tft.setTextSize(2);
    tft.setCursor(100, 110);
    tft.print("Loading...");
    
    if (!nes.loadROM(romPath)) {
        Serial.printf("Failed to load ROM: %s\n", romPath);
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(0xF800);  // 红色错误提示
        tft.setTextSize(2);
        tft.setCursor(60, 100);
        tft.print("Load Failed!");
        tft.setTextColor(MENU_HINT_COLOR);
        tft.setTextSize(1);
        tft.setCursor(40, 130);
        tft.print("Unsupported mapper or bad ROM");
        tft.setCursor(50, 150);
        tft.print("Supported: Mapper 0-4");
        tft.setCursor(60, 170);
        tft.print("Returning to menu...");
        delay(3000);
        tft.fillScreen(MENU_BG_COLOR);
        drawMainMenu();
        return false;
    }
    
    nes.reset();
    
    // 应用抽帧开关设置
    nes.setFrameskipEnabled(ENABLE_FRAMESKIP);
    
    // 设置 PPU 的帧缓冲
    nes.getPPU().frameBuffer = frame_buf[render_buf_idx];
    
    // 彻底清屏为黑色 (清除菜单残留)
    clearScreenForGame();
    
    // 开始运行游戏并启用音频
    resetFrameScheduler(3);
    game_start_ms = millis();
    last_rendered_ms = 0;
    gameRunning = true;
    Serial.println("ROM loaded successfully");

    gameJustEntered = true;
    return true;
}

// ================ 返回主菜单 ================
void returnToMainMenu() {
    // 停止游戏并静音
    gameRunning = false;
    muteAudio();
    
    currentState = STATE_MENU;
    selectedIndex = 0;
    scrollOffset = 0;
    pauseMenuIndex = 0;
    
    // 清屏并重绘菜单
    tft.fillScreen(MENU_BG_COLOR);
    drawMainMenu();
}

void loadROM() {
    // 现在使用菜单选择，这里只是扫描ROM列表
    if (sdCardAvailable) {
        scanROMFiles();
    }
}

// ---------------- Audio (I2S) ----------------
static void initializeAudio() {
    // Install and configure I2S driver
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_I2S_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false
    };

    esp_err_t res = i2s_driver_install((i2s_port_t)I2S_NUM, &i2s_config, 0, NULL);
    if (res != ESP_OK) {
        Serial.printf("I2S driver install failed: %d\n", res);
    } else {
        Serial.println("I2S driver installed");
    }

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_LRCLK_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    i2s_set_pin((i2s_port_t)I2S_NUM, &pin_config);
    i2s_zero_dma_buffer((i2s_port_t)I2S_NUM);

    // Let APU know sample rate (if needed)
    nes.apu.setSampleRate(AUDIO_SAMPLE_RATE);

    // APU 独占 Core 0，仿真+渲染+推屏全在 Core 1 (loop)
    // APU::clock() 内部会在缓冲区满时阻塞写入 I2S
    // I2S DMA 的消费速度 (44100 Hz) 自然控制 APU 时钟速度
    xTaskCreatePinnedToCore(apu_task, "APU", 2048, &nes.apu, 1, NULL, 0);
}

// APU task: 像 Anemoia 一样无限循环调用 clock()
// 每 2 次循环调用一次 clock()，匹配 NES APU 时序（APU 时钟 = CPU 时钟 / 2）
// I2S 写入阻塞会自然控制速度
static void apu_task(void* arg) {
    APU* apu = (APU*)arg;
    while (1) {
        if (gameRunning) {
            apu->clock();
        } else {
            // 游戏未运行时，输出静音并稍作等待
            vTaskDelay(1);
        }
    }
}

// 静音音频输出
static void muteAudio() {
    // 清空 I2S DMA 缓冲区
    i2s_zero_dma_buffer((i2s_port_t)I2S_NUM);
    // 增加延迟确保缓冲区完全清空
    delay(50);
    i2s_zero_dma_buffer((i2s_port_t)I2S_NUM);
}

// ================ 主程序 ================
void setup() {
    initializeSerial();
    initializeScreen();
    initializeButtons();
    initializeSD();
    loadROM();  // 扫描 ROM 文件列表
    
    // 初始化音频 (I2S) 并在另一个 CPU core 上运行音频任务
    initializeAudio();
    
    // 创建显示任务在 Core 0
    frame_queue = xQueueCreate(1, sizeof(uint8_t));
    if (frame_queue) {
        xTaskCreatePinnedToCore(display_task, "Display", 4096, nullptr, 1, nullptr, 0);
    }
    
    // 显示主菜单
    currentState = STATE_MENU;
    drawMainMenu();
}

void loop() {
    // 根据当前状态处理不同逻辑
    switch (currentState) {
        case STATE_MENU:
            handleMenuInput();
            delay(50);  // 菜单模式降低刷新率，节省资源
            return;
            
        case STATE_PAUSED:
            handlePauseInput();
            delay(50);
            return;
            
        case STATE_PLAYING:
            // 正常游戏逻辑
            if (gameJustEntered) {
                clearScreenForGame();   // ⭐ 强制清左右黑边
                gameJustEntered = false;
            }
            break;
    }

    // ===== Anemoia 风格游戏运行逻辑 =====
    // 帧级别调度：目标 60Hz 仿真 (16639µs/帧)
    #define FRAME_TIME_US 16639
    static bool pauseKeyReleased = true;  // 暂停组合键是否已释放

    // 更新按键输入
    updateButtons();
    
    // 检测 START + SELECT 组合键进入暂停菜单
    if (buttons.START && buttons.SELECT) {
        if (pauseKeyReleased) {
            pauseKeyReleased = false;
            currentState = STATE_PAUSED;
            pauseMenuIndex = 0;
            // 暂停游戏并静音
            gameRunning = false;
            muteAudio();
            // 等待按键释放，防止立即退出暂停
            delay(100);
            while (digitalRead(START_BUTTON) == LOW || digitalRead(SELECT_BUTTON) == LOW) {
                delay(10);
            }
            delay(100);
            drawPauseMenu();
            return;
        }
    } else {
        pauseKeyReleased = true;
    }
    
    uint8_t controllerState = 0;
    if (buttons.A)      controllerState |= 0x01;
    if (buttons.B)      controllerState |= 0x02;
    if (buttons.SELECT) controllerState |= 0x04;
    if (buttons.START)  controllerState |= 0x08;
    if (buttons.UP)     controllerState |= 0x10;
    if (buttons.DOWN)   controllerState |= 0x20;
    if (buttons.LEFT)   controllerState |= 0x40;
    if (buttons.RIGHT)  controllerState |= 0x80;
    nes.setController(0, controllerState);

    // 初始化帧计时
    if (next_frame_us == 0) next_frame_us = esp_timer_get_time();

    // 自适应抽帧：只有在主循环已经落后于目标帧节奏时才跳过本帧渲染。
    int64_t frameLagUs = (int64_t)esp_timer_get_time() - (int64_t)next_frame_us;
    bool frameskipPhaseAllowsSkip = (frameskip_phase == 0 ||
                                     frameskip_phase == 2 ||
                                     frameskip_phase == 4 ||
                                     frameskip_phase == 6 ||
                                     frameskip_phase == 8);
    bool shouldSkipFrame = ENABLE_FRAMESKIP &&
                           (force_render_frames == 0) &&
                           (consecutive_skipped_frames == 0) &&
                           frameskipPhaseAllowsSkip &&
                           (frameLagUs > (FRAME_TIME_US / 2));
    nes.requestFrameSkip(shouldSkipFrame);

    // 执行一帧
    uint32_t emu0 = micros();
    //runFrame(); // 方法1: 行级调度
    nes.clock(); // 方法2: 帧级调度
    last_emulation_us = micros() - emu0;
    
    // 入队帧缓冲用于 DMA 显示
    tryEnqueueFrame();

    if (last_rendered_ms == 0 && millis() - game_start_ms > 3500) {
        gameRunning = false;
        muteAudio();
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(0xF800);
        tft.setTextSize(2);
        tft.setCursor(58, 95);
        tft.print("Game Start Failed");
        tft.setTextColor(MENU_HINT_COLOR);
        tft.setTextSize(1);
        tft.setCursor(42, 130);
        tft.print("Unsupported or unstable ROM");
        tft.setCursor(65, 150);
        tft.print("Returning to menu...");
        delay(3000);
        tft.fillScreen(MENU_BG_COLOR);
        drawMainMenu();
        currentState = STATE_MENU;
        return;
    }

    if (shouldSkipFrame) {
        consecutive_skipped_frames++;
    } else {
        consecutive_skipped_frames = 0;
        if (force_render_frames > 0) force_render_frames--;
    }
    frameskip_phase++;
    if (frameskip_phase >= 9) frameskip_phase = 0;

    // FPS 统计
    fps_count++;
    uint32_t curMs = millis();
    if (fps_last_ms == 0) fps_last_ms = curMs;
    if (curMs - fps_last_ms >= 1000) {
        FPS_PRINT("FPS:%u  EMU:%uus  DMA:%uus\n",
            fps_count, last_emulation_us, last_dma_us);
        fps_count = 0;
        fps_last_ms = curMs;
    }

    // 帧限制
    uint64_t now = esp_timer_get_time();
    if (now < next_frame_us) {
        ets_delay_us(next_frame_us - now);
    }
    next_frame_us += FRAME_TIME_US;
    #undef FRAME_TIME_US
}
