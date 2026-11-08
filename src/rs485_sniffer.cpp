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

static int huaweiAddrToIndex(uint8_t addr) {
    // Primary addresses: 0xD6 -> 0, 0xD7 -> 1, ... up to 0xDD -> 7
    if (addr >= 0xD6 && addr <= 0xDD) {
        return addr - 0xD6;
    }
    // Extended pool: 0xE0 -> 8, ... up to 0xE7 -> 15
    if (addr >= 0xE0 && addr <= 0xE7) {
        return 8 + (addr - 0xE0);
    }
    // Standard addresses 1..16 -> 0..15
    if (addr >= 1 && addr <= MAX_BATTERIES) {
        return addr - 1;
    }
    return -1;
}

static bool isValidBmsAddr(uint8_t addr) {
    return huaweiAddrToIndex(addr) >= 0;
}

void RS485Sniffer::parseFrame(const uint8_t* frame, size_t len, BatteryPack* packs, SemaphoreHandle_t mutex) {
    if (len < 5) return;
    uint8_t addr = frame[0];
    uint8_t fc = frame[1];
    uint8_t byteCount = frame[2];

    int packIdx = huaweiAddrToIndex(addr);
    if (fc != 0x03 || packIdx < 0 || packIdx >= MAX_BATTERIES) return;
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

    bool isHuaweiAddr = (addr >= 0xD6 && addr <= 0xDD) || (addr >= 0xE0 && addr <= 0xE7);

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        BatteryPack& pack = packs[packIdx];
        pack.isOnline = true;
        pack.lastSeenMs = millis();

        if (isHuaweiAddr) {
            pack.isHuawei = true;
            // Huawei ESM protocol: distinguished by byteCount (registers count)
            if (byteCount == 14) {
                // HW_Q_BASE: 0x0000 x 7 registers
                // 0: Bus Voltage (x0.01 V)
                // 1: Pack Voltage (x0.01 V)
                // 2: Current (signed int16, x0.01 A, + Charge, - Discharge)
                // 3: SOC (%)
                // 4: SOH (%)
                // 5: T max (°C)
                // 6: T min (°C)
                pack.voltage = regs[1] * 0.01f;
                pack.current = (int16_t)regs[2] * 0.01f;
                pack.soc = (uint8_t)regs[3];
                pack.soh = (uint8_t)regs[4];
                pack.tempMax = (int16_t)regs[5];
                pack.tempAvg = ((int16_t)regs[5] + (int16_t)regs[6]) / 2;
                pack.tempPcb = (int16_t)regs[5];

                if (pack.current > 0.5f) pack.statusName = "Charging";
                else if (pack.current < -0.5f) pack.statusName = "Discharging";
                else pack.statusName = "Standby";
            }
            else if (byteCount == 30) {
                // HW_Q_CELL: 0x0022 x 15 registers (Cell voltages in mV)
                uint16_t minMv = 5000, maxMv = 0;
                for (int i = 0; i < CELLS_PER_BATTERY; i++) {
                    pack.cells[i] = regs[i];
                    if (pack.cells[i] > 1000 && pack.cells[i] < 4500) {
                        if (pack.cells[i] < minMv) minMv = pack.cells[i];
                        if (pack.cells[i] > maxMv) maxMv = pack.cells[i];
                    }
                }
                pack.cellMin = (minMv == 5000) ? 0 : minMv;
                pack.cellMax = maxMv;
                pack.cellDiff = (pack.cellMax > pack.cellMin) ? (pack.cellMax - pack.cellMin) : 0;
            }
            else if (byteCount == 10) {
                // HW_Q_ALARM: 0x0046 x 5 registers (0x0046..0x004A)
                // [0]: 0x0046 sysf (system fault)
                // [1]: 0x0047 reserved/detail
                // [2]: 0x0048 protection
                // [3]: 0x0049 warning
                // [4]: 0x004A module status
                for (int i = 0; i < 5; i++) {
                    pack.huaweiAlarm[i] = regs[i];
                }
                pack.errorRaw = regs[0];       // sysf
                pack.protectionRaw = regs[2];  // prot
                pack.warningRaw = regs[3];     // warn
                pack.statusRaw = regs[4];      // modst

                uint16_t modst = regs[4];
                if (regs[2] != 0 || (regs[0] & 0x7E)) {
                    pack.statusName = "Protect";
                } else if (modst & 0x0001) {
                    pack.statusName = "Shutdown";
                }
            }
            else if (byteCount == 2) {
                // HW_Q_STATE: 0x000A x 1 (0: Charge, 1: Discharge, 2: Full, 3: Standby/Free)
                uint16_t st = regs[0];
                if (st == 0) pack.statusName = "Charging";
                else if (st == 1) pack.statusName = "Discharging";
                else if (st == 2) pack.statusName = "Full";
                else if (st == 3) pack.statusName = "Standby";
            }
            else if (byteCount == 4) {
                // Cycles (0x0042..0x0043)
                pack.cycles = ((uint32_t)regs[0] << 16) | regs[1];
            }
        } else {
            // Standard generic Pylon/Seplos monolithic frame (regs contiguous)
            pack.isHuawei = false;
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
        }

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

            // Validate Modbus FC03 and valid battery address (Huawei 0xD6..0xDD / 0xE0..0xE7 or standard 1..16)
            // Minimum Modbus response is 1 register = 2 bytes, max 125 registers = 250 bytes
            if (fc == 0x03 && isValidBmsAddr(addr) && byteCount >= 2 && byteCount <= 125 && (byteCount % 2 == 0)) {
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
        // Battery 1 (Huawei ESM #1 - 0xD6)
        BatteryPack& b1 = packs[0];
        b1.isOnline = true;
        b1.isHuawei = true;
        b1.lastSeenMs = millis();
        b1.voltage = 51.85f + (float)(random(-5, 5)) / 100.0f;
        b1.current = 15.4f + (float)(random(-5, 5)) / 10.0f;
        b1.soc = 85;
        b1.soh = 100;
        b1.tempPcb = 24;
        b1.tempAvg = 23;
        b1.tempMax = 24;
        b1.statusName = "Charging";
        b1.cycles = 142;
        b1.balanceMask = 0;

        uint16_t base1 = 3455 + random(-3, 3);
        uint16_t b1Min = 5000, b1Max = 0;
        for (int i = 0; i < CELLS_PER_BATTERY; i++) {
            b1.cells[i] = base1 + (i % 3) * 2 - (i % 2) * 1;
            if (b1.cells[i] < b1Min) b1Min = b1.cells[i];
            if (b1.cells[i] > b1Max) b1Max = b1.cells[i];
        }
        b1.cellMin = b1Min;
        b1.cellMax = b1Max;
        b1.cellDiff = b1Max - b1Min;

        // Battery 2 (Huawei ESM #2 - 0xD7)
        BatteryPack& b2 = packs[1];
        b2.isOnline = true;
        b2.isHuawei = true;
        b2.lastSeenMs = millis();
        b2.voltage = 51.82f + (float)(random(-5, 5)) / 100.0f;
        b2.current = 15.2f + (float)(random(-5, 5)) / 10.0f;
        b2.soc = 84;
        b2.soh = 100;
        b2.tempPcb = 25;
        b2.tempAvg = 24;
        b2.tempMax = 25;
        b2.statusName = "Charging";
        b2.cycles = 138;
        b2.balanceMask = 0;

        uint16_t base2 = 3453 + random(-3, 3);
        uint16_t b2Min = 5000, b2Max = 0;
        for (int i = 0; i < CELLS_PER_BATTERY; i++) {
            b2.cells[i] = base2 + (i % 2) * 2;
            if (b2.cells[i] < b2Min) b2Min = b2.cells[i];
            if (b2.cells[i] > b2Max) b2Max = b2.cells[i];
        }
        b2.cellMin = b2Min;
        b2.cellMax = b2Max;
        b2.cellDiff = b2Max - b2Min; // delta ~4 mV (typical Huawei ESM)

        // Reset demo packs 3..15 to offline to cleanly represent 2-battery Huawei ESM bank by default
        for (int i = 2; i < MAX_BATTERIES; i++) {
            packs[i].isOnline = false;
            packs[i].statusName = "Offline";
        }

        xSemaphoreGive(mutex);
    }
}

