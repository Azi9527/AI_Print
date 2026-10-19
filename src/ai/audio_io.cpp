#include "audio_io.h"
#include "ai_config.h"
#include <driver/i2s.h>
#include <Wire.h>
#include <Preferences.h>
#include <ES8311.h>
#include <ES7210.h>

AudioIO audio;

static ES8311 es8311;                 // 0x18 播放
static ES7210 es7210;                 // 0x41 录音

// 直接读写一颗 I2C 芯片的寄存器。
//   读：只用于开机自检打印。
//   写：★ 用于绕过 lib/ES7210 里写错的字长寄存器（见下面 setFormat 的说明）。
// ★ 2026-10-03 第六版：读法改成和 lib/ES7210 的 readReg 一致，并且重试三次。
//   上一版用的是"重复起始(endTransmission(false)) + delay(1)"，在这块板子上会
//   间歇性 NACK：老陈的日志里 0x60 读到 0xFF、0x40 读到 0x42，看着像模拟供电
//   没起来，其实芯片好好的 —— 同一份日志里 [VAD] rawL=2161 就是铁证。
//   自检数据不可信比没有自检更糟（会把人带到沟里），所以改成
//   "STOP + delay(2) + 最多重试 3 次"，三次都读不到才认 0xFF。
static uint8_t i2cPeek(uint8_t addr, uint8_t reg) {
    uint8_t v = 0xFF;
    for (int k = 0; k < 3; k++) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        if (Wire.endTransmission(true) != 0) { delay(2); continue; }
        delay(2);
        if (Wire.requestFrom((uint8_t)addr, (uint8_t)1) == 1 && Wire.available()) {
            v = (uint8_t)Wire.read();
            if (v != 0xFF) return v;      // 拿到有效值就收工
        }
        delay(2);
    }
    return v;
}

// 写寄存器：ES7210 写后需要几毫秒稳定时间（库的 writeReg 里也是 delay(3)）
static bool i2cPoke(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    const bool ok = (Wire.endTransmission() == 0);
    delay(3);
    return ok;
}

// ES7210 MICx_GAIN 寄存器值 → 增益 dB（数据手册 REG 0x43~0x46）
//   bit[4] = SELMICx（1 = 选中该路 MIC 作输入），bit[3:0] = 增益档
//   0~11 每档 3dB；12=34.5dB 13=36dB 14=37.5dB
static int micGainDb(uint8_t reg) {
    switch (reg & 0x0F) {
        case 12: return 34;   // 34.5dB，日志取整
        case 13: return 36;
        case 14: return 37;   // 37.5dB
        default: return (reg & 0x0F) * 3;
    }
}

// 交织 → 只取第 0 路（左）。
//   · 标准 I2S(I2S_CH_NUM=2)：SDOUT1 左=ADC1(MIC1)、右=ADC2(MIC2)，取左 = 麦克风。
//   · TDM(I2S_CH_NUM=4)：SDOUT1 一帧 = MIC1,MIC2,MIC3,MIC4，取第 0 路 = MIC1 麦克风。
static inline int takeLeft(const int16_t* src, int cnt, int16_t* dst) {
    int o = 0;
    for (int i = 0; i + I2S_CH_NUM <= cnt; i += I2S_CH_NUM) dst[o++] = src[i];
    return o;
}

// 前置声明：去直流 RMS。定义在下面（VAD 段），但 playChunk() 的
// 打断检测（barge-in）更早用到它。
static int frameRms(const int16_t* s, int n);

// 双二阶（Direct Form II Transposed）—— 用于工频陷波。
struct Biquad {
    float b0, b1, b2, a1, a2, z1, z2;
    void init(float f0, float Q, float fs) {
        const float w0 = 6.2831853f * f0 / fs;
        const float al = sinf(w0) / (2.0f * Q);
        const float a0 = 1.0f + al;
        b0 = 1.0f / a0;
        b1 = -2.0f * cosf(w0) / a0;
        b2 = b0;
        a1 = b1;
        a2 = (1.0f - al) / a0;
        z1 = 0.0f; z2 = 0.0f;
    }
    inline float run(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ------------------------------------------------------------------
// I2S：单端口全双工。录音/播放都是 16k/16bit，时钟共用。
// 注意：录音与播放不同时进行（按住说话是半双工），最省事也最稳。
// ------------------------------------------------------------------
static bool i2sInit() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    cfg.sample_rate = SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    // ★ 2026-10-03 第四版：改成明确的"左+右"双声道，然后在软件里抽左声道。
    //   原因：ES7210 的 SDOUT1 上就是标准的 I2S 双声道（左=ADC1、右=ADC2），
    //   用 FMT_ONLY_LEFT 去"让驱动帮我们挑单声道"依赖驱动的 mono 行为，
    //   不同 ESP-IDF 版本实现不一致（有的会复制、有的会跳采样）；
    //   自己抽最踏实，而且哪一路有信号一眼可查。
#if I2S_TDM_MODE
    // ★★★ TDM 四声道（2026-10-12）★★★
    //   ES7210 的 SDOUT1 一帧输出 ADC1~ADC4（每 WS 周期 4×16bit=64bit），
    //   这样才能读到老陈板上接在 MIC3 的"ES8311 输出回采线"。
    //   ⚠ 与 TX(ES8311 播放) 共用同一份 config ⇒ 播放也走 64bit 帧：
    //     TX 数据按 4 路交织写，ch0/ch1 = 左右声道、ch2/ch3 填 0，ES8311 只取前两路。
    //     若实测播放异常，把 ai_config.h 的 I2S_TDM_MODE 改回 0 即可。
    cfg.channel_format = I2S_CHANNEL_FMT_MULTIPLE;
    cfg.total_chan  = I2S_CH_NUM;
    cfg.chan_mask   = (i2s_channel_t)(I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1 |
                                      I2S_TDM_ACTIVE_CH2 | I2S_TDM_ACTIVE_CH3);
    cfg.left_align    = false;   // 与 ES7210 "TDM I²S" 对齐
    cfg.big_edin      = false;
    cfg.bit_order_msb = true;    // MSB 先出（与 I2S 标准一致）
    cfg.skip_msk      = false;   // 四路数据全进 DMA，由固件自己抽
#else
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
#endif
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    // ★ 改版26：DMA 缓冲 8×512 → 8×256（每声道 256 帧）。
    //   原因①：TX 积压上限 256ms→128ms —— "参考→回声"延迟 D 的主要成分就是
    //     TX DMA 积压，越小 AEC 的搜索范围/收敛难度越小（MAXD 2400 才罩得住）。
    //   原因②：被打断后 DMA 里没播完的残留少一半，闭嘴更干脆。
    //   代价：中断频率翻倍（每 16ms 一次，可忽略）。
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = 0;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = PIN_I2S_MCK;    // MCLK 给 ES8311/ES7210
    pins.bck_io_num = PIN_I2S_BCK;
    pins.ws_io_num = PIN_I2S_WS;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num = PIN_I2S_DIN;

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;
    if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
    i2s_zero_dma_buffer(I2S_NUM_0);
    return true;
}

bool AudioIO::begin() {
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW);     // 先静音

    if (!i2sInit()) { Serial.println("[AUDIO] I2S init fail"); return false; }

    // 编解码器初始化（库来自 lib/ES8311、lib/ES7210，实测工程验证过）
    //
    // ★★ 2026-10-03 重要修正 —— 麦克风"听不见"的根因就在这里 ★★
    //   ES7210 库的 init() 已经做完一切：MIC1/MIC3 打开、PGA=16dB、
    //   MICBIAS=0x70(2.87V)、ADC1 使能、三段上电、复位启动。
    //   我们原来在 init() 之后又调了五个函数，里面有三个是"帮倒忙"：
    //     ① setMICBias(true) —— 这个参数的语义是**电压档位**（0x70=2.87V），
    //        不是开关！传 true(=1) 会把 0x70 覆盖成 0x01，MICBIAS 直接
    //        掉到几乎没电压 → 麦克风输出恒定 → VAD 日志里 peak 永远=1。
    //     ② setMICGain(0, 30) —— 库的 mic 编号是 1..4，传 0 落到 switch 的
    //        default 分支直接 return，等于没设。
    //     ③ enableAllADCs(true) —— 把 ADC2~ADC4 也打开，多出来的空通道
    //        混进 I2S 数据流。
    //   结论：除了必要的增益，别再动 ES7210 的寄存器。
    if (es8311.begin(PIN_SDA, PIN_SCL)) {
        es8311.init();                // 内含 16bit/I2S/DAC/HP/去静音
    } else Serial.println("[AUDIO] ES8311 not found @0x18");

