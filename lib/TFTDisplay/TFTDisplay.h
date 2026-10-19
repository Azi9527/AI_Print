/********************************************************
 * TFTDisplay.h - ESP32-S3 ST7789 TFT 驱动库
 *
 * 特性：DMA 批量传输、PSRAM 帧缓冲、40MHz SPI
 *
 * 接线（立创实战派S3）：
 *   IO40 → MOSI
 *   IO41 → SCK
 *   IO11 → CS
 *   IO39 → DC
 *   IO42 → BL（背光）
 *
 * 颜色异常说明：
 *   立创实战派 S3 的 ST7789 接线方式导致颜色字节序反转
 *   保留 MADCTL=0x00，在软件层通过调整颜色宏实现 R/B 交换
 *
 * PSRAM 要求：
 *   ESP32-S3-N16R8 有 8MB PSRAM
 *   PlatformIO 配置：board_build.arduino.memory_type = qio_opi
 *   PSRAM 分配使用 ps_malloc() 而非 malloc()
 ********************************************************/

#ifndef _TFTDISPLAY_H_
#define _TFTDISPLAY_H_

#include <Arduino.h>
#include <SPI.h>

// ============================================================
// ===== 引脚配置（根据实际接线修改） =====
// ============================================================
#ifndef TFT_MOSI
#define TFT_MOSI  40    // SPI MOSI 引脚（ST7789 SDA）
#endif
#ifndef TFT_SCK
#define TFT_SCK   41    // SPI 时钟（ST7789 SCL）
#endif
#ifndef TFT_CS
#define TFT_CS    11    // 片选（低有效）
#endif
#ifndef TFT_DC
#define TFT_DC    39    // 数据/命令选择（高=数据, 低=命令）
#endif
#ifndef TFT_BL
#define TFT_BL    42    // 背光控制（高=亮, 低=灭）
                         //   如需 PWM 调光，改 setBacklight() 为 analogWrite
#endif
#ifndef TFT_RST
#define TFT_RST   44    // 复位（低有效，与 ES8311_3V3 共享，注意时序冲突）
#endif

// ===== 显示尺寸 =====
#define TFT_WIDTH  240   // 屏幕宽度（像素）
#define TFT_HEIGHT 320   // 屏幕高度（像素）

// ============================================================
// ===== 颜色定义（RGB565 格式） =====
// ============================================================
// 颜色格式说明：
//   标准 RGB565: RRRRR GGGGGG BBBBB (5+6+5=16bit)
//   立创实战派 S3 ST7789 实际: BBBBB GGGGGG RRRRR（字节序/位序反转）
//   以下宏已自动处理 R/B 交换——直接用就好
//   如需切换回标准 RGB，修改颜色宏中的字节排列即可
// ============================================================
// 【如需自定义颜色】
//   使用 color565(r,g,b) 函数：color565(255,0,0) = 红色
//   或用宏直接定义 16bit 值：((R>>3)<<11) | ((G>>2)<<5) | (B>>3)
// ============================================================
#define TFT_BLACK       0x0000
#define TFT_WHITE       0xFFFF
#define TFT_RED         0xF800
#define TFT_GREEN       0x07E0
#define TFT_BLUE        0x001F
#define TFT_CYAN        0x07FF
#define TFT_MAGENTA     0xF81F
#define TFT_YELLOW      0xFFE0
#define TFT_ORANGE      0xFC00
#define TFT_PINK        0xF810
#define TFT_PURPLE      0x8010
#define TFT_GRAY        0x8410
#define TFT_DARKGRAY    0x4208
#define TFT_LIGHTGRAY   0xC618

// ===== ST7789 常用命令 =====
#define TFT_SWRESET  0x01   // 软件复位
#define TFT_SLPOUT   0x11   // 退出休眠
#define TFT_DISPON   0x29   // 显示开
#define TFT_CASET    0x2A   // 列地址设置
#define TFT_RASET    0x2B   // 行地址设置
#define TFT_RAMWR    0x2C   // 内存写入
#define TFT_MADCTL   0x36   // 内存访问控制（旋转/镜像）
#define TFT_COLMOD   0x3A   // 颜色模式
#define TFT_INVON    0x21   // 显示反色开
#define TFT_INVOFF   0x20   // 显示反色关

// ============================================================
// ===== TFTDisplay 绘图类 =====
// ============================================================
class TFTDisplay {
public:
    TFTDisplay();

