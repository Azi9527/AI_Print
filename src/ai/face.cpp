// ================================================================
// face.cpp — 表情动画（照老陈给的蓝色小机器人形象重画）
//
//   造型参考图：蓝色圆身体 + 白描边 + 顶天线(杆+球) + 天线下白色灯泡条
//               + 深色椭圆"屏幕脸"(带白边) + 蓝色点阵竖眼 + 粉腮红 + 微笑
//   五官全画在"屏幕"里，对话文字浮层也正好落在屏幕下半部 —— 概念一致。
//
//   【保留的架构】（前三版验证过的，别动）：
//   · 整帧先画进 FB（PSRAM），与 PREV 逐行比对，只推变化的行
//   · render() 最后调 screenText.overlayDialog() 把文字合成进同一块 FB
//   · 动画跑独立任务(core0, FACE_TICK_MS 一帧)；屏幕访问走 GfxGuard
// ================================================================
#include "face.h"
#include "gfx.h"
#include "screen_text.h"
#include "ai_config.h"
#include <math.h>

Face face;

static const float PI_F = 3.14159265f;

// ---------------- 帧缓冲（PSRAM 优先）----------------
static uint16_t* FB   = nullptr;    // 当前帧
static uint16_t* PREV = nullptr;    // 上一帧

// ---------------- 版面（缓冲坐标）----------------
static int   FW = 320, FH = 213, FOY = 27;  // 尺寸 & 在屏上的 y
static int   CX2 = 160;     // 身体中心 x
static int   CY2 = 116;     // 身体中心 y
static int   R2  = 87;      // 身体半径
static float S2  = 1.58f;   // 线条/小部件缩放因子（= R2 / 55）
static int   EYE_Y  = 90;
static int   EYE_DX = 36;
static int   MOUTH_Y = 107;

// ---------------- 调色板（照参考图取色）----------------
static uint16_t C_BG, C_BODY, C_BODY_DK, C_BODY_HI, C_WHITE,
                C_SCREEN, C_SCREEN_ED, C_EYE, C_EYE_DIM,
                C_BLUSH, C_MOUTH, C_MOUTH_IN,
                C_WAVE, C_WAVE2, C_WAVE3,
                C_LAMP_ON, C_LAMP_OFF, C_ANT, C_ERR;

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return TFTDisplay::color565(r, g, b);
}

static void initPalette() {
    C_BG        = TFT_BLACK;
    C_BODY      = rgb( 82,164,229);   // 机身主蓝（参考图 ~#52A4E5）
    C_BODY_DK   = rgb( 54,128,199);   // 机身下部暗蓝
    C_BODY_HI   = rgb(130,196,242);   // 左上高光
    C_WHITE     = rgb(244,248,252);   // 白描边 / 灯泡条
    C_SCREEN    = rgb( 10, 17, 38);   // 屏幕脸（近黑深蓝）
    C_SCREEN_ED = rgb( 60, 72,100);   // 屏幕暗边（白边内圈）
    C_EYE       = rgb( 88,197,240);   // 点阵眼亮青蓝
    C_EYE_DIM   = rgb( 40,120,170);   // 眼睛暗态（眨眼线）
    C_BLUSH     = rgb(244,116,148);   // 粉腮红
    C_MOUTH     = rgb(226,244,255);   // 微笑线（亮白蓝）
    C_MOUTH_IN  = rgb( 60,170,230);   // 张嘴时的口腔/舌
    C_WAVE      = rgb(120,210,255);   // 说话声波
    C_WAVE2     = rgb( 70,160,225);
    C_WAVE3     = rgb( 40,110,175);
    C_LAMP_ON   = rgb(255,213, 79);   // 灯泡亮黄
    C_LAMP_OFF  = rgb(150,132, 62);   // 灯泡暗黄
    C_ANT       = rgb( 82,164,229);   // 天线
    C_ERR       = rgb(255, 96, 96);   // 出错红
}

