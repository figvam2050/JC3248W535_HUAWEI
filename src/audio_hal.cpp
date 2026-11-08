#include "audio_hal.h"
#include <math.h>

AudioAlert BmsAudio;

static bool s_i2s_initialized = false;

void AudioAlert::begin() {
    if (s_i2s_initialized) return;

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err == ESP_OK) {
        i2s_set_pin(I2S_NUM_0, &pin_config);
        s_i2s_initialized = true;
        Serial.println("[Audio] NS4168 I2S driver initialized successfully");
    } else {
        Serial.printf("[Audio] Failed to install I2S driver: %d\n", err);
    }
}

void AudioAlert::playBeep(uint32_t freq, uint32_t durationMs) {
    if (!s_i2s_initialized) return;

    const uint32_t sample_rate = 16000;
    size_t total_samples = (sample_rate * durationMs) / 1000;
    int16_t buffer[256]; // Stereo: Left + Right interleaved

    size_t samples_left = total_samples;
    uint32_t sample_index = 0;

    while (samples_left > 0) {
        size_t chunk_samples = (samples_left > 128) ? 128 : samples_left;
        for (size_t i = 0; i < chunk_samples; i++) {
            float t = (float)(sample_index + i) / (float)sample_rate;
            int16_t sample = (int16_t)(sinf(2.0f * M_PI * freq * t) * 12000.0f);
            buffer[i * 2] = sample;     // L
            buffer[i * 2 + 1] = sample; // R
        }

        size_t bytes_written = 0;
        i2s_write(I2S_NUM_0, buffer, chunk_samples * 4, &bytes_written, portMAX_DELAY);

        sample_index += chunk_samples;
        samples_left -= chunk_samples;
    }

    // Flush silence to avoid pop sound
    memset(buffer, 0, sizeof(buffer));
    size_t bytes_written = 0;
    i2s_write(I2S_NUM_0, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
}

void AudioAlert::playClick() {
    playBeep(2400, 20); // Fast, tactile 20ms click at 2.4kHz
}

void AudioAlert::playAlarm() {
    // 2-tone urgent siren
    for (int k = 0; k < 2; k++) {
        playBeep(880, 100);
        delay(30);
        playBeep(1760, 150);
        delay(40);
    }
}

void AudioAlert::playTestMelody() {
    // Elegant ascending polyphonic chime (C5 -> E5 -> G5 -> C6)
    playBeep(523, 70);
    delay(20);
    playBeep(659, 70);
    delay(20);
    playBeep(784, 80);
    delay(20);
    playBeep(1046, 150);
}

