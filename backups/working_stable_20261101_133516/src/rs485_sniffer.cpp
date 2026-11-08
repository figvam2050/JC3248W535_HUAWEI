#include "rs485_sniffer.h"

RS485Sniffer Sniffer;

uint16_t RS485Sniffer::calculateCRC(const uint8_t* buf, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)buf[pos];
        for (int i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

void RS485Sniffer::begin(HardwareSerial* serialPort) {
    _port = serialPort;
    // Passive sniffer on GPIO 44 (RX only, TX not used)
    _port->begin(9600, SERIAL_8N1, RS485_RX_PIN, -1);
    _rxLen = 0;
}

void RS485Sniffer::parseFrame(const uint8_t* frame, size_t len, BatteryPack* packs, SemaphoreHandle_t mutex) {
    if (len < 5) return;
    uint8_t addr = frame[0];
    uint8_t fc = frame[1];
    uint8_t byteCount = frame[2];

    if (fc != 0x03 || addr < 1 || addr > MAX_BATTERIES) return;
    if (len != (size_t)(byteCount + 5)) return;

    // Check CRC
    uint16_t expectedCrc = (frame[len - 1] << 8) | frame[len - 2];
    if (calculateCRC(frame, len - 2) != expectedCrc) return;

    // We have real RS485 traffic: disable demo mode!
    demoMode = false;

    // Parse registers
    size_t regCount = byteCount / 2;
    const uint8_t* data = &frame[3];
    uint16_t regs[130];
    if (regCount > 130) regCount = 130;
    for (size_t i = 0; i < regCount; i++) {
        regs[i] = (data[i * 2] << 8) | data[i * 2 + 1];
    }

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        BatteryPack& pack = packs[addr - 1];
        pack.isOnline = true;
        pack.lastSeenMs = millis();

        if (regCount > 0) pack.voltage = regs[0] / 100.0f;
        if (regCount > 1) pack.current = (int16_t)regs[1] / 100.0f;

        uint16_t minMv = 5000, maxMv = 0;
        for (int i = 0; i < CELLS_PER_BATTERY; i++) {
            if (i + 2 < (int)regCount) {
                pack.cells[i] = regs[2 + i];
                if (pack.cells[i] > 1000 && pack.cells[i] < 4500) {
                    if (pack.cells[i] < minMv) minMv = pack.cells[i];
                    if (pack.cells[i] > maxMv) maxMv = pack.cells[i];
                }
            }
        }
        pack.cellMin = (minMv == 5000) ? 0 : minMv;
        pack.cellMax = maxMv;
        pack.cellDiff = (pack.cellMax > pack.cellMin) ? (pack.cellMax - pack.cellMin) : 0;

        if (regCount > 18) pack.tempPcb = (int16_t)regs[18];
        if (regCount > 19) pack.tempAvg = (int16_t)regs[19];
        if (regCount > 20) pack.tempMax = (int16_t)regs[20];
        if (regCount > 21) pack.capacityRemaining = regs[21];
        if (regCount > 23) pack.soh = regs[23];
        if (regCount > 24) pack.soc = regs[24];

        if (regCount > 25) {
            pack.statusRaw = regs[25];
            uint8_t low = pack.statusRaw & 0xFF;
            if (low == 0x01) pack.statusName = "Charging";
            else if (low == 0x02) pack.statusName = "Discharging";
            else if (low == 0x04) pack.statusName = "Protect";
            else if (low == 0x08) pack.statusName = "Chg Limit";
            else pack.statusName = "Standby";
            if (pack.statusRaw & 0x8000) pack.statusName += " + Heat";
        }

        if (regCount > 26) pack.warningRaw = regs[26];
        if (regCount > 27) pack.protectionRaw = regs[27];
        if (regCount > 28) pack.errorRaw = regs[28];
        if (regCount > 30) pack.cycles = ((uint32_t)regs[29] << 16) | regs[30];
        if (regCount > 38) pack.balanceMask = regs[38];

        xSemaphoreGive(mutex);
    }
}

void RS485Sniffer::update(BatteryPack* packs, SemaphoreHandle_t mutex) {
    if (demoMode) {
        updateDemo(packs, mutex);
        return;
    }

    if (!_port) return;

    while (_port->available()) {
        uint8_t b = _port->read();
        if (_rxLen < sizeof(_rxBuf)) {
            _rxBuf[_rxLen++] = b;
        } else {
            _rxLen = 0; // Buffer overflow protection
        }
    }

    // Process frame if complete
    if (_rxLen >= 5) {
        size_t i = 0;
        while (i + 4 < _rxLen) {
            uint8_t addr = _rxBuf[i];
            uint8_t fc = _rxBuf[i + 1];
            uint8_t byteCount = _rxBuf[i + 2];

            if (fc == 0x03 && addr >= 1 && addr <= MAX_BATTERIES && byteCount >= 10 && byteCount <= 120 && (byteCount % 2 == 0)) {
                size_t frameLen = byteCount + 5;
                if (i + frameLen <= _rxLen) {
                    parseFrame(&_rxBuf[i], frameLen, packs, mutex);
                    i += frameLen;
                    continue;
                } else {
                    break; // Frame is still receiving
                }
            }
            i++;
        }
        if (i > 0) {
            memmove(_rxBuf, &_rxBuf[i], _rxLen - i);
            _rxLen -= i;
        }
    }

    // Check timeouts for offline state (> 6 sec silence)
    uint32_t now = millis();
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < MAX_BATTERIES; i++) {
            if (packs[i].isOnline && (now - packs[i].lastSeenMs > 6000)) {
                packs[i].isOnline = false;
                packs[i].statusName = "Offline";
            }
        }
        xSemaphoreGive(mutex);
    }
}