// ================================================================
// 抗锯齿绘制原语（全部只写 FB，不碰屏幕）
// ================================================================
static inline void blendPx(int x, int y, uint16_t c, float a) {
    if ((unsigned)x >= (unsigned)FW || (unsigned)y >= (unsigned)FH) return;
    if (a <= 0) return;
    uint16_t& d = FB[(size_t)y * FW + x];
    if (a >= 1) { d = c; return; }
    // RGB565 快速按通道混合
    const int dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    const int cr = (c >> 11) & 0x1F, cg = (c >> 5) & 0x3F, cb = c & 0x1F;
    d = (uint16_t)((((int)(dr + (cr - dr) * a)) << 11)
                 | (((int)(dg + (cg - dg) * a)) << 5)
                 |  (int)(db + (cb - db) * a));
}

static void aaDiscA(float cx, float cy, float r, uint16_t c, float alpha) {
    const int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    for (int y = y0; y <= y1; y++) {
        const float dy = (float)y - cy;
        if (fabsf(dy) > r) continue;
        const float span = sqrtf(fmaxf(0.0f, r * r - dy * dy));
        const int x0 = (int)floorf(cx - span), x1 = (int)ceilf(cx + span);
        for (int x = x0; x <= x1; x++) {
            const float ddx = (float)x - cx;
            const float dist = sqrtf(ddx * ddx + dy * dy);
            float a = alpha * (r - dist);        // 边缘 1px 渐变
            if (a > 1.0f) a = 1.0f;
            blendPx(x, y, c, a);
        }
    }
}

static inline void aaDisc(float cx, float cy, float r, uint16_t c) {
    aaDiscA(cx, cy, r, c, 1.0f);
}

static void aaRing(float cx, float cy, float r, float thick, uint16_t c) {
    const float ri = r - thick * 0.5f, ro = r + thick * 0.5f;
    const int y0 = (int)floorf(cy - ro), y1 = (int)ceilf(cy + ro);
    for (int y = y0; y <= y1; y++) {
        const float dy = (float)y - cy;
        const float out2 = ro * ro - dy * dy;
        if (out2 < 0) continue;
        const float dxo = sqrtf(out2);
        const float in2 = ri * ri - dy * dy;
        for (int x = (int)floorf(cx - dxo); x <= (int)ceilf(cx + dxo); x++) {
            const float ddx = (float)x - cx;
            const float dist = sqrtf(ddx * ddx + dy * dy);
            float a = fminf(dist - ri, ro - dist) + 0.5f;
            if (a < 0) a = 0;
            if (a > 1) a = 1;
            blendPx(x, y, c, a);
        }
        (void)in2;
    }
}

static void aaOval(float cx, float cy, float rx, float ry, uint16_t c) {
    if (rx <= 0 || ry <= 0) return;
    const float scale = fminf(rx, ry);
    const int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry);
    for (int y = y0; y <= y1; y++) {
        const float ny = ((float)y - cy) / ry;
        for (int x = (int)floorf(cx - rx); x <= (int)ceilf(cx + rx); x++) {
            const float nx = ((float)x - cx) / rx;
            const float d = sqrtf(nx * nx + ny * ny) * scale;
            float a = scale - d + 0.5f;
            if (a < 0) a = 0;
            if (a > 1) a = 1;
            blendPx(x, y, c, a);
        }
    }
}

// 带整体透明度的椭圆（做柔和的明暗用；aaOval 是实心不透）
static void aaOvalA(float cx, float cy, float rx, float ry, uint16_t c, float alpha) {
    if (rx <= 0 || ry <= 0 || alpha <= 0) return;
    const float scale = fminf(rx, ry);
    const int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry);
    for (int y = y0; y <= y1; y++) {
        const float ny = ((float)y - cy) / ry;
        for (int x = (int)floorf(cx - rx); x <= (int)ceilf(cx + rx); x++) {
            const float nx = ((float)x - cx) / rx;
            const float d = sqrtf(nx * nx + ny * ny) * scale;
            float a = (scale - d + 0.5f) * alpha;
            if (a < 0) a = 0;
            if (a > 1) a = 1;
            blendPx(x, y, c, a);
        }
    }
}

static void aaLine(float x0, float y0, float x1, float y1, float thick, uint16_t c) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = sqrtf(dx * dx + dy * dy);
    const int steps = (int)(len * 2) + 1;
    for (int i = 0; i <= steps; i++) {
        const float t = (float)i / steps;
        aaDisc(x0 + dx * t, y0 + dy * t, thick * 0.5f, c);
    }
}

