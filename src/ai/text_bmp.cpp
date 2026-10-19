#include "text_bmp.h"
#include "ai_config.h"
#include "device_config.h"
#include "gfx.h"       // ★ GfxGuard：与屏幕共用的 u8g2 实例必须互斥（见 printText 注释）
#include <U8g2lib.h>
#include <ThermalPrinter.h>
// ★ qrcodegen.hpp 的 Ecc 枚举里有 LOW / HIGH 成员，会被 Arduino 的
//   GPIO 宏（#define LOW 0x0）打爆。include 前后压栈/弹栈这对宏。
#pragma push_macro("LOW")
#pragma push_macro("HIGH")
#undef LOW
#undef HIGH
#include <qrcodegen.hpp>
#pragma pop_macro("HIGH")
#pragma pop_macro("LOW")
#include <vector>
#include <JPEGDEC.h>   // 文生图：JPEG 解码（百度回的是 baseline JPEG，正好支持）

// ── U8g2 伪显示 ──────────────────────────────────────
// 借用 ST7586S 384x160 全缓冲构造器，但物理引脚全部指向 GPIO21(LCD_TE，
// 屏上根本没接的脚)。不调用 sendBuffer()，所以数据只会留在内存缓冲里。
static U8G2_ST7586S_JLX384160_F_4W_SW_SPI u8g2(
    U8G2_R0, /*clk*/ 21, /*data*/ 21, /*cs*/ U8X8_PIN_NONE,
    /*dc*/ U8X8_PIN_NONE, /*reset*/ U8X8_PIN_NONE);

static const uint8_t* FONT16 = u8g2_font_wqy16_t_gb2312; // 全 GB2312 中文
static const uint8_t* FONT12 = u8g2_font_wqy12_t_gb2312;
static const int CHUNK_H = 160;   // U8g2 缓冲高
extern ThermalPrinter printer;

TextBmp textBmp;

// ================================================================
// 共享中文画布 —— 屏幕显示复用同一份 U8g2 实例
// ================================================================
namespace cnCanvas {

void begin() {
    u8g2.begin();
    u8g2.enableUTF8Print();
    u8g2.setFontPosTop();
    // ★ 2026-10-10 修"文字缺顶"：默认 TEXT 模式参考上沿只用 ascent_A(wqy12=8)，
    //   而中文字形高 12~13 ⇒ fontPosTop 下中文顶部 3~4 行画到 y<0 被裁掉。
    //   改按全字符集真实高度算参考线（wqy12 参考=11+1=12，中文顶正好=0 行）。
    u8g2.setFontRefHeightAll();
}

void setFont(int px) {
    u8g2.setFont(px >= 14 ? FONT16 : FONT12);
}

void clear() { u8g2.clearBuffer(); }

void draw(int x, int y, const char* utf8) { u8g2.drawUTF8(x, y, utf8); }

int width(const char* utf8) { return u8g2.getUTF8Width(utf8); }

const uint8_t* buf() { return u8g2.getBufferPtr(); }

} // namespace cnCanvas

// ================================================================
// u8g2 缓冲 → 打印机横向位图
//   u8g2 : 行优先，像素(x,y) → buf[y*48 + (x>>3)] 的第 (7-(x&7)) 位
//   打印 : out[y][x/8] 的第 (7-(x&7)) 位（MSB=最左点）
//   ⇒ 两者格式完全相同，只需按行搬字节（stride 都是 48）。
// ★ 2026-10-03 修正：原来按"8x8 tile / 纵向字节"转换，打印出来整片错位。
// ================================================================
static void tileToPrintLines(const uint8_t* src, uint8_t* out,
                             int w, int h) {
    const int wBytes = w / 8;
    for (int y = 0; y < h; y++)
        memcpy(out + y * wBytes, src + (size_t)y * cnCanvas::STRIDE, wBytes);
}