    if (es7210.begin(PIN_SDA, PIN_SCL)) {
        // ⚠️ 注意：别信 init() 里那句 "MIC1_GAIN=0x10 // 16dB" 的注释 ——
        //    0x10 的低 4 位是 0，真实增益 **0dB**，见下面第五版的说明。
        //    init() 管的是 MICBIAS 2.87V / ADC1 使能 / 三段上电时序，这部分是对的。
        es7210.init();

        // ★★★ 2026-10-03 第四版：终于挖到真凶 —— 库里写错了寄存器值 ★★★
        //
        //   ES7210 的字长在 SDP_CFG1(0x11) 的 bit[7:5]（数据手册 REGISTER 0x11）：
        //       000 = 24bit   001 = 20bit   010 = 18bit   011 = **16bit**   100 = 32bit
        //   Espressif 官方 esp-bsp/components/es7210 里也是这么写的：
        //       case ES7210_I2S_BITS_16B: reg_val = 0x60;  break;
        //       case ES7210_I2S_BITS_18B: reg_val = 0x40;  break;   ← 注意这行
        //       case ES7210_I2S_BITS_24B: reg_val = 0x00;  break;
        //       case ES7210_I2S_BITS_32B: reg_val = 0x80;  break;
        //
        //   而 lib/ES7210 的 setFormat(0, 16) 里写的是：
        //       if (bits == 16) val |= 0x40;      ← **0x40 是 18bit，不是 16bit！**
        //   所以上一版虽然把 setFormat 加回去了，ES7210 实际输出的是 18bit，
        //   和 ESP32 的 16bit 槽位依旧对不上 → 日志照旧 [VAD] raw=5、
        //   ASR 只能瞎猜"我不知道"。库这里就是错的，别再用它，直接写寄存器。
        //
        //   0x60 = 0b0110_0000 ：bit[7:5]=011 → 16bit；bit[1:0]=00 → I2S 协议。
        i2cPoke(0x41, 0x11, 0x60);   // SDP_CFG1：I2S 协议 + 16bit 字长
#if I2S_TDM_MODE
        i2cPoke(0x41, 0x12, 0x02);   // SDP_CFG2=10：TDM I²S —— ADC1~ADC4 全复用 SDOUT1
#else
        i2cPoke(0x41, 0x12, 0x00);   // SDP_CFG2：非 TDM，ADC12→SDOUT1、ADC34→SDOUT2
#endif

        // ★★★ 2026-10-03 第五版：真凶 —— 麦克风 PGA 实际是 0dB ★★★
        //
        //   ES7210 数据手册 REGISTER 0x43 (MIC1 GAIN) 原文：
        //       bit[7:5] 保留
        //       bit[4]   SELMIC1  —— 1 = 选中 MIC1P/MIC1N 作为输入
        //       bit[3:0] MIC1GAIN —— 0=0dB 1=3dB 2=6dB ... 11=33dB 13=36dB 14=37.5dB
        //
        //   而 lib/ES7210 的 init() 里是这样写的：
        //       writeReg(ES7210_MIC1_GAIN, 0x10);   // 16dB     ← 注释看错位了！
        //   0x10 拆开 = bit4 置 1（选中 MIC1）+ bit[3:0]=0 → **增益 0dB**，
        //   根本不是 16dB。Espressif 官方驱动写的是 mic_gain | 0x10，
        //   那个 0x10 就是"选中 MIC"位，低 4 位才是增益档。
        //
        //   后果：麦克风只有几十 mV 输出，不给前置放大就直接进 ADC ——
        //   信号"干净但小 40dB"，日志表现为 [VAD] peak=1 raw=5，
        //   只有极大声音（喇叭贴着喊）才能勉强顶到 raw=51。
        //   注意它并不是"没信号"：ASR 偶尔能听对（"你好。"）就是证据。
        //
        //   修法：直接补上增益。0x10 | 13 = 0x1D = 选中 MIC1 + 36dB。
        //   ★ 2026-10-04：MIC2 也一并打开（0x44）—— SDOUT1 上"左=ADC1(MIC1)、
        //     右=ADC2(MIC2)"，开 MIC2 就能从右声道看出板子到底有没有第二颗麦：
        //       开机 [VAD] listening 那行 rawR 有明显数字 → 有第二颗麦（后续可做双麦）
        //       rawR 依旧 ≈2                        → 只有一颗麦，保持单麦
        //     对左声道（识别用的那路）没有任何影响。
        i2cPoke(0x41, 0x43, 0x10 | MIC_PGA_CODE);   // MIC1：选中 + 增益（帧内第 0 路 = 语音）
#if I2S_TDM_MODE
        // ★ TDM：MIC3 打开当**硬件回采参考**（老陈板上回采线接的就是 MIC3）。
        //   回采是 ES8311 的线路电平，比麦克风高得多 ⇒ 用很低增益（从 0dB 起，防削顶）。
        i2cPoke(0x41, 0x44, 0x00);                     // MIC2 关（本机不用）
        i2cPoke(0x41, 0x45, 0x10 | AEC_REF_MIC2_GAIN); // MIC3：选中 + 回采增益
        i2cPoke(0x41, 0x46, 0x00);                     // MIC4 关
        Serial.printf("[AUDIO] TDM 4 声道：MIC1=语音(%ddB) MIC3=回采参考(%d档) MIC2/4关\n",
                      micGainDb(0x10 | MIC_PGA_CODE), AEC_REF_MIC2_GAIN);
#elif MIC_UNUSED_OFF
        // ★ 2026-10-12：不用的通道全关（它们只是干扰源，本机不读）
        i2cPoke(0x41, 0x44, 0x00);                  // MIC2 PGA 关（右声道探针停用）
        i2cPoke(0x41, 0x45, 0x00);                  // MIC3 PGA 关（这路接着 ES8311 输出）
        i2cPoke(0x41, 0x46, 0x00);                  // MIC4 PGA 关
        Serial.println("[AUDIO] 只开 MIC1，MIC2/3/4 的 PGA 已关闭（录音杂音排查）");
#elif AEC_ENABLE && (AEC_REF_SRC == 1)
        // ★★ 改版30 硬件回采：把 **MIC2** 变成"参考输入"（老陈把 ES8311 输出的回采线
        //   从 MIC3 挪到 MIC2）。寄存器依据（ES7210 手册）：
        //     · SDP_CFG2(0x12) bit[1:0]=00 ⇒ ADC1→SDOUT1 左、**ADC2→SDOUT1 右**（能读到）
        //     · ADC2 使能 = REG 0x61 bit1（库的 init 把它关着）
        //     · MIC2 增益 = REG 0x44（bit4=SELMIC2 + bit[3:0]=增益档）
        //   ⚠ ES8311 输出是线路电平，用麦克风那档 36dB 必削顶 ⇒ 用独立的小档位。
        i2cPoke(0x41, 0x61, 0x02);                       // 打开 ADC2（右声道）
        i2cPoke(0x41, 0x44, 0x10 | AEC_REF_MIC2_GAIN);   // MIC2：选中 + 回采增益
        i2cPoke(0x41, 0x45, 0x00);                       // MIC3 关（回采线已挪走，悬空是噪声源）
        i2cPoke(0x41, 0x46, 0x00);                       // MIC4 关
        Serial.printf("[AEC] 参考源 = MIC2 硬件回采（右声道，MIC2 增益档 %d）\n",
                      AEC_REF_MIC2_GAIN);
#else
        // ★ 改版32：软件参考模式下也**打开 ADC2 + MIC2**（36dB），把它当"探针"用 ——
        //   播放时右声道的电平会跟音频一起涨不涨，直接回答"老陈那根回采线到底接在
        //   哪里"。这对左声道（识别用的那路）没有任何影响。
        i2cPoke(0x41, 0x61, 0x02);                  // 打开 ADC2（右声道）
        i2cPoke(0x41, 0x44, 0x10 | MIC_PGA_CODE);   // MIC2：选中 + 增益（右声道探针）
        i2cPoke(0x41, 0x45, 0x10 | MIC_PGA_CODE);   // MIC3：同上（走 SDOUT2，本机不收）
        Serial.println("[AEC] 参考源 = TX PCM（软件参考，靠延迟搜索对齐）"
                       "；同时开 MIC2 右声道做**回采线探针**");
#endif
        delay(5);
    } else Serial.println("[AUDIO] ES7210 not found @0x41");

    // 回读关键寄存器 —— 这就是"麦克风为什么没声"的体检报告。
    //
    // ⚠️ 判读要点（2026-10-03 踩过）：0x40(模拟供电) 和 0x60(ADC 使能) 在这块
    //    板子上**回读不可靠**（会读到 0x42 / 0xFF），拿它们当判据只会误报
    //    "模拟供电没起来"，把人带偏。所以下面只对**可信且有意义**的三个报警：
    //        0x11 字长（要 0x60） / 0x41 MICBIAS（要 0x70） / 0x43 MIC1 增益（要 0x1x）
    //    最终判据永远是 [VAD] listening 那行的 rawL —— 有几百就说明通路是通的。
    {
        const uint8_t id1  = i2cPeek(0x41, 0x3D);   // CHIP_ID1
        const uint8_t id0  = i2cPeek(0x41, 0x3E);   // CHIP_ID0
        const uint8_t sdp1 = i2cPeek(0x41, 0x11);   // 字长/协议（要 0x60）
        const uint8_t sdp2 = i2cPeek(0x41, 0x12);   // SDOUT/通道映射（非TDM 要 0x00）
        const uint8_t ana  = i2cPeek(0x41, 0x40);   // 模拟供电（回读不稳，仅参考）
        const uint8_t bias = i2cPeek(0x41, 0x41);   // MICBIAS12 电压档（要 0x70 = 2.87V）
        const uint8_t pga1 = i2cPeek(0x41, 0x43);   // MIC1：SELMIC1 + 增益
        const uint8_t pga3 = i2cPeek(0x41, 0x45);   // MIC3
        const uint8_t adc1 = i2cPeek(0x41, 0x60);   // ADC1 使能（回读不稳，仅参考）
        const uint8_t pdn1 = i2cPeek(0x41, 0x4B);   // MIC12 上电（要 0x0F）
        Serial.printf("[AUDIO] ES7210 id=%02X%02X 0x11=%02X(要60) 0x12=%02X 0x41=%02X(要70) "
                      "0x4B=%02X(要0F) | 参考(回读不稳) 0x40=%02X 0x60=%02X\n",
                      id1, id0, sdp1, sdp2, bias, pdn1, ana, adc1);
        // ★ 增益单独一行 —— 这是"麦克风有没有被放大"的唯一凭证
        Serial.printf("[AUDIO] MIC增益 0x43=%02X SELMIC1=%d %.1fdB | 0x45=%02X %.1fdB "
                      "（目标 %d dB）\n",
                      pga1, (pga1 >> 4) & 1, micGainDb(pga1) / 1.0,
                      pga3, micGainDb(pga3) / 1.0, micGainDb(0x10 | MIC_PGA_CODE));

        if (0xFF == sdp1 && 0xFF == bias)
            Serial.println("[AUDIO] ★ ES7210 寄存器全是 FF —— I2C 无应答：查地址/供电/SDA-SCL");
        else if ((sdp1 & 0xE0) != 0x60)
            Serial.println("[AUDIO] ★ ES7210 字长不是 16bit（bit7:5 应为 011）—— 数据必然错位");
        else if ((bias & 0x70) == 0)
            Serial.println("[AUDIO] ★ ES7210 MICBIAS 没打开（0x41 应为 0x70）—— 麦克风不会有信号");
        else if (((pga1 >> 4) & 1) == 0)
            Serial.println("[AUDIO] ★ MIC1 没被选中（0x43 的 bit4 SELMIC1 应为 1）");
        else if ((pga1 & 0x0F) == 0)
            Serial.println("[AUDIO] ★ MIC1 增益是 0dB —— 麦克风等于没放大，声音会小 30~40dB");
        else
            Serial.println("[AUDIO] ✓ ES7210 关键寄存器正常（最终看 [VAD] listening 的 rawL）");

        // ES8311（播放侧）：0x09/0x0A 应为 0x0C（I2S + 16bit），0x32 是数字音量
        Serial.printf("[AUDIO] ES8311 0x09=%02X 0x0A=%02X(都要0C) 0x32=%02X\n",
                      i2cPeek(0x18, 0x09), i2cPeek(0x18, 0x0A), i2cPeek(0x18, 0x32));
    }

    _codecReady = true;

    Preferences pref;
    pref.begin("aiprint", true);
    // 键名跟着映射规则走：vol → vol2 → vol3。
    // 每次改了 0~255 → 寄存器 的换算就要换键名，否则板子里存着的旧值
    // （上一版按 +24dB 记的）会原样套到新公式上，等于没改。别偷懒。
    _volume = pref.getUChar("vol3", REC_VOL_DEFAULT);
    pref.end();

    applyVolume();
    Serial.printf("[AUDIO] ready, volume=%u (0~255)\n", _volume);
    return true;
}

// 精简版寄存器体检 —— 开机 + 每 30 秒各一次。
// 目的：老陈贴日志时总是从中间截，开机的 [AUDIO] ES7210 那几行从来没露过面。
// 现在把最关键的数字跟着心跳一起打，怎么截都能看见。
void AudioIO::codecDiag() {
    if (!_codecReady) return;
    const uint8_t sdp1 = i2cPeek(0x41, 0x11);   // 字长
    const uint8_t bias = i2cPeek(0x41, 0x41);   // MICBIAS
    const uint8_t pga1 = i2cPeek(0x41, 0x43);   // MIC1：SELMIC1 + 增益
    // 只报这三个 —— 0x40/0x60 的回读不稳（会读到 0x42/0xFF），
    // 放进周期日志只会误导；真正的判据是 [VAD] listening 那行的 rawL。
    const bool ok = ((sdp1 & 0xE0) == 0x60) && ((bias & 0x70) != 0) &&
                    (((pga1 >> 4) & 1) == 1) && ((pga1 & 0x0F) != 0);
    Serial.printf("[DIAG] ES7210 字长=%02X(要60) MICBIAS=%02X(要70) MIC1=%02X(%d档/%ddB SEL=%d) "
                  "vol=%u %s\n",
                  sdp1, bias, pga1, pga1 & 0x0F, micGainDb(pga1), (pga1 >> 4) & 1,
                  _volume, ok ? "OK" : "★异常");
}

void AudioIO::applyVolume() {    if (_codecReady) {
        // ★ 2026-10-03 第三版音量映射（前两版都不对，这次算清楚了）
        //   ES8311 的 0x32 (DAC Volume)：0x00=-95.5dB，0xBF=0dB，0xFF=+32dB，
        //   每步 0.5dB。板子上 DAC 后面还挂了 NS4150B 功放（约 +24dB）。
        //
        //   第一版：把用户音量 150 直接当寄存器值写 → -20.5dB，等于没声。
        //   第二版：映到 0xC0~0xFF，默认 190 ≈ +24dB —— 加上功放快 +48dB，
        //           16bit PCM 直接削顶 → 老陈听到的"特别大、发硬、失真"。
        //   本版：映到 -16dB(0x9F) ~ +12dB(0xD7)，默认 150 ≈ 0dB，
        //         既不削顶，又给"音量大点/小点"留了上下调整空间。
        //         真要安静可以再降到 0x9F 附近；静音交给 MUTE 寄存器。
        uint8_t reg = 0x00;
        if (!_muted)
            reg = (uint8_t)(0x9F + (uint16_t)_volume * (0xD7 - 0x9F) / 255);
        es8311.setVolume(reg);
        es8311.setMute(_muted);
    }
    Preferences pref;
    pref.begin("aiprint", false);
    pref.putUChar("vol3", _volume);
    pref.end();
}

