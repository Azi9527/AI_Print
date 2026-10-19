// ================================================================
// ai_print.cpp — AI 语音热敏打印机「阿蓝」主程序
// 硬件：ESP32-S3R8（立创实战派S3 同源板）+ ES7210/ES8311 + JX-2R-17 + ST7789
//
// 【2026-10-03 改版 2】
//   · 屏幕：横屏 320x240；顶部状态栏(WiFi/纸张/电量) +
//           中部表情动画(听/思考/说话) + 下部对话文字 + 最底状态行
//   · 交互：免按键。常时监听(VAD)，检测到你开口就自动录。
//           按键(GPIO8) = 唤醒 / 一键强制说一句话（VAD 不灵时的兜底）
//   · 语音命令："打印" / "画个…" / "音量…" / "电量"，其余走 AI 对话
//   · 联网成功后自我介绍一下
//
// 编译：pio run -e ai_voice
// ================================================================
#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <ThermalPrinter.h>
#include <TFTDisplay.h>
#include "ai/ai_config.h"
#include "ai/gfx.h"
#include "ai/audio_io.h"
#include "ai/cloud_ai.h"
#include "ai/device_config.h"
#include "ai/clip_upload.h"
#include "ai/ota_update.h"
#include "ai/net_diag.h"
#include "ai/status_ui.h"
#include "ai/screen_text.h"
#include "ai/face.h"
#include "ai/text_bmp.h"
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>   // ets_printf：ROM 直出 UART0，绕过 Serial 对象

// ── ★ 串口兜底探针（2026-10-10）────────────────────────────────
// 治"ROM 日志有、进 app 就静默"。ets_printf 走 ROM 启动时配好的
// UART0（GPIO43@115200），不经过 Arduino 的 Serial —— 哪怕 Serial 映射
// 错了它也出得来。判读：
//   三条全无     = 固件没烧进去 / app 没被执行
//   只出 RAW0    = 崩在全局构造之后、setup 之前
//   出到 RAW     = 崩在串口初始化附近
//   RAW0/RAW/BOOT 齐 = 串口链路没问题，问题在别处
struct RawBootProbe {
    RawBootProbe() { ets_printf("\r\n[RAW0] global ctor OK —— flash 里的 app 确实在跑\r\n"); }
};
static RawBootProbe _rawBootProbe;

// printer 不加 static：text_bmp.cpp 里 extern 引用它打印文本
ThermalPrinter printer;
static TFTDisplay     tft;

static String   lastReply;        // 最近一条 AI 回复（可打印）
static bool     btnLast = false;
static bool     screenOn = true;
static uint32_t lastActivity = 0; // 用于空闲关背光
static int      lastEv = 0;       // VAD 状态去抖
static uint32_t wakeUntil = 0;    // 唤醒保持到什么时候（ms）；0 = 未唤醒

// ── 诊断：阶段日志（重启时看串口停在哪一步）──────────────────
static void stage(const char* what) {
    Serial.printf("[STAGE] %-22s heap=%u psram=%u\n", what,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
}

static const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "上电";
        case ESP_RST_EXT:      return "外部复位脚";
        case ESP_RST_SW:       return "软件复位(正常)";
        case ESP_RST_PANIC:    return "★程序崩溃(异常)";
        case ESP_RST_INT_WDT:  return "★中断看门狗";
        case ESP_RST_TASK_WDT: return "★任务看门狗";
        case ESP_RST_WDT:      return "★看门狗";
        case ESP_RST_DEEPSLEEP:return "深睡唤醒";
        case ESP_RST_BROWNOUT: return "★电压跌落(供电不足!)";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "未知";
    }
}

// ── 状态提示（屏幕底部一行 + 串口）───────────────────────────
static void netStatus(const String& msg) {
    screenText.showStatus(msg);
    Serial.printf("[UI] %s\n", msg.c_str());
}

// ── OTA / 配网屏（face 任务此时还没启动，整屏独占随便画）──────
// 中文字行：cnCanvas 渲 1bpp → 转 RGB565 → pushPixels 上屏
static uint16_t* sCnFb = nullptr;   // 一行的 RGB565 中转（PSRAM）
static void cnLine(int x, int y, const char* utf8, int px,
                   uint16_t fg = TFT_WHITE, uint16_t bg = TFT_BLACK) {
#if AI_ENABLE_UI
    GfxGuard g;
    if (!sCnFb) sCnFb = (uint16_t*)heap_caps_malloc(320 * 24 * 2, MALLOC_CAP_SPIRAM);
    if (!sCnFb) return;
    cnCanvas::clear();
    cnCanvas::setFont(px);
    cnCanvas::draw(0, 0, utf8);
    const uint8_t* b = cnCanvas::buf();
    int h = px + 8;
    int w = tft.width() - x;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            sCnFb[(size_t)yy * w + xx] = cnCanvas::pixel(b, xx, yy) ? fg : bg;
    tft.pushPixels(x, y, w, h, sCnFb);
#endif
}

// 配网门户界面（AP 起来时 WiFiManager 回调这里）
static void showPortalScreen() {
#if AI_ENABLE_UI
    { GfxGuard g; tft.fillScreen(TFT_BLACK); }
    cnLine(100, 22, "配网模式", 20, TFT_CYAN);
    char s[64];
    snprintf(s, sizeof(s), "1. 手机连接WiFi: %s", cloud.apName().c_str());
    cnLine(16, 70,  s, 16);
    cnLine(16, 94,  "2. 自动弹出页面，或浏览器打开", 16);
    cnLine(44, 118, "192.168.4.1", 16, TFT_YELLOW);
    cnLine(16, 142, "3. 选择家里的WiFi并输入密码", 16);
    cnLine(16, 180, "配好后机器会自动继续启动", 16, TFT_DARKGRAY);
#endif
}

// 连网失败界面（10 秒后自动重试）
static void showNetFailScreen() {
#if AI_ENABLE_UI
    { GfxGuard g; tft.fillScreen(TFT_BLACK); }
    cnLine(88, 70, "WiFi连接失败", 16, TFT_RED);
    cnLine(80, 110, "10秒后自动重试", 16);
#endif
}

// OTA 进度界面：进度条 + 版本号（OtaUpdate 下载期间回调）
static void otaProgress(int pct, const char* ver) {
#if AI_ENABLE_UI
    static bool inited = false;
    static int  lastPct = -1;
    GfxGuard g;
    if (!inited) {
        tft.fillScreen(TFT_BLACK);
        cnLine(96, 36, "固件升级中...", 16, TFT_CYAN);
        char s[64];
        snprintf(s, sizeof(s), "V%s 升级到 V%s", FW_VERSION, ver);
        cnLine(48, 76, s, 12);
        tft.drawRect(40, 116, 240, 20, TFT_WHITE);
        inited = true;
    }
    if (pct == lastPct) return;
    lastPct = pct;
    tft.fillRect(42, 118, 236 * pct / 100, 16, TFT_GREEN);
    if (pct == 0) tft.drawRect(40, 116, 240, 20, TFT_WHITE);
    char s[8];
    snprintf(s, sizeof(s), "%d%%", pct);
    tft.setCursor(148, 146);
    tft.print(s, TFT_WHITE, TFT_BLACK);
#endif
}

