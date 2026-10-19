/**
 * ES7210 - 4-Channel ADC Codec Library for ESP32
 * Implementation based on ESP32-S3 智能AI主板
 */

#include "ES7210.h"

ES7210::ES7210() : _wire(&Wire), _addr(ES7210_ADDR) {}

ES7210::ES7210(uint8_t addr) : _wire(&Wire), _addr(addr) {}

bool ES7210::begin(TwoWire *wire) {
    _wire = wire;
    return isPresent();
}

bool ES7210::begin(uint8_t sda, uint8_t scl, TwoWire *wire) {
    _wire = wire;
    _wire->begin(sda, scl);
    delay(10);
    return isPresent();
}

bool ES7210::writeReg(uint8_t reg, uint8_t val) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(val);
    uint8_t err = _wire->endTransmission();
    delay(3);  // ES7210 requires delay
    return (err == 0);
}

uint8_t ES7210::readReg(uint8_t reg) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->endTransmission(true);
    delay(2);
    
    if (_wire->requestFrom(_addr, (uint8_t)1) == 1 && _wire->available()) {
        return _wire->read();
    }
    return 0xFF;
}

bool ES7210::isPresent() {
    _wire->beginTransmission(_addr);
    return (_wire->endTransmission() == 0);
}

void ES7210::reset() {
    writeReg(ES7210_RESET_CTL, 0xFF);
    delay(10);
    writeReg(ES7210_RESET_CTL, 0x32);
    delay(10);
}

void ES7210::init() {
    Serial.println("[ES7210] Init...");
    
    // Reset
    reset();
    
    // I2S format
    writeReg(ES7210_MST_CLK_CTL, 0x04);
    writeReg(ES7210_MST_LRCDIVH, 0x02);
    delay(5);
    
    // PGA gain - MIC1/MIC3 enabled, MIC2/MIC4 off
    writeReg(ES7210_MIC1_GAIN, 0x10);  // 16dB
    writeReg(ES7210_MIC2_GAIN, 0x00);  // off
    writeReg(ES7210_MIC3_GAIN, 0x10);  // 16dB
    writeReg(ES7210_MIC4_GAIN, 0x00);  // off
    delay(5);
    
    // Preamplifier
    writeReg(ES7210_MIC1_LP, 0x08);
    writeReg(ES7210_MIC2_LP, 0x00);
    writeReg(ES7210_MIC3_LP, 0x08);
    writeReg(ES7210_MIC4_LP, 0x00);
    delay(5);
    
    // MIC bias voltage (2.87V)
    writeReg(ES7210_MICBIAS12, 0x70);
    writeReg(ES7210_MICBIAS34, 0x70);
    delay(5);
    
    // ADC enable
    writeReg(0x60, 0x02);  // ADC1
    writeReg(0x61, 0x00);  // ADC2 off
    delay(5);
    
    // HPF enable
    writeReg(ES7210_ADC34_HPF2, 0x06);
    writeReg(ES7210_ADC34_HPF1, 0x06);
    delay(5);
    
    // Power on sequence
    powerOn();
    
    Serial.println("[ES7210] Init done!");
}

void ES7210::powerOn() {
    // Three-stage power on with retry
    writeReg(ES7210_ANALOG_SYS, 0x00);
    delay(50);
    
    writeReg(ES7210_ANALOG_SYS, 0x03);  // VREF only
    delay(80);
    
    writeReg(ES7210_ANALOG_SYS, 0xC3);  // VREF + VMID + MICBIAS
    delay(100);
    
    writeReg(ES7210_MIC12_PDN, 0x0F);
    writeReg(ES7210_MIC34_PDN, 0x0F);
    delay(100);
    
    // Verify
    uint8_t v40 = readReg(ES7210_ANALOG_SYS);
    uint8_t v4B = readReg(ES7210_MIC12_PDN);
    uint8_t v4C = readReg(ES7210_MIC34_PDN);
    
    if (v40 != 0xC3 || v4B != 0x0F || v4C != 0x0F) {
        Serial.println("[ES7210] Retry power on...");
        writeReg(ES7210_ANALOG_SYS, 0x00);
        delay(100);
        writeReg(ES7210_ANALOG_SYS, 0xC3);
        delay(100);
        writeReg(ES7210_MIC12_PDN, 0x0F);
        writeReg(ES7210_MIC34_PDN, 0x0F);
        delay(100);
    }
    
    // Start
    writeReg(ES7210_RESET_CTL, 0x71);
    delay(10);
    writeReg(ES7210_RESET_CTL, 0x41);
    delay(20);
}

