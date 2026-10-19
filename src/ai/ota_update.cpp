#include "ota_update.h"
#include "ai_config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>

// 版本比较：按 "." 分段比数字（1.0.10 > 1.0.9）。a<b:-1 a==b:0 a>b:1
static long readNum(const char*& p) {
    long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == '.') p++;
    return v;
}
static int verCmp(const char* a, const char* b) {
    const char* pa = a; const char* pb = b;
    for (int i = 0; i < 4; i++) {
        long va = readNum(pa), vb = readNum(pb);
        if (va != vb) return va < vb ? -1 : 1;
        if (!*pa && !*pb) return 0;
    }
    return 0;
}

// 进度钩子：Update 的全局回调 → 转给外部画屏
static void (*sProg)(int, const char*) = nullptr;
static const char*  sVer = nullptr;
static void onUpdateProgress(size_t done, size_t total) {
    if (sProg && total) sProg((int)((uint64_t)done * 100 / total), sVer);
}

void OtaUpdate::check(void (*onProgress)(int pct, const char* ver)) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] 没联网，跳过检查");
        return;
    }

    // 1. 拉引导文件
    HTTPClient http;
    http.begin(OTA_MANIFEST_URL);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[OTA] 引导文件拉取失败 HTTP %d，跳过\n", code);
        http.end();
        return;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Serial.printf("[OTA] 引导文件解析失败: %s，跳过\n",
                      body.substring(0, 80).c_str());
        return;
    }
    const char* ver = doc["VERSION"] | "";
    const char* url = doc["up_url"] | "";
    if (!*ver || !*url) {
        Serial.println("[OTA] 引导文件缺 VERSION/up_url，跳过");
        return;
    }

    int c = verCmp(FW_VERSION, ver);
    Serial.printf("[OTA] 当前 %s / 服务器 %s → %s\n", FW_VERSION, ver,
                  c < 0 ? "有新版本，升级" : "已是最新，跳过");
    if (c >= 0) return;

    // 2. 下载固件写入 OTA 分区
    Serial.printf("[OTA] 下载固件 %s\n", url);
    sProg = onProgress; sVer = ver;
    if (sProg) sProg(0, ver);

    http.begin(url);
    http.setTimeout(30000);
    code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[OTA] 固件下载失败 HTTP %d，跳过\n", code);
        http.end();
        return;
    }
    int total = http.getSize();
    if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) {
        Serial.printf("[OTA] OTA 分区申请失败(%dB)——固件比分区大？\n", total);
        http.end();
        return;
    }
    Update.onProgress(onUpdateProgress);
    size_t written = Update.writeStream(http.getStream());
    http.end();

    if (Update.end(true) && written) {
        Serial.printf("[OTA] 写入完成 %u 字节，重启生效\n", (unsigned)written);
        if (sProg) sProg(100, sVer);
        delay(800);                      // 给屏幕最后一帧的时间
        ESP.restart();                   // 不返回
    }
    Serial.printf("[OTA] 写入失败 written=%u err=%d\n",
                  (unsigned)written, (int)Update.getError());
    Update.printError(Serial);
    Update.abort();
}
