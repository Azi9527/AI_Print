#include "cloud_ai.h"
#include "ai_config.h"
#include "device_config.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <Preferences.h>

CloudAI cloud;

// ── URL 百分号编码（UTF-8）─────────────────────────────────────
String CloudAI::urlEncode(const String& s) {
    String out;
    char buf[5];
    for (unsigned int i = 0; i < s.length(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else { snprintf(buf, sizeof(buf), "%%%02X", c); out += buf; }
    }
    return out;
}

// （已删 base64 编码路径：改用原始 PCM 直传，见 asr()）

// 配网 AP 名："LGM_" + MAC 后 4 位（去掉冒号）
String CloudAI::apName() {
    String mac = WiFi.macAddress();          // 形如 AA:BB:CC:DD:EE:FF
    mac.replace(":", "");
    return "LGM_" + mac.substring(mac.length() - 4);
}

bool CloudAI::begin(void (*onPortal)()) {
    if (_onStatus) _onStatus("WiFi...");
    // ★ 配网/重连阶段就降 TX 功率：天线失配时全功率发射会加剧芯片发热和供电波动
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFiManager wm;
    wm.setConnectTimeout(10);
    wm.setConfigPortalTimeout(180);   // 配网门户 3 分钟超时
    if (onPortal)                     // AP 起来时通知外部（屏上显示配网步骤）
        wm.setAPCallback([onPortal](WiFiManager*) { onPortal(); });
    String ap = apName();
    Serial.printf("[NET] 配网 AP = %s\n", ap.c_str());
    _connected = wm.autoConnect(ap.c_str());
    if (_connected) {
        // ★ 电池供电的考虑：降低发射功率（RSSI 很好时够用）。
        WiFi.setTxPower(WIFI_POWER_11dBm);
        // ★★★ 2026-10-04 晚：**关掉 modem sleep** —— 这是"TTS 声音断断续续/
        //   一句话播不完整"的真凶。开了 modem sleep 后射频只在 DTIM 周期醒来，
        //   长文本 TTS 流式下载会被压到 15~20KB/s（低于播放需要的 32KB/s），
        //   播放期间 DMA 反复饿死 ⇒ 音频断续。关掉后吞吐和延迟立刻正常。
        //   代价：待机多耗几十 mA（这块板一直插着充电，可接受）。
        //   想省电：把 WIFI_NO_SLEEP 改 0（代价是 TTS 又可能断续）。
        WiFi.setSleep(WIFI_NO_SLEEP ? false : true);
        Serial.printf("[NET] WiFi OK, RSSI=%d, TX=11dBm, modem-sleep=%s\n",
                      WiFi.RSSI(), WIFI_NO_SLEEP ? "关(吞吐优先)" : "开(省电)");
        if (_onStatus) _onStatus("WiFi OK");
    } else {
        Serial.println("[NET] WiFi FAIL");
        if (_onStatus) _onStatus("WiFi FAIL");
    }
    return _connected;
}

// ── 百度 access_token ──────────────────────────────────────────
// 三条失败路径全部打日志，别再出现"静默返回空"查不出原因
bool CloudAI::ensureToken() {
    if (_token.length()) return true;

    const String key = String(BAIDU_API_KEY);
    const String sec = String(BAIDU_SECRET_KEY);
    if (key.indexOf("在这里填") >= 0 || sec.indexOf("在这里填") >= 0 ||
        key.length() < 8 || sec.length() < 8) {
        Serial.println("[NET] ★★ 百度 Key 还没填！改 src/ai/ai_config.h 的 "
                       "BAIDU_API_KEY / BAIDU_SECRET_KEY");
        return false;
    }

    HTTPClient http;
    String url = "https://openapi.baidu.com/oauth/2.0/token"
                 "?grant_type=client_credentials&client_id=" + key +
                 "&client_secret=" + sec;
    http.begin(url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[NET] token HTTP %d: %s\n", code,
                      http.getString().substring(0, 160).c_str());
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body);
    if (e) {
        Serial.printf("[NET] token JSON 解析失败(%s): %s\n", e.c_str(),
                      body.substring(0, 160).c_str());
        return false;
    }
    if (!doc["error"].isNull()) {          // 百度明确拒绝（Key 错/配额/应用类型不对）
        Serial.printf("[NET] token 被拒: %s — %s\n",
                      (const char*)(doc["error"] | ""),
                      (const char*)(doc["error_description"] | ""));
        return false;
    }
    _token = doc["access_token"] | "";
    if (!_token.length()) {
        Serial.printf("[NET] token 字段为空: %s\n", body.substring(0, 160).c_str());
        return false;
    }
    Serial.printf("[NET] Baidu token OK (%u 字符)\n", (unsigned)_token.length());
    return true;
}

// ── ASR：百度短语音识别 ─────────────────────────────────────
// 用官方 JSON 方式（兼容性最好），但 base64 与整个 JSON body 都在
// PSRAM 里拼装 —— 8 秒录音 256KB → base64 342KB，内部 RAM 只有 320KB，
// 放内部必崩，放 PSRAM 则毫无压力（板上有 8MB）。
//
// ★ 2026-10-09：极速版优先 + 权限失败自动降级（详见 ai_config.h ASR_SPEEDY）。
static int8_t sProState = 0;   // 0=还没试过  1=可用  -1=不可用（试过被拒）