static void aaArc(float cx, float cy, float r, float thick,
                  float a0, float a1, uint16_t c) {
    const float len = fabsf(a1 - a0) * r;
    const int steps = (int)(len * 2) + 1;
    for (int i = 0; i <= steps; i++) {
        const float a = a0 + (a1 - a0) * (float)i / steps;
        aaDisc(cx + cosf(a) * r, cy + sinf(a) * r, thick * 0.5f, c);
    }
}

// ================================================================
// 造型层（照参考图）
// ================================================================

// 两侧小手臂：先画，被身体盖掉内侧一半，露出外侧月牙
static void drawArms() {
    const float ay = (float)CY2 + 16.0f * S2;
    const float ax = (float)R2 + 9.0f * S2;
    const float r  = 10.5f * S2;
    aaRing(CX2 - ax, ay, r, 4.0f, C_WHITE);
    aaRing(CX2 + ax, ay, r, 4.0f, C_WHITE);
    aaDisc(CX2 - ax, ay, r - 1.5f, C_BODY_DK);
    aaDisc(CX2 + ax, ay, r - 1.5f, C_BODY_DK);
}

// 圆身体：白描边 + 主蓝 + 下部渐暗 + 左上高光弧
static void drawBody() {
    aaRing((float)CX2, (float)CY2, (float)R2 + 2.0f, 8.0f, C_WHITE); // 白边（露 ~5px）
    aaDisc((float)CX2, (float)CY2, (float)R2, C_BODY);
    for (int y = CY2; y <= CY2 + R2; y++) {          // 下半部压暗，出体积感
        const float t = (float)(y - CY2) / (float)R2;
        const int half = (int)sqrtf(fmaxf(0.0f,
                (float)R2 * R2 - (float)(y - CY2) * (y - CY2)));
        for (int x = CX2 - half; x <= CX2 + half; x++)
            blendPx(x, y, C_BODY_DK, 0.45f * t * t);
    }
    aaArc((float)CX2, (float)CY2, (float)R2 * 0.82f, 8.0f,
          PI_F * 1.12f, PI_F * 1.48f, C_BODY_HI);    // 左上高光
}

// 天线：白杆 + 顶部小球（呼吸发光 + 高光点）
static void drawAntenna(uint32_t now) {
    const float top   = (float)(CY2 - R2 + 2);          // 接在头顶白边上
    const float ballY = 3.5f + 5.0f * S2;               // 球心（留出缓冲区顶）
    const float ballR = 4.6f * S2;
    aaLine((float)CX2, top, (float)CX2, ballY + ballR * 0.4f, 2.6f * S2, C_WHITE);
    const float ph = (float)(now % 2000) / 2000.0f;
    const float glow = 0.35f + 0.65f * (0.5f - 0.5f * cosf(ph * 2.0f * PI_F));
    aaDiscA((float)CX2, ballY, ballR + 3.5f, C_EYE, 0.30f * glow);  // 光晕
    aaDisc((float)CX2, ballY, ballR, C_ANT);
    aaDisc((float)CX2 - ballR * 0.3f, ballY - ballR * 0.3f, ballR * 0.35f, C_WHITE);
}

// 天线下面的白色灯泡条：思考时灯泡点亮闪烁，平时微亮
static void drawLamp(bool thinking, uint32_t now) {
    const float y = (float)(CY2 - (int)(R2 * 0.62f));
    const float half = 19.0f * S2;
    aaLine((float)CX2 - half, y, (float)CX2 + half, y, 13.0f * S2, C_WHITE);
    float on = 0.35f;
    if (thinking) {
        const float ph = (float)(now % 900) / 900.0f;
        on = 0.35f + 0.65f * (0.5f - 0.5f * cosf(ph * 2.0f * PI_F));
    }
    const float lr = 3.4f * S2;
    if (thinking) aaDiscA((float)CX2, y, lr + 4.0f, C_LAMP_ON, 0.45f * on);
    aaDisc((float)CX2, y, lr, C_LAMP_OFF);
    aaDiscA((float)CX2, y, lr, C_LAMP_ON, on);
}