void AudioIO::setVolume(uint8_t vol) {
    _volume = vol;
    _muted = false;
    applyVolume();
    Serial.printf("[AUDIO] volume -> %u\n", _volume);
}
void AudioIO::volumeUp(uint8_t step) {
    setVolume(min(255, (int)_volume + step));
}
void AudioIO::volumeDown(uint8_t step) {
    setVolume(_volume > step ? _volume - step : 0);
}
void AudioIO::setMute(bool mute) {
    _muted = mute;
    applyVolume();
}

bool AudioIO::recordStart() {
    if (_recording || !_codecReady) return false;
    if (!_recBuf) _recBuf = (uint8_t*)ps_malloc(SAMPLE_RATE * 2 * REC_MAX_SECONDS);
    if (!_recBuf) { Serial.println("[AUDIO] PSRAM rec buf fail"); return false; }
    _recLen = 0;
    _recording = true;
    i2s_zero_dma_buffer(I2S_NUM_0);
    return true;
}

uint32_t AudioIO::recordStop() {
    if (!_recording) return 0;
    _recording = false;
    // 丢弃开头 100ms（按键抖动/环境噪声）
    if (_recLen > SAMPLE_RATE * 2 / 10) {
        memmove(_recBuf, _recBuf + SAMPLE_RATE * 2 / 10,
                _recLen - SAMPLE_RATE * 2 / 10);
        _recLen -= SAMPLE_RATE * 2 / 10;
    }
    Serial.printf("[AUDIO] recorded %u bytes (%.1fs)\n",
                  _recLen, _recLen / (float)(SAMPLE_RATE * 2));
    return _recLen;
}

// 主循环里高频调用：录音期间持续从 I2S DMA 取数
void AudioIO::recordPoll() {
    if (!_recording || !_recBuf || _recLen >= SAMPLE_RATE * 2 * REC_MAX_SECONDS)
        return;
    size_t br = 0;
    if (i2s_read(I2S_NUM_0, _chunk, sizeof(_chunk), &br, 0) != ESP_OK || br < 4)
        return;
    const int mono = takeLeft(_chunk, (int)(br / 2), _mono);
    if (mono < 2) return;
    uint32_t space = SAMPLE_RATE * 2 * REC_MAX_SECONDS - _recLen;
    uint32_t take  = (uint32_t)mono * 2;
    if (take > space) take = space;
    memcpy(_recBuf + _recLen, _mono, take);
    _recLen += take;
}

// ================================================================
// ★★ 回声抵消（AEC）—— 2026-10-04 晚
//
// 硬件事实（老陈）：**ES8311 的音频输出既接功放，又直接接到 ES7210 的 MIC3**。
//   ⇒ 播出来的声音是"电"回麦克风的（不是空气回声）：几乎无混响、延迟固定且很小。
// I2S 是全双工、收发共用同一组 MCLK/BCK/WS：
//   ⇒ **我们写进 TX 的 PCM 就是最准的参考信号**（比去读 MIC3 还准：不受 ADC 噪声
//     影响，而且天然同步）。
//
// 模型：mic[n] ≈ Σ_{k=0..TAPS-1} w[k] · tx[n - D - k]
//   D = 参考→回声的固定延迟（codec 群延迟 + I2S 收发缓冲错位），由相关搜索给出；
//   w = NLMS 自适应滤波系数；残差 e = mic - ŷ 就是**纯人声**。
//
// 两个必须防的坑：
//   ① 双讲：用户说话时若继续自适应，滤波器会去学用户的声音、把人也抵消掉。
//      ⇒ 残差能量远大于"回声估计"时**冻结**更新。
//   ② 延迟漂移：TX/RX 的 DMA 填充量变化、RX 丢样都会让 D 漂 —— 定期重估。
// ================================================================

// 把一段 TX（单声道）样本推进参考环形缓冲，并推进 TX 时间轴。
void AudioIO::_aecPushRef(const int16_t* s, uint32_t n) {
    uint32_t w = _refW;
    for (uint32_t i = 0; i < n; i++) {
        _ref[w] = s[i];
        w = (w + 1) & (AEC_RING - 1);
    }
    _refW = w;
    _refTotal += n;
}

// ★ 改版30 硬件回采：把**右声道**（MIC2 = ES8311 输出回采）推进参考环形缓冲。
//   关键：它和左声道（麦克风）是**同一个 I2S 帧里抽出来的** ⇒ 参考与麦克风天然同步，
//   D ≡ 0 —— 所以硬件回采模式下**完全不需要延迟搜索**（这是它比 TX PCM 参考强的根因：
//   TX PCM 的时间戳是"写进 DMA 那一刻"，实际播出还要等 DMA 积压，而积压随网络抖动）。
void AudioIO::_aecPushRefRight(const int16_t* src, int cnt) {
    uint32_t w = _refW;
    uint32_t pushed = 0;
    // 硬件回采：抽 AEC_REF_CH 那一路（标准 I2S = 右声道/MIC2；TDM = MIC3）
    for (int i = 0; i + I2S_CH_NUM <= cnt; i += I2S_CH_NUM) {
        _ref[w] = src[i + AEC_REF_CH];
        w = (w + 1) & (AEC_RING - 1);
        pushed++;
    }
    _refW = w;
    _refTotal += pushed;
}

// 用最近 AEC_SEARCH_WIN 个**原始** RX 样本，对参考做归一化互相关，找延迟 D。
//   电回声没有混响，相关峰很尖，几百毫秒的窗就足够定死。
void AudioIO::_aecSearchDelay() {
    if (_rxHistTotal < (uint32_t)(AEC_SEARCH_WIN * 2)) {         // RX 历史还不够
        _lastAecSearch = millis() - AEC_SEARCH_MS + 40;          // 40ms 后再试
        return;
    }
    // ★ 改版31：参考环也要攒够。否则相关窗口会落在"还没推过参考"的空区里，
    //   搜出来的是纯噪声里的假峰（改版30 那串"每轮都不同的 D"根子就在这）。
    //   要求：至少 MAXD + WIN 个参考样本，任何 D ≤ MAXD 都有完整的参考窗。
    if (_refTotal < (uint32_t)(AEC_SEARCH_MAXD + AEC_SEARCH_WIN)) {
        _lastAecSearch = millis() - AEC_SEARCH_MS + 60;
        return;
    }

    // RX 窗按时间顺序摊平（_rxHistN 指向"下一个要写"= 最老）
    static int16_t rxw[AEC_SEARCH_WIN];
    for (int i = 0; i < AEC_SEARCH_WIN; i++)
        rxw[i] = _rxHist[(_rxHistN + i) & (AEC_SEARCH_WIN - 1)];

    float prx = 0;
    for (int i = 0; i < AEC_SEARCH_WIN; i++) prx += (float)rxw[i] * rxw[i];
    if (prx < 40000.0f) {                                        // 太安静，定不了位
        _lastAecSearch = millis() - AEC_SEARCH_MS + 100;
        return;
    }

    const int32_t rxLast   = (int32_t)_rxHistTotal - 1;          // 窗内最新样本的绝对下标
    const int32_t refNew   = (int32_t)_refTotal - 1;
    // 环形里最老的可相关起点：环形没绕回来时，下标必须 ≥ 窗长
    int32_t refOld = (int32_t)_refTotal - AEC_RING + AEC_SEARCH_WIN;
    if (refOld < AEC_SEARCH_WIN) refOld = AEC_SEARCH_WIN;

    int   bestD  = -1;
    float bestNC = 0;
    // 粗搜（步长 4 样本 = 0.25ms）→ 细化（±4，步长 1）
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && bestD < 0) return;
        const int dLo = (pass == 0) ? 0 : bestD - 4;
        const int dHi = (pass == 0) ? AEC_SEARCH_MAXD : bestD + 4;
        const int dSt = (pass == 0) ? 4 : 1;
        for (int D = dLo; D <= dHi; D += dSt) {
            if (D < 0) continue;
            const int32_t r1 = rxLast - D;
            const int32_t r0 = r1 - (AEC_SEARCH_WIN - 1);
            if (r0 < refOld || r1 > refNew) continue;
            float c = 0, pref = 0;
            int32_t j = r0 & (AEC_RING - 1);
            for (int t = 0; t < AEC_SEARCH_WIN; t++) {
                const float rv = (float)_ref[j];
                c += rv * rxw[t];
                pref += rv * rv;
                j = (j + 1) & (AEC_RING - 1);
            }
            if (pref < 40000.0f) continue;
            const float nc = c / sqrtf(pref * prx);
            if (nc > bestNC) { bestNC = nc; bestD = D; }
        }
    }
    _lastAecSearch = millis();                                   // 这轮搜过了

    if (bestD < 0 || bestNC < AEC_MIN_CORR) {
        // ★ 改版29：原来这里**闷着不吭声** —— 于是上板日志里"一条 [AEC] 都没有"，
        //   完全看不出是"峰太弱"还是"搜索没跑"。现在把最佳峰也报出来（限流 500ms）。
#if AEC_DEBUG
        static uint32_t lastWeak = 0;
        if (millis() - lastWeak > 500) {
            lastWeak = millis();
            Serial.printf("[AEC] 相关峰太弱：最佳 D=%d（%.1fms，相关=%.2f < %.2f）"
                          " —— 参考与回声还没对上\n",
                          bestD, bestD / 16.0f, bestNC, AEC_MIN_CORR);
        }
#endif
        _pendD = -1; return;                                     // 这段没回声，沿用上次 D
    }

    if (bestD < (int)(AEC_TAPS - 1)) bestD = (int)(AEC_TAPS - 1); // 参考窗要够整段

    // ★★ 改版30：把"连续两次一致才认账"换成**投票制** —— 看最近 8 次搜索里
    //   有多少次落在同一个位置（±24 样本 = 1.5ms），攒够 3 票才改 D。
    //   为什么（改版29 日志）：单次搜索会偶发假峰（D 一会儿 127、一会儿 1632），
    //   "连续两次相等"要么被假峰打断、要么两次恰好撞上同一个假峰就锁错了。
    //   投票能容忍个别假峰：真峰会反复出现在同一位置，假峰每次都换地方。
    _dHist[_dHistW & 7] = bestD;
    _dHistW++;
    int votes = 0;
    for (int i = 0; i < 8; i++) {
        const int d = _dHist[i];
        if (d < 0) continue;
        const int diff = (d > bestD) ? (d - bestD) : (bestD - d);
        if (diff <= 24) votes++;
    }

    // 已经锁在附近、而且确实有共识 ⇒ 别动，省得反复清零滤波器
    const int dDiff = (bestD > (int)_aecD) ? (bestD - (int)_aecD)
                                           : ((int)_aecD - bestD);
    if (dDiff < 16 && votes >= 2) { _pendD = -1; return; }

    if (votes < 3 || bestNC < AEC_LOCK_CORR) {      // 共识不够 / 相关太弱 → 继续观察
#if AEC_DEBUG
        static uint32_t lastCandLog = 0;
        if (millis() - lastCandLog >= 600) {
            lastCandLog = millis();
            Serial.printf("[AEC] D 候选 %d（%.1fms，相关=%.2f，投票 %d/8）"
                          " —— 攒够 3 票且相关≥%.2f 才改\n",
                          bestD, bestD / 16.0f, bestNC, votes, AEC_LOCK_CORR);
        }
#endif
        _pendD = bestD;
        return;
    }

    Serial.printf("[AEC] 延迟锁定 D %d→%d 样本（%.1f→%.1fms，相关=%.2f，投票 %d/8）"
                  " ⇒ 滤波器清零重学\n",
                  (int)_aecD, bestD, _aecD / 16.0f, bestD / 16.0f, bestNC, votes);
    _aecD = bestD;
    memset(_aW, 0, sizeof(_aW));
    _aErrP = _aEchoP = 0;
    _dHistW = 0;                                    // 锁定后清票，免得旧票干扰下一轮
    for (int i = 0; i < 8; i++) _dHist[i] = -1;
}