// ── 背光唤醒 ─────────────────────────────────────────────────
static void wakeScreen() {
    lastActivity = millis();
    if (!screenOn) {
#if AI_ENABLE_UI
        tft.setBacklight(1);
#endif
        screenOn = true;
        Serial.println("[UI] 背光开");
    }
}

// ── 缺纸闸门：缺纸就挡住打印（说"强制打印"可绕过）──────────
static bool ttsPlay(const String& text);   // 前置声明：下面缺纸闸门也要用它播报
static bool paperGate(const String& said) {
    if (statusUI.paperOk()) return true;
    if (said.indexOf("强制") >= 0 || said.indexOf("强行") >= 0 ||
        said.indexOf("忽略") >= 0 || said.indexOf("不管") >= 0)
        return true;
    screenText.setReply("打印头里没纸了，装好纸再说一声。要是明明有纸，就说\"强制打印\"。");
    netStatus("缺纸，没打印");
    face.setState(FACE_ERROR);
    ttsPlay("打印头里没纸了，装好纸再说一声。");   // ★ 语音回应
    audio.beep(400, 250);
    return false;
}

// ── TTS 流式播放：边下边播，第一声不用等整段下完 ───────────────
//   原来 tts() 要把整段 PCM（几秒话≈300KB）全部下载完才开始响，
//   老陈反馈"串口都打出回复了，声音还要等一会"。改成流式后
//   第一块到达（约 1~2 秒）就出声。
//    ★ 2026-10-04 加了 PSRAM 缓存：命令类回复是固定句子，第一次联网合成后
//    把 PCM 留下来，之后直接本地播 —— 老陈说的"说完要等好久才应"，
//    大部分时间其实耗在"为一句固定的话重新联网合成"上，这一招直接省掉。
struct TtsCacheEntry {
    String   text;
    uint8_t* pcm = nullptr;
    uint32_t len = 0;
};
static TtsCacheEntry ttsCache[TTS_CACHE_N];

// 清掉缓存里最没用的一条（最短的），给新句子腾地方
static void ttsCacheEvict() {
    int slot = 0;
    uint32_t shortest = 0xFFFFFFFF;
    for (int i = 0; i < TTS_CACHE_N; i++) {
        if (!ttsCache[i].pcm) { slot = i; return; }
        if (ttsCache[i].len < shortest) { shortest = ttsCache[i].len; slot = i; }
    }
    free(ttsCache[slot].pcm);
    ttsCache[slot].pcm = nullptr;
    ttsCache[slot].text = "";
    ttsCache[slot].len = 0;
}

static bool ttsPlay(const String& text) {
    if (!text.length()) return false;

#if TTS_CACHE_ON
    // ---- ① 命中缓存：本地直接播，不用联网 ----
    for (int i = 0; i < TTS_CACHE_N; i++) {
        TtsCacheEntry& e = ttsCache[i];
        if (e.pcm && e.text == text) {
            Serial.printf("[TTS] ★缓存命中 %u 字节（免联网，立等可听）\n", (unsigned)e.len);
            if (!audio.playBegin()) return false;
            face.setState(FACE_SPEAK);
            ClipUp::capBegin();                    // ★ 镜像一份→播完上传到控制台
            ClipUp::capFeed(e.pcm, e.len);
            audio.playChunk(e.pcm, e.len);
            audio.playEnd();
            ClipUp::capEndAndUpload(text);
            return true;
        }
    }
#endif

    // ---- ② 联网流式播放，顺便把 PCM 攒下来备用 ----
    if (!audio.playBegin()) return false;
    ClipUp::capBegin();          // ★ 同时镜像一份完整回复音频 → 播完传到控制台

    uint8_t* cap    = nullptr;   // 攒下来的 PCM（PSRAM）
    uint32_t capLen = 0;
    uint32_t capBuf = 0;
#if TTS_CACHE_ON
    bool keep = (text.length() <= TTS_CACHE_MAXTEXT);
#else
    bool keep = false;
#endif

    bool ok = cloud.ttsStream(text,
        [&](const uint8_t* data, uint32_t len, bool first) -> bool {
            if (first) face.setState(FACE_SPEAK);   // 真正出声了才切"说话"表情
            audio.playChunk(data, len);             // ★ 内部会边放边听麦克风（打断检测）
            ClipUp::capFeed(data, len);

            // ★ 你在播报中间开口了 → 立刻让 TTS 收工，不再下载剩下的音频
            if (audio.bargedIn()) return false;

            if (!keep) return true;
            if (capLen + len > capBuf) {
                uint32_t nb = capBuf ? capBuf : 32768;
                while (nb < capLen + len) nb *= 2;
                if (nb > TTS_CACHE_MAXBYTES) { free(cap); cap = nullptr; keep = false; return true; }
                uint8_t* p = (uint8_t*)ps_malloc(nb);
                if (!p) { free(cap); cap = nullptr; keep = false; return true; }
                if (cap) { memcpy(p, cap, capLen); free(cap); }
                cap = p; capBuf = nb;
            }
            memcpy(cap + capLen, data, len);
            capLen += len;
            return true;
        });
    audio.playEnd();

    // ★ 被打断：不哔、也不把"半截音频"存进缓存（此刻 bargedIn 还没被主流程清掉）
    if (audio.bargedIn()) { free(cap); ClipUp::capAbort(); return false; }

    if (!ok) { free(cap); ClipUp::capAbort(); audio.beep(600, 250); return false; }

    ClipUp::capEndAndUpload(text);   // ★ 回复音频上传（后台任务，不阻塞）

#if TTS_CACHE_ON
    // ---- ③ 存进缓存表 ----
    if (keep && cap && capLen) {
        ttsCacheEvict();
        for (int i = 0; i < TTS_CACHE_N; i++) {
            if (!ttsCache[i].pcm) {
                ttsCache[i].text = text;
                ttsCache[i].pcm  = cap;
                ttsCache[i].len  = capLen;
                Serial.printf("[TTS] 已缓存 #%d \"%s\" %u 字节\n",
                              i, text.c_str(), (unsigned)capLen);
                break;
            }
        }
    } else {
        free(cap);
    }
#else
    free(cap);
#endif
    return true;
}

// ── 测试页：MAC 地址二维码 + IP + 固件版本（长按按键 0.6 秒触发）──
static void printTestPage() {
    if (!paperGate("测试页")) return;
    netStatus("打印测试页…");
    face.setState(FACE_THINK);

    const String mac = WiFi.macAddress();           // "AABBCCDDEEFF"
    String macFmt;
    for (unsigned i = 0; i < mac.length(); i += 2) {
        if (i) macFmt += ':';
        macFmt += mac.substring(i, i + 2);
    }
    String info = "MAC: " + macFmt
                + "\nIP: " + WiFi.localIP().toString()
                + "\n固件: 阿蓝 2026-10-04";

    bool ok = textBmp.printTestPage(macFmt, info);
    netStatus(ok ? "测试页打印完成" : "打印失败");
    audio.beep(ok ? 1200 : 400, 200);
    face.setState(FACE_IDLE);
    lastActivity = millis();
}