void RS485Sniffer::updateDemo(BatteryPack* packs, SemaphoreHandle_t mutex) {
    static uint32_t lastDemoTick = 0;
    if (millis() - lastDemoTick < 1000) return;
    lastDemoTick = millis();

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Battery 1: Healthy pack charging
        BatteryPack& b1 = packs[0];
        b1.isOnline = true;
        b1.lastSeenMs = millis();
        b1.voltage = 51.85f + (float)(random(-10, 10)) / 100.0f;
        b1.current = 15.4f + (float)(random(-5, 5)) / 10.0f;
        b1.soc = 76;
        b1.soh = 99;
        b1.tempPcb = 24;
        b1.tempAvg = 23;
        b1.tempMax = 25;
        b1.statusName = "Charging";
        b1.cycles = 42;
        b1.balanceMask = 0;

        uint16_t base1 = 3450 + random(-5, 5);
        uint16_t b1Min = 5000, b1Max = 0;
        for (int i = 0; i < CELLS_PER_BATTERY; i++) {
            b1.cells[i] = base1 + (i % 3) * 3 - (i % 2) * 2;
            if (b1.cells[i] < b1Min) b1Min = b1.cells[i];
            if (b1.cells[i] > b1Max) b1Max = b1.cells[i];
        }
        b1.cellMin = b1Min;
        b1.cellMax = b1Max;
        b1.cellDiff = b1Max - b1Min;

        // Battery 2: Pack with delta & active balancer
        BatteryPack& b2 = packs[1];
        b2.isOnline = true;
        b2.lastSeenMs = millis();
        b2.voltage = 51.72f + (float)(random(-10, 10)) / 100.0f;
        b2.current = 14.8f + (float)(random(-5, 5)) / 10.0f;
        b2.soc = 74;
        b2.soh = 97;
        b2.tempPcb = 26;
        b2.tempAvg = 25;
        b2.tempMax = 28;
        b2.statusName = "Charging";
        b2.cycles = 118;
        b2.balanceMask = (1 << 11) | (1 << 13); // Cell 12 and 14 balancing

        uint16_t base2 = 3445 + random(-4, 4);
        uint16_t b2Min = 5000, b2Max = 0;
        for (int i = 0; i < CELLS_PER_BATTERY; i++) {
            if (i == 4) b2.cells[i] = base2 - 32 + random(-2, 2);       // Lagging cell #5
            else if (i == 11) b2.cells[i] = base2 + 18 + random(-2, 2); // High cell #12 [BAL]
            else b2.cells[i] = base2 + (i % 4) * 2 + random(-1, 2);
            if (b2.cells[i] < b2Min) b2Min = b2.cells[i];
            if (b2.cells[i] > b2Max) b2Max = b2.cells[i];
        }
        b2.cellMin = b2Min;
        b2.cellMax = b2Max;
        b2.cellDiff = b2Max - b2Min; // ~50 mV delta

        // Demo periodic simulation: every 15 seconds simulate a mild warning on Pack #2
        static uint8_t warnCounter = 0;
        warnCounter++;
        if ((warnCounter % 15) >= 11) {
            b2.warningRaw = 0x0002; // Bit 1: Cell OV Warning
        } else {
            b2.warningRaw = 0;
        }

        // Battery 3: Healthy pack in float/charge
        BatteryPack& b3 = packs[2];
        b3.isOnline = true;
        b3.lastSeenMs = millis();
        b3.voltage = 51.80f + (float)(random(-8, 8)) / 100.0f;
        b3.current = 15.1f + (float)(random(-4, 4)) / 10.0f;
        b3.soc = 75;
        b3.soh = 98;
        b3.tempPcb = 25;
        b3.tempAvg = 24;
        b3.tempMax = 26;
        b3.statusName = "Charging";
        b3.cycles = 55;
        b3.balanceMask = 0;

        uint16_t base3 = 3448 + random(-4, 4);
        uint16_t b3Min = 5000, b3Max = 0;
        for (int i = 0; i < CELLS_PER_BATTERY; i++) {
            b3.cells[i] = base3 + (i % 3) * 2;
            if (b3.cells[i] < b3Min) b3Min = b3.cells[i];
            if (b3.cells[i] > b3Max) b3Max = b3.cells[i];
        }
        b3.cellMin = b3Min;
        b3.cellMax = b3Max;
        b3.cellDiff = b3Max - b3Min;
        b3.warningRaw = 0;

        // Battery 4: Healthy pack in charge
        BatteryPack& b4 = packs[3];
        b4.isOnline = true;
        b4.lastSeenMs = millis();
        b4.voltage = 51.88f + (float)(random(-8, 8)) / 100.0f;
        b4.current = 15.6f + (float)(random(-4, 4)) / 10.0f;
        b4.soc = 77;
        b4.soh = 100;
        b4.tempPcb = 24;
        b4.tempAvg = 23;
        b4.tempMax = 25;
        b4.statusName = "Charging";
        b4.cycles = 28;
        b4.balanceMask = 0;

        uint16_t base4 = 3452 + random(-4, 4);
        uint16_t b4Min = 5000, b4Max = 0;
        for (int i = 0; i < CELLS_PER_BATTERY; i++) {
            b4.cells[i] = base4 + (i % 2) * 3;
            if (b4.cells[i] < b4Min) b4Min = b4.cells[i];
            if (b4.cells[i] > b4Max) b4Max = b4.cells[i];
        }
        b4.cellMin = b4Min;
        b4.cellMax = b4Max;
        b4.cellDiff = b4Max - b4Min;
        b4.warningRaw = 0;

        xSemaphoreGive(mutex);
    }
}

