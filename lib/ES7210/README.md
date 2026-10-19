# ES7210 + ES8311 Audio Codec Libraries for ESP32

基于 ESP32-S3 智能AI主板的音频编解码器库。

## ES7210 - 4通道ADC

### 功能
- 4通道ADC (MIC1-MIC4)
- I2S/DSP/TDM 格式支持
- PGA增益控制 (0-30dB)
- MIC偏置电压控制
- 高通滤波器

### 使用示例

```cpp
#include "ES7210.h"

ES7210 adc;

void setup() {
    Wire.begin(1, 2);  // SDA, SCL
    
    if (adc.begin(&Wire)) {
        adc.init();
        adc.setMICGain(1, ES7210_PGA_GAIN_16DB);
        adc.enableMIC(1, true);
    }
}
```

### API

| 方法 | 说明 |
|------|------|
| `begin()` | 初始化I2C |
| `init()` | 初始化芯片 |
| `setMICGain(mic, gain)` | 设置MIC增益 |
| `enableMIC(mic, enable)` | 使能MIC |
| `setMICBias(voltage)` | 设置MIC偏置电压 |
| `setMute(mute)` | 静音控制 |
| `enableHPF(enable)` | 高通滤波器 |

---

## ES8311 - 单声道DAC

### 功能
- 单声道DAC带耳机输出
- I2S接口
- 音量控制
- 低功耗

### 使用示例

```cpp
#include "ES8311.h"

ES8311 dac;

void setup() {
    Wire.begin(1, 2);
    
    if (dac.begin(&Wire)) {
        dac.init();
        dac.setVolume(0xDF);
        dac.enableDAC(true);
        dac.enableHP(true);
    }
}
```

### API

| 方法 | 说明 |
|------|------|
| `begin()` | 初始化I2C |
| `init()` | 初始化芯片 |
| `setVolume(volume)` | 设置音量 (0x00-0xFF) |
| `setMute(mute)` | 静音控制 |
| `enableDAC(enable)` | 使能DAC |
| `enableHP(enable)` | 使能耳机输出 |

---

## 引脚定义 (ESP32-S3 智能AI主板)

| 功能 | GPIO |
|------|------|
| I2C SDA | 1 |
| I2C SCL | 2 |
| I2S MCK | 38 |
| I2S BCK | 14 |
| I2S WS | 13 |
| I2S DOUT | 45 |
| I2S DIN | 12 |
| PA Enable | 46 |
| Codec Power | 44 |

## 参考

- ES7210 I2C地址: 0x41
- ES8311 I2C地址: 0x18
- 采样率: 48kHz
- MCLK: 12.288MHz