// 逐样本处理：把 io[] 里的**原始麦克风**就地换成"减掉电回声后的残差"，返回残差 rms。
int AudioIO::_aecProcess(int16_t* io, int n) {
    // ★★ 改版29（致命 bug 修复）：TX 还没出声时，**RX 时间轴绝不能推进**。
    //   playBegin() 一开泵任务就猛读麦克风，而播放任务要等 ttsStream 连上 HTTPS
    //   才有数据（实测 704ms！），这段时间 _refTotal 一直是 0、_rxTotal 却涨了
    //   ~11000。于是"回声在 RX 上的位置"= 11000 + 真实延迟，**远超搜索上限 2400**
    //   ⇒ 相关搜索永远找不到峰 ⇒ 一条 [AEC] 都没有、ERLE 恒 0（改版28 上板实测）。
    //   修法：TX 还没出声之前，RX 只做"排空 + 测底噪"，不计数、不进搜索窗。
    if (_refTotal == 0) {
        int64_t acc = 0;
        for (int i = 0; i < n; i++) {
            _aecDc += ((float)io[i] - _aecDc) * 0.001f;   // 让直流跟踪器先稳下来
            const float d = (float)io[i] - _aecDc;
            acc += (int64_t)(d * d);
        }
        _aecRawRms = _aecResRms = (int)sqrtf((float)acc / n);
        _aecErle = 0;
        return _aecResRms;                                // 无回声可减，原样返回
    }

    // ① 去直流（就地）+ 原始 RX 进搜索窗
    //   ★★ 改版30 修的真 bug：原来搜索窗存的是**带直流偏置**的原始样本，而 ES7210
    //     输出本身有 DC 偏置 ⇒ 相关算式的分母（窗能量）被 DC 项撑大、分子没跟着涨
    //     ⇒ 归一化相关整体被摊薄（改版29 日志"相关峰太弱：最佳 D=95 相关=0.11"），
    //     真实峰淹在噪声里，冒出来的全是假峰 ⇒ D 每次都不同 ⇒ 永远锁不上。
    //     现在**先去直流再把样本写进搜索窗**，而且去直流后的值就地写回 io[]，
    //     下面第 ② 步直接用它（避免同一个样本被跟踪两次）。
    {
        uint32_t h = _rxHistN;
        for (int i = 0; i < n; i++) {
            _aecDc += ((float)io[i] - _aecDc) * 0.001f;   // 很慢的跟踪器（≈2Hz 高通）
            float d = (float)io[i] - _aecDc;
            if (d > 32767.0f) d = 32767.0f; else if (d < -32768.0f) d = -32768.0f;
            io[i] = (int16_t)d;                            // 就地换成去直流样本
            _rxHist[h] = io[i];                            // 搜索窗只存干净样本
            h = (h + 1) & (AEC_SEARCH_WIN - 1);
        }
        _rxHistN = h;
        _rxHistTotal += n;
    }

    const int32_t refNew = (int32_t)_refTotal - 1;
    const int32_t refOld = (int32_t)_refTotal - AEC_RING;
    int64_t accRaw = 0, accRes = 0;

    for (int i = 0; i < n; i++) {
        const float d = (float)io[i];      // ★ ①里已经去过直流，这里直接用
        accRaw += (int64_t)(d * d);

        // 参考窗绝对下标区间：[m-D-TAPS+1 , m-D]
        const int32_t r1 = (int32_t)(_rxTotal + i) - _aecD;
        const int32_t r0 = r1 - (AEC_TAPS - 1);
        if (r0 < refOld || r0 < 0 || r1 > refNew) {   // 参考还没到位（播放刚开始）
            accRes += (int64_t)(d * d);               // 原样输出
            continue;
        }

        // 环形两段拼成连续窗
        {
            const int32_t j = r0 & (AEC_RING - 1);
            int c0 = (int)(AEC_RING - j);
            if (c0 > (int)AEC_TAPS) c0 = (int)AEC_TAPS;
            memcpy(_xref, _ref + j, (size_t)c0 * 2);
            if (c0 < (int)AEC_TAPS) memcpy(_xref + c0, _ref, (size_t)(AEC_TAPS - c0) * 2);
        }

        // ② 回声估计 ŷ 与参考能量
        float y = 0.0f, px = 0.0f;
        for (int k = 0; k < AEC_TAPS; k++) {
            const float xv = (float)_xref[k];
            y  += _aW[k] * xv;
            px += xv * xv;
        }

        // ③ 残差（输入已去直流，这里只减回声）
        const float e = d - y;

        // ④ 双讲冻结：回声估计已经很强、而残差比它大得多 ⇒ 是用户在说话，别学他
        _aErrP  = _aErrP  * 0.97f + e * e * 0.03f;
        _aEchoP = _aEchoP * 0.97f + y * y * 0.03f;
        const bool freeze = (_aEchoP > 1.0e6f) && (_aErrP > 9.0f * _aEchoP);

        // ⑤ NLMS 更新（带极缓慢泄漏，防长期漂移）
        if (!freeze) {
            const float mu = AEC_MU * e / (px + 4000.0f);
            if (mu != 0.0f) {
                for (int k = 0; k < AEC_TAPS; k++)
                    _aW[k] = (_aW[k] + mu * (float)_xref[k]) * 0.99998f;
            }
        }

        float ev = e;
        if (ev > 32000.0f) ev = 32000.0f;
        else if (ev < -32000.0f) ev = -32000.0f;
        io[i] = (int16_t)ev;
        accRes += (int64_t)io[i] * io[i];
    }

    _rxTotal += n;

    // ⑥ 诊断：回声抑制量 ERLE = 10log10(麦克风能量 / 残差能量)
    const float rawE = (float)accRaw / n, resE = (float)accRes / n;
    _aecRawRms = (int)sqrtf(rawE);
    _aecResRms = (int)sqrtf(resE);
    if (rawE > 1.0f && resE > 1.0f) {
        const int erle = (int)(10.0f * log10f(rawE / resE));
        _aecErle = (_aecErle * 3 + erle) / 4;      // 平滑，避免单帧跳变
    }
    // ★ 收敛闩锁：**确实有声**（rms>300）且残差明显小于麦克风（≥8dB）才认账。
    //   一旦认账就不再撤销 —— 否则用户一说话残差变大、ERLE 掉下来，
    //   正好在最需要判定的时刻把判定关掉。这个闩锁就是"不许再自激"的保险。
    if (rawE > 90000.0f && _aecErle >= 8) _aecReady = true;

    // ⑦ 定期重估延迟 D
#if AEC_REF_SRC == 0
    //   ★ 硬件回采（AEC_REF_SRC=1）根本不需要搜索：参考=同帧采的右声道，D恒为0。
    //   ★ 改版30：一旦 AEC 认账（_aecReady=ERLE≥8dB），**冻结 D 不再重搜** ——
    //     改版29 日志里正是"锁定 D=127 → 刚要收敛 → 下一次搜索又把 D 改走 → 立刻崩"。
    const bool freezeD = (AEC_FREEZE_AFTER_READY != 0) && _aecReady;
    if (!freezeD && (millis() - _lastAecSearch >= AEC_SEARCH_MS)) {
        _lastAecSearch = millis();
        _aecSearchDelay();
    }
#endif

    return _aecResRms;
}

bool AudioIO::playBegin() {
    if (!_codecReady) return false;
    // 正在录一句话的时候别放音（会把扬声器的声音也录进去）
    if (_listening && _listenState == 1) return false;
    i2s_zero_dma_buffer(I2S_NUM_0);   // 清掉上次播放的残留（防"噗"声 / 打断后的尾巴）
    _bargeIn = false; _bargeFrames = 0; _playFloor = 0;
    _floorInit = false; _bargeRmsMax = 0; _lastBargeLog = millis();   // ★ 打断检测重新起步
    _refMaxRms = 0; _lastRefRms = 0;    // ★ 改版32：MIC2 右声道探针，每段播放重新计
    _playStartMs = millis();                                          // ★ 起播保护期起点
#if AEC_ENABLE
    // ★ AEC 起步：参考/RX 两条时间轴都归零
    _refW = 0; _refTotal = 0; _rxTotal = 0;
    _rxHistN = 0; _rxHistTotal = 0;
    memset(_aW, 0, sizeof(_aW));      // 系数每次重学（路径短，几十毫秒就收敛）
    _aErrP = _aEchoP = 0;
    _pendD = -1;                      // ★ 上次的"延迟候选"作废，重新确认
    // ★★ 改版31：**不要**在起播瞬间就搜 D —— 那时队列/DMA 还没稳、参考环里样本也少
    //   （首包还有 ~700ms 的 TLS 握手），搜出来的是"启动瞬态"的 D（实测每轮不同：
    //   276 ↔ 127）⇒ 滤波器反复清零重学 ⇒ ERLE 卡在 8dB。等播放稳定后再搜。
    _lastAecSearch = millis() + AEC_WARMUP_MS - AEC_SEARCH_MS;
    _aecErle = 0; _aecReady = false; _aecRawRms = _aecResRms = 0;
    _lastRefRms = 0;
    // ★★ 改版31：投票历史**跨轮保留**（不再清零）—— D 是链路常数，上一轮投出来的
    //   结果这一轮照样有效；保留它能让本轮更快锁定同一个 D，而不是从零攒票。
#if AEC_REF_SRC == 1
    // ★ 硬件回采：参考与麦克风同帧采样 ⇒ D ≡ 0，固定死，永远不搜。
    _aecD = 0;
#else
    // ★ TX PCM 参考：D **沿用上次播放估到的值**（它是"写进 DMA → 播出"的路径延迟，
    //   板子/链路不变就该是常数，没必要每次从头搜）。搜索会继续在投票制下迭代修正。
#endif
#endif
    // ★★ 播放队列 + 独立播放任务（改版28）—— 把"下载"和"播放"拆开 ★★
    //   老陈实测：ESP32 纯下载 143KB/s（PCM）/177KB/s（MP3），是播放需要的 32KB/s 的 4 倍多
    //   ⇒ 慢的从来不是网络，是"边下边播"里 socket 被 i2s_write 阻塞。现在队列一隔，
    //     网络可一口气跑在前面 ~1 秒，抖动全靠队列吸收。
    if (!_pq) {
        _pqCap = (uint32_t)PLAY_QUEUE_KB * 1024u;
        // ★★ 改版32：队列从 32KB 放大到 512KB —— 放得下的只有 PSRAM（内部 RAM 只剩
        //   ~190KB 空闲）。安全性：队列只在 core1 的"生产者(ttsPlay)"和"消费者
        //   (sPlayTask)"之间传递，**两者同核**，不存在跨核缓存可见性问题。
        if (_pqCap > 65536u) _pq = (uint8_t*)ps_malloc(_pqCap);   // 大队列 → PSRAM
        else                 _pq = (uint8_t*)malloc(_pqCap);
        if (!_pq && _pqCap > 65536u) {           // PSRAM 分配失败：退回小队列保功能
            _pqCap = 64u * 1024u;
            _pq = (uint8_t*)malloc(_pqCap);
            if (_pq) Serial.println("[PLAY] ★ PSRAM 队列分配失败，退回 64KB 内部 RAM");
        }
        if (_pq) Serial.printf("[PLAY] 播放队列 %uKB 已就绪（下载/播放解耦，%s）\n",
                               (unsigned)(_pqCap / 1024u),
                               (_pqCap > 65536u) ? "PSRAM" : "内部RAM");
        else     Serial.println("[PLAY] ★ 队列分配失败，退回直写 I2S（旧行为）");
    }
    _pqW = _pqR = 0;
    _playDone = false;
    _playRun  = (_pq != nullptr);
    _playTask = nullptr;
    if (_playRun) {
        // ★ 固定在 core1：和生产者在同一个核 ⇒ 队列读写不存在跨核缓存可见性问题
        if (xTaskCreatePinnedToCore(sPlayTask, "playtx", 4096, this, 4,
                                    &_playTask, 1) != pdPASS) {
            _playRun = false; _playTask = nullptr;
            Serial.println("[PLAY] ★ 播放任务创建失败，退回直写 I2S（旧行为）");
        }
    }
#if VAD_BARGE_ENABLE && VAD_AUTO_TRIGGER
    // ★★ 启动麦克风泵任务：独立于网络节奏、按实时速率抽干 RX。
    //   这是 AEC 收敛的前提 —— D 稳了 NLMS 才学得成（详见 sAecPump 注释）。
    //   ★ 只在"确实在监听"时启动（和旧代码的 `_listening` 门一致）：
    //     开机问候语等播放发生在 listenBegin() 之前，那时预滚缓冲还没分配，
    //     起泵就会写空指针（改版26 首烧实测：StoreProhibited）。
    _lastTxRms = 0;
    if (_listening && !_recording) {
        _pumpRun = true;
        if (xTaskCreatePinnedToCore(sAecPump, "aecpump", 6144, this, 2,
                                    &_pumpTask, 0) != pdPASS) {
            _pumpRun = false; _pumpTask = nullptr;
            Serial.println("[BARGE] ★ 泵任务创建失败，本段播放不做打断检测");
        }
    }
#endif
    digitalWrite(PIN_PA_EN, HIGH);    // 开功放
    delay(5);
    return true;
}

