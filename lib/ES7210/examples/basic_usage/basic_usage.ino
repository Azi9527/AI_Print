/**
 * ES7210 + ES8311 Basic Usage Example
 * 
 * This example shows how to use ES7210 and ES8311 libraries
 * for audio recording and playback on ESP32-S3.
 */

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include "ES7210.h"
#include "ES8311.h"

// Pin definitions (ESP32-S3 智能AI主板)
#define PIN_SDA      1
#define PIN_SCL      2
#define PIN_MCK      38
#define PIN_BCK      14
#define PIN_WS       13
#define PIN_DOUT     45
#define PIN_DIN      12
#define PIN_PA       46    // Amplifier enable
#define PIN_3V3      44    // Audio codec power

#define SAMPLE_RATE  48000
#define BUFFER_SIZE  1024

ES7210 adc;
ES8311 dac;

void setupI2S() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    cfg.sample_rate = SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 512;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;

    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);

    i2s_pin_config_t pins = {};
    pins.mck_io_num = PIN_MCK;
    pins.bck_io_num = PIN_BCK;
    pins.ws_io_num  = PIN_WS;
    pins.data_out_num = PIN_DOUT;
    pins.data_in_num  = PIN_DIN;

    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_start(I2S_NUM_0);
    Serial.println("[I2S] Full-duplex started");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ES7210 + ES8311 Example ===");

    // Power on audio codecs
    pinMode(PIN_3V3, OUTPUT);
    digitalWrite(PIN_3V3, HIGH);
    delay(100);

    // Initialize I2C
    Wire.begin(PIN_SDA, PIN_SCL);
    delay(10);

    // Initialize I2S
    setupI2S();
    delay(50);

    // Initialize ES7210 (ADC)
    if (adc.begin(&Wire)) {
        Serial.println("[ES7210] Found!");
        adc.init();
        adc.printRegisters();
    } else {
        Serial.println("[ES7210] Not found!");
    }

    // Initialize ES8311 (DAC)
    if (dac.begin(&Wire)) {
        Serial.println("[ES8311] Found!");
        dac.init();
        dac.setVolume(0xDF);
        dac.printRegisters();
    } else {
        Serial.println("[ES8311] Not found!");
    }

    // Enable amplifier
    pinMode(PIN_PA, OUTPUT);
    digitalWrite(PIN_PA, HIGH);
    delay(50);

    Serial.println("Ready! Sending audio loop...");
}

void loop() {
    // Simple audio loopback (ADC -> DAC)
    int32_t buffer[BUFFER_SIZE];
    size_t bytesRead, bytesWritten;
    
    // Read from ADC (ES7210)
    i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytesRead, 100);
    
    if (bytesRead > 0) {
        // Write to DAC (ES8311)
        i2s_write(I2S_NUM_0, buffer, bytesRead, &bytesWritten, 100);
    }
}