// 单次 POST + 解析；denied=true 仅当"这个模型没权限"（触发降级），网络抖动不降级。
static String asrPostOnce(const uint8_t* body, size_t blen, int pid,
                          const char* ep, bool& denied) {
    denied = false;
    HTTPClient http;
    http.begin(ep);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(20000);
    const int code = http.POST((uint8_t*)body, blen);
    if (code != HTTP_CODE_OK) {
        Serial.printf("[ASR] HTTP %d (%s pid=%d)\n", code, ep, pid);
        if (code == 401 || code == 403) denied = true;   // 鉴权/权限
        http.end();
        return "";
    }
    String resp = http.getString();
    http.end();
    if (!resp.length()) { Serial.println("[ASR] 响应为空"); return ""; }

    JsonDocument r;
    DeserializationError e = deserializeJson(r, resp);
    if (e) {
        Serial.printf("[ASR] JSON 解析失败(%s): %s\n", e.c_str(),
                      resp.substring(0, 160).c_str());
        return "";
    }
    const int err = r["err_no"] | -1;
    if (err != 0) {
        Serial.printf("[ASR] err %d %s (pid=%d)\n", err,
                      (const char*)(r["err_msg"] | ""), pid);
        // 6=无权限访问数据 / 3302=鉴权失败(含能力未开通) / 3304=… → 判"没开通"
        if (err == 6 || err == 3302 || err == 3304) denied = true;
        return "";
    }
    return r["result"][0] | "";
}

String CloudAI::asr(const uint8_t* pcm, uint32_t len) {
    if (!ensureToken()) { Serial.println("[ASR] 跳过（没有 token）"); return ""; }
    if (!len) return "";

    // 1) base64（PSRAM）
    size_t b64Cap = ((len + 2) / 3) * 4 + 8;
    uint8_t* b64 = (uint8_t*)ps_malloc(b64Cap);
    if (!b64) { Serial.println("[ASR] PSRAM 不足(base64)"); return ""; }
    size_t b64Len = 0;
    if (mbedtls_base64_encode(b64, b64Cap, &b64Len, pcm, len) != 0) {
        free(b64); Serial.println("[ASR] base64 编码失败"); return "";
    }
    b64[b64Len] = 0;

    // 2) JSON body 缓冲（两种 pid 共用，只换 dev_pid）
    size_t bodyCap = b64Len + 400;
    char* body = (char*)ps_malloc(bodyCap);
    if (!body) { free(b64); Serial.println("[ASR] PSRAM 不足(body)"); return ""; }
    auto buildBody = [&](int pid) -> int {
        return snprintf(body, bodyCap,
            "{\"format\":\"pcm\",\"rate\":%d,\"channel\":1,\"cuid\":\"esp32s3-printer\","
            "\"token\":\"%s\",\"dev_pid\":%d,\"len\":%u,\"speech\":\"%s\"}",
            SAMPLE_RATE, _token.c_str(), pid, (unsigned)len, (char*)b64);
    };

    String text;

#if ASR_SPEEDY
    // 3a) 优先极速版（80001）
    if (sProState >= 0) {
        const int n = buildBody(80001);
        if (n > 0 && (size_t)n < bodyCap) {
            bool denied = false;
            text = asrPostOnce((const uint8_t*)body, (size_t)n, 80001,
                               "https://vop.baidu.com/pro_api", denied);
            if (text.length()) {
                sProState = 1;
                free(b64); free(body);
                Serial.printf("[ASR速] \"%s\"\n", text.c_str());
                return text;
            }
            if (!denied) {   // 极速版本身通了但这次没结果（没听清/网络）→ 直接返回
                free(b64); free(body);
                return "";
            }
            sProState = -1;  // 没开通 → 记住，后面直接用标准版
            Serial.println("[ASR] ★ 极速版无权限（控制台未开通）→ 自动降级到标准版");
        }
    }
#endif

    // 3b) 标准版
    const int n = buildBody(1537);
    if (n <= 0 || (size_t)n >= bodyCap) {
        free(b64); free(body); Serial.println("[ASR] body 组装失败"); return "";
    }
    {
        bool denied = false;
        text = asrPostOnce((const uint8_t*)body, (size_t)n, 1537,
                           "https://vop.baidu.com/server_api", denied);
    }
    free(b64); free(body);
    if (text.length()) Serial.printf("[ASR] \"%s\"\n", text.c_str());
    return text;
}

// ── LLM（OpenAI 兼容 /chat/completions）────────────────────────
void CloudAI::resetChat() { _histN = 0; }

