#include "status_ui.h"
#include "ai_config.h"
#include "gfx.h"
#include <WiFi.h>

StatusUI statusUI;

static const int BAR_H = SCREEN_BAR_H;   // 见 ai_config.h

void StatusUI::begin(TFTDisplay* tft) { _tft = tft; }

int StatusUI::batteryPercent(bool& charging) {
    // GPIO9: VBAT 100k/100k 半分压；GPIO10: VBUS 分压（>1.2V=已插）
    int mvBat = analogReadMilliVolts(PIN_BAT_ADC) * 2;
    int mvChg = analogReadMilliVolts(PIN_CHG_DET);
    charging = (mvChg > 1200);
    if (mvBat <= BAT_MV_MIN) return 0;
    if (mvBat >= BAT_MV_MAX) return 100;
    return (mvBat - BAT_MV_MIN) * 100 / (BAT_MV_MAX - BAT_MV_MIN);
}

// ── 纸张状态（网表：PRINT.24 —R74→ GPIO3，R73 上拉到 3V3）──
//   光断续器被纸挡住 → 输出拉低（ADC 小）= 有纸
//   没纸 → 上拉把 ADC 拉到接近 4095 = 缺纸
int StatusUI::paperState() {
    const uint32_t now = millis();
    if (_paperT && (now - _paperT) < 2000) return _paperSt;
    _paperT = now;
    const int adc = analogRead(PIN_PAPER);
    if (adc != _paperAdc) {
        _paperAdc = adc;
        const int st = (adc < PAPER_ADC_THRESH) ? PAPER_OK : PAPER_EMPTY;
        if (st != _paperSt) {
            _paperSt = st;
            Serial.printf("[PAPER] adc=%d -> %s\n", adc,
                          st == PAPER_OK ? "有纸" : "缺纸");
        } else {
            Serial.printf("[PAPER] adc=%d (%s)\n", adc,
                          st == PAPER_OK ? "有纸" : "缺纸");
        }
    }
    return _paperSt;
}

void StatusUI::drawBattery(int x, int y, int pct, bool charging) {
    // 外框 24x12 + 极头
    _tft->drawRect(x, y, 24, 12, TFT_WHITE);
    _tft->fillRect(x + 24, y + 3, 3, 6, TFT_WHITE);
    int fill = pct * 20 / 100;
    uint16_t color = pct > 20 ? TFT_GREEN : TFT_RED;
    if (fill > 0) _tft->fillRect(x + 2, y + 2, fill, 8, color);
    if (charging) {  // 闪电
        _tft->fillTriangle(x + 12, y + 1, x + 9, y + 7, x + 14, y + 7, TFT_YELLOW);
        _tft->fillTriangle(x + 14, y + 7, x + 11, y + 7, x + 12, y + 11, TFT_YELLOW);
    }
    _tft->setCursor(x + 30, y + 2);
    String s = String(pct) + "%";
    _tft->print(s.c_str(), TFT_WHITE, TFT_BLACK);
}

void StatusUI::drawWifi(int x, int y, int rssi) {
    // 三格信号：底宽顶窄，> -60 满格，> -72 两格，否则一格
    int bars = (rssi > -60) ? 3 : (rssi > -72 ? 2 : 1);
    for (int i = 0; i < 3; i++) {
        int h = 4 + i * 4;
        uint16_t c = (i < bars) ? TFT_CYAN : TFT_DARKGRAY;
        _tft->fillRect(x + i * 7, y + 12 - h, 5, h, c);
    }
    _tft->setCursor(x + 26, y + 4);
    String s = String(rssi) + "dB";
    _tft->print(s.c_str(), TFT_WHITE, TFT_BLACK);
}

// ── 打印机图标 ★ 2026-10-04 改 ──────────────────────────────
//   老陈要求：状态栏别再用"纸"字/纸形，直接画一台**打印机**，
//     有纸 = 绿色；缺纸 = 黄色（闪烁）。
//   14×16 像素内画：上方待打的纸 + 机身外框 + 出纸口狭缝 + 下方已出的纸。
void StatusUI::drawPrinter(int x, int y, int st) {
    const bool empty = (st == PAPER_EMPTY);
    // 有纸：绿色。缺纸：黄/橙交替闪（400ms），显眼又不刺眼。
    const uint16_t c = empty ? (((millis() / 400) & 1) ? TFT_YELLOW : TFT_ORANGE)
                             : TFT_GREEN;

    _tft->fillRect(x + 4, y,      7,  4, c);   // 上方：待打的纸（从机身伸出）
    _tft->drawRect(x,     y + 3, 14, 10, c);   // 机身外框
    _tft->fillRect(x + 2, y + 7, 10,  1, c);   // 出纸口（狭缝）
    _tft->fillRect(x + 4, y + 13, 7,  3, c);   // 下方：已打出的纸
}

void StatusUI::drawBar() {
    if (!_tft) return;
    GfxGuard g;
    bool chg;
    int pct = batteryPercent(chg);
    int rssi = WiFi.RSSI();
    int paper = paperState();

    _tft->fillRect(0, 0, _tft->width(), BAR_H, TFT_BLACK);
    _tft->drawFastHLine(0, BAR_H, _tft->width(), TFT_DARKGRAY);
    drawWifi(4, 4, rssi);
    drawPrinter(_tft->width() - 112, 3, paper);
    drawBattery(_tft->width() - 80, 4, pct, chg);
    _lastBat = pct; _lastChg = chg; _lastRssi = rssi;
    _lastDraw = millis();
}

void StatusUI::bootAnim() {
    if (!_tft) return;
    GfxGuard g;
    _tft->fillScreen(TFT_BLACK);
    // 【横屏适配】以屏幕中心为基准，尺寸自适应
    const int cx = _tft->width() / 2;
    const int cy = _tft->height() / 2;
    // 扩散圆环 ×3（AI "脑波" 意象）
    for (int r = 4; r <= 46; r += 2) {
        _tft->drawCircle(cx, cy - 20, r, TFT_CYAN);
        delay(16);
        if (r > 18) _tft->drawCircle(cx, cy - 20, r - 18, TFT_BLACK);
    }
    _tft->setCursor(cx - 80, cy + 34);
    _tft->print("A-LAN  AI PRINTER", TFT_WHITE, TFT_BLACK);
    // 进度点
    for (int i = 0; i < 10; i++) {
        _tft->fillCircle(cx - 45 + i * 10, cy + 62, 3, TFT_GREEN);
        delay(55);
    }
    _tft->setCursor(cx - 40, cy + 82);
    _tft->print("HELLO!", TFT_GREEN, TFT_BLACK);
    delay(350);
    _tft->fillScreen(TFT_BLACK);
}
