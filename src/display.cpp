#include <SSD1306Wire.h>
#include "display.h"
#include "motion.h"
#include "net.h"

// LILYGO LoRa32 V2.1_1.6.1 OLED: driven by the ThingPulse SSD1306Wire driver
// (the Adafruit driver hangs this panel's I2C). Its RST line is intentionally
// NOT toggled — driving GPIO16 hangs the I2C peripheral (arduino-esp32 #4278),
// which was the TG1WDT boot loop. This matches OpenMQTTGateway's board config.
static SSD1306Wire oled(OLED_ADDR, OLED_SDA, OLED_SCL, GEOMETRY_128_64);
static bool s_ok = false;

void displayInit() {
#if !OLED_ENABLED
  // OLED disabled (see config.h): status is exposed over web/MQTT instead.
  Serial.println(F("OLED disabled (OLED_ENABLED=0)"));
  s_ok = false;
  return;
#else
  oled.init();
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.setFont(ArialMT_Plain_10);
  oled.clear();
  oled.drawString(0, 0, "Awning");
  oled.display();
  s_ok = true;
#endif
}

static const char* stateName(AwningState s) {
  switch (s) {
    case ST_RETRACTED: return "RETRACTED";
    case ST_EXTENDED:  return "EXTENDED";
    default:           return "MOVING";
  }
}

void displayRender() {
  if (!s_ok) return;
  oled.clear();
  oled.setTextAlignment(TEXT_ALIGN_LEFT);

  AwningState st = motionState();
  char buf[32];

  if (motionMoving()) {
    // State (16pt) + big progress bar + target (FR-5.3)
    oled.setFont(ArialMT_Plain_16);
    oled.drawString(0, 0, motionDir() == DIR_EXTEND ? "EXTENDING" : "RETRACTING");
    int w = (int)(motionProgress() * (OLED_W - 4));
    oled.drawRect(0, 24, OLED_W, 16);
    oled.fillRect(2, 26, w, 12);
    snprintf(buf, sizeof(buf), "-> %.2f m", motionTargetM());
    oled.drawString(0, 44, buf);
  } else {
    // State (16pt) + large position readout (24pt) (FR-5.2)
    oled.setFont(ArialMT_Plain_16);
    oled.drawString(0, 0, motionSafetyRetract() ? "SAFE RETRACT" : stateName(st));
    oled.setFont(ArialMT_Plain_24);
    snprintf(buf, sizeof(buf), "%.2f m", motionPositionM());
    oled.drawString(0, 22, buf);
  }

  // Footer: connectivity + IP / AP (FR-5.4/5.5)
  oled.drawHorizontalLine(0, 52, OLED_W);
  oled.setFont(ArialMT_Plain_10);
  if (netPortalActive())       oled.drawString(0, 53, "AP:" AP_SSID);
  else if (netWifiConnected()) oled.drawString(0, 53, netIP());
  else                         oled.drawString(0, 53, "WiFi...");
  oled.setTextAlignment(TEXT_ALIGN_RIGHT);
  oled.drawString(OLED_W, 53, netMqttConnected() ? "MQ" : (netWifiConnected() ? "W" : ""));

  oled.display();
}
