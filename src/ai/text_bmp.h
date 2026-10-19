// text_bmp.h — 中文文本渲染为热敏打印位图（U8g2 伪显示方案）
// 同时对外提供共享的 U8g2 中文画布（cnCanvas），供屏幕显示复用，
// 避免再开一份 U8g2 全缓冲实例（那样要多占 7.7KB 且双份字库）。
#pragma once
#include <Arduino.h>

class TextBmp {
public:
    // 渲染 UTF-8 文本（自动折行）并整页打印。成功返回 true。
    bool printText(const String& text, int fontSize = 16);
    // 【答"画个XX"】把字符画（'#'/'.'）放大打印。cols×rows 为字符画尺寸。
    bool printAsciiArt(const String& art, int blockPx = 6);
    // 【测试页】160x160 二维码（内容=qrText，一般为 MAC 地址）水平居中，
    //   下方印 infoText（用 \n 分行，自动折行）。成功返回 true。
    bool printTestPage(const String& qrText, const String& infoText);
    // 【文生图】JPEG 字节流 → 解码 → 缩放到 384 宽灰度图，缓存在内部
    //   PSRAM（长期持有，直到下一张覆盖）。返回缓冲指针，w/h 带回尺寸。
    //   供屏幕预览（face.showImage）与打印（printPhotoPending）共用。
    const uint8_t* decodePhoto(const uint8_t* jpg, uint32_t len, int& w, int& h);
    // 屏上那张图还在吗（生成了还没丢）
    bool photoReady(int& w, int& h);
    // 把缓存的灰度图 二值化/抖动 成 1bit 打印并走纸（说"打印"时调）
    bool printPhotoPending();
    // 丢掉屏上那张图（说"取消/算了/不打了"时调，释放 PSRAM）
    void clearPhoto();
};

extern TextBmp textBmp;

// ================================================================
// 共享中文画布（1bpp，384×160，全 GB2312 字库）
//
// 【缓冲布局 —— 2026-10-03 第二次修正，这次是对着 U8g2 源码核的】
//
// U8g2 的 ST7586S 全缓冲构造器用的是 **horizontal_right_lsb** 而不是
// SSD1306 那种 vertical_top_lsb，见库源码：
//     src/clib/u8g2_d_setup.c : u8g2_Setup_st7586s_jlx384160_f()
//         u8g2_SetupBuffer(..., u8g2_ll_hvline_horizontal_right_lsb, ...)
//     src/clib/u8g2_ll_hvline.c : 行 245 与 350
//         offset = y * tile_width(=48) + (x >> 3);
//         mask   = 0x80 >> (x & 7);
//
//   ⇒ 像素 (x,y) 在 buf[y*48 + (x>>3)] 的第 (7-(x&7)) 位
//   ⇒ 也就是"行优先、每字节 8 个水平像素、MSB 在最左"
//   ⇒ 正好与热敏打印头的位图格式一模一样（见 tileToPrintLines）
//
//   ★ 之前按 vertical 布局取（b[(y>>3)*384 + x] >> (y&7)）→ 屏幕下方
//     文字整片花屏、打印也错位。顶部状态栏不花是因为它走 TFT 自带
//     字体，不经过这里。
//   缓冲区总大小 = 48 字节/行 × 160 行 = 7680 字节 ✓
// ================================================================
namespace cnCanvas {
enum { W = 384, H = 160, STRIDE = W / 8 };   // STRIDE = 48 字节/行

void           begin();         // 初始化（可重复调用）
void           setFont(int px); // 12 / 16，其他值按 12 处理
void           clear();
void           draw(int x, int y, const char* utf8);
int            width(const char* utf8);   // 文本像素宽（需先 setFont）
const uint8_t* buf();                     // 1bpp 行优先缓冲

// 取像素：x∈[0,W) y∈[0,H)
inline bool pixel(const uint8_t* b, int x, int y) {
    return (b[y * STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1;
}
}
