// status_ui.h — 屏幕状态栏（WiFi 信号 / 纸张 / 电量 / 充电）+ 开机动画
#pragma once
#include <Arduino.h>
#include <TFTDisplay.h>

class StatusUI {
public:
    enum { PAPER_OK = 0, PAPER_EMPTY = 1 };

    void begin(TFTDisplay* tft);
    // 读取电池：返回 0~100，charging=是否插 USB
    int  batteryPercent(bool& charging);
    // 纸张状态（GPIO3 光断续器，缓存 2 秒）
    int  paperState();
    bool paperOk() { return paperState() == PAPER_OK; }
    int  paperAdc() const { return _paperAdc; }
    // 刷新状态栏（建议每 5s 调一次或状态变化时调）
    void drawBar();
    // 开机动画（阻塞，总时长约 1.2s）
    void bootAnim();

private:
    TFTDisplay* _tft = nullptr;
    uint32_t    _lastDraw = 0;
    int         _lastBat = -1;
    bool        _lastChg = false;
    int         _lastRssi = 0;
    int         _paperSt = PAPER_OK;
    int         _paperAdc = -1;
    uint32_t    _paperT = 0;

    void drawBattery(int x, int y, int pct, bool charging);
    void drawWifi(int x, int y, int rssi);
    // 【打印机图标】有纸=绿色；缺纸=黄色闪烁（老陈 2026-10-04 要求，
    //   原来的"纸 + 斜杠"图标换成打印机造型，颜色也更直观）
    void drawPrinter(int x, int y, int st);
};

extern StatusUI statusUI;