// 深色椭圆"屏幕脸"（带白边）—— 五官和文字都落在它上面
static void drawScreen() {
    const float rx = (float)R2 * 0.92f, ry = (float)R2 * 0.66f;
    const float cy = (float)CY2 + (float)R2 * 0.05f;
    aaOval((float)CX2, cy, rx + 2.5f * S2, ry + 2.5f * S2, C_WHITE);  // 白边
    aaOval((float)CX2, cy, rx + 0.8f * S2, ry + 0.8f * S2, C_SCREEN_ED);
    aaOval((float)CX2, cy, rx, ry, C_SCREEN);
}

// 蓝色点阵竖眼。openPct=眨眼压扁；pdx=整只眼左右移（思考）；err=>< 眼
static void drawEyes(int openPct, int pdx, bool err) {
    const int ex1 = CX2 - EYE_DX, ex2 = CX2 + EYE_DX;
    const float rx = 8.2f * S2;
    const float ry = 11.5f * S2 * openPct / 100.0f;

    if (err) {   // >_< ：每只眼两条短斜线
        const float w = 7.0f * S2, h = 5.5f * S2, th = 2.6f * S2;
        aaLine(ex1 - w, EYE_Y - h, ex1 + w, EYE_Y + h * 0.4f, th, C_ERR);
        aaLine(ex1 + w, EYE_Y - h * 0.2f, ex1 - w, EYE_Y + h, th, C_ERR);
        aaLine(ex2 - w, EYE_Y - h * 0.2f, ex2 + w, EYE_Y + h, th, C_ERR);
        aaLine(ex2 + w, EYE_Y - h, ex2 - w, EYE_Y + h * 0.4f, th, C_ERR);
        return;
    }

    if (ry < 2.0f) {   // 眨眼：一条细线
        aaLine((float)ex1 - rx, (float)EYE_Y, (float)ex1 + rx, (float)EYE_Y,
               2.4f * S2, C_EYE_DIM);
        aaLine((float)ex2 - rx, (float)EYE_Y, (float)ex2 + rx, (float)EYE_Y,
               2.4f * S2, C_EYE_DIM);
        return;
    }

    aaOval((float)ex1 + pdx, (float)EYE_Y, rx, ry, C_EYE);
    aaOval((float)ex2 + pdx, (float)EYE_Y, rx, ry, C_EYE);
    // ★ 2026-10-04：原来这里用深色(C_SCREEN)画了两条水平暗线想要"点阵感"，
    //   但老陈看着像"眼珠里插了两条黑线"，不自然。改成下缘一层柔和的暗调
    //   （半透明、不出边界），有体积感又不突兀；再把底部略微收窄，像颗圆眼珠。
    for (int e = 0; e < 2; e++) {
        const float ex = (e == 0 ? (float)ex1 : (float)ex2) + pdx;
        aaOvalA(ex, (float)EYE_Y + ry * 0.42f, rx * 0.94f, ry * 0.52f,
                C_EYE_DIM, 0.45f);
    }
    // 顶部高光
    aaDiscA((float)ex1 + pdx - rx * 0.30f, EYE_Y - ry * 0.45f,
            rx * 0.26f, C_WHITE, 0.75f);
    aaDiscA((float)ex2 + pdx - rx * 0.30f, EYE_Y - ry * 0.45f,
            rx * 0.26f, C_WHITE, 0.75f);
}

// 粉腮红（半透明，压在屏幕内眼睛外下方）
static void drawBlush() {
    const float bx = 30.0f * S2, by = 12.0f * S2;
    const float r = 6.5f * S2;
    aaDiscA((float)CX2 - bx, (float)EYE_Y + by, r, C_BLUSH, 0.55f);
    aaDiscA((float)CX2 + bx, (float)EYE_Y + by, r, C_BLUSH, 0.55f);
}

