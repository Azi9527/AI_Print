// ota_update.h — OTA 在线升级
// 引导文件：http://www.szslgm.com/tools/updata-ai-print.json
//   {"up_url":"...bin","VERSION":"1.0.1"}
// 流程：拉引导文件 → 比版本 → 服务器更新就下载写入并自动重启；
//       不需要升级 / 检查失败 → 直接返回（正常启动，不挡开机）。
#pragma once
#include <Arduino.h>

namespace OtaUpdate {
// onProgress(百分比 0~100, 新版本号)：下载期间回调，画进度条用
void check(void (*onProgress)(int pct, const char* ver));
}
