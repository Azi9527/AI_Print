#include "device_config.h"
#include "ai_config.h"
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>

DeviceConfig gCfg;

static void clampAll() {
    if (gCfg.volume < 0) gCfg.volume = 0;
    if (gCfg.volume > 100) gCfg.volume = 100;
    if (gCfg.ttsPer < 0) gCfg.ttsPer = 0;
    if (gCfg.ttsSpd < 0) gCfg.ttsSpd = 0;
    if (gCfg.ttsSpd > 15) gCfg.ttsSpd = 15;
    if (gCfg.ttsPit < 0) gCfg.ttsPit = 0;
    if (gCfg.ttsPit > 15) gCfg.ttsPit = 15;
    if (gCfg.imgThreshold < 100) gCfg.imgThreshold = 100;
    if (gCfg.imgThreshold > 200) gCfg.imgThreshold = 200;
    if (gCfg.historyTurns < 0) gCfg.historyTurns = 0;
    if (gCfg.historyTurns > 6) gCfg.historyTurns = 6;
    if (gCfg.replyLen < 0) gCfg.replyLen = 0;
    if (gCfg.replyLen > 2) gCfg.replyLen = 2;
    if (gCfg.uploadAudio < 0) gCfg.uploadAudio = 0;
    if (gCfg.uploadAudio > 1) gCfg.uploadAudio = 1;
}

static void setDefaults() {
    strlcpy(gCfg.deviceName, "阿蓝", sizeof(gCfg.deviceName));
    gCfg.volume = 60;
    gCfg.ttsPer = 0;
    gCfg.ttsSpd = 5;
    gCfg.ttsPit = 5;
    gCfg.persona[0] = 0;
    gCfg.imgThreshold = IMG_THRESHOLD;
    gCfg.historyTurns = 2;
    strlcpy(gCfg.greeting,
            "我是阿蓝，一个可以陪你聊天的打印机器人。想聊天，或者让我画张图打印出来，都行。我们开始聊点什么？",
            sizeof(gCfg.greeting));
    strlcpy(gCfg.weatherCity, "深圳", sizeof(gCfg.weatherCity));
    gCfg.replyLen = 1;
    gCfg.uploadAudio = 1;   // 默认上传录音（控制台比对识别率用；嫌吵可在网页关掉）
}

void DeviceConfig::load() {
    setDefaults();
    Preferences p;
    p.begin("alicfg", true);
    String j = p.getString("cfg", "");
    p.end();
    if (j.length()) {
        JsonDocument d;
        if (deserializeJson(d, j) == DeserializationError::Ok) fromJson(d);
    }
    clampAll();
}

bool DeviceConfig::save() {
    clampAll();
    JsonDocument d;
    toJson(d);
    String j;
    serializeJson(d, j);
    Preferences p;
    p.begin("alicfg", false);
    bool ok = p.putString("cfg", j);
    p.end();
    return ok;
}

void DeviceConfig::toJson(JsonDocument& d) const {
    d["deviceName"] = deviceName;
    d["volume"] = volume;
    d["ttsPer"] = ttsPer;
    d["ttsSpd"] = ttsSpd;
    d["ttsPit"] = ttsPit;
    d["persona"] = persona;
    d["imgThreshold"] = imgThreshold;
    d["historyTurns"] = historyTurns;
    d["greeting"] = greeting;
    d["weatherCity"] = weatherCity;
    d["replyLen"] = replyLen;
    d["uploadAudio"] = uploadAudio;
}

bool DeviceConfig::fromJson(JsonDocument& d) {
    const char* n = d["deviceName"] | (const char*)nullptr;
    if (n && strlen(n)) strlcpy(deviceName, n, sizeof(deviceName));
    if (!d["volume"].isNull())    volume = d["volume"].as<int>();
    if (!d["ttsPer"].isNull())    ttsPer = d["ttsPer"].as<int>();
    if (!d["ttsSpd"].isNull())    ttsSpd = d["ttsSpd"].as<int>();
    if (!d["ttsPit"].isNull())    ttsPit = d["ttsPit"].as<int>();
    const char* ps = d["persona"] | (const char*)nullptr;
    if (ps) strlcpy(persona, ps, sizeof(persona));
    if (!d["imgThreshold"].isNull()) imgThreshold = d["imgThreshold"].as<int>();
    if (!d["historyTurns"].isNull()) historyTurns = d["historyTurns"].as<int>();
    const char* gr = d["greeting"] | (const char*)nullptr;
    if (gr) strlcpy(greeting, gr, sizeof(greeting));
    const char* wc = d["weatherCity"] | (const char*)nullptr;
    if (wc && strlen(wc)) strlcpy(weatherCity, wc, sizeof(weatherCity));
    if (!d["replyLen"].isNull()) replyLen = d["replyLen"].as<int>();
    if (!d["uploadAudio"].isNull()) uploadAudio = d["uploadAudio"].as<int>();
    clampAll();
    return true;
}

String cfgDeviceId() {
    String mac = WiFi.macAddress();          // AA:BB:CC:DD:EE:FF
    String id = mac.substring(9, 11) + mac.substring(12, 14) + mac.substring(15, 17);
    id.toUpperCase();
    return id;                               // 控制台就填这 6 位
}

String cfgSystemPrompt() {
    String s = LLM_SYSTEM_PROMPT;
    s += cfgLenRule();
    if (gCfg.persona[0]) { s += " "; s += gCfg.persona; }
    return s;
}

// 回复长度三档：措辞拼进 system prompt；开场合句提醒“会讲笑话/故事”
String cfgLenRule() {
    switch (gCfg.replyLen) {
    case 0: return "每次回复控制在40字以内，一句话说完。";
    case 1: return "每次回复一般不超过100字，两三句讲清楚；让你讲笑话、小故事时大方开讲，把内容讲完整。";
    default:return "回答可以生动详细，一般不超过250字；让你讲笑话、小故事时把内容讲完整、讲有趣。";
    }
}

int cfgReplyMaxTokens() {
    switch (gCfg.replyLen) {
    case 0: return 60;
    case 1: return 160;
    default:return 350;
    }
}

bool DeviceConfig::cloudFetch() {
    String id = cfgDeviceId();
    String url = String(CFG_API_URL) + "?id=" + id;
    Serial.printf("[CFG] 设备ID=%s → 拉取云端配置\n", id.c_str());   // 控制台就填这6位

    HTTPClient http;
    if (!http.begin(url)) return false;
    http.setTimeout(8000);
    int code = http.GET();
    String resp = http.getString();
    http.end();
    if (code != 200) {
        Serial.printf("[CFG] 云端拉取 http %d\n", code);
        return false;
    }
    JsonDocument d;
    if (deserializeJson(d, resp)) return false;
    JsonObject c = d["cfg"];
    if (c.isNull()) return false;
    JsonDocument cj;
    cj.set(c);
    if (!fromJson(cj)) return false;
    save();                                   // 存本地缓存，断网下次用
    Serial.printf("[CFG] 云端配置已应用（设备 %s）\n", id.c_str());
    return true;
}
