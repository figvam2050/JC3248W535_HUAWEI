#ifndef AUDIO_HAL_H
#define AUDIO_HAL_H

#include <Arduino.h>
#include <driver/i2s.h>

// Audio pins for NS4168 I2S amplifier on JC3248W535
#define I2S_BCK_PIN   42
#define I2S_WS_PIN    2
#define I2S_DATA_PIN  41

class AudioAlert {
public:
    void begin();
    void playClick();
    void playAlarm();
    void playBeep(uint32_t freq, uint32_t durationMs);
    void playTestMelody();
};

extern AudioAlert BmsAudio;

#endif