// ── 扩展能力·天气（2026-10-12）：Open-Meteo 免费接口，不要 key ──
//   返回喂给 LLM 的精简 JSON；失败返回空串。code=WMO 天气码，模型自己会解释。
String CloudAI::queryWeatherRaw(const String& city) {
    // ① 城市名 → 经纬度（走 http：ESP32 连 Open-Meteo 的 https 会失败，http 快且稳）
    String gUrl = "http://geocoding-api.open-meteo.com/v1/search?name="
                + urlEncode(city) + "&count=1&language=zh&format=json";
    HTTPClient hg;
    hg.begin(gUrl);
    hg.setTimeout(8000);
    int gc = hg.GET();
    String gResp = hg.getString();
    hg.end();
    if (gc != HTTP_CODE_OK) {
        Serial.printf("[WX] 地理编码 http %d\n", gc);
        return "";
    }
    JsonDocument g;
    if (deserializeJson(g, gResp)) { Serial.println("[WX] 地理编码 JSON 解析失败"); return ""; }
    JsonObject loc = g["results"][0];
    if (loc.isNull()) { Serial.printf("[WX] 没找到城市: %s\n", city.c_str()); return ""; }
    double lat = loc["latitude"] | 0.0, lon = loc["longitude"] | 0.0;
    String lname = loc["name"] | city;

    // ② 经纬度 → 当前天气
    String fUrl = String("http://api.open-meteo.com/v1/forecast?latitude=")
                + String(lat, 3) + "&longitude=" + String(lon, 3)
                + "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto";
    HTTPClient hf;
    hf.begin(fUrl);
    hf.setTimeout(8000);
    int fc = hf.GET();
    String fResp = hf.getString();
    hf.end();
    if (fc != HTTP_CODE_OK) {
        Serial.printf("[WX] 天气 http %d\n", fc);
        return "";
    }
    JsonDocument f;
    if (deserializeJson(f, fResp)) { Serial.println("[WX] 天气 JSON 解析失败"); return ""; }
    JsonObject cur = f["current"];
    if (cur.isNull()) { Serial.println("[WX] 响应无 current 字段"); return ""; }

    String out = String("{\"city\":\"") + lname + "\",\"temp_c\":"
        + String((float)(cur["temperature_2m"] | 0.0), 1)
        + ",\"rh\":" + String((int)(cur["relative_humidity_2m"] | 0))
        + ",\"wind_kmh\":" + String((float)(cur["wind_speed_10m"] | 0.0), 1)
        + ",\"code\":" + String((int)(cur["weather_code"] | 0))
        + ",\"codeMean\":\"0晴 1-3多云 45雾 51-67雨 71-77雪 80-82阵雨 95-99雷暴\"}";
    Serial.printf("[WX] %s: %.90s\n", lname.c_str(), out.c_str());
    return out;
}

String CloudAI::chat(const String& userText) {
    JsonDocument doc;
    doc["model"] = LLM_MODEL;
    doc["stream"] = false;
    // ★ 2026-10-04：300 → LLM_MAX_TOKENS(140)。回复越长，**等第一声的时间越久**
    //   （要等整段生成完才开始念）。配合提示词压到 40 字，首声能提前 2 秒左右。
    doc["max_tokens"] = LLM_MAX_TOKENS;
    // ★ 工具调用（function calling，2026-10-04）：把"画图"注册成工具，
    //   模型自己判断这句话是聊天还是想画图 —— 不再依赖命令词。
    //   模型还会顺带把口语翻译成详细的英文绘图提示词，出图质量更好。
    doc["tools"] = serialized(
        "[{\"type\":\"function\",\"function\":{\"name\":\"draw_image\","
        "\"description\":\"生成一张黑白线稿图片（生成后在屏幕上显示，可再语音打印）。"
        "当用户想让你画画、画个什么、生成图片时调用本工具；普通聊天不要调用。\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"prompt\":{\"type\":\"string\",\"description\":\"绘图提示词：把用户的意思扩写成"
        "具体的英文描述，含主体、动作、构图，并加上 simple black and white line art, "
        "coloring book style, thick clean lines, white background\"}},"
        "\"required\":[\"prompt\"]}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"query_weather\","
        "\"description\":\"查询某个城市当前天气（温度/湿度/风/天气现象）。"
        "当用户问天气、气温、要不要带伞等时调用。用户没说城市就不传 city。\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"city\":{\"type\":\"string\",\"description\":\"城市名，中文，如 北京\"}},"
        "\"required\":[]}}}]");
    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"] = "system"; sys["content"] = cfgSystemPrompt();
    {   // 只带最近 N 轮历史（云端配置 historyTurns，0=不带）
        int from = _histN - gCfg.historyTurns * 2;
        if (from < 0) from = 0;
        if (from & 1) from--;
        for (int i = from; i < _histN; i += 2) {
            JsonObject u = msgs.add<JsonObject>();
            u["role"] = "user";      u["content"] = _history[i];
            if (i + 1 < _histN) {
                JsonObject a = msgs.add<JsonObject>();
                a["role"] = "assistant"; a["content"] = _history[i + 1];
            }
        }
    }
    JsonObject un = msgs.add<JsonObject>();
    un["role"] = "user"; un["content"] = userText;

    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.begin(String(LLM_BASE_URL) + "/chat/completions");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + LLM_API_KEY);
    http.setTimeout(30000);
    int code = http.POST(body);
    String resp = http.getString();
    http.end();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[LLM] http %d: %s\n", code, resp.c_str());
        return "";
    }
    JsonDocument r;
    if (deserializeJson(r, resp)) return "";

    // ★ 模型决定画图（工具调用）：取出它写好的英文提示词，加 [DRAW] 前缀
    //   交回调用方（ai_print.cpp 检测前缀走画图流程）
    JsonObject tc = r["choices"][0]["message"]["tool_calls"][0];
    const char* callName = tc["function"]["name"] | "";
    if (strcmp(callName, "draw_image") == 0) {
        const char* args = tc["function"]["arguments"] | "";
        JsonDocument a;
        String p;
        if (!deserializeJson(a, args)) p = a["prompt"] | "";
        Serial.printf("[LLM] → 画图工具: %.80s\n", p.c_str());
        return "[DRAW]" + p;
    }

    // ★ 扩展能力·天气（2026-10-12）：模型调用 query_weather → 固件查 Open-Meteo
    //   → 把天气数据作为 tool 结果发第二轮 → 模型生成口播回复
    if (strcmp(callName, "query_weather") == 0) {
        const char* args = tc["function"]["arguments"] | "";
        const char* callId = tc["id"] | "";
        JsonDocument a;
        String city;
        if (!deserializeJson(a, args)) city = a["city"] | "";
        if (!city.length()) city = gCfg.weatherCity;
        String w = queryWeatherRaw(city);
        Serial.printf("[LLM] → 天气工具: %s → %s\n", city.c_str(),
                      w.length() ? "OK" : "查询失败");
        if (!w.length()) return "抱歉，天气服务这会儿连不上，等会儿再问我吧。";

        // 第二轮：system + 用户原话 + assistant(tool_calls) + tool 结果
        JsonDocument d2;
        d2["model"] = LLM_MODEL;
        d2["stream"] = false;
        d2["max_tokens"] = cfgReplyMaxTokens();
        JsonArray m2 = d2["messages"].to<JsonArray>();
        JsonObject s2 = m2.add<JsonObject>();
        s2["role"] = "system";
        s2["content"] = cfgSystemPrompt();
        JsonObject u2 = m2.add<JsonObject>();
        u2["role"] = "user"; u2["content"] = userText;
        JsonObject as2 = m2.add<JsonObject>();
        as2["role"] = "assistant";
        JsonArray tcs = as2["tool_calls"].to<JsonArray>();
        JsonObject t0 = tcs.add<JsonObject>();
        t0["id"] = callId; t0["type"] = "function";
        JsonObject f0 = t0["function"];
        f0["name"] = "query_weather"; f0["arguments"] = args;
        JsonObject tl = m2.add<JsonObject>();
        tl["role"] = "tool"; tl["tool_call_id"] = callId; tl["content"] = w;

        String body2;
        serializeJson(d2, body2);
        HTTPClient h2;
        h2.begin(String(LLM_BASE_URL) + "/chat/completions");
        h2.addHeader("Content-Type", "application/json");
        h2.addHeader("Authorization", String("Bearer ") + LLM_API_KEY);
        h2.setTimeout(25000);
        const int c2 = h2.POST(body2);
        String r2s = h2.getString();
        h2.end();
        if (c2 != HTTP_CODE_OK) {
            Serial.printf("[LLM] 天气第二轮 http %d\n", c2);
            return "天气查到了，但我整理的时候出了点岔子，再问一次吧。";
        }
        JsonDocument rd;
        if (deserializeJson(rd, r2s)) return "";
        String reply = rd["choices"][0]["message"]["content"] | "";
        if (!reply.length()) return "";
        if (_histN >= 4) { _history[0] = _history[2]; _history[1] = _history[3]; _histN = 2; }
        _history[_histN++] = userText;
        _history[_histN++] = reply;
        Serial.printf("[LLM] (天气) \"%s\"\n", reply.c_str());
        return reply;
    }

    String reply = r["choices"][0]["message"]["content"] | "";
    if (!reply.length()) return "";

    // 存上下文（最多 2 轮历史 + 本轮）
    if (_histN >= 4) {
        _history[0] = _history[2]; _history[1] = _history[3];
        _histN = 2;
    }
    _history[_histN++] = userText;
    _history[_histN++] = reply;
    Serial.printf("[LLM] \"%s\"\n", reply.c_str());
    return reply;
}

