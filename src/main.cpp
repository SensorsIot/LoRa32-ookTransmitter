#include <Arduino.h>
#include "config.h"
#include "ook.h"
#include "motion.h"
#include "display.h"
#include "net.h"

// ============================================================
// LoRa32 OOK Awning Controller
//   RF:      SX1278 433.92 MHz OOK codeword replay (FR-1)
//   Motion:  open-loop metre positioning, TX task on the app core (FR-4, 2.6)
//   Control: MQTT/HA, LAN web page, USB serial (FR-2, 7, 8)
//   Safety:  HA heartbeat watchdog -> fail-closed retract (FR-4.6)
//   OTA:     ArduinoOTA (FR-9)
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nLoRa32 OOK Awning Controller"));

  if (!ookInit()) {
    Serial.println(F("Radio init failed - transmission disabled (FR-3.1)"));
  }
  displayInit();
  motionInit();     // starts the TX task and queues the boot full-retract (FR-4.10)
  netSetup();       // WiFi/provisioning/MQTT/HTTP/OTA (may block in the captive portal)

  Serial.println(F("Ready. Serial: u=up d=down a=auto m=manual"));
}

static void serialPoll() {
  if (!Serial.available()) return;
  switch (Serial.read()) {
    case 'u': motionClose();          break;   // up = retract
    case 'd': motionOpen();           break;   // down = extend
    case 'a': motionRaw(BTN_AUTO);    break;
    case 'm': motionRaw(BTN_MANUAL);  break;
    case 's': motionStop();           break;
    case 'e': motionEmergency();      break;
    default: break;
  }
}

void loop() {
  netLoop();              // OTA, web, MQTT, periodic publish
  motionWatchdogTick();   // fail-closed on lost HA heartbeat (FR-4.6)
  serialPoll();           // FR-2.1
  static uint32_t lastOled = 0;
  if (millis() - lastOled > 200) { lastOled = millis(); displayRender(); }
}
