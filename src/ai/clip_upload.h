#pragma once
#include <Arduino.h>

// ================================================================
// 录音 / 回复音频上传（控制台比对识别率用）
//   用户录音（ASR 前那段 PCM）+ 识别文字、阿蓝回复的 PCM + 回复文字，
//   都 POST 到云端 api.php?action=clip，控制台按"轮次"配对播放。
//   ★ 全程走后台上传任务，不阻塞语音主流程；失败只打日志、直接丢。
// ================================================================
namespace ClipUp {

// 建队列 + 上传任务（setup 里调一次，联网成功后调）
void begin();

// 开启新一轮对话，返回轮次号（同轮的"用户录音/回复音频"共用一个号，云端靠它配对）
uint32_t newTurn();
uint32_t curTurn();

// 上传用户这句话的原始录音（内部拷一份，调用方 buffer 可立即复用）
//   text = 识别出来的文字（没识别出来传 "(ASR 未识别)" 之类），peak = 原始峰值
bool enqueueUser(const uint8_t* pcm, uint32_t len, const String& text, int peak);

// 同上传录音，但 pcm 的所有权交接给本模块（调用方不要再 free）
//   用途：prepareAsr() 会就地改录音缓冲，想留"原声"必须提前拷一份出来
bool enqueueUserOwned(uint8_t* pcm, uint32_t len, const String& text, int peak);

// ---- 回复音频：先攒后传（TTS 是边下边播，所以播出时顺手镜像一份）----
bool capBegin();                                   // 开缓冲（PSRAM）
void capFeed(const uint8_t* data, uint32_t len);   // 播出的每块喂进来
void capAbort();                                   // 放弃这段（被打断/失败）
bool capEndAndUpload(const String& text);          // 攒好了 → 交给上传任务

int  pending();          // 排队中条数（诊断）
}
