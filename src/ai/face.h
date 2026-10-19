// face.h — 屏幕表情动画（待机 / 听 / 思考 / 说话 / 出错）
//
// 【2026-10-03 第三版】
//   ① 尺寸：表情区从上一版的 96px 扩大到「状态栏以下整屏」（约 213px），
//      脸半径、眼睛、线宽全部跟着 R2 缩放 —— 老陈要的"上下占满屏幕"。
//   ② 文字浮层：对话文字不再单独占一块区域（原来表情 96px + 文字独占
//      下半屏，脸被挤得很小）。现在改成每帧：
//          渲染脸 → 把「你：…/阿蓝：…」直接合成进同一块 RGB565 帧缓冲
//          → 一起推屏
//      文字用带黑色描边的彩色字，浮在动画上面；动画版面完全不受影响。
//      脏行比对照旧有效：只有文字真的变了，那几行才多传一次。
//
//   动画跑在独立 FreeRTOS 任务（core 0），联网等待"思考"时照样在动。
#pragma once
#include <Arduino.h>
#include <TFTDisplay.h>

enum FaceState {
    FACE_IDLE = 0,   // 待机：圆眼微笑，偶尔眨眼，天线球慢慢呼吸
    FACE_LISTEN,     // 听：眼睛放大，眉毛上扬，嘴轻轻张合
    FACE_THINK,      // 思考：眼球左右飘，眉毛一高一低，右上角思考气泡
    FACE_SPEAK,      // 说话：嘴开合，脸两侧声波一圈圈推出去
    FACE_ERROR,      // 出错：>_< 眼，嘴角下垂，红脸
};

class Face {
public:
    void begin(TFTDisplay* tft);
    void setState(FaceState s);
    FaceState state() const { return _state; }
    void tick();                 // 动画推进（算参数 → 渲染 → 推脏行）
    void redrawAll();            // 整帧重画（强制全推）
    void startTask();            // 启动动画任务（core 0）
    int  height() const { return _fh; }   // 表情区高度（缓冲坐标系）

    // 【图片预览】灰度图等比缩放画满表情区，动画停住（老陈定的流程：
    //   生成图先在屏上看，说"打印"再打）。退出：closeImage() 或任意 setState()。
    bool showImage(const uint8_t* gray, int w, int h);
    void closeImage();           // 退出图片预览，恢复表情动画
    bool imageMode() const { return _imageMode; }   // 屏上是否正显示生成的图

private:
    TFTDisplay* _tft = nullptr;
    FaceState   _state = FACE_IDLE;
    uint32_t    _t0 = 0;
    int         _fh = 0;

    // 眨眼
    bool        _blinkOn = false;
    uint32_t    _blinkT = 0;
    uint32_t    _nextBlink = 0;

    // 各状态共用的动画量（每帧重算）
    int         _eyeOpen  = 100;   // 眼睛睁开百分比 0~100（眨眼/听/思考）
    int         _pupilDX  = 0;     // 眼睛整体左右偏移（思考时左右看）
    float       _mouthOpen = 0.35f;// 嘴张开程度 0~1
    int         _waveLevel = 0;    // 声波强弱 0~4
    int         _dots      = 0;    // （保留字段，灯泡条用 now 直接算）

    bool        _forceFull = true; // 下一帧全推（换状态/开机时用）
    bool        _imageMode = false;// 图片预览模式：tick 停住，屏上停一张图

    void render();                 // 把当前表情画进 _fb
    void flushFrame();             // 脏行推送
};

extern Face face;
