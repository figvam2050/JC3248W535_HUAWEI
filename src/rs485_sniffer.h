#ifndef RS485_SNIFFER_H
#define RS485_SNIFFER_H

#include "bms_data.h"

// JST 1.25 Power/UART connector (P1)
#define RS485_RX_PIN 44
#define RS485_TX_PIN 43 // Kept disabled for pure passive sniffer mode

class RS485Sniffer {
public:
    void begin(HardwareSerial* serialPort = &Serial2);
    void update(BatteryPack* packs, SemaphoreHandle_t mutex);
    
    bool demoMode = true; // Enabled by default until hardware RS485 signal arrives
    void updateDemo(BatteryPack* packs, SemaphoreHandle_t mutex);

private:
    HardwareSerial* _port = nullptr;
    uint8_t _rxBuf[512];
    size_t _rxLen = 0;
    
    uint16_t calculateCRC(const uint8_t* buf, size_t len);
    void parseFrame(const uint8_t* frame, size_t len, BatteryPack* packs, SemaphoreHandle_t mutex);
};

extern RS485Sniffer Sniffer;

#endif // RS485_SNIFFER_H