// mode: 0=微笑 1=平线(思考) 2=撇嘴(出错) 3=张嘴(说话, open 控制)
static void drawMouth(int mode, float open) {
    switch (mode) {
        case 0:
            aaArc((float)CX2, MOUTH_Y - 6.0f * S2, 11.0f * S2, 3.2f * S2,
                  PI_F * 0.18f, PI_F * 0.82f, C_MOUTH);
            break;
        case 1:
            aaLine((float)CX2 - 7.0f * S2, (float)MOUTH_Y,
                   (float)CX2 + 7.0f * S2, (float)MOUTH_Y, 2.8f * S2, C_MOUTH);
            break;
        case 2:
            aaArc((float)CX2, MOUTH_Y + 7.0f * S2, 11.0f * S2, 3.2f * S2,
                  PI_F * 1.18f, PI_F * 1.82f, C_ERR);
            break;
        default: {
            const float ry = (2.0f + 7.0f * open) * S2;
            const float rx = (6.5f + 3.0f * open) * S2;
            aaOval((float)CX2, MOUTH_Y + 1.0f, rx, ry, C_SCREEN);
            aaOval((float)CX2, MOUTH_Y + 1.0f + ry * 0.30f,
                   rx * 0.62f, ry * 0.48f, C_MOUTH_IN);              // 舌/亮部
            break;
        }
    }
}

// 说话声波：身体两侧一圈圈推出去
static void drawWaves(int level) {
    if (level <= 0) return;
    const float span = 0.34f * PI_F;
    for (int s = 0; s < 2; s++) {
        const float base = (s == 0) ? 0.0f : PI_F;
        for (int i = 0; i < 3; i++) {
            const float rr = (float)R2 + (14.0f + i * 8.0f) * S2;
            const uint16_t col = (i == 0) ? C_WAVE : (i == 1 ? C_WAVE2 : C_WAVE3);
            const float w = (2.2f + 0.4f * level) * S2;
            aaArc((float)CX2, (float)CY2, rr, w,
                  base - span * 0.5f, base + span * 0.5f, col);
        }
    }
}

// ================================================================
// 渲染整帧
// ================================================================
void Face::render() {
    if (!FB) return;

    for (int i = 0; i < FW * FH; i++) FB[i] = C_BG;

    const bool err = (_state == FACE_ERROR);
    const uint32_t now = millis();

    drawArms();
    drawBody();
    drawAntenna(now);
    drawLamp(_state == FACE_THINK, now);
    drawScreen();

    int pdx = _pupilDX, open = _eyeOpen;
    if (_blinkOn && !err) open = 0;
    drawEyes(open, pdx, err);

    if (!err) drawBlush();

    int mmode = 3;
    if (_state == FACE_IDLE)       mmode = 0;
    else if (_state == FACE_THINK) mmode = 1;
    else if (err)                  mmode = 2;
    drawMouth(mmode, _mouthOpen);

    if (_state == FACE_SPEAK) drawWaves(_waveLevel);

    // ★ 最后叠文字浮层：脸画完了再盖上去，所以文字永远在动画上面。
    //   它只改 FB 里的像素，不动任何版面参数 —— 动画布局不受影响。
    screenText.overlayDialog(FB, FW, FH);
}

// ================================================================
// 脏行推送
// ================================================================
static inline bool rowDiff(int y) {
    return memcmp(FB + (size_t)y * FW, PREV + (size_t)y * FW,
                  (size_t)FW * 2) != 0;
}

void Face::flushFrame() {
    if (!_tft || !FB || !PREV) return;

    if (_forceFull) {
        _tft->pushPixels(0, FOY, FW, FH, FB);
        memcpy(PREV, FB, (size_t)FW * FH * 2);
        _forceFull = false;
        return;
    }

    int y = 0;
    while (y < FH) {
        while (y < FH && !rowDiff(y)) y++;
        if (y >= FH) break;

        const int start = y;
        int end = y;
        while (y < FH) {
            if (rowDiff(y)) { end = y; y++; continue; }
            // 后面 2 行内还有差别就并成一段，少发几次 setWindow
            bool bridge = false;
            for (int k = 1; k <= 2 && y + k < FH; k++)
                if (rowDiff(y + k)) { bridge = true; break; }
            if (!bridge) break;
            y++;
        }
        const int h = end - start + 1;
        _tft->pushPixels(0, FOY + start, FW, h, FB + (size_t)start * FW);
        memcpy(PREV + (size_t)start * FW, FB + (size_t)start * FW,
               (size_t)h * FW * 2);
    }
}

