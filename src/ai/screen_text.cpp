// ================================================================
// screen_text.cpp — 对话文字浮层
//
// 【为什么改成浮层】
//   上一版是"表情区 96px + 下半屏单独一块黑底文字区"。老陈看了说不满意：
//   表情太小、想要脸上下占满屏幕，还希望"你说的"和"回复的"在**同一个
//   位置**用颜色区分、文字**浮在动画上面**、不影响动画布局。
//
//   所以现在：文字不占任何独立版面，由 Face 每帧渲染完脸之后调用
//   overlayDialog()，把文字像素直接写进同一块 RGBA565 帧缓冲。文字是
//   最后画的，自然盖在动画上面；而动画的坐标、半径、布局一个都没改。
//
// 【描边】
//   脸上是深蓝底，纯色文字直接压上去会糊成一片。所以每个文字像素先按
//   3×3 铺一遍近黑描边色，再画本色 —— 任何底色上都读得清。
// ================================================================
#include "screen_text.h"
#include "text_bmp.h"
#include "gfx.h"
#include "ai_config.h"
#include <vector>

ScreenText screenText;

// ---------- 浮层配色 ----------
static uint16_t C_OUTLINE, C_USER, C_BOT, C_STAT;

static void initColors() {
    if (C_OUTLINE) return;
    C_OUTLINE = TFTDisplay::color565(  3,   5,  12);   // 描边：近黑
    C_USER    = TFTDisplay::color565(175, 242, 255);   // 你说的：冰蓝
    C_BOT     = TFTDisplay::color565(255, 212,  96);   // 阿蓝说的：暖黄
    C_STAT    = TFTDisplay::color565(150, 182, 218);   // 状态提示：灰蓝
}

// ================================================================
// UTF-8 折行（按像素宽，需先 cnCanvas::setFont）
// ================================================================
static void wrapUtf8(const String& text, int wPx, std::vector<String>& out) {
    String src = text;
    src += "\n";
    int start = 0;
    for (int i = 0; i <= (int)src.length(); i++) {
        if (src[i] != '\n' && src[i] != '\r') continue;
        String para = src.substring(start, i);
        start = i + 1;
        if (para.length() == 0) { out.push_back(String("")); continue; }

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
            if (cnCanvas::width(test.c_str()) > wPx) {
                if (cur.length() == 0) { out.push_back(piece); cur = String(""); }
                else { out.push_back(cur); cur = piece; }
            } else {
                cur = test;
            }
        }
        out.push_back(cur);
    }
}

