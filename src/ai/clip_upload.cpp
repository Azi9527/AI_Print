#include "clip_upload.h"
#include "ai_config.h"
#include "device_config.h"
#include <WiFi.h>
#include <HTTPClient.h>

namespace {

constexpr int      QDEPTH     = 4;              // 排队上限：超过就丢（宁可丢，不能堵）
constexpr uint32_t REPLY_MAX  = 700 * 1024;     // 回复音频上限（≈21s @16k16bit），超了不上传
constexpr int      TXT_MAX    = 320;            // 文字上限（识别文本/回复文本）
constexpr int      TASK_STACK = 8192;
constexpr int      TASK_PRIO  = 1;

struct Job {
    char     kind[8];        // "user" / "reply"
    uint32_t tid;            // 轮次号（云端按它把用户录音和回复配对）
    uint8_t* pcm;            // PSRAM，任务自己 free
    uint32_t len;
    int      peak;           // 原始峰值（user 才有意义）
    char     text[TXT_MAX];
};

QueueHandle_t sQ    = nullptr;
TaskHandle_t  sTask = nullptr;
uint32_t      sTurn = 1;
portMUX_TYPE  sMux  = portMUX_INITIALIZER_UNLOCKED;

// 回复音频攒在本地，攒完才建 Job
uint8_t* sCap     = nullptr;
uint32_t sCapLen  = 0;
uint32_t sCapBuf  = 0;

String urlEncode(const String& s) {
    static const char* hex = "0123456789ABCDEF";
    String o;
    o.reserve(s.length() + 16);
    for (size_t i = 0; i < s.length(); i++) {
        uint8_t c = (uint8_t)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 0x0F]; }
    }
    return o;
}

// 真正上传一条：POST 原始 PCM，参数走 query（控制台/服务器按它建索引）
void doUpload(Job& j) {
    String url = String(CFG_API_URL)
               + "?action=clip&id=" + cfgDeviceId()
               + "&kind=" + j.kind
               + "&tid=" + String(j.tid)
               + "&peak=" + String(j.peak)
               + "&rate=" + String(SAMPLE_RATE)
               + "&text=" + urlEncode(String(j.text));

    HTTPClient http;
    if (!http.begin(url)) { Serial.println("[CLIP] http.begin 失败"); return; }
    http.setTimeout(15000);
    http.addHeader("Content-Type", "application/octet-stream");
    int code = http.POST(j.pcm, j.len);
    http.end();
    Serial.printf("[CLIP] ↑ %s %u 字节 → http %d（轮 %u）\n",
                  j.kind, (unsigned)j.len, code, (unsigned)j.tid);
}

void taskMain(void*) {
    Job j;
    for (;;) {
        if (xQueueReceive(sQ, &j, portMAX_DELAY) == pdTRUE) {
            if (WiFi.status() == WL_CONNECTED && j.pcm && j.len) doUpload(j);
            else Serial.printf("[CLIP] 没联网/空数据，丢弃 %s\n", j.kind);
            if (j.pcm) free(j.pcm);
        }
    }
}

// 入队（接管 pcm 所有权）
bool push(const char* kind, uint8_t* pcm, uint32_t len, int peak, const String& text, uint32_t tid) {
    if (!pcm || !len) { if (pcm) free(pcm); return false; }
    if (!gCfg.uploadAudio) { free(pcm); return false; }
    if (!sQ) { free(pcm); return false; }

    Job j = {};
    strlcpy(j.kind, kind, sizeof(j.kind));
    j.tid  = tid;
    j.pcm  = pcm;
    j.len  = len;
    j.peak = peak;
    strlcpy(j.text, text.c_str(), sizeof(j.text));
    if (xQueueSend(sQ, &j, 0) != pdTRUE) {
        Serial.println("[CLIP] 队列满，丢弃一条（网络太慢）");
        free(pcm);
        return false;
    }
    return true;
}
}  // namespace

namespace ClipUp {

void begin() {
    if (sQ) return;
    sQ = xQueueCreate(QDEPTH, sizeof(Job));
    if (!sQ) { Serial.println("[CLIP] 队列创建失败"); return; }
    xTaskCreatePinnedToCore(taskMain, "clipup", TASK_STACK, nullptr, TASK_PRIO, &sTask, 1);
    Serial.printf("[CLIP] 录音上传就绪（%s）\n", gCfg.uploadAudio ? "开" : "关");
}

uint32_t newTurn() {
    portENTER_CRITICAL(&sMux);
    uint32_t t = ++sTurn;
    portEXIT_CRITICAL(&sMux);
    return t;
}

uint32_t curTurn() {
    portENTER_CRITICAL(&sMux);
    uint32_t t = sTurn;
    portEXIT_CRITICAL(&sMux);
    return t;
}

bool enqueueUser(const uint8_t* pcm, uint32_t len, const String& text, int peak) {
    if (!pcm || !len || !gCfg.uploadAudio) return false;
    uint8_t* copy = (uint8_t*)ps_malloc(len);
    if (!copy) return false;
    memcpy(copy, pcm, len);
    return push("user", copy, len, peak, text, curTurn());
}

bool enqueueUserOwned(uint8_t* pcm, uint32_t len, const String& text, int peak) {
    return push("user", pcm, len, peak, text, curTurn());
}

bool capBegin() {
    if (!gCfg.uploadAudio) return false;
    sCapLen = 0; sCapBuf = 0;
    if (sCap) { free(sCap); sCap = nullptr; }
    sCap = (uint8_t*)ps_malloc(REPLY_MAX);
    if (!sCap) { Serial.println("[CLIP] PSRAM 不够，本次不上传回复音频"); return false; }
    sCapBuf = REPLY_MAX;
    return true;
}

void capFeed(const uint8_t* data, uint32_t len) {
    if (!sCap || !data || !len) return;
    if (sCapLen + len > sCapBuf) {                     // 超上限：整段放弃
        free(sCap); sCap = nullptr; sCapBuf = 0; sCapLen = 0;
        return;
    }
    memcpy(sCap + sCapLen, data, len);
    sCapLen += len;
}

void capAbort() {
    if (sCap) { free(sCap); sCap = nullptr; }
    sCapLen = 0; sCapBuf = 0;
}

bool capEndAndUpload(const String& text) {
    if (!sCap || !sCapLen) { capAbort(); return false; }
    uint8_t* buf = sCap;
    uint32_t n   = sCapLen;
    sCap = nullptr; sCapLen = 0; sCapBuf = 0;
    return push("reply", buf, n, 0, text, curTurn());
}

int pending() { return sQ ? (int)uxQueueMessagesWaiting(sQ) : 0; }
}  // namespace ClipUp