// ── ASR 同音字纠错 ───────────────────────────────────────────
//   百度 ASR 在远场/噪声下常把命令词听成同音字（"打印"→"大印"）。
//   这里只做"明确的误识别 → 正确词"精确替换，宁可少纠，也绝不把正常的话
//   改坏。以后发现新的误听，往表里加一行即可。
static String fixAsr(String s) {
    static const struct { const char* from; const char* to; } kFix[] = {
        // 打印
        { "大印", "打印" }, { "打应", "打印" }, { "打英", "打印" },
        { "搭印", "打印" }, { "大应", "打印" },
        // 音量 / 静音
        { "阴凉", "音量" }, { "音亮", "音量" }, { "银两", "音量" },
        { "金音", "静音" }, { "近音", "静音" }, { "禁音", "静音" },
        // 电量
        { "电亮", "电量" }, { "垫量", "电量" }, { "电两", "电量" },
        // 机器人名字（★ 唤醒词容错：ASR 对小众名字十有八九听成这些音）
        { "阿兰", "阿蓝" }, { "阿岚", "阿蓝" }, { "阿南", "阿蓝" }, { "阿篮", "阿蓝" },
        { "阿楠", "阿蓝" }, { "阿男", "阿蓝" }, { "啊蓝", "阿蓝" }, { "阿岚岚", "阿蓝" },
        { "阿蓝蓝", "阿蓝" }, { "阿然", "阿蓝" },
        // 画图
        { "话图", "画图" }, { "化图", "画图" }, { "话个", "画个" }, { "化个", "画个" },
        { "话一幅", "画一幅" }, { "话张", "画张" },
    };
    for (const auto& f : kFix) s.replace(f.from, f.to);
    return s;
}

#if WAKE_ENABLE
// ── 唤醒词判定 ★ 2026-10-04 ──────────────────────────────────
//   WAKE_WORDS 是逗号分隔的候选（"阿蓝阿蓝,阿蓝,..."），任一命中即算唤醒。
//   命中时 pos/len 带回那一段的位置，好把它从整句里剪掉。
//   长词优先：在整张表里挑**最长**的匹配 —— 否则"阿蓝"会先把"阿蓝阿蓝"拆坏。
//   ⚠️ 实测结论：这套东西**不好使**（见 ai_config.h WAKE_ENABLE 处说明），默认已关。
static bool wakeHit(const String& s, int& pos, int& len) {
    pos = -1; len = 0;
    const char* p = WAKE_WORDS;
    while (*p) {
        const char* q = strchr(p, ',');
        const int wl = q ? (int)(q - p) : (int)strlen(p);
        if (wl > 0) {
            String w(p, wl);
            const int at = s.indexOf(w);
            if (at >= 0 && wl > len) { pos = at; len = wl; }
        }
        if (!q) break;
        p = q + 1;
    }
    return pos >= 0;
}

// 去掉标点/空格 ★ 这一步是唤醒能不能成的关键：
//   百度 ASR 会**自动加标点**，你说的"阿蓝阿蓝"极常被识别成
//   "阿蓝，阿蓝。" / "阿兰，阿兰。" / "阿蓝 阿蓝" —— 中间一旦插了逗号或空格，
//   按"连续四个字"去匹配就永远匹配不上（第一版就是栽在这里，
//   表现成"喊它没反应"）。所以匹配前统一把标点空格抹掉再比。
static String stripPunct(const String& in) {
    static const char* junk[] = {
        "，", "。", "！", "？", "、", "；", "：", "…", "～", "\t", " ",
        ",", ".", "!", "?", ";", ":", "~"
    };
    String s = in;
    for (const char* j : junk) s.replace(j, "");
    return s;
}
#endif  // WAKE_ENABLE  （wakeHit / stripPunct 只在唤醒闸门里用）

// ── "是不是单纯的打印指令" ★ 2026-10-04 改版16 ────────────────
//   老陈实测："规划一个恐龙打印出来。" 这句里含"打印"，被打印分支截胡，
//   结果只把上一条回复打了一遍 —— 而他要的是「画个恐龙」。
//   所以把"打印"分两种：
//     · 纯打印指令（"打印"/"打出来"/"帮我打印一下"）→ 打上一条回复；
//     · 句子里还夹着别的实质内容（"...一个恐龙打印出来"）→ **不算**，
//       放给 AI 理解（模型多半会走画图工具）。
static bool isPurePrintCmd(const String& s) {
    String t = s;
    // 长词在前，免得"打印出来"被"打印"先吃掉、留下"出来"
    static const char* junk[] = {
        "打印出来", "打印一下", "打印一张", "帮我打印", "打印吧", "打出来", "印出来",
        "帮我打", "打一张", "打一下", "打张", "打印", "打吧", "这张", "那张",
        "一下", "出来", "帮我", "帮忙", "请"
    };
    for (const char* j : junk) t.replace(j, "");
    static const char* punct[] = {
        "，", "。", "！", "？", "、", "；", "：", "…", "～", " ", "\t",
        ",", ".", "!", "?", ";", ":", "~"
    };
    for (const char* p : punct) t.replace(p, "");
    return t.length() <= 1;      // 只剩语气词/空 → 纯打印
}

// ── "画个XX" 判定 ────────────────────────────────────────────
static bool isDrawCommand(const String& s) {
    if (s.startsWith("画")) return true;
    return s.indexOf("画一") >= 0 || s.indexOf("画个") >= 0 || s.indexOf("画张") >= 0 ||
           s.indexOf("画只") >= 0 || s.indexOf("画幅") >= 0 || s.indexOf("画图") >= 0 ||
           s.indexOf("帮我画") >= 0 || s.indexOf("生成图") >= 0 || s.indexOf("来张图") >= 0;
}

// ── 文生图 + 屏上预览（生成后不打；说"打印"才打）★ 老陈定的流程 ──
static void drawAndPreview(const String& prompt) {
    face.closeImage();                 // 先收起上一张，免得新图出来前屏上还挂着旧的
    netStatus("作画中…");
    screenText.setReply("好的，我来画，稍等一下…");
    ttsPlay("好的，我来画，稍等一下。");
    face.setState(FACE_THINK);

    String url = cloud.textToImage(prompt);
    if (url.length()) {
        netStatus("下载图片…");
        uint32_t jpgLen = 0;
        uint8_t* jpg = cloud.downloadImage(url, jpgLen);
        if (jpg) {
            int pw = 0, ph = 0;
            const uint8_t* gray = textBmp.decodePhoto(jpg, jpgLen, pw, ph);
            free(jpg);                       // decodePhoto 已复制出灰度图
            if (gray && face.showImage(gray, pw, ph)) {
                screenText.setReply("画好了！想打出来就说\"打印\"。");
                netStatus("说\"打印\"就打");
                ttsPlay("画好了，就在屏上，想打出来就说打印。");
                audio.beep(1200, 200);
                return;
            }
        }
    }
    netStatus("画图失败");
    face.setState(FACE_ERROR);
    screenText.setReply("没画出来，再说一次试试。");
    ttsPlay("没画出来，再说一次试试。");
    audio.beep(400, 250);
}