// ── UTF-8 折行（按像素宽，用当前 U8g2 字体量）──
static void wrapLines(const String& text, int maxPx, std::vector<String>& lines) {
    String src = text;
    src += "\n";
    int start = 0;
    for (int i = 0; i <= (int)src.length(); i++) {
        if (src[i] != '\n' && src[i] != '\r') continue;
        String para = src.substring(start, i);
        start = i + 1;
        if (para.length() == 0) { lines.push_back(String("")); continue; }
        String cur;
        const char* base = para.c_str();
        const char* p = base;
        while (*p) {
            const char* prev = p;
            uint8_t c = (uint8_t)*p;
            int step = 1;
            if      ((c & 0xE0) == 0xC0) step = 2;
            else if ((c & 0xF0) == 0xE0) step = 3;
            else if ((c & 0xF8) == 0xF0) step = 4;
            p += step;
            String piece = para.substring(prev - base, p - base);
            String test = cur + piece;
            if (u8g2.getUTF8Width(test.c_str()) > maxPx) {
                if (cur.length() == 0) { lines.push_back(piece); cur = String(""); }
                else { lines.push_back(cur); cur = piece; }
            } else {
                cur = test;
            }
        }
        lines.push_back(cur);
    }
}

// ================================================================
// ★★ 打印路径的并发陷阱（2026-10-04 改版16 定位并修复）★★
//   `u8g2` 这个实例是 **屏幕文字浮层和打印共用的**（cnCanvas 命名空间也用它）。
//   屏幕文字由 Face 动画任务渲染：Face::render() → screenText.overlayDialog()
//   → drawTextLine() → **cnCanvas::clear() + cnCanvas::draw()**，跑在 **core 0**、
//   每 90ms 一帧，且带 GfxGuard 保护。
//   而 printText()/printTestPage() 跑在 **core 1 的主循环**里，之前**完全没加锁** ——
//   于是"这边刚把字画进 u8g2 缓冲、那边屏幕正好 clearBuffer 把它抹掉"，
//   结果打到纸上的是一片空白，但函数照样 return true（老陈实测：
//   显示"打印完成"，纸上啥也没有）。
//   ⇒ 修法：所有对 u8g2 的访问一律套 GfxGuard（递归锁，屏幕那边也在用同一把）。
//     注意锁要包住"渲染"，但 **不要**包住 printer.printImage —— 否则动画会卡住
//     整个打印过程（几十秒）。
//   另外加了一行"墨点数"诊断：0 说明缓冲确实是空的，方便一眼定位。
// ================================================================
bool TextBmp::printText(const String& text, int fontSize) {
    const int lineH = fontSize >= 14 ? 20 : 15;
    const int maxLines = CHUNK_H / lineH;

    std::vector<String> lines;
    {   // ★ 持锁：字体设置 + 折行（wrapLines 内部要调 u8g2.getUTF8Width 量宽）
        GfxGuard g;
        u8g2.begin();
        u8g2.enableUTF8Print();
        u8g2.setFont(fontSize >= 14 ? FONT16 : FONT12);
        u8g2.setFontPosTop();
        wrapLines(text, PRINT_W - 6, lines);
    }

    uint8_t* lineData = (uint8_t*)malloc(PRINT_W / 8 * CHUNK_H);
    if (!lineData) return false;
    int totalInk = 0;
    logYieldLAT();                  // ★ UART 日志版：43 让给打印头 LAT（默认版空操作）
    printer.vhHold(true);           // ★ 整个"打印 + 撕纸走纸"期间 GPIO48 恒高（VM=VH，断了电机丢步）
    for (size_t base = 0; base < lines.size(); base += maxLines) {
        int rows = 0;
        {
            GfxGuard g;                 // ★ 只锁"渲染"这一段（几十毫秒）
            u8g2.clearBuffer();
            int n = 0;
            for (size_t i = base; i < lines.size() && n < maxLines; i++, n++)
                u8g2.drawUTF8(2, n * lineH, lines[i].c_str());
            rows = n * lineH;
            if (rows > CHUNK_H) rows = CHUNK_H;
            tileToPrintLines(u8g2.getBufferPtr(), lineData, PRINT_W, rows);
        }
        int ink = 0;                    // 诊断：本块墨点数（=0 说明画布是空的）
        for (int i = 0; i < PRINT_W / 8 * rows; i++)
            ink += __builtin_popcount(lineData[i]);
        totalInk += ink;
        Serial.printf("[PRINT] 文字块 rows=%d 墨点=%d\n", rows, ink);
        if (rows > 0) printer.printImage(lineData, PRINT_W, rows);   // 锁外打印
        delay(60);
    }
    free(lineData);
    printer.feedLines(PRINT_FEED_MM * 8);   // 撕纸边 = PRINT_FEED_MM 毫米
    printer.vhHold(false);                  // ★ 释放 VH 保持（到这里才真正关一次 VH）
    logResumeUART();                        // ★ UART 日志版：43 还给日志口
    Serial.printf("[PRINT] 文字打印结束，总墨点=%d（0 = 画布空，检查并发/文本）\n", totalInk);
    return true;
}

