#pragma once
#include <Arduino.h>

// ============================================================
// LoRa32 OOK Awning Controller - shared configuration
// TTGO LoRa32 T3 v1.6.1 (ESP32 + SX1278), 433.92 MHz OOK
// ============================================================

// ---- SX1278 pin map (T3 v1.6.1) ----
#define LORA_SCK    5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_NSS   18
#define LORA_RST   23
#define LORA_DIO0  26
#define LORA_DIO1  33
#define LORA_DIO2  32   // OOK DATA pin in direct mode (carrier keying)
#define LED_PIN    25   // onboard LED mirrors carrier

// ---- OLED (SSD1306 128x64, I2C) ----
// Uses the ThingPulse SSD1306Wire driver with RST left undriven (per the
// LILYGO V2.1_1.6.1 config in OpenMQTTGateway). Set to 0 to disable the panel.
#define OLED_ENABLED 1
#define OLED_SDA   21
#define OLED_SCL   22
#define OLED_RST   16
#define OLED_ADDR  0x3C
#define OLED_W     128
#define OLED_H     64

// ---- RF ----
static constexpr float FREQ_MHZ = 433.92f;
static constexpr int   TX_POWER = 10;      // dBm; keep low on the bench (NFR-2)

// ---- Codewords (rtl_433 {18} hex, MSB-first) ----
struct Button { const char* name; uint32_t code; };
extern const Button   BUTTONS[];
extern const uint8_t  NUM_BUTTONS;
enum { BTN_UP = 0, BTN_DOWN = 1, BTN_AUTO = 2, BTN_MANUAL = 3 };
static constexpr uint8_t CODE_BITS = 18;

// ---- OOK-PWM timing template (microseconds) ----
static constexpr uint16_t LONG_MARK  = 2150, LONG_SPACE  = 172;
static constexpr uint16_t SHORT_MARK =  416, SHORT_SPACE = 1908;
static constexpr uint16_t RESET_GAP  = 15588;   // inter-word reset (< delayMicroseconds limit)
static constexpr uint8_t  REPEATS     = 12;     // words per transmission (FR-1.5)
static constexpr uint8_t  CMD_REPEATS = 5;      // transmissions per motion action (FR-4.1)

// ---- Awning motion (open-loop, metres) ----
static constexpr float    SPEED_M_PER_S     = 0.10f;  // calibrate: metres per second of run (FR-4.4/4.9)
static constexpr float    MAX_TRAVEL_M      = 3.00f;  // calibrate: full extension (FR-4.4)
static constexpr float    POS_EPSILON_M     = 0.05f;  // no-op threshold (FR-4.8)
static constexpr uint32_t RETRACT_MARGIN_S  = 5;      // extra retract to guarantee end-stop (FR-4.10)
static constexpr uint32_t FULL_RETRACT_S    = 40;     // boot full-close drive time (FR-4.10)
static constexpr uint32_t EMERGENCY_TIMEOUT_S = 120;  // heartbeat-loss watchdog (FR-4.6)

// ---- MQTT / Home Assistant ----
static constexpr uint16_t MQTT_PORT_DEFAULT = 1883;
#define MQTT_BASE_TOPIC      "awning"
#define HA_DISCOVERY_PREFIX  "homeassistant"
#define WATCHDOG_TOPIC       "awning/watchdog"      // HA liveness heartbeat (FR-4.6)
#define WATCHDOG_UNSAFE      "unsafe"               // payload that forces an emergency retract

// MQTT topic tree (under MQTT_BASE_TOPIC)
#define T_CMD_SET       "awning/cmd/set"            // OPEN / CLOSE / STOP
#define T_CMD_POSITION  "awning/cmd/position"       // 0..100 (%)
#define T_CMD_EMERGENCY "awning/cmd/emergency"      // emergency retract button
#define T_STAT_STATE    "awning/status/state"       // open/opening/closing/closed
#define T_STAT_POS      "awning/status/position"    // 0..100 (%)
#define T_STAT_AVAIL    "awning/status/availability" // online/offline (LWT)
#define T_DIAG_IP       "awning/diag/ip"
#define T_DIAG_RSSI     "awning/diag/rssi"
#define T_DIAG_UPTIME   "awning/diag/uptime"
#define T_DIAG_LAST     "awning/diag/last"
#define T_DIAG_SAFETY   "awning/diag/safety"        // ON when in safety/emergency retract

// ---- Provisioning / OTA ----
#define AP_SSID  "Awning-Setup"                     // captive-portal AP (FR-6.2)
static constexpr uint32_t DRD_WINDOW_S = 10;        // double-reset window (FR-6.3)
#define OTA_HOSTNAME  "awning"                       // ArduinoOTA hostname (FR-9.1)