// ── 语音命令解析 ─────────────────────────────────────────────
// 返回 true = 已按命令处理，不再走对话
static bool handleCommand(const String& said) {
    auto has = [&](const char* k) { return said.indexOf(k) >= 0; };

    // ---- 画图命令词命中：先让 LLM 结合上下文补全英文提示词，再画 ----
    //   （"画一只"这种话得看上文才知道画什么；AI 自己识别画图意图的走 [DRAW]）
    if (isDrawCommand(said)) {
        netStatus("想画什么…");
        String p = cloud.expandDrawPrompt(said);
        if (!p.length()) p = said + IMG_STYLE_SUFFIX;   // 兜底：LLM 没回就用原话
        drawAndPreview(p);
        return true;
    }

    // ---- 取消：屏上那张图不要了（★ 要排在"打印"前面，"不打印"里含"打印"）----
    if (has("取消") || has("算了") || has("不打了") || has("不要了") ||
        has("不打印") || has("不画了") || has("扔掉") || has("删掉")) {
        int pw = 0, ph = 0;
        if (textBmp.photoReady(pw, ph) || face.imageMode()) {
            textBmp.clearPhoto();
            face.closeImage();                   // 收起图 → 回表情动画
            screenText.setReply("好的，这张不打了。");
            netStatus("已取消");
            ttsPlay("好的，那张不打了。");
            return true;
        }
    }

    // ---- 打印：屏上有待打的图就打图，否则打印上一条回复文字 ----
    if (has("打印") || has("打出来") || has("印出来")) {
        int pw = 0, ph = 0;
        if (textBmp.photoReady(pw, ph)) {        // ★ 有生成的图：打图
            if (!paperGate(said)) return true;
            screenText.setReply("好的，正在打印画。");
            netStatus("打印中…");
            ttsPlay("好的，正在打印。");
            bool ok = textBmp.printPhotoPending();
            textBmp.clearPhoto();                // 打完了，缓存可以丢（要再打只能重画）
            face.closeImage();                   // ★ 打完才回表情界面
            face.setState(FACE_IDLE);
            netStatus(ok ? "打印完成" : "打印失败");
            screenText.setReply(ok ? "画打好了，可以撕了。" : "打印失败了，检查一下纸。");
            ttsPlay(ok ? "打好了，可以撕下来了。" : "打印失败了，检查一下纸。");
            audio.beep(ok ? 1200 : 400, 200);
            return true;
        }
        if (!isPurePrintCmd(said)) {
            // 句子里除了"打印"还有别的实质内容（如"规划一个恐龙打印出来"）→
            //   那其实是"打印某个东西"，**不在这里处理**，交给下面的 AI 理解
            //   （模型会走画图工具，出图预览后再说"打印"即可）
            Serial.printf("[CMD] 含\"打印\"但另有内容，交给 AI 理解: \"%s\"\n", said.c_str());
        } else {
            if (!paperGate(said)) return true;       // 没图：照旧打上一条回复文字
            String doc = lastReply.length() ? lastReply : said;
            screenText.setReply("好的，正在打印。");
            netStatus("打印中…");
            ttsPlay("好的，正在打印。");              // ★ 打印前语音预告
            bool ok = textBmp.printText(doc, 16);
            netStatus(ok ? "打印完成" : "打印失败");
            screenText.setReply(ok ? "打印好了，可以撕了。" : "打印失败了，检查一下纸。");
            audio.beep(ok ? 1200 : 400, 200);
            return true;
        }
    }

    // ---- 音量 ----
    if (has("音量") || has("声音")) {
        String reply;
        if (has("静音") || has("关掉") || has("闭嘴")) {
            audio.setMute(true);  reply = "好的，已经静音了。";
        } else if (has("取消静音") || has("打开声音")) {
            audio.setMute(false); reply = "好的，声音打开了。";
        } else if (has("最大")) {
            audio.setVolume(255); reply = "好的，音量已经调到最大。";
        } else if (has("大") || has("高")) {
            audio.volumeUp();     reply = "好的，音量已加大。";
        } else if (has("小") || has("低")) {
            audio.volumeDown();   reply = "好的，音量已减小。";
        } else if (has("百分之")) {   // "音量百分之60"
            int pct = 0, idx = said.indexOf("百分之") + 9;  // UTF-8 3字节×3字
            while (idx < (int)said.length() && isdigit(said[idx]))
                pct = pct * 10 + (said[idx++] - '0');
            if (pct) { audio.setVolume(pct * 255 / 100);
                       reply = String("好的，音量调到百分之") + pct + "。"; }
            else     { reply = "音量要调到百分之多少？"; }
        } else {
            reply = String("当前音量 ") + (int)audio.getVolume() + "。";
        }
        screenText.setReply(reply);
        netStatus("音量已调整");
        if (!audio.isMuted()) ttsPlay(reply);   // ★ 语音回应（静音时只走屏幕）
        return true;
    }

    // ---- 电量 ----
    if (has("电量") || has("电池")) {
        bool chg; int pct = statusUI.batteryPercent(chg);
        String msg = chg ? String("正在充电，电量百分之") + pct
                         : String("电池电量百分之") + pct;
        screenText.setReply(msg);
        netStatus("播报电量");
        ttsPlay(msg);
        return true;
    }

    // ---- 纸 ----
    if (has("纸")) {
        bool ok = statusUI.paperOk();
        String msg = ok ? "纸装好了，可以打印。" : "打印头里没有纸。";
        screenText.setReply(msg);
        netStatus(ok ? "有纸" : "缺纸");
        ttsPlay(msg);                            // ★ 语音回应
        return true;
    }
    return false;
}

