// audio_io.h — 录音/播放/音量（ES7210 麦克风 ADC + ES8311 播放 codec）
#pragma once
#include <Arduino.h>
#include "ai_config.h"     // AEC_* 参数（回声抵消）

class AudioIO {
public:
    bool begin();
    // 录音：按住调用 recordStart()，松开 recordStop()。
    // 数据为 16kHz/16bit/单声道 PCM，缓冲在 PSRAM。
    bool recordStart();
    void recordPoll();                // 录音期间主循环高频调用
    // 返回录音字节数（未录音返回 0）
    uint32_t recordStop();
    const uint8_t* recordData() const { return _recBuf; }

    // ---- 常时监听（VAD，免按键对话）------------------------------
    // listenBegin() 后主循环只需高频调用 listenPoll()：
    //   返回 0 = 没人说话，继续等
    //   返回 1 = 检测到说话，正在录
    //   返回 2 = 一句话说完了，数据在 recordData()，长度 recordBytes()
    bool listenBegin();
    void listenEnd();
    int  listenPoll();
    // 按键触发：不等阈值，立刻开始录一个"说话窗口"（VAD 不灵时的兜底）
    void listenForce();
    // 播放结束后调用：清掉 DMA 里的扬声器余音并冷却，防止自激误识别。
    //   ★ 若播报期间被用户打断（barge-in），这里会自动改成"保持录音中"，
    //     不重置状态 —— 直接把用户正在说的那句话接着录下去。
    void listenResume();

    // ---- 打断播报（barge-in）★ 2026-10-04 晚 / 改版26 重构 ----
    // 播放期间由**独立泵任务 sAecPump()** 实时抽麦克风 → AEC → 打断判定；
    // 检测到用户说话就置位 + 立刻停播。
    //   ★ 为什么要独立任务（改版26）：TTS 边下边播，网络喂块一阵一阵，
    //     原来在 playChunk 里"顺路"读麦克风 ⇒ 读的节奏跟网络走 ⇒ RX DMA 积压
    //     忽大忽小 ⇒ 延迟 D 本身在漂 ⇒ 滤波器反复清零永远收敛不了（改版25 日志
    //     实测：D 1333→95→1161→1989→…，ERLE 全是 0/负）。
    bool bargedIn() const { return _bargeIn; }
    // 主流程取用：返回是否刚被打断，并清标志（保留"录音中"状态继续录）
    bool consumeBarge() {
        const bool b = _bargeIn;
        _bargeIn = false; _bargeFrames = 0;
        return b;
    }
    // 录音收尾后、送 ASR 之前调用：80Hz 高通 + 去直流 + 峰值归一化，
    //   就地修改 _recBuf（提升远场/小声的识别率）。受 ASR_PREPROCESS 开关控制。
    void prepareAsr();
    uint32_t recordBytes() const { return _recLen; }
    // ★ 2026-10-09：本段录音的原始峰值（不在收尾时清零）。低于 ASR_MIN_VOICE_PEAK
    //   说明是噪声误触发，调用方据此判"没听清"，不浪费一次 ASR + LLM。
    int      lastRawPeak() const { return _lastRawPeak; }
    float    noiseFloor() const { return _noiseFloor; }

    // 播放 16kHz/16bit/单声道 PCM（自动开功放，阻塞到播完）
    bool playPcm(const uint8_t* data, uint32_t len);

    // ---- 流式播放（TTS 边下边播，省去整段下载的白等时间）----
    //   playBegin() 开功放；playChunk() 只把 PCM **塞进播放队列**（不阻塞网络）；
    //   独立播放任务从队列取数据写 I2S；playEnd() 等队列排空再关功放。
    //   ★ 改版28：下载与播放解耦 —— 老陈实测 ESP32 收包 143KB/s，是播放的 4 倍多，
    //     慢的从来不是网络，是"边下边播"里 socket 被 i2s_write 阻塞。详见 ai_config.h。
    bool playBegin();
    void playChunk(const uint8_t* data, uint32_t len);
    void playEnd();

    // 播放简短提示音（固定频率哔声，无需网络）
    void beep(uint16_t freqHz, uint16_t ms);

    // ---- 音量（0~255，NVS 断电记忆）----
    void setVolume(uint8_t vol);
    uint8_t getVolume() const { return _volume; }
    void volumeUp(uint8_t step = 25);
    void volumeDown(uint8_t step = 25);
    void setMute(bool mute);
    bool isMuted() const { return _muted; }

    // 诊断：打印 ES7210/ES8311 关键寄存器精简表。
    //   开机打一次；主循环每 30 秒再打一次 —— 这样串口日志无论从哪一段
    //   截出来，都能看到麦克风的配置，不会再"刚好没截到"。
    void codecDiag();

private:
    bool _codecReady = false;
    uint8_t  _volume = 150;
    bool     _muted  = false;
    uint8_t* _recBuf = nullptr;      // PSRAM
    uint32_t _recLen = 0;
    bool     _recording = false;
    void applyVolume();