// ================================================================
// 字符画打印（"阿蓝，画一只猫"）
//   art 是若干行 '#'/'*'/'█'（黑）与 '.'/' '（白）组成的图。
//   每个字符放大成 blockPx×blockPx 的点块，整图居中。
// ================================================================
bool TextBmp::printAsciiArt(const String& art, int blockPx) {
    if (blockPx < 1) blockPx = 1;

    // 切成行
    std::vector<String> rows;
    String cur;
    for (int i = 0; i < (int)art.length(); i++) {
        char c = art[i];
        if (c == '\n') { rows.push_back(cur); cur = ""; }
        else if (c != '\r') cur += c;
    }
    rows.push_back(cur);

    // 去掉尾部的空行
    while (rows.size() && rows.back().length() == 0) rows.pop_back();
    if (rows.empty()) return false;

    int cols = 0;
    for (auto& r : rows) if ((int)r.length() > cols) cols = r.length();
    if (cols < 2) return false;

    const int outW = cols * blockPx;
    const int outH = (int)rows.size() * blockPx;
    if (outW > PRINT_W) return false;                 // 太宽打不下
    const int xOff = (PRINT_W - outW) / 2;

    const int wBytes = PRINT_W / 8;
    uint8_t* buf = (uint8_t*)malloc(wBytes * outH);
    if (!buf) return false;
    memset(buf, 0, wBytes * outH);

    for (int y = 0; y < outH; y++) {
        const String& r = rows[y / blockPx];
        for (int c = 0; c < (int)r.length(); c++) {
            char ch = r[c];
            if (ch != '#' && ch != '*' && ch != 'x' && ch != 'X' && ch != 'O' && ch != '@')
                continue;
            const int x0 = xOff + c * blockPx;
            for (int k = 0; k < blockPx; k++) {
                const int x = x0 + k;
                if (x < 0 || x >= PRINT_W) continue;
                buf[y * wBytes + (x >> 3)] |= 0x80 >> (x & 7);
            }
        }
    }
    // ★ 一次打完：打印中途不开关 VH（VM=VH，断了电机就丢步）
    logYieldLAT();                          // ★ UART 日志版：43 让给打印头 LAT
    printer.vhHold(true);                   // ★ 打印 + 撕纸走纸全程 VH 恒高
    printer.printImage(buf, PRINT_W, outH);
    free(buf);
    printer.feedLines(PRINT_FEED_MM * 8);   // 撕纸边 = PRINT_FEED_MM 毫米
    printer.vhHold(false);                  // ★ 释放
    logResumeUART();                        // ★ UART 日志版：43 还给日志口
    return true;
}