// ── 一句话说完了：识别 → 命令/AI → 播报 ───────────────────────
static void turnTaken() {
    const uint32_t len = audio.recordBytes();
    if (len < (uint32_t)SAMPLE_RATE * 2 * 2 / 3) {     // <0.65 秒当噪声
        face.setState(FACE_ERROR);
        netStatus("没听清，说一句完整的话");
        audio.listenResume();
        return;
    }

    face.setState(FACE_THINK);
    // ★ 2026-10-09：电平太低（噪声误触发）直接判"没听清"，不浪费一次 ASR + LLM。
    if (audio.lastRawPeak() < ASR_MIN_VOICE_PEAK) {
        Serial.printf("[ASR] 跳过（本段峰值 %d < %d，判为噪声）\n",
                      audio.lastRawPeak(), ASR_MIN_VOICE_PEAK);
        face.setState(FACE_ERROR);
        netStatus("没听清，再说一遍");
        audio.beep(400, 180);
        audio.listenResume();
        return;
    }
    netStatus("识别中…");
    const uint32_t tTurn = millis();   // ★ 分段计时（定位"慢在哪一段"，优化交互延迟用）
    // ★ 留一份"原声"：prepareAsr() 会就地高通+归一化，想上传用户真声必须提前拷
    ClipUp::newTurn();
    uint8_t* rawCopy = nullptr;
    if (gCfg.uploadAudio) {
        rawCopy = (uint8_t*)ps_malloc(len);
        if (rawCopy) memcpy(rawCopy, audio.recordData(), len);
    }
    audio.prepareAsr();                // ★ 开头静音裁剪 + 80Hz 高通 + 峰值归一化
    String said = cloud.asr(audio.recordData(), len);
    Serial.printf("[TIM] ① ASR  %ums（录音 %.1fs / %u 字节）\n",
                  (unsigned)(millis() - tTurn), len / (SAMPLE_RATE * 2.0f), (unsigned)len);
    // ★ 上传这段原声 + 识别文字（识别失败也传，控制台能直接听出是没听清还是识别错）
    if (rawCopy)
        ClipUp::enqueueUserOwned(rawCopy, len,
                                 said.length() ? said : String("(ASR 未识别)"),
                                 audio.lastRawPeak());
    if (!said.length()) {
        face.setState(FACE_ERROR);
        netStatus("没听清，再说一遍");
        audio.beep(400, 180);
        audio.listenResume();
        return;
    }
    said = fixAsr(said);               // ★ 同音字纠错（"大印"→"打印"…）

#if WAKE_ENABLE
    // ---- 唤醒词闸门 ★ 2026-10-04 ----
    {
        // 已唤醒 = 保持期内，**或**屏上正挂着生成的图（那时当然是在跟它说话，
        //   不该再逼老陈喊一遍唤醒词 —— 图能在屏上挂很久，喊一次不现实）
        const bool awake = ((int32_t)(wakeUntil - millis()) > 0) || face.imageMode();
        // 去标点后再匹配（治"阿蓝，阿蓝。"这种带逗号的输出）
        const String compact = stripPunct(said);
        int wpos = -1, wlen = 0;
        if (wakeHit(compact, wpos, wlen)) {
            String rest = compact.substring(wpos + wlen);
            rest.trim();
            if (!rest.length()) {
                // 整句就是唤醒词（"阿蓝阿蓝"）→ 应一声，等下一句
                wakeUntil = millis() + WAKE_HOLD_MS;
                screenText.setUser("（叫我呢）");
                netStatus("我在");
                Serial.printf("[WAKE] ★ 已唤醒（纯唤醒词）  原文=\"%s\"\n", said.c_str());
                if (WAKE_REPLY[0]) ttsPlay(WAKE_REPLY); else audio.beep(1500, 90);
                audio.listenResume();
                return;
            }
            // 唤醒词后面还带着指令（"阿蓝阿蓝，打印"）→ 剪掉，一步到位
            said = rest;
            wakeUntil = millis() + WAKE_HOLD_MS;
            Serial.printf("[WAKE] ★ 已唤醒 + 指令: \"%s\"  原文=\"%s\"\n",
                          said.c_str(), compact.c_str());
        } else if (!awake) {
            // 没唤醒、也不在保持期 → 静默忽略（不回话、不调大模型，省流量省时间）
            //   屏上把识别原文摆出来，是老陈自己的判断依据：
            //   看清了就知道是"没喊对唤醒词"，而不是"机器坏了"。
            Serial.printf("[WAKE] 未唤醒，忽略: \"%s\"（识别原文）\n", said.c_str());
            screenText.setUser(said);
            screenText.setReply("要先叫我\"阿蓝阿蓝\"，我才会理你哦。");
            netStatus(WAKE_HINT);
            audio.beep(800, 90);
            audio.listenResume();
            return;
        } else {
            wakeUntil = millis() + WAKE_HOLD_MS;   // 已唤醒：续期，继续说就行
            Serial.printf("[WAKE] 保持唤醒: \"%s\"\n", said.c_str());
        }
    }
#endif

    screenText.setUser(said);          // 屏幕上显示"你：…"
    if (handleCommand(said)) {
        // ★ 命令播报（"好的，正在打印"之类）被打断时同理：接着听你说的下一句
        if (audio.consumeBarge()) {
            Serial.println("[BARGE] 命令播报被打断 → 接着听你这句");
            netStatus("在听你说…");
            face.setState(FACE_THINK);
            return;
        }
        audio.listenResume();
        return;
    }

    // ---- AI 对话 ----
    //   走到这里说明是闲聊 → 屏上若还挂着生成的图，先收起来（回表情动画）
    if (face.imageMode()) face.closeImage();
    netStatus("思考中…");
    const uint32_t tLlm = millis();
    String reply = cloud.chat(said);
    Serial.printf("[TIM] ② LLM  %ums\n", (unsigned)(millis() - tLlm));

    // ★ 模型自己决定要画图（工具调用，免命令词）：提示词模型已写好（英文），
    //   不再拼风格后缀，直接生成
    if (reply.startsWith("[DRAW]")) {
        drawAndPreview(reply.substring(6));
        audio.listenResume();
        return;
    }

    if (!reply.length()) {
        screenText.setReply("（大模型没回话：查一下账户余额和网络）");
        netStatus("AI 出错");
        face.setState(FACE_ERROR);
        audio.beep(400, 250);
        audio.listenResume();
        return;
    }
    lastReply = reply;
    screenText.setReply(reply);        // 屏幕先出字（不用等语音）
    netStatus("说话中…");
    const uint32_t tTts = millis();
    ttsPlay(reply);                    // 边下边播，第一声快得多
    Serial.printf("[TIM] ③ TTS  %ums ｜ 整轮 %ums（真正听到声音看上面 [TTS] 首包）\n",
                  (unsigned)(millis() - tTts), (unsigned)(millis() - tTurn));

    // ★★ 打断播报（barge-in）：如果刚才你在我说话中间插嘴了，这里要**接着听你说**，
    //    绝不能重置监听（重置 = 你刚说的一半白说，又得重来一遍）。
    //    打断时 audio 内部已经把状态切成"录音中"、预滚尾部也接进了录音缓冲，
    //    所以这里直接返回，主循环的 listenPoll() 会把这句话录完。
    if (audio.consumeBarge()) {
        Serial.println("[BARGE] 播报被打断 → 不回待机，接着听你这句");
        netStatus("在听你说…");
        face.setState(FACE_THINK);
        return;
    }

    netStatus("在听，随时说吧");
    face.setState(FACE_IDLE);
    audio.listenResume();
}