    // ---- VAD 常时监听状态 ----
    uint8_t* _pre = nullptr;         // 预滚环形缓冲（internal RAM）
    uint32_t _preCap = 0, _preLen = 0, _prePos = 0;
    int16_t  _chunk[256 * I2S_CH_NUM]; // 每次从 I2S 取的原始样本（I2S_CH_NUM 路交织，256 帧）
                                       //   I2S_CH_NUM：2=标准I2S(左/右)；4=TDM(MIC1~MIC4)
    int16_t  _mono[256];             // 抽掉右声道后的单声道样本（录音/VAD 都用它）
    int      _thr = 0;               // 本次通话锁定的判定阈值
    int      _speechFrames = 0;      // 连续超阈值帧数（诊断/兼容保留）
    int      _silenceFrames = 0;     // 连续静音帧数
    uint8_t  _speechMask = 0;        // 触发投票：最近 8 帧超阈情况（bit0=最新）
    float    _rmsHist[8] = {0};      // 收尾判定用的 RMS 滑动平均窗（用前 VAD_SMOOTH_N 个）
    uint8_t  _rmsHistN = 0;          // 滑窗已填帧数（≤VAD_SMOOTH_N）
    uint8_t  _rmsHistIdx = 0;        // 滑窗写指针
    int      _rmsAvg = 0;            // 滑窗平均 RMS（收尾判定用）
    float    _noiseFloor = 30.0f;    // 自适应噪声底（已在 listenBegin 标定）
    bool     _listening = false;
    int      _listenState = 0;       // 0=等待 1=录音中
    uint32_t _cooldownUntil = 0;
    int      _peak = 0;              // 诊断用：一秒内的峰值 RMS
    int      _peakRaw = 0;           // 诊断用：一秒内原始样本最大绝对值
                                     //   （和 _peak 一起看，就能分清"麦克风
                                     //     没信号"还是"RMS 算错"）
    int      _lastRawPeak = 0;       // ★ 本段录音的原始峰值（收尾时写入，供 lastRawPeak() 读）
    uint32_t _lastLog = 0;
    // ---- 打断播报（barge-in）状态 ----
    bool     _bargeIn = false;       // 播放中检测到用户说话
    float    _playFloor = 0;         // 播放期的"回声基线"（慢跟踪喇叭声，用户声会跳出去）
    bool     _floorInit = false;     // ★ 回声包络是否已起步（首帧直接赋值，之后快升慢降跟随回声）
    int      _bargeFrames = 0;       // 连续过门帧数
    int      _bargeRmsMax = 0;       // ★ 诊断：本报告周期内播放期最大 rms（对比门限看差多少）
    uint32_t _lastBargeLog = 0;      // ★ 诊断：上次打印播放期电平的时间
    uint32_t _playStartMs = 0;       // ★ 本次播放起始时刻（起播保护期用）
    uint32_t _minRecBytes = 0;       // ★ 改版31：本次录音的最小长度（字节）。打断后置为
                                     //   VAD_BARGE_MIN_REC_MS 对应的字节数，防止"刚开口
                                     //   就被 VAD 判说完"导致只录到 0.8s、ASR 识别为空。