// ★ 2026-10-09：把"想画什么"扩写成英文绘图提示词。
//   治两个毛病：① 文生图对英文提示词更敏感；② "画一只"脱离上下文不知道画什么。
String CloudAI::expandDrawPrompt(const String& said) {
    JsonDocument doc;
    doc["model"] = LLM_MODEL;
    doc["stream"] = false;
    doc["max_tokens"] = 160;
    doc["temperature"] = 0.7;

    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] = String(
        "你是绘图提示词转换器。结合对话上下文，把用户想画的东西写成一句具体的英文绘图提示词："
        "说清主体、动作、构图；风格固定为 " IMG_STYLE_EN
        "。只输出这句英文提示词本身，不要引号、不要解释、不要换行。");
    for (int i = 0; i < _histN; i += 2) {
        JsonObject u = msgs.add<JsonObject>();
        u["role"] = "user";      u["content"] = _history[i];
        if (i + 1 < _histN) {
            JsonObject a = msgs.add<JsonObject>();
            a["role"] = "assistant"; a["content"] = _history[i + 1];
        }
    }
    JsonObject un = msgs.add<JsonObject>();
    un["role"] = "user"; un["content"] = said;

    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.begin(String(LLM_BASE_URL) + "/chat/completions");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + LLM_API_KEY);
    http.setTimeout(20000);
    const int code = http.POST(body);
    String resp = http.getString();
    http.end();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[IMG] 提示词扩写 http %d\n", code);
        return "";
    }
    JsonDocument r;
    if (deserializeJson(r, resp)) return "";
    String p = r["choices"][0]["message"]["content"] | "";
    p.replace("\r", " "); p.replace("\n", " ");
    p.trim();
    if (p.startsWith("\"")) p.remove(0, 1);          // 去掉模型偶尔加的引号
    if (p.endsWith("\""))   p.remove(p.length() - 1);
    p.trim();
    Serial.printf("[IMG] 提示词(LLM): %.140s\n", p.c_str());
    return p;
}

// ================================================================
// 点阵画生成（"阿蓝，画一只猫"）
//   让大模型直接吐 cols×rows 的 '#'/'.' 字符画，再放大成热敏位图。
//   不用接文生图 API，省事、免费、可控。
// ================================================================
static int countArtLines(const String& s) {
    int n = 0;
    for (unsigned i = 0; i < s.length(); i++) if (s[i] == '\n') n++;
    return n + (s.length() ? 1 : 0);
}