// 喂一段 16bit 单声道 PCM —— 只往播放队列里塞，**不碰 I2S**（改版28）。
//   队列满时才短暂让出（vTaskDelay(1)）等播放任务消费，天然形成背压；
//   但网络下载和放音彻底解耦了：socket 读循环不再被 i2s_write 拖住。
void AudioIO::playChunk(const uint8_t* data, uint32_t len) {
    if (!len || (len & 1)) return;              // 必须成对的 16bit 样本
    if (!_playRun || !_pq) { _playDirect(data, len); return; }   // 兜底：直写 I2S

    uint32_t pos = 0;
    while (pos < len) {
        if (_bargeIn || !_playRun) return;      // 被打断 / 任务没了：丢掉剩余音频
        const uint32_t lvl = _pqW - _pqR;
        if (lvl >= _pqCap) { vTaskDelay(1); continue; }   // 队列满：等放音消费
        const uint32_t space = _pqCap - lvl;
        uint32_t n = len - pos;
        if (n > space) n = space;
        const uint32_t idx   = _pqW & (_pqCap - 1);
        const uint32_t first = (n < (_pqCap - idx)) ? n : (_pqCap - idx);
        memcpy(_pq + idx, data + pos, first);
        if (n > first) memcpy(_pq, data + pos + first, n - first);
        __sync_synchronize();                   // ★ 数据先落地，再发布写指针
        _pqW += n;
        pos += n;
    }
}

// 兜底路径：队列/任务不可用时，按老办法直接展开双声道写 I2S（阻塞）。
void AudioIO::_playDirect(const uint8_t* data, uint32_t len) {
    static int16_t stereo[1024 * I2S_CH_NUM];   // 装得下 1024 个 mono 样本展开成 I2S_CH_NUM 路
    uint32_t pos = 0;
    while (pos < len) {
        if (_bargeIn) return;
        uint32_t chunk = len - pos;
        // ★ 上限 = sizeof(stereo)/I2S_CH_NUM（mono 字节数）：stereo 要同时装 mono（前
        //   nSamp 个 int16）+ 展开后的 I2S_CH_NUM 路 ⇒ 只够 capacity/I2S_CH_NUM 个样本。
        //   写单位同样要小（AEC 参考时间轴，见配置）。
        uint32_t lim = (uint32_t)PLAY_WRITE_UNIT;
        if (lim == 0 || lim > (uint32_t)(sizeof(stereo) / I2S_CH_NUM)) lim = (uint32_t)(sizeof(stereo) / I2S_CH_NUM);
        if (chunk > lim) chunk = lim;
        chunk &= ~1u;
        if (!chunk) break;
        const int16_t* src = (const int16_t*)(data + pos);
        const int nSamp = (int)(chunk / 2);
        memcpy(stereo, src, chunk);
        _lastTxRms = frameRms(stereo, nSamp);
#if AEC_ENABLE && (AEC_REF_SRC == 0)
        _aecPushRef(stereo, (uint32_t)nSamp);
#endif
        for (int i = nSamp - 1; i >= 0; i--) {  // ★ 倒着展开：ch0/ch1=左右声道，其余补 0
            const int16_t v = stereo[i];
            stereo[I2S_CH_NUM * i]     = v;
            stereo[I2S_CH_NUM * i + 1] = v;
            for (int c = 2; c < I2S_CH_NUM; c++) stereo[I2S_CH_NUM * i + c] = 0;
        }
        size_t written;
        i2s_write(I2S_NUM_0, stereo, (size_t)chunk * I2S_CH_NUM, &written, portMAX_DELAY);
        pos += chunk;
    }
}

// ================================================================
// ★★ 独立播放任务（改版28）—— 下载/播放解耦的消费者端 ★★
//
// 从播放队列取 mono PCM → 推 AEC 参考 → 展开双声道 → i2s_write（阻塞在 DMA）。
// 生产者（ttsPlay/socket 读循环）因此永远不会被放音节奏卡住。
//
// ★ AEC 参考在这里推（而不是在生产者的 playChunk 里）：因为参考必须代表
//   "**真正正在播出**的 PCM"，写 I2S 之前这一刻推最准 ⇒ TX 时间轴平滑 ⇒
//   "参考→回声"延迟 D 才是常数（改版25 日志里 D 乱跳 = 播放断流所致）。
// ================================================================
void AudioIO::sPlayTask(void* arg) {
    AudioIO* self = (AudioIO*)arg;
    static int16_t stereo[1024 * I2S_CH_NUM];
    // ★ 每次写 I2S 的 mono 字节数。**不能大** —— 见 ai_config.h PLAY_WRITE_UNIT：
    //   这一批样本的参考是"写 I2S 那一刻"一次性推进环形的，而它们实际播出要横跨
    //   这个单位那么久。单位越大，参考时间轴越糊，AEC 越锁不上（改版28 就是 2048B
    //   = 整个 TX DMA = 128ms，直接把相关峰抹平了）。
    uint32_t CH = (uint32_t)PLAY_WRITE_UNIT;
    if (CH == 0) CH = 512;                                        // 配置写错就兜底
    if (CH > (uint32_t)(sizeof(stereo) / I2S_CH_NUM)) CH = (uint32_t)(sizeof(stereo) / I2S_CH_NUM);

    while (true) {
        if (!self->_playRun) break;
        if (self->_bargeIn)  break;             // 被打断：立刻收工（playEnd 会清 DMA）
        const uint32_t lvl = self->_pqW - self->_pqR;
        if (lvl == 0) {
            if (self->_playDone) break;         // 输入结束且已播空 → 正常收工
            vTaskDelay(1);
            continue;
        }
        uint32_t n = (lvl < CH) ? lvl : CH;
        n &= ~1u;
        if (!n) { vTaskDelay(1); continue; }
        __sync_synchronize();                   // ★ 读到新 lvl 之后才允许读数据

        const uint32_t idx   = self->_pqR & (self->_pqCap - 1);
        const uint32_t first = (n < (self->_pqCap - idx)) ? n : (self->_pqCap - idx);
        memcpy((uint8_t*)stereo, self->_pq + idx, first);
        if (n > first) memcpy((uint8_t*)stereo + first, self->_pq, n - first);
        __sync_synchronize();
        self->_pqR += n;                        // ★ 先读走数据，再推进读指针

        const int nSamp = (int)(n / 2);
        self->_lastTxRms = frameRms(stereo, nSamp);
#if AEC_ENABLE && (AEC_REF_SRC == 0)
        // ★ 软件参考模式：把"正在写出去的 PCM"推进参考环（硬件回采模式不用它，
        //   参考来自麦克风那一侧的右声道，见 sAecPump）。
        self->_aecPushRef(stereo, (uint32_t)nSamp);
#endif
        for (int i = nSamp - 1; i >= 0; i--) {  // ★ 倒着展开：ch0/ch1=左右声道，其余补 0
            const int16_t v = stereo[i];
            stereo[I2S_CH_NUM * i]     = v;
            stereo[I2S_CH_NUM * i + 1] = v;
            for (int c = 2; c < I2S_CH_NUM; c++) stereo[I2S_CH_NUM * i + c] = 0;
        }
        size_t written;
        i2s_write(I2S_NUM_0, stereo, (size_t)n * I2S_CH_NUM, &written, portMAX_DELAY);
    }
    self->_playTask = nullptr;                  // 先告诉 playEnd"我退了"，再自杀
    vTaskDelete(nullptr);
}

// 等播放任务退场（带上限，防止任务卡死把主循环拖住）
void AudioIO::_waitPlayExit(uint32_t ms) {
    const uint32_t t0 = millis();
    while (_playTask && (millis() - t0) < ms) delay(1);
}