void ES7210::powerOff() {
    writeReg(ES7210_ANALOG_SYS, 0x00);
    writeReg(ES7210_MIC12_PDN, 0x00);
    writeReg(ES7210_MIC34_PDN, 0x00);
    writeReg(ES7210_RESET_CTL, 0xFF);
}

void ES7210::standby() {
    writeReg(ES7210_RESET_CTL, 0x71);
}

void ES7210::setMICBias(uint8_t voltage) {
    writeReg(ES7210_MICBIAS12, voltage);
    writeReg(ES7210_MICBIAS34, voltage);
}

void ES7210::setMICGain(uint8_t mic, uint8_t gain) {
    uint8_t reg;
    switch (mic) {
        case 1: reg = ES7210_MIC1_GAIN; break;
        case 2: reg = ES7210_MIC2_GAIN; break;
        case 3: reg = ES7210_MIC3_GAIN; break;
        case 4: reg = ES7210_MIC4_GAIN; break;
        default: return;
    }
    writeReg(reg, gain & 0x1F);
}

void ES7210::enableMIC(uint8_t mic, bool enable) {
    uint8_t regLP, regPDN;
    switch (mic) {
        case 1: regLP = ES7210_MIC1_LP; regPDN = ES7210_MIC12_PDN; break;
        case 2: regLP = ES7210_MIC2_LP; regPDN = ES7210_MIC12_PDN; break;
        case 3: regLP = ES7210_MIC3_LP; regPDN = ES7210_MIC34_PDN; break;
        case 4: regLP = ES7210_MIC4_LP; regPDN = ES7210_MIC34_PDN; break;
        default: return;
    }
    writeReg(regLP, enable ? 0x08 : 0x00);
}

void ES7210::enableAllMICs(bool enable) {
    for (int i = 1; i <= 4; i++) {
        enableMIC(i, enable);
    }
}

void ES7210::enableADC(uint8_t adc, bool enable) {
    uint8_t reg = 0x60 + adc - 1;
    writeReg(reg, enable ? 0x02 : 0x00);
}

void ES7210::enableAllADCs(bool enable) {
    for (int i = 1; i <= 4; i++) {
        enableADC(i, enable);
    }
}

void ES7210::setMute(bool mute) {
    uint8_t val = mute ? 0x3F : 0x3C;
    writeReg(ES7210_ADC34_MUTE, val);
    writeReg(ES7210_ADC12_MUTE, val);
}

void ES7210::enableHPF(bool enable) {
    uint8_t val = enable ? 0x06 : 0x00;
    writeReg(ES7210_ADC34_HPF2, val);
    writeReg(ES7210_ADC34_HPF1, val);
    writeReg(ES7210_ADC12_HPF2, val);
    writeReg(ES7210_ADC12_HPF1, val);
}

void ES7210::setFormat(uint8_t format, uint8_t bits) {
    uint8_t val = format;
    if (bits == 16) val |= 0x40;
    else if (bits == 24) val |= 0x00;
    else if (bits == 32) val |= 0x80;
    writeReg(ES7210_SDP_CFG1, val);
}

uint8_t ES7210::readChipID() {
    return readReg(ES7210_CHIP_ID1);
}

uint8_t ES7210::readVersion() {
    return readReg(ES7210_CHIP_VER);
}

void ES7210::printRegisters() {
    Serial.println("[ES7210] Registers:");
    Serial.printf("  RESET=0x%02X\n", readReg(ES7210_RESET_CTL));
    Serial.printf("  ANALOG_SYS=0x%02X\n", readReg(ES7210_ANALOG_SYS));
    Serial.printf("  MICBIAS12=0x%02X\n", readReg(ES7210_MICBIAS12));
    Serial.printf("  MIC1_GAIN=0x%02X\n", readReg(ES7210_MIC1_GAIN));
    Serial.printf("  MIC12_PDN=0x%02X\n", readReg(ES7210_MIC12_PDN));
    Serial.printf("  CHIP_ID=0x%02X 0x%02X\n", readReg(ES7210_CHIP_ID1), readReg(ES7210_CHIP_ID0));
}