// ================================================================
// 测试页（长按按键 0.6 秒触发）
//   上半部：160x160 二维码（内容 = MAC 地址），水平居中
//   下半部：MAC / IP / 固件版本，16px 中文
//   整页自己组位图（不用 cnCanvas 的 160 行限制），一次打完。
// ================================================================
bool TextBmp::printTestPage(const String& qrText, const String& infoText) {
    GfxGuard g;     // ★ 与屏幕共用 u8g2，整段持锁（测试页只几秒，动画卡一下无妨）
    u8g2.begin();
    u8g2.enableUTF8Print();
    u8g2.setFontPosTop();

    const int wBytes = PRINT_W / 8;      // 48 字节/行
    // ★ 顶部"硬自检条" = 供电阶梯（8 档密度 × 4 行 = 32 行，不经 u8g2，纯字节填充）
    const int BLK_ROWS = 4;              //   每档 4 行（3 行实打 + 1 行留白分隔，方便在纸上数档）
    const int BLK_N    = 8;              //   8 档：每行 48/96/144/192/240/288/336/384 点
    const int DIAG_H   = BLK_ROWS * BLK_N;

    // ---- 1. 生成二维码（Nayuki qrcodegen，MIT）----
    //   MAC 17 字节 → Version 1(21 模块)/ECC-L 就够；用 MEDIUM 更耐脏，
    //   Version 2 = 25 模块 → scale 6 = 150px，仍在 160 盒内。
    qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
        qrText.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int modules = qr.getSize();
    const int scale   = 160 / modules;          // v1→7, v2→6, v3→5 …
    if (scale < 1) return false;
    const int qrPx  = scale * modules;          // 实际二维码边长（≤160）
    const int qrX0  = (PRINT_W - qrPx) / 2;     // 水平居中
    const int qrY0  = DIAG_H + 16;              // 自检条下留 16px 白边

    // ---- 2. 文本行先渲染进 U8g2 缓冲（一次最多 160px 高）----
    u8g2.setFont(FONT16);
    std::vector<String> lines;
    wrapLines(infoText, PRINT_W - 24, lines);
    if (lines.size() > 7) lines.resize(7);      // 一页放得下就行
    const int textY0 = qrY0 + 160 + 24;         // 二维码下隔 24px
    u8g2.clearBuffer();
    for (size_t i = 0; i < lines.size(); i++)
        u8g2.drawUTF8(12, i * 20, lines[i].c_str());
    const int textRows = (int)lines.size() * 20;

    // ---- 3. 组整页位图 ----
    const int pageH  = textY0 + textRows + 16;
    uint8_t* page = (uint8_t*)malloc((size_t)wBytes * pageH);
    if (!page) return false;
    memset(page, 0, (size_t)wBytes * pageH);

    // 3·★ 供电阶梯自检条（8 档密度，每档 3 行实打 + 1 行留白，共 32 行）
    //   全部直接填字节，不经过任何字体/画布 —— 把软件因素整个排除掉。
    //   第 k 档（k=1..8）每行黑点数 = 384×k/8 = 48k：
    //       48 / 96 / 144 / 192 / 240 / 288 / 336 / 384 点
    //     图案 = 每 8 个点里前 k 个点黑（x&7 < k）→ 同样点数但分布均匀，
    //     不会像"左半全黑"那样局部过热。纸上就是 8 道由浅到深的灰带。
    //   ★ 为什么是这 8 档：第 4 档 = 192 点/行，正好是老陈那份"能正常打印"的代码
    //     （只打抖动照片、黑点≈50%）的工作点 —— 已证明这块板扛得住。
    //   ★ 判读（从上往下数第几道断）：
    //     8 道全出          → 满行全黑也扛得住，供电这关彻底过
    //     断在第 5~8 道     → 峰值电流受限，门限落在 200~384 点之间
    //     断在第 4 道或更前 → 连已证正常的工作点都打不了 ⇒ 跟密度无关，
    //                         是本次固件的硬件状态/接线变了（查 PVH、FPC、纸压）
    //     第 1 道就没有     → 打印头/PVH/接线不通
    //   ⚠️ PRT_SPLIT_DOTS=1 时第 5~8 档会被拆成两半（每半 ≤192 点）：
    //     它要是正好都打过了，就反过来证明"限点数"这一招管用。
    for (int k = 1; k <= BLK_N; k++) {
        for (int r = 0; r < BLK_ROWS - 1; r++) {   // 每档最后 1 行留白
            const int y = (k - 1) * BLK_ROWS + r;
            uint8_t* row = page + (size_t)y * wBytes;
            for (int x = 0; x < PRINT_W; x++)
                if ((x & 7) < k) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        }
    }

    // 3a. 二维码：模块 → scale×scale 点块
    for (int my = 0; my < modules; my++) {
        for (int mx = 0; mx < modules; mx++) {
            if (!qr.getModule(mx, my)) continue;
            for (int dy = 0; dy < scale; dy++) {
                const int y = qrY0 + my * scale + dy;
                uint8_t* row = page + (size_t)y * wBytes;
                for (int dx = 0; dx < scale; dx++) {
                    const int x = qrX0 + mx * scale + dx;
                    row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
                }
            }
        }
    }
    // 3b. 文本：U8g2 缓冲行拷进页（两个缓冲都是行优先 48 字节/行，直接搬）
    const uint8_t* cb = u8g2.getBufferPtr();
    for (int y = 0; y < textRows; y++)
        memcpy(page + (size_t)(textY0 + y) * wBytes, cb + (size_t)y * cnCanvas::STRIDE, wBytes);

    // ★ 分段统计墨点：哪一段是 0 一眼就能看出来
    auto inkOf = [&](int y0, int y1) {
        int k = 0;
        for (int y = y0; y < y1 && y < pageH; y++)
            for (int i = 0; i < wBytes; i++)
                k += __builtin_popcount(page[(size_t)y * wBytes + i]);
        return k;
    };
    Serial.printf("[PRINT] 测试页 %dx%d  阶梯每行点数:", PRINT_W, pageH);
    for (int k = 1; k <= BLK_N; k++)                            // 应为 48 96 … 384
        Serial.printf(" %d", inkOf((k - 1) * BLK_ROWS, k * BLK_ROWS) / (BLK_ROWS - 1));
    Serial.printf("  二维码墨点=%d  文字墨点=%d(%d行)\n",
                  inkOf(qrY0, qrY0 + qrPx), inkOf(textY0, textY0 + textRows), (int)lines.size());

    // ---- 4. 一次打完（★ 老陈 2026-10-04 要求：打印中不许开关 VH！）
    //   电机驱动 AB8833 的 VM 就是 VH（原理图 U16-12），打印中途 VH 断一下
    //   电机就断一下电 → 走纸丢步。page 本来就是连续缓冲，直接整页一次打，
    //   VH 从头到尾只开关一次。（原来按 240 行分块是老结构残留，纯属多余）
    logYieldLAT();                          // ★ UART 日志版：43 让给打印头 LAT
    printer.vhHold(true);                   // ★ 打印 + 撕纸走纸全程 VH 恒高（老陈 2026-10-04）
    printer.printImage(page, PRINT_W, pageH);
    free(page);
    printer.feedLines(PRINT_FEED_MM * 8);   // 撕纸边 = PRINT_FEED_MM 毫米
    printer.vhHold(false);                  // ★ 释放
    logResumeUART();                        // ★ UART 日志版：43 还给日志口
    return true;
}

