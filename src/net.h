#pragma once
#include "config.h"

// WiFi provisioning + MQTT/Home Assistant + HTTP + OTA (FR-6..FR-9).

// Load NVS settings, run WiFiManager (captive portal on demand, double-reset
// re-provision), then start MQTT, the web server, and ArduinoOTA. May block in
// the captive portal until provisioned.
void netSetup();

// Service DRD, OTA, MQTT, the web server, and periodic publishing. Call from loop().
void netLoop();

// --- Connectivity status (for the OLED) ---
bool        netWifiConnected();
bool        netMqttConnected();
bool        netPortalActive();
String      netIP();
int         netRSSI();
String      netSSID();