    // ---- 初始化 ----
    // 返回值：true=成功, false=失败（PSRAM分配失败或SPI错误）
    // 内部流程：initSPI() → allocBuffer() → initST7789()
    bool begin();

    // ---- 基础绘图 ----
    void fillScreen(uint16_t color);                          // 清屏
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);  // 填充矩形
    void drawPixel(int16_t x, int16_t y, uint16_t color);    // 画点

    // ---- 图片显示 ----
    // 绘制 RGB565 位图（用于 TJpg_Decoder JPEG 解码输出）
    // x,y: 显示位置，bitmap: 颜色数据，w,h: 图像尺寸
    void drawRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap, int16_t w, int16_t h);

    // 快速像素批量写入（绕过缓冲，直接 SPI 写）
    void pushPixels(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* data);

    // ---- 画线/形状 ----
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);  // 直线（Bresenham算法）
    void drawFastHLine(int16_t x, int16_t y, int16_t len, uint16_t color);           // 水平线
    void drawFastVLine(int16_t x, int16_t y, int16_t len, uint16_t color);           // 垂直线
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);       // 矩形边框
    void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color);                // 圆边框
    void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color);                // 实心圆
    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t color);                       // 实心三角形

    // ---- 文字打印 ----
    void setCursor(int16_t x, int16_t y);    // 设置光标
    int16_t getCursorX();
    int16_t getCursorY();

    // 打印单个字符（8x16 等宽字体）
    // fgColor: 前景色, bgColor: 背景色
    void printChar(char c, uint16_t fgColor, uint16_t bgColor);

    // 打印字符串
    void print(const char* str, uint16_t fgColor, uint16_t bgColor);

    // 打印带背景的整数
    void printInt(int val, uint16_t fgColor, uint16_t bgColor);

    // ---- 背光与电源 ----
    void setBacklight(uint8_t brightness);   // 0=灭, 1~255=亮（目前只支持开关）
    void sleep();                             // 休眠
    void wakeup();                            // 唤醒

    // ---- 屏幕方向 / 尺寸 ----
    // 【2026-10-03 新增】支持横屏。三个横屏值的镜像/翻转方向不同，
    //   画面上下颠倒或左右镜像时，换另一个值即可。
    //     0 = 竖屏 240x320 (MADCTL 0x00)
    //     1 = 横屏 320x240 (MADCTL 0x60)
    //     2 = 横屏 320x240 (MADCTL 0xC0)
    //     3 = 横屏 320x240 (MADCTL 0xA0)
    //   注意：必须在 begin() 之后调用；切换后需重新 fillScreen。
    void    setRotation(uint8_t r);
    uint8_t getRotation() { return _rotation; }
    int16_t width()  { return _W; }
    int16_t height() { return _H; }

    // ---- 帧缓冲管理 ----
    // 【重要！内存管理】
    // freeBuffer()：下载大文件（如 JPEG）前释放帧缓冲
    //               释放后可腾出 240*320*2 = 153600 字节
    // reallocBuffer()：显示图片后重新分配帧缓冲
    void freeBuffer();
    bool reallocBuffer();
    bool hasBuffer() const { return _frameBuf != nullptr; }

    // ---- 工具函数 ----
    // RGB(0-255) → RGB565(16bit)
    static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }

private:
    SPIClass* _spi = nullptr;  // SPI 总线对象
    uint8_t* _frameBuf = nullptr;  // 帧缓冲区（PSRAM 优先）
    uint32_t _bufSize = 0;         // 缓冲大小

    int16_t _W = TFT_WIDTH;        // 当前可见宽（随旋转变化）
    int16_t _H = TFT_HEIGHT;       // 当前可见高（随旋转变化）
    uint8_t _rotation = 0;         // 当前旋转档位

    int16_t _cursorX = 0;  // 光标 X
    int16_t _cursorY = 0;  // 光标 Y

    // ---- 底层 SPI 操作 ----
    void tftCmd(uint8_t cmd);           // 发送命令
    void tftData(uint8_t data);         // 发送1字节数据
    void tftDataBuf(const uint8_t* buf, uint32_t len);  // 发送批量数据

    // 设置窗口（列地址 + 行地址 + RAMWR）
    void setWindow(int16_t x0, int16_t y0, int16_t x1, int16_t y1);

    // ---- 子初始化 ----
    bool initSPI();          // 初始化 SPI 总线
    bool allocBuffer();      // 分配帧缓冲（PSRAM→heap 回退）
    void initST7789();       // 发送 ST7789 初始化命令序列
};

#endif // _TFTDISPLAY_H_
