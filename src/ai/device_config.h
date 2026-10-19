#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// 网页上位机可配的设备参数（NVS 持久化，JSON blob）
// 生效项：设备名 / 音量 / TTS 音色·语速·音调 / 人设附加 / 图片二值化阈值
struct DeviceConfig {
    char deviceName[33];   // 设备名（屏上称呼，也回给网页）
    int  volume;           // 0~100（应用时映射到 ES8311 的 0~255）
    int  ttsPer;           // 百度音色 per：0 度小美 / 1 度小宇 / 3 度逍遥 / 4 度丫丫 / 110 度小童 / 111 度小萌
    int  ttsSpd;           // 语速 0~15（默认 5）
    int  ttsPit;           // 音调 0~15（默认 5）
    char persona[257];     // 人设附加词（拼在默认提示词后面，空=只用默认）
    int  imgThreshold;     // 打印二值化阈值 0~255（默认 145）
    int  historyTurns;     // 对话带的历史轮数 0~6（默认 2）
    char greeting[161];    // 开机打招呼语（云端可配）
    char weatherCity[33];  // 天气查询默认城市（模型没听出城市时用）
    int  replyLen;         // 回复长度档位 0=简短(~40字) 1=适中(~100字) 2=详细(~250字)
    int  uploadAudio;      // 1=把用户录音和回复音频上传到控制台（对比识别率用）

    void load();           // NVS 读，缺省用默认
    bool save();           // NVS 写
    void toJson(JsonDocument& d) const;
    bool fromJson(JsonDocument& d);   // 校验+限幅，失败返回 false
    bool cloudFetch();     // 从云端 API 拉本机(MAC后6位)配置，成功则应用+存NVS
};

extern DeviceConfig gCfg;   // 全局唯一实例

// 设备 ID = MAC 后 6 位（去冒号，大写），控制台/云端都按它认设备
String cfgDeviceId();

// 默认系统提示词（ai_config.h 的 LLM_SYSTEM_PROMPT）+ 长度规则 + 附加人设拼好
String cfgSystemPrompt();
// 回复长度的措辞（拼进 system prompt）和 max_tokens（weather 第二轮等）
String cfgLenRule();
int    cfgReplyMaxTokens();