// ================================================================
// ★★ 播放期麦克风泵任务（改版26）—— AEC 能否收敛的关键 ★★
//
// 职责：播放期间以 20ms 周期独立抽干 RX DMA → AEC 减回声 → 打断判定。
//
// 为什么必须是独立任务（改版25 的血泪日志）：
//   TTS 边下边播，网络喂块一阵一阵（16KB/s 时 ~250ms 才来一块）。原来在
//   playChunk 里读麦克风，读的节奏跟网络走 ⇒ RX DMA 积压在 0~256ms 之间
//   来回摆 ⇒ "参考→回声"延迟 D 本身就在漂 ⇒ 相关搜索每次搜出不同的 D
//   （日志：D 1333→95→1161→1989→776→…）⇒ 滤波器每 500ms 清零重学一次
//   ⇒ NLMS 永远收敛不了，ERLE 全是 0 或负数。
//   泵任务按实时速率抽干 RX 后，RX 积压恒为 ~0，D 只剩
//   "TX DMA 积压(≤2048) + codec 群延迟" —— 网速正常时是个稳定常数。
// ================================================================
void AudioIO::sAecPump(void* arg) {
    AudioIO* self = (AudioIO*)arg;
    // ★★ 改版32：缓冲 512→1024 个 int16（2048 字节 = 32ms 立体声），并且**非阻塞抽干**。
    //   原来的写法（固定读 1024 字节 + timeout 20ms）余量为零：RX 的生产速率正好是
    //   1024 字节 / 16ms，只要循环周期比 16ms 略长，RX DMA 就会缓慢积压 → 溢出丢样本
    //   → 参考/回声两条时间轴出现跳跃 → 相关搜索永远锁不住（改版31 日志"D 候选在
    //   41↔2659 之间乱跳"的一大嫌疑）。现在读到读不动为止，真没数据才让出 1ms。
    static int16_t raw[1024];
    static int16_t mono[512];
    uint32_t quietSince = 0;          // ★ 改版32：TX 静音窗起点（本段播放内有效）
    while (self->_pumpRun) {
        // 监听被中途关掉（listenEnd）就收工，别再碰预滚缓冲
        if (!self->_listening) break;
        size_t br = 0;
        if (i2s_read(I2S_NUM_0, raw, sizeof(raw), &br, 0) != ESP_OK || br < 4) {
            vTaskDelay(1);            // RX 空了：让出 1ms 再来
            continue;
        }
        const int cnt = (int)(br / 2);         // 交织样本总数（左=麦克风 / 右=参考）
        const int n = takeLeft(raw, cnt, mono);
        if (n < 2) continue;

        // ★★ 回采探针：测 AEC_REF_CH 那一路的电平（标准I2S=MIC2 / TDM=MIC3）。
        //   播放时它该跟着音频涨 ⇒ 说明"ES8311 输出回采线"确实接到了这一路。
        {
            int64_t racc = 0; int m = 0;
            for (int i = 0; i + I2S_CH_NUM <= cnt; i += I2S_CH_NUM) {
                const int v = raw[i + AEC_REF_CH]; racc += (int64_t)v * v; m++;
            }
            const int rr = m ? (int)sqrtf((float)racc / m) : 0;
            self->_lastRefRms = rr;
            if (rr > self->_refMaxRms) self->_refMaxRms = rr;
        }
#if AEC_ENABLE && (AEC_REF_SRC == 1)
        // ★★ 硬件回采（改版30）：参考 = 本块的**右声道**（MIC2 = ES8311 输出回采）。
        //   ★ 必须先推参考、再处理麦克风：两者同帧同长，推完 D≡0 就精确对齐。
        //     （TX PCM 参考模式相反：参考在播放任务里推，麦克风在这里读，两者隔着网络节奏。）
        self->_aecPushRefRight(raw, cnt);
#endif

        // AEC：就地减掉回声，返回**残差** rms。
        //   检测与预滚全部用残差 —— "自己的播报不再干扰自己"的关键。
#if AEC_ENABLE
        const int rms = self->_aecProcess(mono, n);
#else
        const int rms = frameRms(mono, n);
#endif
        // 播放期间的音频也进预滚 —— 触发后能把"你说的前半句"接上
        //   （AEC 之后 mono 已经是残差 = 纯人声，录进去更干净）
        self->_prePush((const uint8_t*)mono, (uint32_t)n * 2);
        if (rms > self->_bargeRmsMax) self->_bargeRmsMax = rms;

        // ★ 残差底噪（对称慢平均）：AEC 之后，只剩回声时残差是"干净的低电平"，
        //   用户一开口残差会**整体跳上去** → 均值的 2.5 倍当门就够。
        if (!self->_floorInit) { self->_playFloor = (float)rms; self->_floorInit = true; }
        else self->_playFloor += ((float)rms - self->_playFloor) * 0.06f;
        if (self->_playFloor < 40.0f) self->_playFloor = 40.0f;

        // ---- ★★ 改版32 新增：TX 静音窗（"免费"的灵敏探测窗）----
        //   TTS 句与句之间本来就有几百毫秒静音（日志里 `TXrms=0/1/2/4` 那些行）。
        //   喇叭不出声的那段时间回声≈0，残差就是干净的环境噪声 ⇒ 这时候开门判定
        //   既**不需要 AEC 收敛**，也**绝不会自激**（没有回声可以撞穿门）。
        //   ⇒ 等于把 TTS 每个句间停顿都变成一个灵敏的人声探测窗，
        //     这是"TX PCM 参考这条 AEC 路线还没收敛"时最实在的补救。
        //   ⚠ 必须等 HOLD：`_lastTxRms` 是"刚写进 DMA 的块"，真正播出还要等 DMA 积压
        //     (~128ms)；不等待就会在"上一句还在响"的混响尾巴上开门 ⇒ 误触。
        if (self->_lastTxRms < VAD_BARGE_QUIET_TX) {
            if (!quietSince) quietSince = millis();
        } else {
            quietSince = 0;
        }
        const bool quietWin =
            quietSince && (millis() - quietSince >= VAD_BARGE_QUIET_HOLD_MS);

        int gate = (int)(self->_playFloor * VAD_BARGE_MULT);
        if (gate < VAD_BARGE_MIN) gate = VAD_BARGE_MIN;

        // ★ 判定放行条件：
        //   ① 起播保护期已过（TTS 起音 + 功放启动的瞬态）；
        //   ② 二选一即可放行：
        //      · **TX 静音窗**（上面）—— 此时没有回声，门可以放很低；
        //      · 或者 AEC 已经**确实在抵消回声**（ERLE≥8dB，_aecReady 闩锁）。
        //   两者都不满足就不判定 —— 宁可"暂时打不断"，也绝不再"自己打断自己"。
        bool guard = (millis() - self->_playStartMs) < VAD_BARGE_SKIP_MS;
        if (quietWin) {
            int qg = (int)(self->_noiseFloor * 3.0f);          // 无回声时可贴地判定
            if (qg < VAD_BARGE_QUIET_GATE) qg = VAD_BARGE_QUIET_GATE;
            if (qg < gate) gate = qg;                          // 只降不升
        } else {
#if AEC_ENABLE
            if (!self->_aecReady) guard = true;
#endif
        }

        if (!guard && (rms > gate || rms > VAD_BARGE_HARD)) {
            if (++self->_bargeFrames >= VAD_BARGE_FRAMES) {
                self->_bargeTrigger(rms, gate);
                break;                    // 打断成立，泵任务收工（playEnd 会等它退场）
            }
        } else {
            self->_bargeFrames = 0;
        }
#if VAD_BARGE_DEBUG
        if (millis() - self->_lastBargeLog >= 500) {
            // 判读速查：
            //   门         = 当前判定门（静音窗内会降到 ~400；否则 = 底噪×倍数，下限 650）
            //   TXrms      = 刚写进 DMA 的这块音频的能量（<600 即"静音窗"候选）
            //   参考R      = 右声道(MIC2)电平 —— 播放时该跟着音频涨；恒 0 = 回采线没接 MIC2
            //   静音窗/AEC就绪 = 两条放行途径，任一为 1 才会判定打断
            Serial.printf("[BARGE] 残差 rms=%d 底噪=%.0f 门=%d 峰值=%d | "
                          "麦克风原始=%d ERLE=%ddB TXrms=%d 参考R=%d 静音窗=%d AEC就绪=%d\n",
                          rms, self->_playFloor, gate, self->_bargeRmsMax,
                          self->_aecRawRms, self->_aecErle, self->_lastTxRms,
                          self->_lastRefRms, quietWin ? 1 : 0,
                          self->_aecReady ? 1 : 0);
            self->_lastBargeLog = millis(); self->_bargeRmsMax = 0;
        }
#endif
    }
    self->_pumpTask = nullptr;            // 先告诉 playEnd"我退了"，再自杀
    vTaskDelete(nullptr);
}

// 打断成立：置位 + 把预滚尾部（用户开口的起音 250ms）接进录音缓冲，
// 主循环的 listenPoll() 接着把这句话录完。由泵任务调用。
void AudioIO::_bargeTrigger(int rms, int gate) {
    _bargeIn = true;
    Serial.printf("[BARGE] ★ 听到你说话了，停止播报"
                  "（残差 rms=%d 门=%d 底噪=%.0f 峰值=%d ERLE=%ddB）\n",
                  rms, gate, _playFloor, _bargeRmsMax, _aecErle);
    if (_recBuf) {
        const uint32_t cap = (uint32_t)SAMPLE_RATE * 2 * REC_MAX_SECONDS;
        _recLen = 0;
        uint32_t tail = (uint32_t)SAMPLE_RATE * 2 * VAD_BARGE_PREROLL_MS / 1000;
        uint32_t n2 = (_pre && _preCap) ? _preLen : 0;
        if (tail && n2 > tail) n2 = tail;      // 只回带尾部，别把喇叭声全带进来
        if (n2 > cap) n2 = cap;
        if (n2) {
            const uint32_t st = (_prePos + _preCap - n2) % _preCap;
            for (uint32_t i = 0; i < n2; i++) _recBuf[_recLen++] = _pre[(st + i) % _preCap];
        }
        _preLen = _prePos = 0;
        int t = (int)(_noiseFloor * VAD_MULT);
        if (t < VAD_MIN_RMS) t = VAD_MIN_RMS;
        _thr = t;
        _speechMask = 0xFF;
        _rmsHistN = _rmsHistIdx = 0;
        _rmsAvg = 0;
        _silenceFrames = 0;
        // ★ 改版31：打断后**至少**录 VAD_BARGE_MIN_REC_MS —— 否则用户刚开口 ~0.8s
        //   就被判"说完"，只录到一小段碎片，ASR 识别为空（老陈实测过）。
        _minRecBytes = (uint32_t)SAMPLE_RATE * 2 * VAD_BARGE_MIN_REC_MS / 1000;
        _listenState = 1;      // ★ 已在录音中，主循环直接续录
    }
}

void AudioIO::playEnd() {
    // ① 通知播放任务"输入结束"，等它把队列里剩下的音频播完。
    //    边等边看打断 —— 用户在缓冲里开口，照样要立刻停。
    _playDone = true;
    // ★★ 改版32：队列放大到 512KB(16s) 后，下载往往在播放刚开始时就结束了，队列里
    //   还压着十几秒音频 ⇒ 原来写死的 3000ms 上限会把音频尾巴**直接切掉**。
    //   改成"看有没有进展"：只要队列在减少就继续等，连续 3s 一点都不动才判卡死收工。
    uint32_t lastLvl = 0xFFFFFFFFu, lastChg = millis();
    while (_playRun && !_bargeIn) {
        const uint32_t lvl = _pqW - _pqR;
        if (lvl == 0) break;                           // 队列已排空
        if (lvl != lastLvl) { lastLvl = lvl; lastChg = millis(); }
        if (millis() - lastChg > 3000) break;          // 3s 毫无进展 = 播放任务卡死
        delay(1);
    }
#if VAD_BARGE_ENABLE && VAD_AUTO_TRIGGER
    // ② 再停泵任务（让它一直守到此刻，覆盖"最后一段还在播"的时间窗）
    _pumpRun = false;
    const uint32_t tPump = millis();
    while (_pumpTask && millis() - tPump < 200) delay(2);

    // ★★ 回采探针结论 —— **一次烧录定案"回采参考到底读没读到"**。
    //   判读：本段播放里 AEC_REF_CH 那一路（TDM=MIC3 / 标准I2S=MIC2）的最大电平。
    //     ≥300  ⇒ 有回采信号 ⇒ 硬件回采可用（把 AEC_REF_SRC 改成 1 重编即生效）
    //     <300  ⇒ 该路没信号
    if (_refMaxRms >= 300)
        Serial.printf("[AEC] ★ 回采探针(通道%d)：本段峰值=%d ⇒ **读到回采参考**！"
                      "把 AEC_REF_SRC 改成 1 重编即启用硬件回采（与麦克风同帧，D≡0）\n",
                      AEC_REF_CH, (int)_refMaxRms);
    else
        Serial.printf("[AEC] 回采探针(通道%d)：本段峰值=%d（≈静音）⇒ 该路没回采信号"
                      "（I2S_TDM_MODE=%d）\n", AEC_REF_CH, (int)_refMaxRms, I2S_TDM_MODE);
#endif
    // ③ 被打断：先停播放任务（免得它又往 DMA 里写），再清 TX DMA 让喇叭当场闭嘴。
    //    TX DMA 里最多还排着 128ms（8×256 帧）没播完的音频，不清就是
    //    "叫它停它还在说"。
    if (_bargeIn) {
        _playRun = false;
        _waitPlayExit(300);
        i2s_zero_dma_buffer(I2S_NUM_0);
        digitalWrite(PIN_PA_EN, LOW);
        return;
    }
    // ④ 正常收工：停播放任务，等 DMA 里最后一块走完再关功放（避免"啪"声/尾音被切）
    _playRun = false;
    _waitPlayExit(500);
    vTaskDelay(pdMS_TO_TICKS(PLAY_DRAIN_MS));
    digitalWrite(PIN_PA_EN, LOW);
}

bool AudioIO::playPcm(const uint8_t* data, uint32_t len) {
    if (!playBegin()) return false;
    playChunk(data, len);
    playEnd();
    return true;
}

