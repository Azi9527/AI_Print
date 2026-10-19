/**
 * ES8311 - Mono Audio Codec Library for ESP32
 * Implementation based on ESP32-S3 智能AI主板
 */

#include "ES8311.h"

ES8311::ES8311() : _wire(&Wire), _addr(ES8311_ADDR) {}

ES8311::ES8311(uint8_t addr) : _wire(&Wire), _addr(addr) {}

bool ES8311::begin(TwoWire *wire) {
    _wire = wire;
    return isPresent();
}

bool ES8311::begin(uint8_t sda, uint8_t scl, TwoWire *wire) {
    _wire = wire;
    _wire->begin(sda, scl);
    delay(10);
    return isPresent();
}

bool ES8311::writeReg(uint8_t reg, uint8_t val) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(val);
    return (_wire->endTransmission() == 0);
}

uint8_t ES8311::readReg(uint8_t reg) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->endTransmission(false);
    
    if (_wire->requestFrom(_addr, (uint8_t)1) == 1 && _wire->available()) {
        return _wire->read();
    }
    return 0xFF;
}

bool ES8311::isPresent() {
    _wire->beginTransmission(_addr);
    return (_wire->endTransmission() == 0);
}

void ES8311::reset() {
    writeReg(ES8311_RESET_CTL, 0x1F);
    delay(20);
    writeReg(ES8311_RESET_CTL, 0x02);
    delay(20);
    writeReg(ES8311_RESET_CTL, 0x82);
    delay(20);
}

void ES8311::init() {
    Serial.println("[ES8311] Init...");
    
    // Reset
    reset();
    
    // Clock enable
    writeReg(ES8311_CLK_ON_OFF, 0x35);
    
    // Clock divider
    writeReg(ES8311_CLK_DIV, 0x00);
    writeReg(ES8311_ADC_OSR, 0x10);
    writeReg(ES8311_DAC_OSR, 0x10);
    writeReg(ES8311_CLK_DIV2, 0x00);
    writeReg(ES8311_BCLK_DIV, 0x03);
    writeReg(ES8311_LRCK_H, 0x01);
    writeReg(ES8311_LRCK_L, 0x7B);
    
    // I2S format
    writeReg(ES8311_ADC_FORMAT, 0x0C);  // I2S, 16-bit
    writeReg(ES8311_DAC_FORMAT, 0x0C);  // I2S, 16-bit
    
    // Analog power
    writeReg(ES8311_ADC_VOLUME3, 0x31);
    writeReg(ES8311_ADC_ALC2, 0x6A);
    writeReg(ES8311_ADC_MUTE, 0x00);
    writeReg(ES8311_ADC_VOLUME1, 0x00);
    writeReg(ES8311_ADC_ALC1, 0x1F);  // ADC analog gain max
    writeReg(ES8311_DAC_VOLUME, 0xEF);  // DAC digital volume
    writeReg(ES8311_HP_DRIVER, 0x1F);   // HP left
    writeReg(ES8311_HP_CAL, 0x1F);      // HP right
    
    // DAC output config
    writeReg(ES8311_DAC_CTL1, 0xC1);  // DAC enable + analog power
    writeReg(ES8311_DAC_CTL2, 0x10);  // OUT1 output enable
    
    // Volume and mute
    writeReg(ES8311_MUTE_CTL, 0x00);   // Unmute
    writeReg(ES8311_VOLUME_CTL, 0xDF); // Digital volume
    writeReg(0x37, 0x08);
    
    // Verify
    uint8_t chipID = readReg(ES8311_CHIP_ID);
    uint8_t version = readReg(ES8311_CHIP_VERSION);
    Serial.printf("[ES8311] ChipID=0x%02X, Version=0x%02X\n", chipID, version);
    Serial.println("[ES8311] Init done!");
}

void ES8311::powerOn() {
    writeReg(ES8311_RESET_CTL, 0x82);
    delay(10);
    writeReg(ES8311_CLK_ON_OFF, 0x35);
    enableDAC(true);
    enableHP(true);
}

void ES8311::powerOff() {
    enableHP(false);
    enableDAC(false);
    writeReg(ES8311_CLK_ON_OFF, 0x00);
    writeReg(ES8311_RESET_CTL, 0x1F);
}

void ES8311::setVolume(uint8_t volume) {
    writeReg(ES8311_VOLUME_CTL, volume);
}

void ES8311::setMute(bool mute) {
    writeReg(ES8311_MUTE_CTL, mute ? 0x03 : 0x00);
}

uint8_t ES8311::getVolume() {
    return readReg(ES8311_VOLUME_CTL);
}

void ES8311::enableDAC(bool enable) {
    uint8_t val = enable ? 0xC1 : 0x00;
    writeReg(ES8311_DAC_CTL1, val);
}

void ES8311::enableHP(bool enable) {
    uint8_t val = enable ? 0x1F : 0x00;
    writeReg(ES8311_HP_DRIVER, val);
    writeReg(ES8311_HP_CAL, val);
}

void ES8311::setFormat(uint8_t format, uint8_t bits) {
    uint8_t val = format;
    if (bits == 16) val |= 0x0C;
    else if (bits == 24) val |= 0x04;
    else if (bits == 32) val |= 0x08;
    writeReg(ES8311_ADC_FORMAT, val);
    writeReg(ES8311_DAC_FORMAT, val);
}

uint8_t ES8311::readChipID() {
    return readReg(ES8311_CHIP_ID);
}

uint8_t ES8311::readVersion() {
    return readReg(ES8311_CHIP_VERSION);
}

void ES8311::printRegisters() {
    Serial.println("[ES8311] Registers:");
    Serial.printf("  RESET=0x%02X\n", readReg(ES8311_RESET_CTL));
    Serial.printf("  CLK_ON_OFF=0x%02X\n", readReg(ES8311_CLK_ON_OFF));
    Serial.printf("  DAC_CTL1=0x%02X\n", readReg(ES8311_DAC_CTL1));
    Serial.printf("  VOLUME=0x%02X\n", readReg(ES8311_VOLUME_CTL));
    Serial.printf("  CHIP_ID=0x%02X\n", readReg(ES8311_CHIP_ID));
}
