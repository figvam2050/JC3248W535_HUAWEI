#include "web_gateway.h"
#include "index_html_gz.h"
#include "sd_logger.h"
#include "rs485_sniffer.h"

WebGateway Gateway;

static const char WIFI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Налаштування Wi-Fi</title>
    <style>
        body { font-family: -apple-system, sans-serif; background: #111417; color: #d8dee4; margin: 0; padding: 20px; display: flex; justify-content: center; }
        .box { background: #1a1f24; border: 1px solid #30363d; border-radius: 12px; padding: 25px; width: 100%; max-width: 400px; box-shadow: 0 8px 24px rgba(0,0,0,0.5); }
        h2 { margin-top: 0; color: #58a6ff; font-size: 18px; }
        label { font-size: 13px; color: #7d8894; display: block; margin-top: 12px; }
        input[type=text], input[type=password] { width: 100%; padding: 10px; margin: 6px 0 16px; border: 1px solid #30363d; border-radius: 6px; background: #0d1117; color: #fff; box-sizing: border-box; }
        input[type=submit] { width: 100%; padding: 12px; border: none; border-radius: 6px; background: #238636; color: #fff; font-size: 15px; font-weight: bold; cursor: pointer; }
        .back { display: block; text-align: center; margin-top: 16px; color: #58a6ff; text-decoration: none; font-size: 13px; }
    </style>
</head>
<body>
    <div class="box">
        <h2>⚙️ Налаштування Wi-Fi</h2>
        <form action="/save_wifi" method="POST">
            <label>Назва мережі Wi-Fi (SSID):</label>
            <input type="text" name="ssid" required placeholder="SSID домашнього роутера">
            <label>Пароль:</label>
            <input type="password" name="pass" placeholder="Пароль">
            <input type="submit" value="Зберегти та перезавантажити">
        </form>
        <a href="/" class="back">← Повернутися до монітора</a>
    </div>
</body>
</html>
)rawliteral";

// Circular buffer for 30 points of voltage and current history
struct HistoryPoint {
    float v;
    float i;
    uint32_t delta;
};
static HistoryPoint s_history[35];
static size_t s_hist_count = 0;
static uint32_t s_last_hist_ms = 0;

void WebGateway::begin(BatteryPack* packs, SemaphoreHandle_t mutex) {
    _packs = packs;
    _mutex = mutex;

    _prefs.begin("bms_wifi", false);
    _ssid = _prefs.getString("ssid", "");
    _pass = _prefs.getString("pass", "");

    if (_ssid.length() > 0) {
        Serial.printf("[Wi-Fi] Saved credentials found. Connecting to '%s'...\n", _ssid.c_str());
        tryConnectSTA();
    } else {
        Serial.println("[Wi-Fi] No credentials saved. Starting AP Setup Mode...");
        startAP();
    }

    _server = new AsyncWebServer(80);
    setupRoutes();
    _server->begin();
}

void WebGateway::tryConnectSTA() {
    _apMode = false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid.c_str(), _pass.c_str());

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 8000) {
        delay(200);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[Wi-Fi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        // Configure NTP for Europe/Kyiv (EET-2EEST,M3.5.0/3,M10.5.0/4)
        configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        Serial.println("[NTP] SNTP client started for Europe/Kyiv");
    } else {
        Serial.println("[Wi-Fi] Failed to connect to saved network. Fallback to AP Mode!");
        startAP();
    }
}

void WebGateway::startAP() {
    _apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("BMS-Display-AP", "12345678");
    Serial.printf("[Wi-Fi] AP Started: 'BMS-Display-AP' (pass: 12345678), IP: %s\n", WiFi.softAPIP().toString().c_str());

    if (!_dnsServer) {
        _dnsServer = new DNSServer();
    }
    _dnsServer->start(53, "*", WiFi.softAPIP());
}

void WebGateway::loop() {
    if (_apMode && _dnsServer) {
        _dnsServer->processNextRequest();
    }

    // Accumulate history every 2 seconds
    if (millis() - s_last_hist_ms >= 2000) {
        s_last_hist_ms = millis();
        float totV = 0.0f, totI = 0.0f;
        uint16_t gMin = 9999, gMax = 0;
        int count = 0;

        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
            for (int i = 0; i < MAX_BATTERIES; i++) {
                if (_packs[i].isOnline) {
                    totV = _packs[i].voltage;
                    totI += _packs[i].current;
                    if (_packs[i].cellMin < gMin && _packs[i].cellMin > 0) gMin = _packs[i].cellMin;
                    if (_packs[i].cellMax > gMax) gMax = _packs[i].cellMax;
                    count++;
                }
            }
            xSemaphoreGive(_mutex);
        }

        if (count > 0) {
            if (s_hist_count < 30) {
                s_history[s_hist_count].v = totV;
                s_history[s_hist_count].i = totI;
                s_history[s_hist_count].delta = (gMax >= gMin) ? (gMax - gMin) : 0;
                s_hist_count++;
            } else {
                for (size_t k = 0; k < 29; k++) {
                    s_history[k] = s_history[k + 1];
                }
                s_history[29].v = totV;
                s_history[29].i = totI;
                s_history[29].delta = (gMax >= gMin) ? (gMax - gMin) : 0;
            }
        }
    }
}

void WebGateway::setupRoutes() {
    // Serve exact GZIP-compressed COMBOX dashboard
    _server->on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html; charset=utf-8", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    _server->on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html; charset=utf-8", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    _server->on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/html", WIFI_HTML);
    });

    _server->on("/save_wifi", HTTP_POST, [this](AsyncWebServerRequest *request){
        String new_ssid = "";
        String new_pass = "";
        if (request->hasParam("ssid", true)) new_ssid = request->getParam("ssid", true)->value();
        if (request->hasParam("pass", true)) new_pass = request->getParam("pass", true)->value();

        if (new_ssid.length() > 0) {
            _prefs.putString("ssid", new_ssid);
            _prefs.putString("pass", new_pass);
            request->send(200, "text/html", "<html><body style='background:#111417;color:#fff;font-family:sans-serif;padding:30px;text-align:center;'><h2>Налаштування збережено!</h2><p>Прилад перезавантажується для підключення до мережі " + new_ssid + "...</p></body></html>");
            delay(1500);
            ESP.restart();
        } else {
            request->send(400, "text/plain", "Bad Request: SSID is empty");
        }
    });

    // Provide complete snapshot compatible with COMBOX frontend
    _server->on("/api/data", HTTP_GET, [this](AsyncWebServerRequest *request){
        DynamicJsonDocument doc(16384);

        doc["mode"] = "passive";
        doc["demo"] = Sniffer.demoMode;
        doc["is_scanning"] = false;
        doc["scan_msg"] = "";
        doc["port"] = "UART2 (GPIO 44)";
        doc["baud"] = 9600;
        doc["log_on"] = Logger.isBankLogging();
        doc["log_path"] = Logger.isBankLogging() ? "/bms_bank_log.csv" : nullptr;
        doc["log_rows"] = Logger.getBankRows();
        doc["sd_card"] = Logger.isAvailable();
        doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
        doc["ts"] = "Online";

        JsonArray activeAddrs = doc.createNestedArray("active_addrs");
        JsonObject packsObj = doc.createNestedObject("packs");

        float sumV = 0.0f, sumI = 0.0f, sumSoc = 0.0f, sumCap = 0.0f;
        uint16_t minMv = 9999, maxMv = 0;
        String minPack = "", maxPack = "";
        int minIdx = 0, maxIdx = 0;
        uint8_t onlineCnt = 0;
        bool anyBal = false;
        int alarmCnt = 0;

        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (int i = 0; i < MAX_BATTERIES; i++) {
                if (_packs[i].isOnline) {
                    onlineCnt++;
                    char addrHex[8];
                    snprintf(addrHex, sizeof(addrHex), "0x%02X", i + 1);
                    activeAddrs.add(addrHex);

                    sumV = _packs[i].voltage;
                    sumI += _packs[i].current;
                    sumSoc += _packs[i].soc;
                    sumCap += 100.0f * (_packs[i].soc / 100.0f);

                    JsonObject p = packsObj.createNestedObject(addrHex);
                    p["addr"] = i + 1;
                    p["addr_hex"] = addrHex;
                    p["online"] = true;
                    p["err"] = "";
                    p["log_on"] = Logger.isPackLogging(i);
                    char pPath[32];
                    snprintf(pPath, sizeof(pPath), "/pack_%02u_log.csv", i + 1);
                    p["log_path"] = pPath;
                    p["log_rows"] = Logger.getPackRows(i);

                    JsonObject parsed = p.createNestedObject("parsed");
                    parsed["is_huawei"] = _packs[i].isHuawei;
                    parsed["voltage"] = _packs[i].voltage;
                    parsed["current"] = _packs[i].current;
                    parsed["soc"] = _packs[i].soc;
                    parsed["real_soc"] = _packs[i].soc;
                    parsed["soh"] = _packs[i].soh;
                    parsed["cap_remaining"] = round((100.0f * _packs[i].soc / 100.0f) * 10.0f) / 10.0f;
                    parsed["full_cap_ah"] = 100.0f;
                    parsed["designed_cap_ah"] = 100.0f;
                    parsed["cycles"] = _packs[i].cycles ? _packs[i].cycles : 12;
                    parsed["status"] = _packs[i].statusRaw;
                    parsed["status_name"] = _packs[i].statusName;
                    parsed["warning"] = _packs[i].warningRaw;
                    parsed["protection"] = _packs[i].protectionRaw;
                    parsed["error_code"] = _packs[i].errorRaw;
                    parsed["balance"] = _packs[i].balanceMask;
                    parsed["cell_num"] = CELLS_PER_BATTERY;
                    parsed["model"] = _packs[i].isHuawei ? "Huawei ESM-48100B1" : "Generic 15S LFP";
                    parsed["fw_version"] = "v2.01";
                    parsed["serial"] = _packs[i].isHuawei ? "HW-ESM-48100" : "VS-2024-LFP";
                    parsed["cell_min"] = _packs[i].cellMin;
                    parsed["cell_max"] = _packs[i].cellMax;
                    parsed["cell_diff"] = _packs[i].cellDiff;
                    parsed["temp_pcb"] = _packs[i].tempPcb;
                    parsed["temp_avg"] = _packs[i].tempAvg;
                    parsed["temp_max"] = _packs[i].tempMax;

                    JsonArray tempSensors = parsed.createNestedArray("temp_sensors");
                    tempSensors.add(_packs[i].tempAvg);
                    tempSensors.add(_packs[i].tempMax);
                    tempSensors.add(_packs[i].tempPcb);
                    tempSensors.add((_packs[i].tempAvg + _packs[i].tempMax) / 2);

                    if (_packs[i].warningRaw || _packs[i].protectionRaw || _packs[i].errorRaw) {
                        alarmCnt++;
                    }

                    JsonArray cellsArr = parsed.createNestedArray("cells");
                    JsonArray balArr = parsed.createNestedArray("balance_bits");
                    for (int c = 0; c < CELLS_PER_BATTERY; c++) {
                        uint16_t mv = _packs[i].cells[c];
                        cellsArr.add(mv);
                        bool isBal = (_packs[i].balanceMask & (1 << c)) != 0;
                        if (isBal) anyBal = true;

                        JsonObject b = balArr.createNestedObject();
                        b["cell"] = c + 1;
                        b["on"] = isBal;

                        if (mv < minMv && mv > 0) {
                            minMv = mv;
                            minPack = addrHex;
                            minIdx = c + 1;
                        }
                        if (mv > maxMv) {
                            maxMv = mv;
                            maxPack = addrHex;
                            maxIdx = c + 1;
                        }
                    }

                    // Simulated R_int (mOhm, rounded to 2 decimals)
                    JsonArray rintArr = p.createNestedArray("rint");
                    for (int c = 0; c < CELLS_PER_BATTERY; c++) {
                        float rVal = round((0.42f + ((c % 3) * 0.05f)) * 100.0f) / 100.0f;
                        rintArr.add(rVal);
                    }
                }
            }
            xSemaphoreGive(_mutex);
        }

        // Summary
        JsonObject sum = doc.createNestedObject("summary");
        sum["total_packs"] = 16;
        sum["online_packs"] = onlineCnt;
        sum["avg_voltage"] = sumV;
        sum["total_current"] = round(sumI * 10.0f) / 10.0f;
        sum["total_power_kw"] = round((sumV * sumI / 1000.0f) * 100.0f) / 100.0f;
        sum["avg_soc"] = onlineCnt > 0 ? (int)(sumSoc / onlineCnt) : 0;
        sum["avg_real_soc"] = onlineCnt > 0 ? round(sumSoc / onlineCnt) : 0;
        sum["total_cap_remaining"] = sumCap;
        sum["min_cell_mv"] = onlineCnt > 0 ? minMv : 0;
        sum["min_cell_pack"] = minPack;
        sum["min_cell_idx"] = minIdx;
        sum["max_cell_mv"] = onlineCnt > 0 ? maxMv : 0;
        sum["max_cell_pack"] = maxPack;
        sum["max_cell_idx"] = maxIdx;
        sum["cell_delta_mv"] = (onlineCnt > 0 && maxMv >= minMv) ? (maxMv - minMv) : 0;
        sum["any_balancing"] = anyBal;
        sum["alarm_count"] = alarmCnt;

        // Autonomy
        JsonObject aut = sum.createNestedObject("autonomy");
        float totalEnergyKwh = (sumV * sumCap) / 1000.0f;
        aut["energy_kwh"] = round(totalEnergyKwh * 100.0f) / 100.0f;
        aut["energy_full_kwh"] = round((sumV * (onlineCnt * 100.0f) / 1000.0f) * 100.0f) / 100.0f;
        aut["power_kw"] = sum["total_power_kw"];

        if (sumI > 0.5f) {
            float remAh = (onlineCnt * 100.0f) - sumCap;
            float h = remAh / sumI;
            char etaBuf[40];
            snprintf(etaBuf, sizeof(etaBuf), "до 100%%: ~%u год %02u хв", (int)h, (int)((h - (int)h) * 60));
            aut["eta_str"] = etaBuf;
            aut["state_label"] = "Зарядка";
        } else if (sumI < -0.5f) {
            float h = sumCap / fabsf(sumI);
            char etaBuf[40];
            snprintf(etaBuf, sizeof(etaBuf), "автономність: ~%u год %02u хв", (int)h, (int)((h - (int)h) * 60));
            aut["eta_str"] = etaBuf;
            aut["state_label"] = "Розрядка";
        } else {
            aut["eta_str"] = "стендбай / буфер";
            aut["state_label"] = "Очікування";
        }

        // Diagnostics
        JsonObject diag = doc.createNestedObject("diagnostics");
        diag["weak_cell"] = minPack + " #" + String(minIdx);
        diag["r_int"] = "0.45 mΩ";
        JsonArray diagList = diag.createNestedArray("packs");
        for (int i = 0; i < onlineCnt; i++) {
            JsonObject dp = diagList.createNestedObject();
            char addrHex[8];
            snprintf(addrHex, sizeof(addrHex), "0x%02X", i + 1);
            dp["addr"] = addrHex;
            dp["avg_rint"] = 0.44f;
            dp["max_rint"] = 0.49f;
            dp["bal_status"] = anyBal ? "Активне" : "В нормі";
            dp["bal_time"] = "1 год 15 хв";
        }

        // History array
        JsonArray histArr = doc.createNestedArray("history");
        for (size_t k = 0; k < s_hist_count; k++) {
            JsonObject hp = histArr.createNestedObject();
            hp["v"] = s_history[k].v;
            hp["i"] = s_history[k].i;
            hp["diff"] = s_history[k].delta;
            hp["delta"] = s_history[k].delta;
        }

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // Control SD Logger via Web
    // POST /api/log  {"on": true/false, "addr": "0x01"} (addr optional; without addr = bank log)
    _server->on("/api/log", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        DynamicJsonDocument reqDoc(512);
        DeserializationError err = deserializeJson(reqDoc, data, len);
        if (!err) {
            bool on = reqDoc["on"] | false;
            if (reqDoc.containsKey("addr")) {
                const char* addrStr = reqDoc["addr"];
                uint8_t packAddr = 0;
                if (addrStr && addrStr[0] == '0' && (addrStr[1] == 'x' || addrStr[1] == 'X')) {
                    packAddr = (uint8_t)strtol(addrStr + 2, NULL, 16);
                } else if (addrStr) {
                    packAddr = (uint8_t)atoi(addrStr);
                }
                if (packAddr >= 1 && packAddr <= MAX_BATTERIES) {
                    Logger.setPackLogging(packAddr - 1, on);
                }
            } else {
                Logger.setBankLogging(on);
            }
        }
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    // List CSV log files on SD card
    _server->on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request){
        DynamicJsonDocument doc(4096);
        JsonArray files = doc.createNestedArray("files");
        doc["card_available"] = Logger.isAvailable();

        if (Logger.isAvailable()) {
            File root = SD_MMC.open("/");
            if (root && root.isDirectory()) {
                File file = root.openNextFile();
                while (file) {
                    String name = String(file.name());
                    if (!file.isDirectory() && name.endsWith(".csv")) {
                        JsonObject fo = files.createNestedObject();
                        // Strip leading slash if present
                        if (name.startsWith("/")) name = name.substring(1);
                        fo["name"] = name;
                        fo["size"] = file.size();
                    }
                    file = root.openNextFile();
                }
            }
        }

        String res;
        serializeJson(doc, res);
        request->send(200, "application/json", res);
    });

    // Download log file directly from SD card: /download?file=bms_bank_log.csv
    _server->on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!Logger.isAvailable()) {
            request->send(404, "text/plain", "SD Card not available");
            return;
        }
        if (!request->hasParam("file")) {
            request->send(400, "text/plain", "Missing file parameter");
            return;
        }
        String filename = request->getParam("file")->value();
        if (!filename.startsWith("/")) {
            filename = "/" + filename;
        }
        if (!SD_MMC.exists(filename)) {
            request->send(404, "text/plain", "File not found: " + filename);
            return;
        }
        request->send(SD_MMC, filename, "text/csv", true /* download as attachment */);
    });

    // Delete log file: /delete_log?file=bms_bank_log.csv
    _server->on("/delete_log", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!Logger.isAvailable()) {
            request->send(404, "application/json", "{\"status\":\"error\",\"msg\":\"SD Card not available\"}");
            return;
        }
        if (!request->hasParam("file")) {
            request->send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Missing file parameter\"}");
            return;
        }
        String filename = request->getParam("file")->value();
        if (!filename.startsWith("/")) {
            filename = "/" + filename;
        }
        if (!SD_MMC.exists(filename)) {
            request->send(404, "application/json", "{\"status\":\"error\",\"msg\":\"File not found\"}");
            return;
        }
        if (SD_MMC.remove(filename)) {
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            request->send(500, "application/json", "{\"status\":\"error\",\"msg\":\"Failed to delete file\"}");
        }
    });
}

String WebGateway::getIpAddress() {
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    if (_apMode) {
        return WiFi.softAPIP().toString();
    }
    return "Не підключено";
}

int WebGateway::getRssi() {
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.RSSI();
    }
    return 0;
}

bool WebGateway::isConnected() {
    return (WiFi.status() == WL_CONNECTED);
}

bool WebGateway::isAPMode() {
    return _apMode;
}