// ================================================================
// 文生图（"阿蓝，画一只猫" 真·文生图路线）★ 2026-10-04 新增
//   老陈定的流程：生成后先在屏上看（face.showImage），说"打印"再打。
//   decodePhoto() 把 JPEG 解码成 384 宽灰度图缓存在静态 PSRAM；
//   printPhotoPending() 用 Floyd–Steinberg 抖动成 1bit 再打印。
//   为什么打印必须抖动：热敏头每个点只有"打/不打"两态，直接二值化
//   会把中间调全丢掉，灰阶全变死黑死白；误差扩散用疏密黑点模拟灰阶。
// ================================================================
static uint8_t* s_gray  = nullptr;     // 解码中间缓冲（PSRAM，用完即放）
static int      s_gw = 0, s_gh = 0;
static uint8_t* s_photo = nullptr;     // 384 宽灰度成品（PSRAM，长期持有）
static int      s_pw = 0, s_ph = 0;

// JPEGDEC 解码回调：把每块像素搬进灰度大图（灰度模式每像素 1 字节）
static int jpegGrayCb(JPEGDRAW* d) {
    for (int y = 0; y < d->iHeight; y++) {
        const uint8_t* src = (const uint8_t*)d->pPixels + (size_t)y * d->iWidth;
        uint8_t* dst = s_gray + (size_t)(d->y + y) * s_gw + d->x;
        memcpy(dst, src, d->iWidth);
    }
    return 1;
}