#if AI_ENABLE_PRINT && PRT_HW_TEST
// ================================================================
// ★ 打印头 / 电机 裸测（PRT_HW_TEST=1 时上电即进入，永不返回）
//   把"电机走纸"和"打印头加热"彻底拆开：软件打印路径（u8g2 / 位图 / 分块 / 分半）
//   全部不参与，只发最原始的两条指令，各带独立横幅和耗时。
// ================================================================
static void hwSelfTest() {
    uint8_t page[8 * (PRINT_W / 8)];     // 8 行 × 48 字节
    memset(page, 0xFF, sizeof(page));    // 全黑
    printer.setSplitDots(false);         // 裸测不分半，直接整行加热
    printer.setMotorDelay(4000);         // 步进放慢一倍 → 力矩更大、也看得清

    Serial.println();
    Serial.println("=====================================================");
    Serial.println("  ★ 打印头 / 电机 裸测（PRT_HW_TEST=1）");
    Serial.println("  每轮：① 电机扫描：正转/反转 × 快/慢，各 300 步");
    Serial.println("        ② 打印头打 8 行全黑 × 加热 20ms");
    Serial.println("  看三件事：纸动了没（往哪边动）？纸上有没有黑条？打印头烫不烫？");
    Serial.println("  ★ 电机和加热共用 VH —— 两个都不动 = VH 没送电");
    Serial.println("     （下一步量：GPIO48 电平 → P-MOS 栅极 → VH 对地电压）");
    Serial.println("  ★ 再看 ② 后面那行 [TP] VBAT：20ms 全黑若几乎不掉(<150mV) = VH 没通");
    Serial.println("  ★★ 本版【VH 全程保持】：开机起 GPIO48 恒高、VH 一直有电，不再时通时断。");
    Serial.println("     （原来裸测是\"开 3 秒→关→走纸再开→关\"，所以万用表看到 VH 在跳。）");
    Serial.println("     3V3 轨（GPIO47，低=开）也上电常开。两条电都稳了，剩下的就看执行机构：");
    Serial.println("     ① 电机扫描（正转/反转 × 快/慢，各 300 步）：");
    Serial.println("        某一项纸动了 → 是方向/相序/速度的问题，记住是哪一项；");
    Serial.println("        全部只有嗡嗡、纸纹丝不动 → 机械（压纸轮/齿轮/卡纸）或电机堵转。");
    Serial.println("     ② 8 行全黑 × 20ms → 纸上应有 1mm 黑条；没有 = 打印头/FPC/STB 问题。");
    Serial.println("=====================================================");
    delay(1500);

    // ★★ VH 全程保持（老陈 2026-10-04：打印/走纸期间 GPIO48 必须恒高，一次都不许跳）
    //    原来裸测是"开 3 秒 → 关 → 走纸再开 → 关"，万用表就看到 VH 时通时断。
    //    现在开机就把 VH 按住不放：下方所有 feedSteps/printImage 里的 powerOff()
    //    都被这个闩锁挡住，不会真正关 VH。量电压随时可量，电机走纸期间 VH 也不断。
    printer.vhHold(true);

    int round = 0;
    while (true) {
        round++;
        Serial.printf("\n---- 第 %d 轮 ----\n", round);

        // VH 已全程保持，这里只停 1.5 秒让电源轨沉淀，方便万用表读数
        Serial.println("[裸测] ★ VH 全程保持（GPIO48 恒高）—— 量：①GPIO48 ②Q1基极 ③Q2栅极 ④VH");
        delay(1500);

        // ① ★ 电机扫描：正/反 × 快/慢（老陈 2026-10-04：有噪音却不走 → 定方向、定速度）
        //    每一项跑 300 步，中间停 1.2 秒。**盯纸**：哪一项动了、往哪个方向动。
        //    判读：只有某一项能动 → 就是方向/相序/速度的问题；
        //          全程只嗡嗡、纸纹丝不动 → 机械（压纸轮/齿轮/卡纸）或电机堵转。
        {
            struct { const char* name; bool fwd; uint16_t delayUs; } mt[4] = {
                {"正转 @2ms（基线速度）",        true,  2000},
                {"反转 @2ms（看是不是倒着走）",  false, 2000},
                {"正转 @1ms（快）",              true,  1000},
                {"正转 @8ms（很慢、力矩最大）",  true,  8000},
            };
            for (int k = 0; k < 4; k++) {
                Serial.printf("[裸测] ① 电机 %s：300 步 …\n", mt[k].name);
                printer.setMotorDelay(mt[k].delayUs);
                uint32_t t = millis();
                printer.motorRunSteps(300, mt[k].fwd);
                Serial.printf("[裸测]   完成 %ums  ← ★ 纸动了吗？往哪个方向？\n",
                              (unsigned)(millis() - t));
                delay(1200);
            }
            printer.setMotorDelay(PRT_MOTOR_DELAY_US);   // 还原正常参数
        }

        // ② 打印头单独加热（整行全黑 + 20ms，最猛的合法档）
        Serial.println("[裸测] ② 打印头：8 行全黑 × 20ms …");
        printer.setHeatTimeMax(20);
        printer.setHeatTimeMin(20);
        uint32_t t0 = millis();
        logYieldLAT();                                 // ★ UART 日志版：43 让给打印头
        printer.printImage(page, PRINT_W, 8);
        logResumeUART();                               // ★ UART 日志版：43 还给日志口
        Serial.printf("[裸测] ② 打完，耗时 %ums  ← 纸上应有 1mm 宽黑条；没有 = 加热没通电\n",
                      (unsigned)(millis() - t0));
        printer.setHeatTimeMax(PRT_HEAT_MAX_MS);   // 还原正常加热参数
        printer.setHeatTimeMin(PRT_HEAT_MIN_MS);
        delay(800);

        // 再走一段空纸，把刚才这一段推出来方便看
        printer.feedSteps(400);
        Serial.println("[裸测] 这段已推出：黑条=打印头好 / 空=打印头没通电；纸没动=电机问题");
        delay(2500);
    }
}
#endif