// ================================================================
// 生命周期
// ================================================================
void Face::begin(TFTDisplay* tft) {
    _tft = tft;
    if (!tft) return;

    // ---- 版面：状态栏以下全部给表情 ----
    FW  = tft->width();
    FOY = SCREEN_BAR_H + 1;
    FH  = tft->height() - FOY;           // 横屏 240-27 = 213
    if (FH > 224) FH = 224;
    if (FH < 80)  FH = 80;

    CX2 = FW / 2;
    R2 = (int)(FH * 0.41f);              // 213*0.41 = 87
    const int rLimit = FW / 2 - 20;      // 天线球/声波要占点宽度
    if (R2 > rLimit) R2 = rLimit;
    if (R2 < 22) R2 = 22;

    const int antGap = (int)(R2 * 0.34f);
    CY2 = antGap + R2;
    if (CY2 + R2 > FH - 6) CY2 = FH - 6 - R2;

    S2 = (float)R2 / 55.0f;

    // 五官：参考图里眼睛在屏幕上半、嘴居中偏下
    EYE_Y   = CY2 - (int)(R2 * 0.28f);
    EYE_DX  = (int)(R2 * 0.42f);
    MOUTH_Y = CY2 - (int)(R2 * 0.08f);

    _fh = FH;
    initPalette();

    if (!FB)   FB   = (uint16_t*)ps_malloc((size_t)FW * FH * 2);
    if (!FB)   FB   = (uint16_t*)malloc((size_t)FW * FH * 2);
    if (!PREV) PREV = (uint16_t*)ps_malloc((size_t)FW * FH * 2);
    if (!PREV) PREV = (uint16_t*)malloc((size_t)FW * FH * 2);
    if (!FB || !PREV) {
        Serial.printf("[FACE] ★ 帧缓冲分配失败 (需要 %uKB)\n",
                      (unsigned)((size_t)FW * FH * 2 / 1024));
        return;
    }

    _state     = FACE_IDLE;
    _t0        = millis();
    _blinkOn   = false;
    _eyeOpen   = 100;
    _pupilDX   = 0;
    _mouthOpen = 0.35f;
    _nextBlink = millis() + 1500 + random(2500);
    _forceFull = true;

    render();
    flushFrame();
    Serial.printf("[FACE] 位图 %dx%d @y=%d 身体R=%d S=%.2f\n", FW, FH, FOY, R2, S2);
}

void Face::setState(FaceState s) {
    if (s == _state && !_imageMode) return;

    // ★ 2026-10-04：图片预览模式下，表情切换**不覆盖**屏上的图。
    //   原来这里无条件 `_imageMode = false` —— 于是 ttsPlay() 一出声
    //   （回调里 setState(FACE_SPEAK)）刚生成的图立刻被表情顶掉，
    //   老陈看到的就是"一闪而过"。
    //   现在只记录状态，等 closeImage()（打印完成 / 说"取消"）才真正切回动画。
    if (_imageMode) { _state = s; _t0 = millis(); return; }

    GfxGuard g;
    _state = s;
    _t0 = millis();
    _blinkOn = false;
    _nextBlink = millis() + 2000 + random(3000);

    switch (s) {
        case FACE_LISTEN:
            _eyeOpen = 100; _mouthOpen = 0.30f; _pupilDX = 0; break;
        case FACE_THINK:
            _eyeOpen = 85;  _mouthOpen = 0.0f;  _pupilDX = -4; break;
        case FACE_SPEAK:
            _eyeOpen = 100; _mouthOpen = 0.60f; _pupilDX = 0; _waveLevel = 3; break;
        case FACE_ERROR:
            _eyeOpen = 100; _mouthOpen = 0.0f;  break;
        default:
            _eyeOpen = 100; _mouthOpen = 0.35f; _pupilDX = 0; break;
    }
    _forceFull = true;          // 换状态整帧刷一次，清掉残留
    render();
    flushFrame();
}

void Face::redrawAll() {
    GfxGuard g;
    _forceFull = true;
    render();
    flushFrame();
}

