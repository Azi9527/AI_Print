/**
 * ES7210 - 4-Channel ADC Codec Library for ESP32
 * Based on ESP32-S3 智能AI主板 configuration
 * 
 * Features:
 * - 4-channel ADC (MIC1-MIC4)
 * - I2S/DSP/TDM format support
 * - PGA gain control
 * - MIC bias voltage control
 */

#ifndef ES7210_H
#define ES7210_H

#include <Arduino.h>
#include <Wire.h>

// ES7210 I2C Address
#define ES7210_ADDR 0x41

// ES7210 Register Addresses
#define ES7210_RESET_CTL         0x00
#define ES7210_CLK_ON_OFF        0x01
#define ES7210_MCLK_CTL          0x02
#define ES7210_MST_CLK_CTL       0x03
#define ES7210_MST_LRCDIVH       0x04
#define ES7210_MST_LRCDIVL       0x05
#define ES7210_DIGITAL_PDN       0x06
#define ES7210_ADC_OSR           0x07
#define ES7210_MODE_CFG          0x08
#define ES7210_TCT0_CHPINI       0x09
#define ES7210_TCT1_CHPINI       0x0A
#define ES7210_CHIP_STA          0x0B
#define ES7210_MISC_CTL          0x0D
#define ES7210_DMIC_CTL          0x10
#define ES7210_SDP_CFG1          0x11
#define ES7210_SDP_CFG2          0x12
#define ES7210_ADC34_MUTE        0x14
#define ES7210_ADC12_MUTE        0x15
#define ES7210_ADC34_HPF2        0x20
#define ES7210_ADC34_HPF1        0x21
#define ES7210_ADC12_HPF2        0x22
#define ES7210_ADC12_HPF1        0x23
#define ES7210_CHIP_ID1          0x3D
#define ES7210_CHIP_ID0          0x3E
#define ES7210_CHIP_VER          0x3F
#define ES7210_ANALOG_SYS        0x40
#define ES7210_MICBIAS12         0x41
#define ES7210_MICBIAS34         0x42
#define ES7210_MIC1_GAIN         0x43
#define ES7210_MIC2_GAIN         0x44
#define ES7210_MIC3_GAIN         0x45
#define ES7210_MIC4_GAIN         0x46
#define ES7210_MIC1_LP           0x47
#define ES7210_MIC2_LP           0x48
#define ES7210_MIC3_LP           0x49
#define ES7210_MIC4_LP           0x4A
#define ES7210_MIC12_PDN         0x4B
#define ES7210_MIC34_PDN         0x4C

// MIC Bias Voltage Levels
#define ES7210_MICBIAS_1V58      0x00
#define ES7210_MICBIAS_1V66      0x50
#define ES7210_MICBIAS_1V73      0x70
#define ES7210_MICBIAS_2V87      0x70

// PGA Gain (0x00-0x1E, 0dB-30dB, 2dB/step)
#define ES7210_PGA_GAIN_0DB      0x00
#define ES7210_PGA_GAIN_10DB     0x05
#define ES7210_PGA_GAIN_16DB     0x08
#define ES7210_PGA_GAIN_20DB     0x0A
#define ES7210_PGA_GAIN_30DB     0x0F

// I2S Format
#define ES7210_FORMAT_I2S        0x00
#define ES7210_FORMAT_LJ         0x01
#define ES7210_FORMAT_DSP_A      0x03
#define ES7210_FORMAT_DSP_B      0x13

class ES7210 {
public:
    ES7210();
    ES7210(uint8_t addr);
    
    bool begin(TwoWire *wire = &Wire);
    bool begin(uint8_t sda, uint8_t scl, TwoWire *wire = &Wire);
    
    void reset();
    void init();
    
    // Power control
    void powerOn();
    void powerOff();
    void standby();
    
    // MIC control
    void setMICBias(uint8_t voltage);  // All MICs
    void setMICGain(uint8_t mic, uint8_t gain);
    void enableMIC(uint8_t mic, bool enable);
    void enableAllMICs(bool enable);
    
    // ADC control
    void enableADC(uint8_t adc, bool enable);
    void enableAllADCs(bool enable);
    void setMute(bool mute);
    
    // HPF control
    void enableHPF(bool enable);
    
    // Format
    void setFormat(uint8_t format, uint8_t bits = 16);
    
    // Read chip info
    uint8_t readChipID();
    uint8_t readVersion();
    bool isPresent();
    
    // Debug
    void printRegisters();
    
private:
    TwoWire *_wire;
    uint8_t _addr;
    
    bool writeReg(uint8_t reg, uint8_t val);
    uint8_t readReg(uint8_t reg);
};

#endif // ES7210_H