// ================================================================
void setup() {
    // RAW：ROM 已把 UART0 配到 GPIO43@115200，先直接打一条（不依赖 Serial 对象）
    ets_printf("\r\n[RAW] app 进入 setup() —— UART0/GPIO43 直出\r\n");
#if SERIAL_VIA_UART
    // UART 日志版：只开 TX(GPIO43)，RX 不接——GPIO44 是外设电源脚，不能让 UART 占走
    Serial.begin(115200, SERIAL_8N1, /*rx*/ -1, /*tx*/ 43);
    // ★ 绕开 core 库的引脚 bug（esp32-hal-uart.c:528：uartEnd 只 detach 引脚、不重置
    //   记录的 _txPin ⇒ begin() 内部第二次 uartBegin 因"引脚没变"跳过重新挂载，
    //   43 停在被 detach 的状态，UART0 的输出送不出去）。这里直接调 IDF 接口强制挂回。
    uart_set_pin(UART_NUM_0, 43, -1, -1, -1);
    ets_printf("[RAW2] begin 后已强制把 UART0 TX 挂回 GPIO43\r\n");
#else
    Serial.begin(115200);
#endif
    delay(200);
    gfxLockInit();                       // 屏幕互斥锁，最先建
    gCfg.load();                         // 本地/云端配置（云端在联网后再拉）
    ClipUp::begin();                     // 录音上传后台任务（控制台比对识别率用）

    esp_reset_reason_t rr = esp_reset_reason();
    Serial.printf("\n[BOOT] 复位原因 = %d (%s)\n", (int)rr, resetReasonName(rr));
#if SERIAL_VIA_UART
    Serial.println("[BOOT] 串口模式 = UART0/GPIO43（UART 日志版）");
#else
    Serial.println("[BOOT] 串口模式 = USB-CDC（默认版）");
#endif
    NetDiag::begin();   // 开机计数 + WiFi事件 + 每秒快照（判"反复重启还是界面问题"）
    // ★ 固件编译时间戳：烧录后先看这行，确认板子上跑的确实是刚编译的这一版
    Serial.printf("[BOOT] 固件编译时间 = %s %s   (裸测开关 PRT_HW_TEST=%d)\n",
                  __DATE__, __TIME__, (int)PRT_HW_TEST);
    Serial.printf("[BOOT] 芯片=%s rev%d 核心=%d Flash=%uMB PSRAM=%uKB\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                  (unsigned)(ESP.getFlashChipSize() / 1048576),
                  (unsigned)(ESP.getPsramSize() / 1024));
    stage("boot");

    // 1. 外设总电源（屏+音频 3V3），必须最先开
    pinMode(PIN_POWER_CTRL, OUTPUT);
    digitalWrite(PIN_POWER_CTRL, HIGH);
    delay(30);
    stage("power_ctrl on");

#if AI_ENABLE_PRINT
    printer.begin();
    logResumeUART();   // ★ UART 日志版：begin() 把 GPIO43 抢去做 LAT 了，这里还给日志口
    // ★ 改版17：打印头功率参数从 ai_config.h 灌进去（电源弱时改那几个宏即可，不用翻库）
    printer.setHeatTimeMax(PRT_HEAT_MAX_MS);
    printer.setHeatTimeMin(PRT_HEAT_MIN_MS);
    printer.setMotorDelay(PRT_MOTOR_DELAY_US);
    printer.setLineCool(PRT_LINE_COOL_MS);
    printer.setSplitDots(PRT_SPLIT_DOTS);      // ★ 大黑块行分两次加热，削峰值电流
    printer.setSplitThreshold(PRT_SPLIT_THRESHOLD);  // ★ 门限（没这行写了也不生效）
    printer.setVhActiveLow(PRT_VH_ACTIVE_LOW); // ★ VH 极性（已定案高有效，保持 0）
    printer.setVbatPin(PIN_BAT_ADC);   // ★ 打印期间量电池电压（软件万用表，验供电用）
    Serial.printf("[TP] 参数 heatMax=%ums heatMin=%ums lineCool=%ums motorDelay=%uus split=%d(>%u点拆) VH极性=%s\n",
                  (unsigned)PRT_HEAT_MAX_MS, (unsigned)PRT_HEAT_MIN_MS,
                  (unsigned)PRT_LINE_COOL_MS, (unsigned)PRT_MOTOR_DELAY_US,
                  (int)PRT_SPLIT_DOTS, (unsigned)PRT_SPLIT_THRESHOLD,
                  PRT_VH_ACTIVE_LOW ? "拉低=开" : "拉高=开");
    Serial.println("[TP] 3V3轨(VDDCTL/GPIO47)=常开不停电；VH(GPIO48)只在打印期间开");
    stage("printer begin");

#if PRT_HW_TEST
    hwSelfTest();      // ★ 裸测：只验电机走纸和打印头加热，不返回（验完把 PRT_HW_TEST 改回 0）
#endif
#endif

#if AI_ENABLE_UI
    if (!tft.begin()) Serial.println("[UI] !! 屏幕初始化失败（PSRAM 不足？）");
    tft.setRotation(SCREEN_ROTATION);   // ← 横屏 320x240
    tft.fillScreen(TFT_BLACK);
#if LCD_EDGE_TEST
    // 边缘自检：3px 贴边白框 —— 判"文字缺顶"到底是屏还是软件（看顶边在不在）
    for (int i = 0; i < 3; i++)
        tft.drawRect(i, i, tft.width() - 2 * i, tft.height() - 2 * i, TFT_WHITE);
    Serial.println("[UI] 屏幕边缘自检 1.5s：贴边 3px 白框，顶边缺 = 屏/排线问题");
    delay(1500);
    tft.fillScreen(TFT_BLACK);
#endif
    statusUI.begin(&tft);
    statusUI.bootAnim();                // 开机动画（此时先别启动表情任务）
    screenText.begin(&tft);             // 对话文字区
    face.begin(&tft);                   // 表情（先画一张待机脸）
    stage("tft + face");
#endif

#if AI_ENABLE_AUDIO
    // I2C 在 GPIO1/2 —— 网表实测，勿用旧测试代码的 41/42
    Wire.begin(PIN_SDA, PIN_SCL);
    stage("wire begin");
    audio.begin();
    stage("audio begin");
    // 开机自检音：这一声"嘀"是本地生成的，不走网络。
    //   听得到 → 喇叭/功放/I2S/ES8311 通路全通，后面 TTS 没声就是云那头的问题；
    //   听不到 → 先查 PIN_PA_EN(GPIO46) 极性和喇叭插头，别去怀疑百度。
    audio.beep(880, 120);
    delay(80);
#endif

#if AI_ENABLE_WIFI
    cloud.setOnStatus(netStatus);
    // 联网（带重试）：没配过网/连不上都会开配网门户，屏上显示操作步骤；
    // 门户 3 分钟没人配就超时返回失败 → 显示失败界面，10 秒后再试。
    bool wifiOk = false;
    int  attempt = 0;
    while (!wifiOk) {
        attempt++;
        Serial.printf("\n[NET] ===== 第 %d 次尝试联网 =====\n", attempt);
        wifiOk = cloud.begin(showPortalScreen);
        if (!wifiOk) {
            Serial.println("[NET] 连网失败，10 秒后重试");
            showNetFailScreen();
            delay(10000);
        }
    }
    // OTA 检查：服务器版本更新 → 自动下载升级并重启（不返回）；
    // 已是最新 / 检查失败 → 直接跳过。
    netStatus("检查更新...");
    OtaUpdate::check(otaProgress);

    // 云端配置：按 MAC 后 6 位拉取网页控制台的配置，成功=应用+缓存；
    // 失败（服务器没配/断网）用本地缓存，不挡开机。
    netStatus("同步配置...");
    gCfg.cloudFetch();
    audio.setVolume(gCfg.volume * 255 / 100);   // 云端音量开机生效
    Serial.printf("[CFG] 设备名=%s 音量=%d 音色=%d 语速=%d 阈值=%d\n",
                  gCfg.deviceName, gCfg.volume, gCfg.ttsPer, gCfg.ttsSpd, gCfg.imgThreshold);
    stage("wifi + ota + cfg");
#endif

#if AI_ENABLE_UI
    face.startTask();                   // 动画跑独立任务（必须在 OTA 屏之后启动，否则会盖掉进度条）
#endif

#if AI_ENABLE_WIFI
    // 开机自检：提前把百度 token 拿到。Key 没填 / Key 错 / 应用类型不对，
    // 都会在这一行打印出来 —— 省得说半天话才知道识别挂了。
    if (cloud.isConnected())
        netStatus(cloud.ensureToken() ? "语音服务就绪" : "语音 Key 有问题");
#if TTS_SPEED_TEST
    // ★ 改版27 网络自检：开机纯下载测速（不播放）—— 判定"下载慢"到底是谁的锅
    if (cloud.isConnected()) {
        netStatus("网络自检…");
        cloud.speedTest();
    }
#endif
    stage("wifi done");
#endif

    // 5. 按键
    pinMode(PIN_BTN, INPUT_PULLUP);
#if AI_ENABLE_UI
    pinMode(PIN_PAPER, INPUT);
#endif
    stage("button ready");

#if AI_ENABLE_UI
    statusUI.drawBar();
    Serial.printf("[PAPER] 开机读数 adc=%d (%s)\n", analogRead(PIN_PAPER),
                  statusUI.paperOk() ? "判为有纸" : "判为缺纸");
    screenText.showStatus("准备就绪");
#endif

    // 6. 自我介绍（联网成功后才有意义，TTS 要用网）
#if AI_ENABLE_WIFI
    if (cloud.isConnected()) {
        lastReply = gCfg.greeting;          // 云端可配的开场白（默认=INTRO_TEXT）
        screenText.setReply(gCfg.greeting);
        netStatus("打个招呼…");
        if (!ttsPlay(lastReply))
            netStatus("自我介绍没播出来（TTS 配额？）");
    }
#endif
    face.setState(FACE_IDLE);
#if WAKE_ENABLE
    screenText.showStatus(WAKE_HINT);       // 例：先说"阿蓝阿蓝"叫我
#elif VAD_AUTO_TRIGGER
    screenText.showStatus("直接说话就行，不用按键");
#else
    screenText.showStatus("按一下按键，然后说一句话");
#endif
    stage("intro done");

#if VAD_ENABLE
    audio.listenBegin();                // ← 开启常时监听（按键/自动触发共用）
    stage("listen begin");
#endif

    lastActivity = millis();
#if WAKE_ENABLE
    Serial.printf("[MAIN] ai_voice ready（唤醒词: %s）\n", WAKE_WORDS);
#elif VAD_AUTO_TRIGGER
    Serial.println("[MAIN] ai_voice ready（免按键，直接说话）");
#else
    Serial.println("[MAIN] ai_voice ready（按一下按键说一句）");
#endif
    // 开场白之后先"睡"一会儿，免得把喇叭余音当成唤醒词
    wakeUntil = 0;
}

