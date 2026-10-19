#include "net_diag.h"
#include "ai_config.h"
#include <WiFi.h>
#include <esp_system.h>

// RTC 慢速内存：软件复位/看门狗复位不清零，只有断电才清 —— 判"反复重启"用
#define BOOT_MAGIC 0xA5A55A5A
static RTC_NOINIT_ATTR uint32_t sBootMagic;
static RTC_NOINIT_ATTR uint32_t sBoots;
uint32_t NetDiag::bootCount() { return sBoots; }

static volatile int sLastDiscReason = -1;   // 最近一次 STA 断开原因
static volatile int sEventTick = 0;         // 收到过事件就 +1（区分"没事件"）

// 常见断开原因码（esp_wifi_types.h）
static const char* discName(int r) {
    switch (r) {
        case 2:   return "AUTH_EXPIRE(认证超时)";
        case 8:   return "ASSOC_LEAVE(主动离开)";
        case 15:  return "4WAY_HANDSHAKE_TIMEOUT(★密码错)";
        case 200: return "BEACON_TIMEOUT(信号差/干扰)";
        case 201: return "NO_AP_FOUND(★找不到这个热点)";
        case 202: return "AUTH_FAIL(认证失败/密码错)";
        case 203: return "ASSOC_FAIL(AP拒绝)";
        case 205: return "CONNECTION_FAIL";
        default:  return "其他(查 esp_wifi_types.h)";
    }
}

static void onWiFiEvent(WiFiEvent_t e, WiFiEventInfo_t info) {
    sEventTick++;
    switch (e) {
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            sLastDiscReason = info.wifi_sta_disconnected.reason;
            Serial.printf("[NETDBG] ★STA断开 reason=%d (%s)\n",
                          sLastDiscReason, discName(sLastDiscReason));
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[NETDBG] ★拿到IP %s RSSI=%d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            break;
        case ARDUINO_EVENT_WIFI_AP_START:
            Serial.println("[NETDBG] ★配网热点已开启");
            break;
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.println("[NETDBG] ★有手机连上了配网热点");
            break;
        default: break;   // 其余事件太吵，不打
    }
}

// 每秒一行快照：连上后再打 30 秒就收工（平时刷屏太吵）
static void diagTask(void*) {
    uint32_t connectedSince = 0;
    for (;;) {
        bool conn = (WiFi.status() == WL_CONNECTED);
        if (!conn) connectedSince = 0;
        else if (!connectedSince) connectedSince = millis();

        Serial.printf("[NETDBG] t=%us 启动#%u status=%d rssi=%d 断开reason=%d "
                      "heap=%u vbat=%dmV\n",
                      (unsigned)(millis() / 1000), (unsigned)sBoots,
                      (int)WiFi.status(), (int)WiFi.RSSI(), sLastDiscReason,
                      (unsigned)(ESP.getFreeHeap() / 1024),
                      analogRead(PIN_BAT_ADC) * 3300 / 4095 * 2);  // 100k/100k 分压

        if (conn && millis() - connectedSince > 30000) break;   // 稳了就收工
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(nullptr);
}

void NetDiag::begin() {
    if (sBootMagic != BOOT_MAGIC) { sBootMagic = BOOT_MAGIC; sBoots = 1; }
    else sBoots++;
    Serial.printf("[NETDBG] 本次上电第 %u 次启动（计数狂涨=在反复重启）；"
                  "复位原因=%d\n", (unsigned)sBoots, (int)esp_reset_reason());
    WiFi.onEvent(onWiFiEvent);
    xTaskCreatePinnedToCore(diagTask, "netdiag", 4096, nullptr, 1, nullptr, 0);
}
