#ifndef WEB_GATEWAY_H
#define WEB_GATEWAY_H

#include "bms_data.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DNSServer.h>

class WebGateway {
public:
    void begin(BatteryPack* packs, SemaphoreHandle_t mutex);
    void loop();
    String getIpAddress();
    int getRssi();
    bool isConnected();
    bool isAPMode();

private:
    AsyncWebServer* _server = nullptr;
    DNSServer* _dnsServer = nullptr;
    BatteryPack* _packs = nullptr;
    SemaphoreHandle_t _mutex = nullptr;
    Preferences _prefs;

    bool _apMode = false;
    String _ssid = "";
    String _pass = "";

    void setupRoutes();
    void startAP();
    void tryConnectSTA();
};

extern WebGateway Gateway;

#endif // WEB_GATEWAY_H
