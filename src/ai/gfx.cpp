#include "gfx.h"

SemaphoreHandle_t gfxLock = nullptr;

void gfxLockInit() {
    if (!gfxLock) gfxLock = xSemaphoreCreateRecursiveMutex();
}