static String sanitizeArt(const String& raw, int cols) {
    String out;
    int start = 0;
    const int n = raw.length();
    for (int i = 0; i <= n; i++) {
        if (i != n && raw[i] != '\n') continue;
        String ln = raw.substring(start, i);
        start = i + 1;
        ln.trim();
        if (ln.startsWith("```")) continue;
        String keep;
        for (int k = 0; k < (int)ln.length() && (int)keep.length() < cols; k++) {
            const char c = ln[k];
            if (c == '#' || c == '*' || c == 'X' || c == 'x' || c == '@' || c == 'O')
                keep += '#';
            else if (c == '.' || c == ' ' || c == '_' || c == '-')
                keep += '.';
        }
        if (keep.length() * 2 < (size_t)cols) continue;   // 太短的行不是图
        out += keep;
        out += '\n';
    }
    return out;
}

String CloudAI::chatArt(const String& userText, int cols, int rows) {
    String sys = String("你是点阵图生成器。只输出一张 ") + cols + " 列 x " + rows +
                 " 行的点阵画：用 '#' 表示黑色、'.' 表示白色，每行正好 " + cols +
                 " 个字符。不要输出任何解释、标题、序号或 ``` 代码块标记。"
                 "图形要居中、能看出是什么东西，别只是随机噪点。";

    JsonDocument doc;
    doc["model"] = LLM_MODEL;
    doc["stream"] = false;
    doc["max_tokens"] = 1500;
    doc["temperature"] = 0.7;
    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject s = msgs.add<JsonObject>();
    s["role"] = "system"; s["content"] = sys;
    JsonObject u = msgs.add<JsonObject>();
    u["role"] = "user"; u["content"] = userText;

    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.begin(String(LLM_BASE_URL) + "/chat/completions");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + LLM_API_KEY);
    http.setTimeout(40000);
    int code = http.POST(body);
    String resp = http.getString();
    http.end();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[ART] http %d: %s\n", code, resp.substring(0, 120).c_str());
        return "";
    }
    JsonDocument r;
    if (deserializeJson(r, resp)) {
        Serial.printf("[ART] JSON 解析失败，原始 %u 字节: %s\n",
                      (unsigned)resp.length(), resp.substring(0, 120).c_str());
        return "";
    }
    String reply = r["choices"][0]["message"]["content"] | "";
    // ★ 诊断：sanitizeArt 过滤很严（每行至少 cols/2 个有效字符才算图），
    //   模型如果夹了解释文字或者用了别的字符，整段会被滤空。
    //   把原始回复打出来，"画图失败"就知道是过滤问题还是没回话。
    Serial.printf("[ART] 模型原始回复 %u 字节: %.120s\n",
                  (unsigned)reply.length(), reply.c_str());
    String art = sanitizeArt(reply, cols);
    Serial.printf("[ART] 过滤后 %u 行、%u 字符\n",
                  (unsigned)(art.length() ? countArtLines(art) : 0),
                  (unsigned)art.length());
    return art;
}

// ── TTS（aue=5 = PCM-16k，免 MP3 解码）─────────────────────────
// ⚠️ 百度 TTS 的 JSON 里带 base64 音频，几十秒语音能到 1MB+，
//    绝不能整个读进内部 RAM。这里流式读到 PSRAM，再手工取 "speech" 字段。
static uint8_t* readAllToPsram(HTTPClient& http, size_t& outLen, size_t maxBytes) {
    outLen = 0;
    int contentLen = http.getSize();
    size_t cap = (contentLen > 0) ? (size_t)contentLen + 1 : maxBytes;
    if (cap > maxBytes) cap = maxBytes;
    uint8_t* buf = (uint8_t*)ps_malloc(cap + 1);
    if (!buf) { Serial.println("[HTTP] PSRAM 分配失败"); return nullptr; }

    WiFiClient* stream = http.getStreamPtr();
    uint32_t t0 = millis();
    while ((size_t)outLen < cap) {
        size_t avail = stream->available();
        if (avail) {
            int n = stream->read(buf + outLen, avail);
            if (n > 0) { outLen += n; t0 = millis(); }
        } else {
            if (!http.connected() && !avail) break;
            if (millis() - t0 > 5000) break;     // 5s 无数据视为结束
            delay(1);
        }
    }
    buf[outLen] = 0;
    return buf;
}

// ================================================================
// 文生图（百度千帆 ModelBuilder）★ 2026-10-04 新增
//   POST https://qianfan.baidubce.com/v2/images/generations
//   头 Authorization: Bearer <千帆 API Key>
//   体 {"model":"qwen-image","prompt":"...","size":"512x512"}
//   → {"id":"..","data":[{"url":"http://...jpeg?authorization=..."}]}
//   ⚠️ 实测：size=1024 要 45s、512 只要 20s，所以必须带 size（见 ai_config.h）。
//   ⚠️ Key 是千帆的（bce-v3/ALTAK-...），跟语音的 AK/SK 完全两套。
// ================================================================
String CloudAI::textToImage(const String& prompt) {
    if (strlen(QIANFAN_IMAGE_KEY) < 20) {
        Serial.println("[IMG] ★ 千帆 API Key 还没填（ai_config.h 的 QIANFAN_IMAGE_KEY）");
        return "";
    }
    JsonDocument doc;
    doc["model"] = IMG_MODEL;
    // ★ 2026-10-09：提示词长度保护（flux.1-schnell 上限 512 字符）
    String p = prompt;
    if (p.length() > IMG_PROMPT_MAX) p = p.substring(0, IMG_PROMPT_MAX);
    doc["prompt"] = p;
    doc["size"]   = IMG_SIZE;
    // steps 参数仅 flux 认（qwen-image 会拒），换回 flux 时在下面恢复 doc["steps"]=IMG_STEPS
    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.begin("https://qianfan.baidubce.com/v2/images/generations");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + QIANFAN_IMAGE_KEY);
    http.setTimeout(IMG_TIMEOUT_MS);      // ★ 出图慢，HTTPClient 默认 5s 会直接超时
    const uint32_t t0 = millis();
    const int code = http.POST(body);
    String resp = http.getString();
    http.end();
    const uint32_t secs = (millis() - t0) / 1000;

    if (code != HTTP_CODE_OK) {
        Serial.printf("[IMG] 出图 http %d (%us): %.200s\n", code, (unsigned)secs, resp.c_str());
        return "";
    }
    JsonDocument r;
    if (deserializeJson(r, resp)) {
        Serial.printf("[IMG] 返回不是 JSON: %.120s\n", resp.c_str());
        return "";
    }
    String url = r["data"][0]["url"] | "";
    if (!url.length()) {
        Serial.printf("[IMG] 没拿到 url: %.200s\n", resp.c_str());
        return "";
    }
    Serial.printf("[IMG] 出图成功 %us: %.60s...\n", (unsigned)secs, url.c_str());
    return url;
}