// ================================================================
void loop() {
#if VAD_ENABLE
    // ---------- 常时监听：不用按键 ----------
    const int ev = audio.listenPoll();
    if (ev != lastEv) {
        if (ev == 1) {                       // 检测到开口
            wakeScreen();
            face.setState(FACE_LISTEN);
            netStatus("听你说…");
        } else if (ev == 2) {                // 一句话说完了
            wakeScreen();
            turnTaken();
        }
        lastEv = ev;
    }
#else
    // ---------- 旧模式：按住按键说话 ----------
    audio.recordPoll();
    const bool btnHold = (digitalRead(PIN_BTN) == LOW);
    if (btnHold && !btnLast) {
        wakeScreen();
        if (audio.recordStart()) { face.setState(FACE_LISTEN); netStatus("听你说…"); }
    } else if (!btnHold && btnLast) {
        if (cloud.isConnected()) turnTaken();
        else netStatus("没有网络");
    }
#endif

    // ---------- 按键：单击=说一句话；长按 0.6 秒=打印测试页 ----------
    //   按下瞬间不触发，单击在松手时响应（不增加延迟）；
    //   按住满 600ms 就打测试页（不用等松手），同一次按下不重复触发。
    static uint32_t btnDownAt = 0;
    static bool     btnLongFired = false;
    const bool btn = (digitalRead(PIN_BTN) == LOW);
    if (btn && !btnLast) {
        btnDownAt = millis();
        btnLongFired = false;
        wakeScreen();
    }
    if (btn && !btnLongFired && btnDownAt && millis() - btnDownAt >= 600) {
        btnLongFired = true;
        audio.beep(2000, 60);            // 提示：长按已识别
        Serial.println("[BTN] 长按 → 打印测试页");
#if AI_ENABLE_PRINT
        printTestPage();
#endif
    }
    if (!btn && btnLast) {
        if (!btnLongFired && btnDownAt) {
#if VAD_ENABLE
            wakeUntil = millis() + WAKE_HOLD_MS;   // ★ 按键=明确意图 → 直接当作已唤醒
            audio.listenForce();         // 单击：不等阈值，直接开录
            audio.beep(1500, 60);
            face.setState(FACE_LISTEN);
            netStatus("听你说…");
            Serial.println("[BTN] 按键 → 录一句话");
#endif
        }
        btnDownAt = 0;
    }
    btnLast = btn;

    // ---------- 空闲自动关背光 ----------
#if AI_ENABLE_UI
    if (screenOn && millis() - lastActivity > IDLE_SCREEN_OFF_MS) {
        tft.setBacklight(0);
        screenOn = false;
        Serial.println("[UI] 空闲关背光（说话或按按键唤醒）");
    }
#endif

    // ---------- 状态栏刷新（缺纸时刷快点，动态闪烁）----------
    static uint32_t lastBar = 0;
#if AI_ENABLE_UI
    // 缺纸时刷新快一点，好让打印机图标的黄/橙闪烁看起来是"闪"（周期 400ms）
    const uint32_t barMs = statusUI.paperOk() ? 3000 : 200;
#else
    const uint32_t barMs = 3000;
#endif
    if (millis() - lastBar > barMs) {
#if AI_ENABLE_UI
        if (screenOn) statusUI.drawBar();
#endif
        lastBar = millis();
    }

    // ---------- 心跳 ----------
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat > AI_HEARTBEAT_MS) {
        Serial.printf("[BEAT] %lus heap=%u psram=%u wifi=%d rssi=%d\n",
                      millis() / 1000, (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getFreePsram(),
                      (int)WiFi.status(), (int)WiFi.RSSI());
        lastBeat = millis();
    }

    // ---------- 编解码器体检（手动查表太费劲，干脆 30 秒自动报一次）----------
#if AI_ENABLE_AUDIO
    static uint32_t lastDiag = 0;
    if (millis() - lastDiag > 30000) {
        audio.codecDiag();
        lastDiag = millis();
    }
#endif

    delay(2);
}