const uint8_t* TextBmp::decodePhoto(const uint8_t* jpg, uint32_t len,
                                    int& w, int& h) {
    w = h = 0;
    if (!jpg || len < 256) return nullptr;

    // ---- ① 解码成灰度（按宽度选缩放档，别让 1024 源解出 1MB）----
    static JPEGDEC dec;
    if (!dec.openRAM((uint8_t*)jpg, len, jpegGrayCb)) {
        Serial.printf("[IMG] JPEG 打开失败 err=%d\n", dec.getLastError());
        return nullptr;
    }
    dec.setPixelType(EIGHT_BIT_GRAYSCALE);

    int dw = dec.getWidth(), dh = dec.getHeight();
    int opt = 0;
    if (dw > 1024)     { opt = JPEG_SCALE_QUARTER; dw /= 4; dh /= 4; }
    else if (dw > 512) { opt = JPEG_SCALE_HALF;    dw /= 2; dh /= 2; }

    s_gw = dw; s_gh = dh;
    s_gray = (uint8_t*)ps_malloc((size_t)dw * dh);
    if (!s_gray) {
        Serial.println("[IMG] 灰度缓冲分配失败");
        dec.close();
        return nullptr;
    }
    memset(s_gray, 0, (size_t)dw * dh);
    if (!dec.decode(0, 0, opt)) {
        Serial.printf("[IMG] 解码失败 err=%d\n", dec.getLastError());
        dec.close(); free(s_gray); s_gray = nullptr;
        return nullptr;
    }
    dec.close();

    // ---- ② 目标尺寸：宽撑满 384，高按比例，超过上限则反算 ----
    int tw = PRINT_W;
    int th = (int)((long)dh * PRINT_W / dw + 0.5);
    if (th > IMG_PRINT_MAXH) {
        th = IMG_PRINT_MAXH;
        tw = (int)((long)dw * IMG_PRINT_MAXH / dh + 0.5);
        if (tw < 16) tw = 16;
    }
    if (th < 1) th = 1;
    Serial.printf("[IMG] %dx%d 灰度 → 目标 %dx%d 点\n", dw, dh, tw, th);

    // ---- ③ 面积平均缩放 → 成品灰度（比最近邻取样干净）----
    uint8_t* out = (uint8_t*)ps_malloc((size_t)tw * th);
    if (!out) {
        free(s_gray); s_gray = nullptr;
        Serial.println("[IMG] 缩放缓冲分配失败");
        return nullptr;
    }
    for (int y = 0; y < th; y++) {
        const int sy0 = (int)((long)y * dh / th);
        int sy1 = (int)((long)(y + 1) * dh / th);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < tw; x++) {
            const int sx0 = (int)((long)x * dw / tw);
            int sx1 = (int)((long)(x + 1) * dw / tw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t sum = 0; int cnt = 0;
            for (int sy = sy0; sy < sy1 && sy < dh; sy++) {
                const uint8_t* row = s_gray + (size_t)sy * dw;
                for (int sx = sx0; sx < sx1 && sx < dw; sx++) { sum += row[sx]; cnt++; }
            }
            out[(size_t)y * tw + x] = cnt ? (uint8_t)(sum / cnt) : 255;
        }
    }
    free(s_gray); s_gray = nullptr;

#if IMG_CONTRAST
    // ---- ③b 对比度自动拉伸：按 1%/99% 分位把有效范围铺开 ----
    //   模型出的"线稿"常常是浅灰线压在白底上（整张图挤在 180~255 这一小段），
    //   直接二值化容易断线、留灰。拉伸后线条变实、底色变白 —— 屏幕预览和
    //   打印同时受益（改的是同一份 s_photo）。
    //   两端留 8 / 247 不拉满：避免个别极值点造成死黑/过曝。
    {
        const int n = tw * th;
        uint32_t hist[256] = {0};
        for (int i = 0; i < n; i++) hist[out[i]]++;
        const int cut = n / 100;                 // 1% 分位
        int lo = 0, hi = 255, acc = 0;
        for (int v = 0; v < 256; v++)     { acc += hist[v]; if (acc > cut) { lo = v; break; } }
        acc = 0;
        for (int v = 255; v >= 0; v--)    { acc += hist[v]; if (acc > cut) { hi = v; break; } }
        if (hi - lo >= 24) {                     // 动态范围太小就别折腾
            const int LO = 8, HI = 247;
            for (int i = 0; i < n; i++) {
                int v = (int)out[i] - lo;
                v = v * (HI - LO) / (hi - lo) + LO;
                if (v < 0) v = 0; else if (v > 255) v = 255;
                out[i] = (uint8_t)v;
            }
            Serial.printf("[IMG] 对比度拉伸 %d~%d → %d~%d\n", lo, hi, LO, HI);
        } else {
            Serial.printf("[IMG] 对比度已够（%d~%d），不拉伸\n", lo, hi);
        }
    }
#endif

    // 换上这张新图（旧的直接丢）
    free(s_photo);
    s_photo = out; s_pw = tw; s_ph = th;
    w = tw; h = th;
    Serial.printf("[IMG] 解码 %dx%d → 预览/打印用 %dx%d 灰度\n", dw, dh, tw, th);
    return out;
}