    // ---- ★★ 回声抵消（AEC）★ 2026-10-04 晚 / 改版30 加硬件回采 ----
    //   参考信号两种来源（见 ai_config.h 的 AEC_REF_SRC）：
    //     0 = TX PCM（写进 I2S 的播放数据的副本，靠相关搜索对齐延迟 D）；
    //     1 = **MIC2 硬件回采**（ES8311 输出回采到 ES7210 的 MIC2，从右声道读到）。
    //        同帧采样 ⇒ 与麦克风天然对齐、D≡0、不需要搜索。
    int16_t  _ref[AEC_RING];         // 参考环形缓冲（按"播出顺序"编号）
    uint32_t _refW = 0;              // 环形写指针
    uint32_t _refTotal = 0;          // 已写参考样本数（= TX 时间轴）
    uint32_t _rxTotal = 0;           // 已读 RX 样本数（= RX 时间轴）
    int32_t  _aecD = 260;            // 参考→回声 延迟 D（样本），相关搜索得到并沿用
    float    _aW[AEC_TAPS];          // 自适应滤波器系数
    int16_t  _xref[AEC_TAPS];        // 本样本的参考窗（环形两段拼成连续）
    float    _aErrP = 0, _aEchoP = 0;// 残差/回声估计的平滑能量（双讲冻结判据）
    int16_t  _rxHist[AEC_SEARCH_WIN];// 最近 RX 样本（★ 改版30：存**去直流后**的值，
                                     //   带 DC 会把归一化相关摊薄 ⇒ 假峰满天飞）
    uint32_t _rxHistN = 0;           // 环形写指针
    uint32_t _rxHistTotal = 0;       // 已填原始样本总数
    uint32_t _lastAecSearch = 0;     // 上次重估延迟的时刻
    int      _aecErle = 0;           // 诊断：回声抑制量(dB)
    float    _aecDc = 0;             // ★ 麦克风输入的直流偏置（ES7210 有 DC，慢跟踪后减掉；
                                     //   不减的话残差底噪会虚高 → 门限虚高 → 又打不断）
    bool     _aecReady = false;      // ★ 本次播放里 AEC 是否已"确实在抵消"（ERLE≥8dB）
                                     //   没抵消成功就**不开打断判定** —— 宁可"打不断"
                                     //   也不能再回到"自己打断自己"（老陈的硬要求）
    int      _aecRawRms = 0;         // 诊断：AEC 前的麦克风 rms
    int      _aecResRms = 0;         // 诊断：AEC 后的残差 rms
    int      _pendD = -1;            // ★ 最近一次的延迟候选（诊断/兼容保留）
    int      _dHist[8] = {-1,-1,-1,-1,-1,-1,-1,-1};  // ★ 改版30 投票：最近 8 次搜索到的 D。
                                     //   攒够 3 票（±24 样本内）才改 D —— 单次搜索会偶发
                                     //   假峰，"连续两次一致"要么被打断、要么两次撞上同一个
                                     //   假峰就锁错；投票只认"反复出现在同一位置"的真峰。
    int      _dHistW = 0;            // 投票环形写指针
    // ---- ★ 播放期麦克风泵任务（改版26）----
    volatile bool     _pumpRun = false;   // 泵任务运行标志（playEnd 置 false 停泵）
    TaskHandle_t      _pumpTask = nullptr;
    volatile int      _lastTxRms = 0;     // 最近一块 TX 的 rms（诊断日志用，跨任务）
    volatile int      _lastRefRms = 0;    // ★ 改版30 诊断：最近一块**参考**的 rms。
                                          //   硬件回采模式下就是右声道(MIC2)的电平 ——
                                          //   播放时它该跟着音频大起来，否则说明回采线没接上。
    volatile int      _refMaxRms = 0;     // ★ 改版32 诊断：本次播放里右声道(MIC2)的**最大**电平，
                                          //   播放结束时打印一行结论。用途：**一次烧录就能判定
                                          //   老陈那根回采线到底接在哪**
                                          //     · 播放时涨到几千 ⇒ 其实接在 MIC2 上
                                          //       （把 AEC_REF_SRC 改成 1 立刻见效，不用动硬件）
                                          //     · 恒 <300         ⇒ 接在 MIC3(ADC3)，而
                                          //       REG0x12 bit[1:0]=00 下 SDOUT1 只带 ADC1/ADC2，
                                          //       ADC3 **根本不在我们的 I2S 流里** ⇒ 必须改线到 MIC2
    static void sAecPump(void* arg);      // 泵任务主体（PinnedToCore 0）
    void _bargeTrigger(int rms, int gate);// 打断成立：置位+预滚尾部接进录音缓冲
    void _aecPushRef(const int16_t* s, uint32_t n);
    void _aecPushRefRight(const int16_t* interleaved, int cnt);  // ★ 硬件回采：抽右声道进参考环
    int  _aecProcess(int16_t* io, int n);   // 就地换成残差；返回残差 rms
    void _aecSearchDelay();

    // ---- ★★ 播放队列 + 独立播放任务（改版28）—— 下载/播放解耦 ----
    //   生产者 = 主循环 ttsPlay（只往队列里 memcpy，队列满才短暂让出）；
    //   消费者 = sPlayTask（从队列取 → 推 AEC 参考 → 展开双声道 → i2s_write）。
    //   _pqW/_pqR 是**单调递增的字节计数**（不是下标），队列长度取低 N 位掩码，
    //   这样"满/空"判定不用区分回绕。跨核无共享（两者都在 core1），内部 RAM。
    uint8_t*          _pq = nullptr;      // 播放环形队列（内部 RAM）
    uint32_t          _pqCap = 0;         // 容量（字节，2 的幂）
    volatile uint32_t _pqW = 0;           // 已写入总字节（生产者推进）
    volatile uint32_t _pqR = 0;           // 已读出总字节（消费者推进）
    volatile bool     _playRun = false;   // 播放任务运行标志
    volatile bool     _playDone = false;  // ★ 输入已结束（playEnd 置位）：队列排空即收工
    TaskHandle_t      _playTask = nullptr;
    static void sPlayTask(void* arg);     // 播放任务主体（PinnedToCore 1）
    void _waitPlayExit(uint32_t ms);      // 等播放任务退场（带上限）
    void _playDirect(const uint8_t* data, uint32_t len);  // 队列不可用时的兜底直写

    void _prePush(const uint8_t* d, uint32_t n);
    void _preDumpTo(uint8_t* dst, uint32_t& len, uint32_t cap);
};

extern AudioIO audio;