void AudioIO::beep(uint16_t freqHz, uint16_t ms) {
    const uint32_t n = (uint32_t)SAMPLE_RATE * ms / 1000;
    static int16_t* tone = nullptr;
    if (!tone) tone = (int16_t*)malloc(SAMPLE_RATE / 10 * 2);
    if (!tone) return;
    const uint32_t gen = min(n, (uint32_t)(SAMPLE_RATE / 10));
    // 振幅 9000（约 -11dBFS）：上一版提到 16000 太冲，配合功放会"呲"
    for (uint32_t i = 0; i < gen; i++)
        tone[i] = (int16_t)(sinf(2 * PI * freqHz * i / SAMPLE_RATE) * 9000 *
                            (1.0f - i / (float)gen));  // 淡出防爆音
    playPcm((uint8_t*)tone, gen * 2);
}

// ================================================================
// VAD 常时监听（免按键对话）
//
// 【2026-10-03 重写，原来"说话没反应"的根因】
//   ① 算 RMS 时没去直流偏置：ES7210 输出有 DC 偏置时，"安静"帧的
//      RMS 也有几百，噪声底被顶到很高 → 阈值永远达不到。
//   ② 噪声底只往上爬不会自己校准 → 越跑越聋。
//   ③ VAD_MIN_RMS 定 450 太高（-37dBFS），小音量说话触发不了。
//   现在：开机标定 250ms 环境噪声底 + 逐帧去偏置 RMS + 阈值降到 200，
//   并且每秒打印一次 peak/noise/thr，能不能听到一看日志就知道。
// ================================================================

// 去直流偏置后的 RMS
static int frameRms(const int16_t* s, int n) {
    if (n <= 0) return 0;
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += s[i];
    const int mean = (int)(sum / n);
    int64_t acc = 0;
    for (int i = 0; i < n; i++) {
        const int v = s[i] - mean;
        acc += (int64_t)v * v;
    }
    return (int)sqrtf((float)acc / n);
}

bool AudioIO::listenBegin() {
    if (!_codecReady) return false;
    if (!_recBuf) _recBuf = (uint8_t*)ps_malloc(SAMPLE_RATE * 2 * REC_MAX_SECONDS);
    if (!_recBuf) { Serial.println("[VAD] PSRAM rec buf fail"); return false; }

    _preCap = (uint32_t)SAMPLE_RATE * 2 * VAD_PREROLL_MS / 1000;
    if (!_pre) _pre = (uint8_t*)malloc(_preCap);
    if (!_pre) { Serial.println("[VAD] preroll buf fail"); return false; }

    _preLen = _prePos = 0;
    _recLen = 0;
    _minRecBytes = 0;                 // ★ 改版31：新一轮监听，最小录音门槛复位
    _speechFrames = _silenceFrames = 0;
    _speechMask = 0;
    _rmsHistN = _rmsHistIdx = 0;
    _rmsAvg = 0;
    _listenState = 0;
    _listening = true;
    _peak = 0; _peakRaw = 0;
    _lastLog = millis();
    _cooldownUntil = millis() + 200;
    i2s_zero_dma_buffer(I2S_NUM_0);

    // ---- 先静默 250ms，把扬声器余音/房间混响排空 ----
    //   上一版紧接着自我介绍 TTS 之后立刻标定，噪声底被自己的喇叭声顶到
    //   noise=89、rawL=2161（老陈 2026-10-03 日志里就是这行），阈值跟着虚高。
    {
        size_t br;
        const uint32_t t = millis();
        while (millis() - t < 250)
            i2s_read(I2S_NUM_0, _chunk, sizeof(_chunk), &br, pdMS_TO_TICKS(20));
    }

    // ---- 标定环境噪声底（约 300ms）----
    //   同时统计 raw（原始样本最大绝对值，不去直流）。
    //   标定的同时把"左/右声道各自的原始峰值"分别统计出来 ——
    // 这两个数就是判断麦克风到底通没通的硬指标：
    //   左右都 <200 → ES7210 侧没出数据（看上面 ES7210 自检那行）
    //   左大右小   → 正常（MIC1 接在左声道 ADC1）
    //   左小右大   → 麦克风其实接在 ADC2 上，得改用右声道
    float sum = 0; int n = 0, rawPeak = 0, rawL = 0, rawR = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < 300) {
        size_t br = 0;
        if (i2s_read(I2S_NUM_0, _chunk, sizeof(_chunk), &br,
                     pdMS_TO_TICKS(20)) == ESP_OK && br >= 64) {
            const int cnt = (int)(br / 2);
            for (int i = 0; i + I2S_CH_NUM <= cnt; i += I2S_CH_NUM) {
                // rawL = 第 0 路(MIC1 语音)；rawR = AEC_REF_CH(标准I2S=MIC2，TDM=MIC3 回采)
                const int chR = _chunk[i + AEC_REF_CH];
                const int l = _chunk[i] < 0 ? -_chunk[i] : _chunk[i];
                const int r = chR        < 0 ? -chR      : chR;
                if (l > rawL) rawL = l;
                if (r > rawR) rawR = r;
            }
            const int mono = takeLeft(_chunk, cnt, _mono);
            if (mono < 2) continue;
            sum += frameRms(_mono, mono);
            n++;
            for (int i = 0; i < mono; i++) {
                const int v = _mono[i] < 0 ? -_mono[i] : _mono[i];
                if (v > rawPeak) rawPeak = v;
            }
        }
    }
    _noiseFloor = n ? (sum / n) : 30.0f;
    if (_noiseFloor < 12.0f) _noiseFloor = 12.0f;
    if (_noiseFloor > 2000.0f) _noiseFloor = 2000.0f;

    int thr = (int)(_noiseFloor * VAD_MULT);
    if (thr < VAD_MIN_RMS) thr = VAD_MIN_RMS;
    Serial.printf("[VAD] listening... noise=%.0f rawL=%d rawR=%d thr=%d (preroll=%ums max=%us)\n",
                  _noiseFloor, rawL, rawR, thr, VAD_PREROLL_MS, REC_MAX_SECONDS);
    // 判读基准（PGA = MIC_PGA_CODE 档，默认 36dB，实测 2026-10-03）：
    //   静音：rawL 约 200~500（rawR 恒为 2 —— 只开了 ADC1，右声道本来就空）
    //   正常说话 50cm：rawL 冲到 1000~3000
    //   左右都 <150 → 芯片侧确实没出数据，先看 [AUDIO] MIC增益 那一行
    if (rawL < 150 && rawR < 150)
        Serial.println("[VAD] ★ 左右声道都没信号（36dB 增益下静音时也该有 200~500）→ "
                       "看上面 [AUDIO] MIC增益 那一行，别急着调 VAD 阈值");
    else if (rawR > rawL * 3 && rawR > 800)
        Serial.println("[VAD] ★ 左声道弱、右声道强 → 麦克风可能接在 ADC2，"
                       "要把 takeLeft 改成 takeRight");
    else
        Serial.printf("[VAD] ✓ 麦克风有信号（rawL=%d rawR=%d）；说话时 rawL 该上 1000\n",
                      rawL, rawR);
    return true;
}

void AudioIO::listenEnd() { _listening = false; }

void AudioIO::listenResume() {
    if (!_listening) return;
    // ★★ 播报被打断（barge-in）时：用户已经在说话了 —— **千万不要重置**！
    //   打断那一刻已经把状态切成"录音中"、预滚尾部也接进了 _recBuf；
    //   这里只清标志，让它接着把这句话录完（重置了 = 用户白说一半，又得重说）。
    if (_bargeIn) {
        _bargeIn = false; _bargeFrames = 0;
        Serial.printf("[BARGE] 接着录用户这句话（已带过来 %u 字节）\n", (unsigned)_recLen);
        return;
    }
    i2s_zero_dma_buffer(I2S_NUM_0);   // 丢掉扬声器余音
    // ★ 预滚**不清空**（原来这里是 _preLen = _prePos = 0）：
    //   里面装的是"刚播完这段 + 冷却期"的音频，用户"你一说完我马上接话"的前半句
    //   就在里面；清掉就等于逼他重说一遍 —— 正是老陈抱怨的那个毛病。
    _speechFrames = _silenceFrames = 0;
    _speechMask = 0;
    _rmsHistN = _rmsHistIdx = 0;
    _rmsAvg = 0;
    _listenState = 0;
    _minRecBytes = 0;                 // ★ 改版31：正常恢复监听，门槛复位
    _peak = 0; _peakRaw = 0;
    _playFloor = 0; _floorInit = false; _bargeFrames = 0; _bargeRmsMax = 0;
    // 冷却：TTS 播完后喇叭余音 + 房间混响要这么久才散，这段不判定触发（防自激）。
    //   ★ 注意：冷却期**照样往预滚里填音频**（见 listenPoll），所以不会丢字。
    _cooldownUntil = millis() + VAD_COOLDOWN_MS;
}

// ── ASR 前处理：80Hz 高通 + 去直流 + 峰值归一化（就地改 _recBuf）──────
//   老陈反馈"识别总是不太准"。远场小声 + 空调/桌面低频轰鸣是主因：
//     ① 一阶高通切掉 80Hz 以下（人声在 300~3400Hz，切掉无损，还压低了噪声底）；
//     ② 整段抬到 -4.4dBFS —— 百度对输入端电平偏小的录音识别率明显下降。
//   放大上限 +18dB，且峰值过低（纯静音/底噪）时干脆不放大，免得把噪声一起抬起来。
void AudioIO::prepareAsr() {
#if ASR_PREPROCESS
    if (!_recBuf || _recLen < 4) return;
    int cnt = (int)(_recLen / 2);
    int16_t* s = (int16_t*)_recBuf;

    // ---- ⓿ 裁掉开头的静音（★ 2026-10-04 晚，治"前面几个字没识别"）----
    //   录音开头自带 VAD_PREROLL_MS 的静音预滚；百度 ASR 内置的 VAD 会把开头的
    //   弱起音（"打/包/发"这类塞音）当作静音一起切掉 → 前面几个字就没了。
    //   做法：找第一个"明显有声"的样本，往前留 ASR_LEAD_MS 定为新起点，
    //   前面那段静音整段丢掉（memmove 前移，_recLen 同步缩短）。
    //   这样送进百度的音频**从头就是人声**，它内部的 VAD 没有东西可切。
    {
        int pk = 0;
        for (int i = 0; i < cnt; i++) { const int a = s[i] < 0 ? -s[i] : s[i]; if (a > pk) pk = a; }
        if (pk > 300) {                            // 整段都很轻 = 本来就没说话，别裁
            const int thr0 = pk / 8;               // "有声"门 = 峰值的 1/8（刻意取低，防切掉起音）
            int i0 = -1;
            for (int i = 0; i + 1 < cnt; i++) {    // 连续 2 个样本过门才算起点（抗单点毛刺）
                if ((s[i] > thr0 || s[i] < -thr0) && (s[i + 1] > thr0 || s[i + 1] < -thr0)) { i0 = i; break; }
            }
            if (i0 > 0) {
                int keep = i0 - (int)((int32_t)SAMPLE_RATE * ASR_LEAD_MS / 1000);
                if (keep < 0) keep = 0;
                if (keep * 2 > cnt) keep = 0;      // 要裁掉一半以上 = 判断异常，放弃裁剪
                if (keep > 0) {
                    const int newCnt = cnt - keep;
                    memmove(s, s + keep, (size_t)newCnt * 2);
                    Serial.printf("[ASR] 裁掉开头静音 %dms（%dms → %dms）\n",
                                  keep * 1000 / SAMPLE_RATE,
                                  cnt * 1000 / SAMPLE_RATE, newCnt * 1000 / SAMPLE_RATE);
                    cnt = newCnt;
                    _recLen = (uint32_t)newCnt * 2;
                }
            }
        }
    }

    // ---- ① 80Hz 一阶高通 y[n] = a*(y[n-1] + x[n] - x[n-1]) ----
    const float a = 1.0f / (1.0f + 6.2831853f * ASR_HPF_FC / (float)SAMPLE_RATE);
    float y1 = 0.0f;
    int32_t x1 = 0;
    for (int i = 0; i < cnt; i++) {
        const int32_t x = s[i];
        const float y = a * (y1 + (float)(x - x1));
        x1 = x; y1 = y;
        int32_t v = (int32_t)y;
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        s[i] = (int16_t)v;
    }

    // ---- ①.5 工频陷波（50/100Hz）----
    //   实测录音静音段里 50Hz 一族比人声还强 9.4dB。一阶高通打不干净（100Hz 只 -6dB），
    //   而人声基频 141Hz 不能被砍 ⇒ 用窄带陷波只打工频那两根线（离线预演改善 17dB）。
#if ASR_NOTCH_EN
    {
        Biquad n1, n2;
        n1.init(ASR_NOTCH_F1, ASR_NOTCH_Q, (float)SAMPLE_RATE);
        n2.init(ASR_NOTCH_F2, ASR_NOTCH_Q, (float)SAMPLE_RATE);
        for (int i = 0; i < cnt; i++) {
            const float v = n2.run(n1.run((float)s[i]));
            int32_t iv = (int32_t)v;
            if (iv > 32767) iv = 32767; else if (iv < -32768) iv = -32768;
            s[i] = (int16_t)iv;
        }
    }
#endif

    // ---- ② 去直流 + 峰值归一化 ----
    int64_t sum = 0;
    for (int i = 0; i < cnt; i++) sum += s[i];
    const int mean = (int)(sum / cnt);
    int peak = 0;
    for (int i = 0; i < cnt; i++) {
        const int v = s[i] - mean;
        s[i] = (int16_t)v;
        const int av = v < 0 ? -v : v;
        if (av > peak) peak = av;
    }
    if (peak >= 200) {
        float g = (float)ASR_TARGET_PEAK / (float)peak;
        if (g > ASR_MAX_GAIN) g = ASR_MAX_GAIN;
        if (g > 1.02f) {
            for (int i = 0; i < cnt; i++) {
                int32_t v = (int32_t)(s[i] * g);
                if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                s[i] = (int16_t)v;
            }
        }
        Serial.printf("[ASR] 预处理: 高通%gHz + %s陷波 + 归一化 peak=%d gain=%.2fx (目标%d)\n",
                      (double)ASR_HPF_FC, ASR_NOTCH_EN ? "工频" : "无",
                      peak, (double)g, ASR_TARGET_PEAK);
    } else {
        Serial.printf("[ASR] 预处理: 高通%gHz（峰值仅 %d，判定静音不放大）\n",
                      (double)ASR_HPF_FC, peak);
    }
#endif
}