// 丢掉屏上那张图（"取消"命令用）
void TextBmp::clearPhoto() {
    free(s_photo);
    s_photo = nullptr;
    s_pw = s_ph = 0;
}

// ---- 3×3 均值模糊（权重 1-2-1 / 4 / 1-2-1，除以 16）----
//   专门治"噪点"：缩图后残留的孤立深/浅点，在二值化时会变成一个个小黑点，
//   先抹一道就干净了。只用在打印路径上（屏上预览不需要，保留原始细节）。
static void denoise3x3(int16_t* p, int w, int h) {
    int16_t* t = (int16_t*)ps_malloc((size_t)w * h * 2);
    if (!t) return;
    for (int y = 0; y < h; y++) {
        const int y0 = (y > 0) ? y - 1 : 0;
        const int y1 = (y < h - 1) ? y + 1 : h - 1;
        for (int x = 0; x < w; x++) {
            const int x0 = (x > 0) ? x - 1 : 0;
            const int x1 = (x < w - 1) ? x + 1 : w - 1;
            const int s = p[(size_t)y0 * w + x0] + 2 * p[(size_t)y0 * w + x] + p[(size_t)y0 * w + x1]
                        + 2 * p[(size_t)y * w + x0] + 4 * p[(size_t)y * w + x] + 2 * p[(size_t)y * w + x1]
                        + p[(size_t)y1 * w + x0] + 2 * p[(size_t)y1 * w + x] + p[(size_t)y1 * w + x1];
            t[(size_t)y * w + x] = (int16_t)(s / 16);
        }
    }
    memcpy(p, t, (size_t)w * h * 2);
    free(t);
}

bool TextBmp::photoReady(int& w, int& h) {
    w = s_pw; h = s_ph;
    return s_photo && s_pw > 0 && s_ph > 0;
}