// 下载图片到 PSRAM（图片直链是 http 明文，不花 TLS 时间）
uint8_t* CloudAI::downloadImage(const String& url, uint32_t& outLen) {
    outLen = 0;
    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[IMG] 下载 http %d\n", code);
        http.end();
        return nullptr;
    }
    size_t n = 0;
    uint8_t* buf = readAllToPsram(http, n, IMG_MAX_BYTES);
    http.end();
    if (!buf) return nullptr;
    outLen = (uint32_t)n;
    Serial.printf("[IMG] 图片下载 %u 字节\n", (unsigned)n);
    return buf;
}

uint8_t* CloudAI::tts(const String& text, uint32_t& outLen) {
    outLen = 0;
    if (!ensureToken()) return nullptr;

    // ── 走 GET 方式 ────────────────────────────────────────────────
    // ★ 2026-10-03 修正，这就是"喇叭一直没声音"的元凶。
    //   百度短文本在线合成有两条路：
    //     ① GET + tex=原文(urlencode)      → 直接吐**二进制音频流**
    //     ② POST json，或 tex 传 base64 文本 → 才吐 JSON(内含 base64 speech)
    //   我们走的是 ①，却按 ② 去 strstr 找 "err_no" —— 二进制流里当然
    //   没有，于是 err 恒为 -1、整段音频被丢掉，playPcm 一次也没被调到。
    //   现在改成：首字节是 '{' 才当错误 JSON 解析，否则整段就是 PCM。
    //   aue=4 = pcm-16k，正好配 I2S 的 16kHz（aue=5 其实是 pcm-8k，
    //   按 16k 播会变成快放怪声）。
    //   vol：0~15，默认 5。原来写 9 —— 合成出来就是满幅，再经 DAC/功放必削顶，
    //        听起来"又吵又硬又失真"。回到 5。
    //   per：音色。4=度丫丫(童声，偏尖硬)；0=度小美(女声，最自然)。
    //        想换回来把 per 改 4，想男声试 1(度小宇)/3(度逍遥)。
    String url = "https://tsn.baidu.com/text2audio?tex=" + urlEncode(text)
               + "&tok=" + _token
               + "&cuid=esp32s3-printer&ctp=1&lan=zh"
               + "&spd=" + gCfg.ttsSpd + "&pit=" + gCfg.ttsPit + "&vol=5&per=" + gCfg.ttsPer
               + "&aue=4";
    HTTPClient http;
    http.begin(url);
    http.setTimeout(20000);
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[TTS] http %d\n", code);
        http.end();
        return nullptr;
    }

    size_t bodyLen = 0;
    uint8_t* body = readAllToPsram(http, bodyLen, 8 * 1024 * 1024);  // 上限 8MB
    http.end();
    if (!body) return nullptr;
    if (bodyLen < 64) {
        Serial.printf("[TTS] 响应过短 %u 字节\n", (unsigned)bodyLen);
        free(body);
        return nullptr;
    }

    // 失败时百度回的是 JSON 文本（错误码 + 原因）
    if (body[0] == '{') {
        char* p = strstr((char*)body, "\"err_no\":");
        char* m = strstr((char*)body, "\"err_msg\":\"");
        Serial.printf("[TTS] err %d %s\n", p ? atoi(p + 9) : -1, m ? m + 11 : "");
        free(body);
        return nullptr;
    }

    // 少数情况会带 WAV 头（"RIFF"），就地前移 44 字节把它剥掉
    uint32_t len = (uint32_t)bodyLen;
    if (len > 44 && memcmp(body, "RIFF", 4) == 0) {
        len -= 44;
        memmove(body, body + 44, len);
    }

    outLen = len;
    Serial.printf("[TTS] %u bytes PCM (%.1fs)\n", outLen, outLen / (SAMPLE_RATE * 2.0f));
    return body;      // ⚠️ 返回的就是 body 本体：调用方 free() 它即可
}