// 按键兜底：立刻开始录（不看阈值），静音收尾照旧
void AudioIO::listenForce() {
    if (!_listening || !_recBuf) return;
    const uint32_t cap = (uint32_t)SAMPLE_RATE * 2 * REC_MAX_SECONDS;
    _recLen = 0;
    _preDumpTo(_recBuf, _recLen, cap);   // 把刚才 300ms 也带上
    _preLen = _prePos = 0;
    int thr = (int)(_noiseFloor * VAD_MULT);
    if (thr < VAD_MIN_RMS) thr = VAD_MIN_RMS;
    _thr = thr;
    _speechFrames = _silenceFrames = 0;
    _speechMask = 0xFF;
    _minRecBytes = 0;                 // ★ 改版31：强制录音，门槛复位
    _rmsHistN = _rmsHistIdx = 0;
    _rmsAvg = 0;
    _listenState = 1;
    Serial.printf("[VAD] 按键强制录音 noise=%.0f thr=%d\n", _noiseFloor, thr);
}

void AudioIO::_prePush(const uint8_t* d, uint32_t n) {
    // ★ 防御：_pre 由 listenBegin() 分配。开机问候语等"还没开始监听"的播放
    //   也会走到这里（泵任务），没有缓冲就直接丢 —— 不许写空指针。
    if (!_pre || !_preCap) return;
    for (uint32_t i = 0; i < n; i++) {
        _pre[_prePos] = d[i];
        _prePos = (_prePos + 1) % _preCap;
        if (_preLen < _preCap) _preLen++;
    }
}

void AudioIO::_preDumpTo(uint8_t* dst, uint32_t& len, uint32_t cap) {
    len = 0;
    uint32_t n = _preLen;
    if (n > cap) n = cap;
    const uint32_t start = (_prePos + _preCap - n) % _preCap;
    for (uint32_t i = 0; i < n; i++) dst[len++] = _pre[(start + i) % _preCap];
}

int AudioIO::listenPoll() {
    if (!_listening || !_codecReady) return 0;
    const uint32_t now = millis();

    if (now < _cooldownUntil) {
        // 冷却期：不判定触发（防喇叭自激），但**照样把音频填进预滚** ——
        // 用户"它刚一说完我就接话"的前半句就在这段里，丢了就得让他重说一遍。
        size_t br = 0;
        if (i2s_read(I2S_NUM_0, _chunk, sizeof(_chunk), &br, 0) == ESP_OK && br >= 4) {
            const int n = takeLeft(_chunk, (int)(br / 2), _mono);
            if (n >= 2) _prePush((const uint8_t*)_mono, (uint32_t)n * 2);
        }
        return 0;
    }

    size_t br = 0;
    if (i2s_read(I2S_NUM_0, _chunk, sizeof(_chunk), &br,
                 pdMS_TO_TICKS(20)) != ESP_OK || br < 4)
        return 0;

    const int n = takeLeft(_chunk, (int)(br / 2), _mono);   // 抽左声道 → 单声道
    if (n < 2) return 0;
    const int rms = frameRms(_mono, n);
    if (rms > _peak) _peak = rms;
    {   // 原始样本最大绝对值（不去直流）——诊断用
        int rp = 0;
        for (int i = 0; i < n; i++) {
            const int v = _mono[i] < 0 ? -_mono[i] : _mono[i];
            if (v > rp) rp = v;
        }
        if (rp > _peakRaw) _peakRaw = rp;
    }
    // ---- RMS 滑动平均（VAD_SMOOTH_N 帧）：录音段的收尾判定用它 ----
    _rmsHist[_rmsHistIdx] = (float)rms;
    _rmsHistIdx = (uint8_t)((_rmsHistIdx + 1) % VAD_SMOOTH_N);
    if (_rmsHistN < VAD_SMOOTH_N) _rmsHistN++;
    {
        float acc = 0;
        for (uint8_t i = 0; i < _rmsHistN; i++) acc += _rmsHist[i];
        _rmsAvg = (int)(acc / _rmsHistN);
    }

    const uint32_t cap = (uint32_t)SAMPLE_RATE * 2 * REC_MAX_SECONDS;

    // ---------------- 等待说话 ----------------
    if (_listenState == 0) {
        // 噪声底：低于当前底就快速跟随，高于就极慢上升（避免被说话声顶高）
        if (rms < _noiseFloor) _noiseFloor = _noiseFloor * 0.90f + rms * 0.10f;
        else                   _noiseFloor = _noiseFloor * 0.999f + rms * 0.001f;
        if (_noiseFloor < 8.0f) _noiseFloor = 8.0f;
        if (_noiseFloor > 2000.0f) _noiseFloor = 2000.0f;

        int thr = (int)(_noiseFloor * VAD_MULT);
        if (thr < VAD_MIN_RMS) thr = VAD_MIN_RMS;

        _prePush((const uint8_t*)_mono, (uint32_t)n * 2);

        // 8 帧窗口投票：最近 8 帧超阈的帧数 ≥ VAD_ON_FRAMES 才算"开口"。
        //   比"连续 N 帧"稳：说话音节起伏不会把计数清零（治叫不应），
        //   偶发噪声也凑不满票数（治乱触发）。bit0 = 最新一帧。
        _speechMask = (uint8_t)((_speechMask << 1) | (rms > thr ? 1 : 0));
        const int onCnt = __builtin_popcount(_speechMask);
        _speechFrames = onCnt;   // 兼容旧诊断字段

        // 诊断：每秒报一次电平，能不能"听到"看这一行。
        //   peak = 去直流后的 RMS 峰值，用来和阈值比
        //   raw  = 原始样本最大绝对值（不去直流）
        //   两个都小  → 麦克风真没信号（供电/偏置/通道错）
        //   raw 大有峰 → 信号进来了，只是直流偏置吃掉了动态，是算法问题
        if (now - _lastLog >= VAD_LOG_MS) {
            // n = 本帧抽完左声道后的样本数（16k 下 256 个 ≈ 16ms）
            Serial.printf("[VAD] peak=%d raw=%d noise=%.0f thr=%d 投票=%d/%d br=%u n=%d\n",
                          _peak, _peakRaw, _noiseFloor, thr, onCnt, VAD_ON_FRAMES,
                          (unsigned)br, n);
            _peak = 0; _peakRaw = 0;
            _lastLog = now;
        }

#if VAD_AUTO_TRIGGER
        if (onCnt >= VAD_ON_FRAMES) {
            _thr = thr;
            _recLen = 0;
            _preDumpTo(_recBuf, _recLen, cap);   // 倒进预滚，不吃掉第一个字
            _preLen = _prePos = 0;
            _speechFrames = _silenceFrames = 0;
            _speechMask = 0xFF;                  // 录音中视为"持续有话音"
            _minRecBytes = 0;                    // ★ 改版31：正常触发，门槛复位
            _listenState = 1;
            _peak = 0; _peakRaw = 0;
            Serial.printf("[VAD] 开始说话 rms=%d thr=%d noise=%.0f (投票%d/%d)\n",
                          rms, thr, _noiseFloor, onCnt, VAD_ON_FRAMES);
            return 1;
        }
#endif
        return 0;
    }

    // ---------------- 录音中 ----------------
    if (_recLen < cap) {
        uint32_t take = (uint32_t)n * 2;
        if (_recLen + take > cap) take = cap - _recLen;
        memcpy(_recBuf + _recLen, _mono, take);
        _recLen += take;
    }

    // 结束判定：用 3 帧滑动平均 RMS 比 0.6 倍阈值的回差。
    //   （用原始单帧 rms 会被话音里字与字的瞬时低谷骗到，把句子提前切断）
    if (_rmsAvg < (int)(_thr * 0.6f)) _silenceFrames++;
    else                              _silenceFrames = 0;

    // 至少录够 0.5 秒才允许收尾（避免一句话被门槛切碎）；
    //   ★ 改版31：打断场景下门槛更高（`_minRecBytes` = VAD_BARGE_MIN_REC_MS 对应的字节数），
    //   治"刚开口就被判说完 ⇒ 只录到 0.8s ⇒ ASR 空"。
    uint32_t minBytes = (uint32_t)SAMPLE_RATE;             // 0.5s 兜底
    if (_minRecBytes > minBytes) minBytes = _minRecBytes;
    const bool minOk = _recLen >= minBytes;
    if ((_silenceFrames >= VAD_OFF_FRAMES && minOk) || _recLen >= cap) {
        _listenState = 0;
        _minRecBytes = 0;
        _speechFrames = _silenceFrames = 0;
        _peak = 0; _peakRaw = 0;
        _lastLog = now;
        // 量本段原始峰值：>1000 正常；几百偏低；<500 基本是噪声误触发（调用方据此判"没听清"）
        {
            const int cnt = (int)(_recLen / 2);
            int rp = 0;
            const int16_t* s = (const int16_t*)_recBuf;
            for (int i = 0; i < cnt; i++) {
                const int a = s[i] < 0 ? -s[i] : s[i];
                if (a > rp) rp = a;
            }
            _lastRawPeak = rp;
            Serial.printf("[VAD] 说完了 %u bytes (%.1fs) 本段raw峰值=%d "
                          "首8样本=%d %d %d %d %d %d %d %d\n",
                          _recLen, _recLen / (float)(SAMPLE_RATE * 2), rp,
                          s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
            if (rp >= 1000)      Serial.println("[VAD] ✓ 录音电平正常（峰值 >1000）");
            else if (rp >= 500)  Serial.println("[VAD] ~ 录音电平偏低但可用；识别不准则离麦克风近一点说");
            else                 Serial.println("[VAD] ★ 整段录音没有有效音频 → 麦克风通路问题，"
                                                "把 [AUDIO] MIC增益 那一行发我");
        }
        return 2;
    }
    return 1;
}
