#ifndef BMS_DATA_H
#define BMS_DATA_H

#include <Arduino.h>

#define MAX_BATTERIES 16
#define CELLS_PER_BATTERY 15

struct BatteryPack {
    bool isOnline = false;
    uint32_t lastSeenMs = 0;
    
    float voltage = 0.0f;          // Pack voltage (V)
    float current = 0.0f;          // Pack current (+ Charge, - Discharge)
    uint16_t cells[CELLS_PER_BATTERY] = {0}; // Cell voltages in mV
    uint16_t cellMin = 0;          // Min cell mV
    uint16_t cellMax = 0;          // Max cell mV
    uint16_t cellDiff = 0;         // Delta (Max - Min) mV
    
    int16_t tempPcb = 0;           // PCB temp °C
    int16_t tempAvg = 0;           // Avg temp °C
    int16_t tempMax = 0;           // Max temp °C
    
    uint16_t capacityRemaining = 0;// Ah
    uint8_t soh = 0;               // State of Health %
    uint8_t soc = 0;               // BMS Reported SoC %
    float adaptiveSoc = 0.0f;      // OCV-calculated SoC %
    
    uint16_t statusRaw = 0;
    String statusName = "Offline";
    
    uint16_t warningRaw = 0;
    uint16_t protectionRaw = 0;
    uint16_t errorRaw = 0;
    uint32_t balanceMask = 0;      // Bit 0..14 active balancing
    uint32_t cycles = 0;
};

struct BankSummary {
    float totalVoltage = 0.0f;
    float totalCurrent = 0.0f;
    float totalPowerKw = 0.0f;
    float avgSoc = 0.0f;
    uint16_t globalMinCell = 0;
    uint16_t globalMaxCell = 0;
    uint16_t globalDeltaMv = 0;
    uint8_t onlineCount = 0;
    bool hasAlarms = false;
};

#endif // BMS_DATA_H

