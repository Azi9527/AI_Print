// cloud_ai.h — WiFi + 语音识别 + 大模型对话 + 语音合成
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <functional>

class CloudAI {
public:
    // 配网 AP 名："LGM_" + MAC 后 4 位（配网提示屏也用它，保持一致）
    String apName();

    // 连 WiFi（失败进配网门户）。onPortal：配网门户（AP）开起来时回调，
    // 用来在屏上显示配网操作步骤；不关心传 nullptr。
    bool begin(void (*onPortal)() = nullptr);
    bool isConnected() const { return _connected; }

    // 语音识别：PCM 16k/16bit/mono → 文本（百度 ASR）
    String asr(const uint8_t* pcm, uint32_t len);

    // 获取百度 access_token（开机自检也调它）。
    // 失败原因会打到串口：Key 未填 / HTTP 码 / 百度返回的错误描述
    bool   ensureToken();

    // 大模型对话：维护多轮上下文，返回回复文本（工具调用：画图/查天气）
    String chat(const String& userText);
    void   resetChat();

    // 查天气原始数据（Open-Meteo 免费接口）→ 喂给 LLM 的 JSON；失败返回空串
    String queryWeatherRaw(const String& city);

    // 点阵画生成：让大模型输出 cols×rows 的 '#'/'.' 点阵
    // 返回多行字符串（每行 cols 个字符），失败返回空串
    String chatArt(const String& userText, int cols, int rows);

    // ★ 2026-10-09：把"想画什么"扩写成英文绘图提示词（结合最近 2 轮上下文）。
    //   失败返回空串（调用方退回"原话 + 中文风格后缀"）。
    String expandDrawPrompt(const String& said);

    // 语音合成：文本 → 16k/16bit/mono PCM（百度 TTS，aue=4 免解码）
    // 返回堆上缓冲（调用方负责 free），失败返回 nullptr
    uint8_t* tts(const String& text, uint32_t& outLen);

    // 流式合成+播放（★推荐）：边下边播，第一声最多等 1~2 秒，
    // 不用像 tts() 那样整段下完才开始响。
    //   sink(data,len,first)：收到一段 PCM 就调一次（len 恒为偶数字节），
    //   first=true 标志第一块（可用来切表情/开功放）。
    // 返回 false = 合成失败（HTTP 错误或百度报错 JSON）
    //   ★ 用 std::function（不再是函数指针）：调用方要"边播边把 PCM 攒进
    //     PSRAM 做缓存"，必须能捕获局部变量。
    //   ★ 返回 false = 调用方要求**立刻停止合成**（用于"打断播报"barge-in：
    //     用户一开口，sink 里检测到就返回 false，ttsStream 马上收工不再下载）。
    typedef std::function<bool(const uint8_t* data, uint32_t len, bool first)> TtsChunkFn;
    bool ttsStream(const String& text, TtsChunkFn sink);

    // ── 文生图（百度千帆）★ 2026-10-04 新增 ──────────────────────
    // 文本 → 图片 URL（同步等模型出图，实测 qwen-image 512² 约 20 秒）；
    // 失败返回空串。用的是千帆 API Key（Bearer），不是语音那套 AK/SK。
    String textToImage(const String& prompt);

    // 把图片 URL 下载到 PSRAM，返回缓冲指针（调用方负责 free），失败 nullptr
    uint8_t* downloadImage(const String& url, uint32_t& outLen);

    // ★ 网络自检（改版27）：纯下载测速 —— 不经 sink、不播放，量 ESP32 的真实收包能力。
    //   只在 TTS_SPEED_TEST=1 时由 main 调用；见 ai_config.h 的判读说明。
    void speedTest();

    // 屏上提示当前网络阶段（回调由 main 注入）
    void setOnStatus(void (*cb)(const String& msg)) { _onStatus = cb; }

    int  rssi() const { return WiFi.RSSI(); }

private:
    bool   _connected = false;
    String _token;                    // 百度 access_token（带过期管理简化版）
    String _history[8];               // 4 轮上下文（user/assistant 交替）
    int    _histN = 0;
    void   (*_onStatus)(const String&) = nullptr;
    String urlEncode(const String& s);
};

extern CloudAI cloud;