// 超宽就在末尾收一个省略号（需先 cnCanvas::setFont）
static String fitEllipsis(String s, int maxPx) {
    if (cnCanvas::width(s.c_str()) <= maxPx) return s;
    const String ell = "\u2026";
    int n = s.length();
    while (n > 0) {
        while (n > 0 && ((uint8_t)s[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        String t = s.substring(0, n) + ell;
        if (cnCanvas::width(t.c_str()) <= maxPx) return t;
    }
    return ell;
}

// ================================================================
// 把一行文字画进 RGB565 缓冲（带 1px 描边）
//   ★ 只改 fb 的像素，不碰屏幕，所以调用者（Face）的脏行比对依然有效。
// ================================================================
static void drawTextLine(uint16_t* fb, int fw, int fh,
                         int x, int y, int px, const String& s, uint16_t color) {
    if (!fb || !s.length()) return;
    const int hpx = px + 4;
    if (x < 0) x = 0;
    if (y + hpx <= 0 || y >= fh || x >= fw) return;

    cnCanvas::clear();
    cnCanvas::setFont(px);
    cnCanvas::draw(0, 0, s.c_str());
    const uint8_t* b = cnCanvas::buf();

    int wpx = cnCanvas::width(s.c_str());
    if (wpx > fw - x) wpx = fw - x;
    if (wpx <= 0) return;

    // ① 描边：文字像素的 3×3 邻域铺近黑
    for (int sy = 0; sy < hpx; sy++) {
        const int ty = y + sy;
        if (ty < 0 || ty >= fh) continue;
        for (int sx = 0; sx < wpx; sx++) {
            if (!cnCanvas::pixel(b, sx, sy)) continue;
            const int cxp = x + sx;
            const int ya = (ty > 0) ? ty - 1 : 0;
            const int yb = (ty < fh - 1) ? ty + 1 : fh - 1;
            const int xa = (cxp > 0) ? cxp - 1 : 0;
            const int xb = (cxp < fw - 1) ? cxp + 1 : fw - 1;
            for (int ny = ya; ny <= yb; ny++) {
                uint16_t* row = fb + (size_t)ny * fw;
                for (int nx = xa; nx <= xb; nx++) row[nx] = C_OUTLINE;
            }
        }
    }
    // ② 本体
    for (int sy = 0; sy < hpx; sy++) {
        const int ty = y + sy;
        if (ty < 0 || ty >= fh) continue;
        uint16_t* row = fb + (size_t)ty * fw;
        for (int sx = 0; sx < wpx; sx++)
            if (cnCanvas::pixel(b, sx, sy)) row[x + sx] = color;
    }
}

// ================================================================
// 折行缓存：只在文字变化时重排
// ================================================================
void ScreenText::relayout(int maxW) {
    cnCanvas::setFont(12);   // ★ 12px：老陈反馈 16px 字太大、占屏太多

    _luN = 0;
    if (_user.length()) {
        std::vector<String> v;
        wrapUtf8(_user, maxW, v);
        if (v.empty()) v.push_back(_user);
        _lu[0] = fitEllipsis(v[0], maxW);      // 你说的只留 1 行
        _luN = 1;
    }

    _lrN = 0;
    if (_reply.length()) {
        std::vector<String> v;
        wrapUtf8(_reply, maxW, v);
        if (v.empty()) v.push_back(_reply);
        const int maxLines = 4;
        if ((int)v.size() > maxLines) {
            v.resize(maxLines);
            v[maxLines - 1] = fitEllipsis(v[maxLines - 1], maxW);
        }
        for (size_t i = 0; i < v.size() && _lrN < 4; i++)
            _lr[_lrN++] = v[i];
    }
    _dirty = false;
}

// ================================================================
// 半透明衬底：把矩形区域往深色压一个透明度。
//   老陈实拍反馈：文字直接压在脸的白色描边/声波上，冰蓝的“你：…”
//   一行撞上白环几乎看不见（感觉“最上面一行没显示”）。
//   给文字块垫一层半透明近黑底，任何背景上都读得清，
//   且只改 fb 像素 —— Face 的脏行比对依然有效。
// ================================================================
static void blendBackdrop(uint16_t* fb, int fw, int fh,
                          int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fw - 1) x1 = fw - 1;
    if (y1 > fh - 1) y1 = fh - 1;
    const float a = 0.45f;
    const int cr = 2, cg = 4, cb = 8;                 // 目标：近黑深蓝
    for (int y = y0; y <= y1; y++) {
        uint16_t* row = fb + (size_t)y * fw;
        for (int x = x0; x <= x1; x++) {
            const uint16_t d = row[x];
            const int dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
            row[x] = (uint16_t)((((int)(dr + (cr - dr) * a)) << 11)
                              | (((int)(dg + (cg - dg) * a)) << 5)
                              |  (int)(db + (cb - db) * a));
        }
    }
}

// ================================================================
// 合成到帧缓冲
// ================================================================
void ScreenText::overlayDialog(uint16_t* fb, int fw, int fh) {
    if (!fb || !_tft) return;

    const int PADX = 10;
    const int maxW = fw - PADX * 2;
    const int LH   = 15;                  // 12px 字行高

    if (_dirty) { GfxGuard g; if (_dirty) relayout(maxW); }

    const bool hasDlg = (_luN > 0 || _lrN > 0);
    if (hasDlg) {
        // 「你：…」1 行 + 「阿蓝：…」至多 4 行，底部对齐贴底留 4px
        const int lines = _luN + _lrN;
        int y = fh - 4 - LH * lines;
        if (y < 0) y = 0;
        blendBackdrop(fb, fw, fh, 4, y - 3, fw - 5, fh - 2);
        if (_luN > 0) drawTextLine(fb, fw, fh, PADX, y, 12, _lu[0], C_USER);
        y += LH;
        for (int i = 0; i < _lrN; i++)
            drawTextLine(fb, fw, fh, PADX, y + i * LH, 12, _lr[i], C_BOT);
        return;
    }

    // 还没开始对话 → 底部居中一行小字提示（也垫衬底，压在脸上照样读得清）
    if (_status.length()) {
        cnCanvas::setFont(12);
        int w = cnCanvas::width(_status.c_str());
        int x = (w < maxW) ? (fw - w) / 2 : PADX;
        const int y = fh - 19;
        blendBackdrop(fb, fw, fh, 4, y - 2, fw - 5, fh - 2);
        drawTextLine(fb, fw, fh, x, y, 12, _status, C_STAT);
    }
}

// ================================================================
// 生命周期 / 文本更新
//   ★ 这些函数只改字符串，不推屏 —— 屏幕由 Face 动画任务负责刷。
//     改完之后 Face 下一帧（≤90ms）就会把新文字合成进去。
// ================================================================
void ScreenText::begin(TFTDisplay* tft) {
    _tft = tft;
    cnCanvas::begin();
    initColors();
    _user = ""; _reply = ""; _status = "";
    _luN = _lrN = 0;
    _dirty = true;
}

void ScreenText::clearDialog() {
    GfxGuard g;
    _user = ""; _reply = "";
    _dirty = true;
}

void ScreenText::setUser(const String& s) {
    GfxGuard g;
    _user = "你：" + s;
    _reply = "";            // 新一轮开始，先清掉上一轮的回复，避免看串
    _dirty = true;
}

void ScreenText::setReply(const String& s) {
    GfxGuard g;
    _reply = s.length() ? String(ROBOT_NAME "：") + s : String("");
    _dirty = true;
}

void ScreenText::setDialog(const String& user, const String& reply) {
    GfxGuard g;
    _user  = user.length()  ? String("你：") + user : String("");
    _reply = reply.length() ? String(ROBOT_NAME "：") + reply : String("");
    _dirty = true;
}

void ScreenText::showStatus(const String& s) {
    GfxGuard g;
    _status = s;
}
