// net_diag.h — 联网诊断（查"联网异常 + 图标一直跳"是反复重启还是界面重画）
//   · 开机计数（RTC 保留，断电才清零）→ 计数狂涨 = 板子在反复重启
//   · WiFi 事件回调 → 断开 reason 码（密码错/找不到热点/信号差一眼分清）
//   · 每秒快照 → status/RSSI/断开原因/内存/电池电压
#pragma once
#include <Arduino.h>

namespace NetDiag {
void begin();          // setup() 里尽早调（装回调 + 起快照任务）
uint32_t bootCount();  // 本次上电以来的第几次启动
}