// ================================================================
// 图片预览模式：灰度图等比缩放画满表情区（状态栏以下），动画停住。
//   给"文生图先看后打"用。面积平均缩放（同打印路径），灰度直接映射
//   成 RGB565 灰阶 —— 彩屏看灰度图比 1bit 抖动更清楚。
// ================================================================
bool Face::showImage(const uint8_t* gray, int w, int h) {
    if (!_tft || !FB || !gray || w < 8 || h < 8) return false;
    GfxGuard g;
    _imageMode = true;

    // 白底 + 等比 fit 居中
    for (int i = 0; i < FW * FH; i++) FB[i] = rgb(250, 250, 250);
    int tw = FW;
    int th = (int)((long)h * FW / w + 0.5);
    if (th > FH) { th = FH; tw = (int)((long)w * FH / h + 0.5); }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    const int x0 = (FW - tw) / 2, y0 = (FH - th) / 2;

    for (int y = 0; y < th; y++) {
        const int sy0 = (int)((long)y * h / th);
        int sy1 = (int)((long)(y + 1) * h / th);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        uint16_t* dst = FB + (size_t)(y0 + y) * FW + x0;
        for (int x = 0; x < tw; x++) {
            const int sx0 = (int)((long)x * w / tw);
            int sx1 = (int)((long)(x + 1) * w / tw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t sum = 0; int cnt = 0;
            for (int sy = sy0; sy < sy1 && sy < h; sy++) {
                const uint8_t* row = gray + (size_t)sy * w;
                for (int sx = sx0; sx < sx1 && sx < w; sx++) { sum += row[sx]; cnt++; }
            }
            const int v = cnt ? (int)(sum / cnt) : 255;
            dst[x] = rgb(v, v, v);
        }
    }
    _forceFull = true;
    flushFrame();
    Serial.printf("[FACE] 图片预览 %dx%d → %dx%d @(%d,%d)\n", w, h, tw, th, x0, y0);
    return true;
}

void Face::closeImage() {
    if (!_imageMode) return;
    GfxGuard g;
    _imageMode = false;
    _forceFull = true;
    render();
    flushFrame();
}

void Face::tick() {
    if (!_tft || !FB) return;
    if (_imageMode) return;        // 图片预览模式：画面停住，不重绘表情
    const uint32_t now = millis();
    GfxGuard g;

    if (_state == FACE_ERROR && now - _t0 > 2600) { setState(FACE_IDLE); return; }

    // ---- 眨眼（说话时不眨，免得嘴动眼也动显得乱）----
    if (_state != FACE_SPEAK) {
        if (_blinkOn) {
            if (now - _blinkT > 140) {
                _blinkOn = false;
                _nextBlink = now + 1800 + random(3200);
            }
        } else if (now >= _nextBlink) {
            _blinkOn = true; _blinkT = now;
        }
    } else {
        _blinkOn = false;
    }

    const uint32_t el = now - _t0;
    switch (_state) {
        case FACE_IDLE:
            _eyeOpen = 100; _pupilDX = 0; _mouthOpen = 0.35f;
            break;
        case FACE_LISTEN:
            // 嘴轻轻一张一合（像在"专注听"）
            _eyeOpen = 100;
            _pupilDX = 0;
            _mouthOpen = 0.20f + 0.18f * (0.5f + 0.5f * sinf(el / 480.0f));
            break;
        case FACE_THINK:
            // 眼睛(整只)左右看 + 头顶灯泡闪烁
            _eyeOpen  = 85;
            _pupilDX  = ((el / 420) & 1) ? 4 : -4;
            _mouthOpen = 0.0f;
            break;
        case FACE_SPEAK: {
            static const float tab[5] = {0.22f, 0.70f, 1.00f, 0.50f, 0.18f};
            const int k = (int)((el / 105) % 5);
            _mouthOpen = tab[k];
            _waveLevel = k;
            break;
        }
        case FACE_ERROR:
            break;
    }

    render();
    flushFrame();
}

// ================================================================
// 独立动画任务（core 0）—— 主循环在跑网络/放音时照样动
// ================================================================
static void faceAnimTask(void*) {
    for (;;) {
        face.tick();
        vTaskDelay(pdMS_TO_TICKS(FACE_TICK_MS));
    }
}

void Face::startTask() {
    xTaskCreatePinnedToCore(faceAnimTask, "faceAnim", 4096, nullptr, 1, nullptr, 0);
}
