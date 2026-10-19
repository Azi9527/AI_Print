// gfx.h — 屏幕访问互斥锁
// 表情动画跑在独立 FreeRTOS 任务里（core 0），主循环也会画屏，
// 两边同时访问 SPI 会打乱 ST7789 的命令流，所以统一加锁。
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t gfxLock;
void gfxLockInit();          // setup() 里最先调用

// 作用域锁（递归锁，允许嵌套）
struct GfxGuard {
    GfxGuard() {
        if (gfxLock) xSemaphoreTakeRecursive(gfxLock, portMAX_DELAY);
    }
    ~GfxGuard() {
        if (gfxLock) xSemaphoreGiveRecursive(gfxLock);
    }
};