// ================================================================
// 流式 TTS：边下载边把 PCM 交给 sink → 播放延迟从"整段下完"降到
// 第一块到达（约 1~2 秒）。12 秒的自我介绍原来要白等 4~5 秒。
// ================================================================
// ================================================================
// ★ 网络自检（改版27）：纯下载测速（不播放、不经 sink）
//   同一句话分别用 aue=4(PCM) / aue=3(MP3)，各用 2KB / 16KB 读缓冲把 socket 抽干丢弃，
//   打印 首字节 / 总用时 / KB/s。目的：把"TTS 下载慢"的责任分清楚 ——
//     ① 两档缓冲都远快于播放需要的 32KB/s ⇒ WiFi/TLS 没问题，慢在"边下边播"的
//        串行结构（socket 被 i2s_write 阻塞拖住、TCP 窗口关闭）；
//     ② 都 ≈15KB/s ⇒ ESP32 收包就这速度，必须降码率（aue=3 MP3 ≈8KB/s）。
//   PC 端实测同一接口 PCM=445KB/s、MP3=110KB/s ⇒ 服务器/链路没问题。
// ================================================================
void CloudAI::speedTest() {
#if TTS_SPEED_TEST
    if (!ensureToken()) { Serial.println("[SPD] 没拿到 token，跳过测速"); return; }
    const char* text = "老陈你好，我是阿蓝。现在做一次网络下载速度自检，一二三四五六七八九十。";
    Serial.println("[SPD] ===== 纯下载测速（不播放，丢弃数据）=====");
    static uint8_t sink0[16384];
    const size_t bufs[2] = { 2048, 16384 };
    const int    aues[2] = { 4, 3 };              // 4=PCM-16k(现用)  3=MP3-16k/64kbps
    for (int ai = 0; ai < 2; ai++) {
        for (int bi = 0; bi < 2; bi++) {
            const size_t BS = bufs[bi];
            const String url = String("https://tsn.baidu.com/text2audio?tex=") + urlEncode(text)
                + "&tok=" + _token
                + "&cuid=esp32s3-speedtest&ctp=1&lan=zh&spd=5&pit=5&vol=5&per=0&aue=" + String(aues[ai]);
            HTTPClient http;
            const uint32_t t0 = millis();
            if (!http.begin(url)) { Serial.printf("[SPD] aue=%d 连接失败\n", aues[ai]); continue; }
            http.setTimeout(8000);
            const int code = http.GET();
            if (code != HTTP_CODE_OK) {
                Serial.printf("[SPD] aue=%d http %d\n", aues[ai], code);
                http.end(); continue;
            }
            const uint32_t tHead = millis() - t0;
            const int clen = http.getSize();          // Content-Length（-1 = 未知）
            WiFiClient* st = http.getStreamPtr();
            uint32_t total = 0, tFirst = 0, tLast = 0, nextMark = 65536;
            uint32_t lastData = millis();
            while (true) {
                const size_t avail = st->available();
                if (avail) {
                    const int n = st->read(sink0, min(avail, BS));
                    if (n > 0) {
                        total += n;
                        const uint32_t now = millis() - t0;
                        if (!tFirst) tFirst = now;
                        tLast = now; lastData = millis();
                        if (total >= nextMark) {
                            Serial.printf("[SPD]   %u KB @ %u ms（首字节后 %u ms）\n",
                                          (unsigned)(total / 1024), (unsigned)now,
                                          (unsigned)(now - tFirst));
                            nextMark += 65536;
                        }
                    }
                } else {
                    // ★★ 关键修正（改版27b）：**收到 Content-Length 就收工**。
                    //   上一版没这一条，数据下完之后 HTTP keep-alive 连接还开着，
                    //   循环一直傻等到 15s 超时 ⇒ 打印出来的 KB/s 被超时摊薄成假数
                    //   （实测两只都"卡"在某个字节数上一动不动，就是这个原因）。
                    if (clen > 0 && total >= (uint32_t)clen) break;
                    if (millis() - lastData > 1200) break;   // 1.2s 没新数据 = 结束
                    if (!http.connected()) break;
                    if (millis() - t0 > 15000) break;
                    delay(1);
                }
            }
            const uint32_t win = (tLast > tFirst) ? (tLast - tFirst) : 0;   // 首→末字节的下载窗口
            Serial.printf("[SPD] ★ aue=%d 缓冲=%uB  Content-Length=%d  实收 %u B  "
                          "首字节 %u ms  末字节 %u ms  下载窗 %u ms  ⇒ %.1f KB/s（有效速率）\n",
                          aues[ai], (unsigned)BS, clen, (unsigned)total,
                          (unsigned)tFirst, (unsigned)tLast, (unsigned)win,
                          win ? (total / 1024.0f) / (win / 1000.0f) : 0.0f);
            http.end();
            delay(200);
        }
    }
    Serial.println("[SPD] ===== 测速结束（判读见 ai_config.h TTS_SPEED_TEST 注释）=====");
#endif
}

