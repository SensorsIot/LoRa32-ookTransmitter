#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "display.h"
#include "motion.h"
#include "net.h"

static Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, OLED_RST);
static bool s_ok = false;

void displayInit() {
  Wire.begin(OLED_SDA, OLED_SCL);
  s_ok = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!s_ok) { Serial.println(F("SSD1306 init failed")); return; }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println(F("Awning"));
  oled.display();
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
  oled.clearDisplay();

  // Header: name + connectivity indicators (FR-5.4)
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(F("Awning"));
  oled.setCursor(80, 0);
  oled.print(netWifiConnected() ? F("wifi") : F("----"));
  oled.setCursor(110, 0);
  oled.print(netMqttConnected() ? F("mq") : F("--"));
  oled.drawFastHLine(0, 10, OLED_W, SSD1306_WHITE);

  AwningState st = motionState();

  if (motionMoving()) {
    // Progress bar + countdown to the counter-stop (FR-5.3)
    oled.setTextSize(1);
    oled.setCursor(0, 16);
    oled.print(motionDir() == DIR_EXTEND ? F("EXTENDING") : F("RETRACTING"));
    oled.setCursor(84, 16);
    oled.printf("%.2fm", motionTargetM());
    int w = (int)(motionProgress() * (OLED_W - 4));
    oled.drawRect(0, 30, OLED_W, 12, SSD1306_WHITE);
    oled.fillRect(2, 32, w, 8, SSD1306_WHITE);
    oled.setCursor(0, 46);
    oled.printf("stop at %.2f m", motionTargetM());
  } else {
    // Large centred state with a direction glyph (FR-5.2)
    oled.setTextSize(1);
    oled.setCursor(0, 20);
    oled.print(st == ST_RETRACTED ? F("^") : F("v"));
    oled.setTextSize(2);
    oled.setCursor(14, 18);
    oled.print(stateName(st));
    oled.setTextSize(1);
    oled.setCursor(0, 40);
    oled.printf("pos %.2f m", motionPositionM());
    if (motionSafetyRetract()) { oled.setCursor(84, 40); oled.print(F("SAFE")); }
  }

  // Footer: IP (FR-5.4/5.5)
  oled.drawFastHLine(0, 54, OLED_W, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 56);
  if (netPortalActive())      oled.print(F("AP: " AP_SSID));
  else if (netWifiConnected()) oled.print(netIP());
  else                         oled.print(F("WiFi..."));

  oled.display();
}
