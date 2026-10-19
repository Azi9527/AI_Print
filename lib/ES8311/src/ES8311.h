/**
 * ES8311 - Mono Audio Codec Library for ESP32
 * Based on ESP32-S3 智能AI主板 configuration
 * 
 * Features:
 * - Mono DAC with headphone output
 * - I2S interface
 * - Volume control
 * - Low power consumption
 */

#ifndef ES8311_H
#define ES8311_H

#include <Arduino.h>
#include <Wire.h>

// ES8311 I2C Address
#define ES8311_ADDR 0x18

// ES8311 Register Addresses
#define ES8311_RESET_CTL         0x00
#define ES8311_CLK_ON_OFF        0x01
#define ES8311_CLK_DIV           0x02
#define ES8311_ADC_OSR           0x03
#define ES8311_DAC_OSR           0x04
#define ES8311_CLK_DIV2          0x05
#define ES8311_BCLK_DIV          0x06
#define ES8311_LRCK_H            0x07
#define ES8311_LRCK_L            0x08
#define ES8311_ADC_FORMAT        0x09
#define ES8311_DAC_FORMAT        0x0A
#define ES8311_ADC_MUTE          0x0B
#define ES8311_ADC_VOLUME1       0x0C
#define ES8311_ADC_VOLUME2       0x0D
#define ES8311_ADC_VOLUME3       0x0E
#define ES8311_ADC_ALC1          0x0F
#define ES8311_ADC_ALC2          0x10
#define ES8311_DAC_VOLUME        0x11
#define ES8311_DAC_CTL1          0x12
#define ES8311_DAC_CTL2          0x13
#define ES8311_HP_DRIVER         0x14
#define ES8311_HP_CAL            0x15
#define ES8311_MISC_CTL          0x1C
#define ES8311_GPIO_SEL          0x1D
#define ES8311_GPIO_CTL          0x1E
#define ES8311_MUTE_CTL          0x31
#define ES8311_VOLUME_CTL        0x32
#define ES8311_CHIP_ID           0xFD
#define ES8311_CHIP_VERSION      0xFF

// Volume range (0x00-0xFF, -95.5dB to +32dB)
#define ES8311_VOLUME_MIN        0x00
#define ES8311_VOLUME_MAX        0xFF
#define ES8311_VOLUME_MUTE       0x00

class ES8311 {
public:
    ES8311();
    ES8311(uint8_t addr);
    
    bool begin(TwoWire *wire = &Wire);
    bool begin(uint8_t sda, uint8_t scl, TwoWire *wire = &Wire);
    
    void reset();
    void init();
    
    // Power control
    void powerOn();
    void powerOff();
    
    // Volume control
    void setVolume(uint8_t volume);  // 0x00-0xFF
    void setMute(bool mute);
    uint8_t getVolume();
    
    // DAC control
    void enableDAC(bool enable);
    void enableHP(bool enable);
    
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

#endif // ES8311_H