bool CloudAI::ttsStream(const String& text, TtsChunkFn sink) {
    if (!ensureToken()) return false;
    if (!sink) return false;

    // ★ TTS_USE_HTTP（见 ai_config.h）：0=https（安全，但要付 TLS 握手的钱）；
    //   1=http（省掉整个 TLS 握手，首包快 0.3~0.5s，代价是 tex/token 明文）。
    const String url = String(TTS_USE_HTTP ? "http://" : "https://")
                     + "tsn.baidu.com/text2audio?tex=" + urlEncode(text)
                     + "&tok=" + _token
                     + "&cuid=esp32s3-printer&ctp=1&lan=zh"
                     + "&spd=" + gCfg.ttsSpd + "&pit=" + gCfg.ttsPit + "&vol=5&per=" + gCfg.ttsPer
                     + "&aue=4";
    // ★ 连接建立用时单独计时 —— 老陈说的"文字出来后要等一会儿才有声音"就是这一段：
    //   HTTPS 每次都要重做完整 TLS 握手（ESP32 上主要是 ECDHE 计算，0.3~0.6s），
    //   加上百度合成首包。先把它量出来（[TTS] 连接 xxxms 首包 xxxms），再决定怎么砍。
    HTTPClient http;
    const uint32_t tConn = millis();
    if (!http.begin(url)) { Serial.println("[TTS] begin fail"); return false; }
    http.setTimeout(20000);
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[TTS] http %d\n", code);
        http.end();
        return false;
    }
    const uint32_t msConn = millis() - tConn;   // 连接（含 TLS 握手）+ 首字节
    WiFiClient* stream = http.getStreamPtr();
    const int contentLen = http.getSize();      // ★ Content-Length（-1 = 未知，百度一般都有）

    // 2KB 小缓冲即可：sink 内部的 i2s_write 会阻塞限流，
    // 内存放不下大段语音，也根本不需要放。
    static uint8_t buf[2048];
    int have = 0;             // buf 里现有的字节数
    int skip = 0;             // RIFF 头还没剥完的字节数
    uint32_t total = 0;
    bool firstChunk = true;
    bool ok = false;
    uint32_t t0 = millis();
    uint32_t tFirst = 0;          // ★ 第一块 PCM 到达的耗时（相对发起连接）——优化前后对比用
    // ★ 改版27b：真实"下载窗口"探针。原来只用"用时"衡量，而 HTTP keep-alive 下
    //   数据下完之后连接还开着，循环要等到 6s 空闲超时 ⇒ 用时被拖长、KB/s 被摊薄，
    //   把"网络慢"看假了。现在用 首字节→末字节 的窗口算有效速率。
    uint32_t tFirstByte = 0, tLastByte = 0;
    bool aborted = false;         // ★ 被调用方叫停（用户打断播报）

    while (true) {
        const size_t avail = stream->available();
        if (avail) {
            const int cap = (int)sizeof(buf) - have;
            if (cap <= 0) { /* 理论上不会：每次循环都先消费 */ }
            else {
                const int n = stream->read(buf + have, min(avail, (size_t)cap));
                if (n > 0) {
                    have += n; t0 = millis();
                    if (!tFirstByte) tFirstByte = millis();
                    tLastByte = millis();
                }
            }
        } else {
            // ★ 收到 Content-Length 就收工（别傻等连接关闭）；否则 6s 无数据视为结束
            if (contentLen > 0 && (int)total >= contentLen) { ok = (total > 800); break; }
            if (!http.connected()) { ok = (total > 800); break; }
            if (millis() - t0 > 6000) { ok = (total > 800); break; }
            delay(1);
            continue;
        }

        // 首块：错误 JSON 检测 / RIFF 头剥离
        if (firstChunk && have >= 1) {
            if (buf[0] == '{') {                 // 百度报错回的是 JSON 文本
                String err;
                while (http.connected() || stream->available()) {
                    if (stream->available()) {
                        int n = stream->read(buf + have, sizeof(buf) - have);
                        if (n > 0) have += n;
                    } else if (millis() - t0 > 3000) break;
                    else delay(1);
                }
                buf[min(have, (int)sizeof(buf) - 1)] = 0;
                char* p = strstr((char*)buf, "\"err_no\":");
                char* m = strstr((char*)buf, "\"err_msg\":\"");
                Serial.printf("[TTS] err %d %s\n", p ? atoi(p + 9) : -1,
                              m ? m + 11 : "");
                http.end();
                return false;
            }
            if (have >= 4 && memcmp(buf, "RIFF", 4) == 0) skip = 44;  // 剥 WAV 头
            firstChunk = false;
        }
        if (skip > 0) {
            const int s = min(skip, have);
            memmove(buf, buf + s, have - s);
            have -= s;
            skip -= s;
            continue;
        }

        // 只输出偶数字节（16bit 样本成对），剩下 1 字节留到下一块拼
        const int use = have & ~1;
        if (use > 0) {
            if (total == 0) tFirst = millis() - tConn;   // ★ 首块到达（= 用户听到声音的延迟）
            if (!sink(buf, use, total == 0)) { aborted = true; break; }   // ★ 被打断 → 停止下载
            total += use;
            buf[0] = buf[use];                   // 把尾巴那 1 字节挪到开头
            have -= use;
        }
    }

    http.end();
    if (aborted)  Serial.printf("[TTS] ★ 被你打断了（已播 %u bytes）\n", (unsigned)total);
    else if (ok) {
        // ★ 下载速率探针：播放 16k/16bit 需要 >32KB/s，低于它就会**断续**（"音频不完整"）。
        //   ★ 改版27b：改用"首字节→末字节"的下载窗口算有效速率（不含 keep-alive 尾巴）。
        const float secs = (millis() - tConn) / 1000.0f;
        const uint32_t win = (tLastByte > tFirstByte) ? (tLastByte - tFirstByte) : 0;
        const float kbs = win ? (total / 1024.0f) / (win / 1000.0f) : 0.0f;
        Serial.printf("[TTS] 连接 %ums 首包 %ums | 共 %u bytes (音频 %.1fs) 用时 %.1fs"
                      " ｜ 下载窗 %.1fs = %.1fKB/s %s\n",
                      (unsigned)msConn, (unsigned)tFirst, total,
                      total / (SAMPLE_RATE * 2.0f), secs, win / 1000.0f, kbs,
                      (kbs < 32.0f) ? "← 比播放慢，会断续！" : "");
    }
    else          Serial.printf("[TTS] 流式数据过短 (%u bytes)\n", (unsigned)total);
    return ok && !aborted;      // 被打断 → 返回 false（调用方据此知道"没播完"）
}