// 说"打印"时才走到这里：灰度 → (去噪) → 二值化/抖动 → 1bpp → 分块打印
bool TextBmp::printPhotoPending() {
    if (!s_photo || s_pw < 16 || s_ph < 1) return false;
    const int tw = s_pw, th = s_ph;

    // ---- ④ 打印用缓冲（宽固定 384，图居中）----
    const int wBytes = PRINT_W / 8;
    const int xOff   = (PRINT_W - tw) / 2;
    int16_t* work = (int16_t*)ps_malloc((size_t)tw * th * 2);
    uint8_t* bmp = (uint8_t*)ps_malloc((size_t)wBytes * th);
    if (!work || !bmp) {
        free(work); free(bmp);
        Serial.println("[IMG] 打印缓冲分配失败");
        return false;
    }
    for (int i = 0; i < tw * th; i++) work[i] = s_photo[i];
    memset(bmp, 0, (size_t)wBytes * th);

#if IMG_DENOISE
    denoise3x3(work, tw, th);                 // 抹掉缩图残留的椒盐点
#endif

#if IMG_BIN_MODE
    // ---- ④a Floyd–Steinberg 误差扩散 → 1bpp（灰阶照片/素描用）----
    for (int y = 0; y < th; y++) {
        for (int x = 0; x < tw; x++) {
            int v = work[(size_t)y * tw + x];
            if (v < 0) v = 0; else if (v > 255) v = 255;
            const int black = (v < 128);              // 深色 = 打点（黑）
            const int err   = v - (black ? 0 : 255);
            if (black) {
                const int px = xOff + x;
                if (px >= 0 && px < PRINT_W)
                    bmp[(size_t)y * wBytes + (px >> 3)] |= (uint8_t)(0x80 >> (px & 7));
            }
            // 标准 FS 权重：右 7/16、左下 3/16、下 5/16、右下 1/16
            if (x + 1 < tw)      work[(size_t)y * tw + x + 1]          += err * 7 / 16;
            if (y + 1 < th) {
                if (x > 0)       work[(size_t)(y + 1) * tw + x - 1]    += err * 3 / 16;
                                 work[(size_t)(y + 1) * tw + x]        += err * 5 / 16;
                if (x + 1 < tw)  work[(size_t)(y + 1) * tw + x + 1]    += err * 1 / 16;
            }
        }
    }
    Serial.printf("[IMG] 二值化：抖动模式\n");
#else
    // ---- ④b 阈值二值化（线稿首选）----
    //   干净利落：只按门限切"打/不打"，不扩散误差 → 没有满地小噪点。
    {
        const int TH = gCfg.imgThreshold;   // 网页云端可调（默认 145）
        for (int y = 0; y < th; y++) {
            const int16_t* row = work + (size_t)y * tw;
            uint8_t* out = bmp + (size_t)y * wBytes;
            for (int x = 0; x < tw; x++) {
                if (row[x] < TH) {
                    const int px = xOff + x;
                    if (px >= 0 && px < PRINT_W)
                        out[px >> 3] |= (uint8_t)(0x80 >> (px & 7));
                }
            }
        }
        Serial.printf("[IMG] 二值化：阈值模式 TH=%d\n", TH);
    }
#endif
    free(work);

    // ---- ⑤ 一次打完 + 走纸（★ 打印中途不开关 VH，VM=VH）----
    logYieldLAT();                                // ★ UART 日志版：43 让给打印头 LAT
    printer.vhHold(true);                         // ★ 打印 + 走纸全程 VH 恒高
    printer.printImage(bmp, PRINT_W, th);
    free(bmp);
    printer.feedLines(IMG_FEED_MM * 8);           // 撕纸边
    printer.vhHold(false);                        // ★ 释放
    logResumeUART();                              // ★ UART 日志版：43 还给日志口
    Serial.println("[IMG] 图片打印完成");
    return true;
}
