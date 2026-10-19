// screen_text.h — 对话文字（浮层版，横屏 320x240）
//
// ★ 2026-10-03 第三版：文字不再单独占屏幕一块区域，也不再自己推屏。
//   改为由 Face 在每帧渲染完后调用 overlayDialog()，把文字直接合成进
//   表情的 RGB565 帧缓冲里 —— 所以文字是"浮在动画上面"的：
//     · 动画仍然是整屏的大脸，版面一点没被挤小
//     · "你说"和"阿蓝说"在同一块底部区域里上下排开，只用颜色区分
//     · 文字带 1px 近黑描边，压在深蓝脸上照样读得清
//     · 顺带白赚了 Face 的脏行比对：只有文字真变了，那几行才多传一次
#pragma once
#include <Arduino.h>
#include <TFTDisplay.h>

class ScreenText {
public:
    void begin(TFTDisplay* tft);

    void setUser(const String& s);     // 更新"你：…"
    void setReply(const String& s);    // 更新"阿蓝：…"
    void setDialog(const String& user, const String& reply);
    void showStatus(const String& s);  // 无对话时，底部居中显示的一行提示
    void clearDialog();

    // 供 Face 调用：把当前文字合成到 RGB565 帧缓冲上（不推屏）
    void overlayDialog(uint16_t* fb, int fw, int fh);

private:
    TFTDisplay* _tft = nullptr;
    String _user, _reply, _status;

    // 折行结果缓存：只在文字变了的时候重排一次，避免每帧做 String/vector
    // 动态分配（动画 11 帧/秒，长期跑会把堆搞碎）
    String _lu[1];
    String _lr[4];
    int    _luN = 0;
    int    _lrN = 0;
    bool   _dirty = true;

    void relayout(int maxW);
};

extern ScreenText screenText;
