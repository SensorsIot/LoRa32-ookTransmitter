#include <Arduino.h>
#include <RadioLib.h>

// ============================================================
// TTGO LoRa32 (SX1278) - 433.92 MHz OOK replay transmitter
//
// Workflow (record & replay):
//   1. Capture a 433 MHz OOK signal on the workbench RTL-SDR:
//        rtl_433 -A -f 433.92M
//      Read the pulse/gap table (microseconds) it prints.
//   2. Transcribe the timing into PULSES[] below - alternating
//      mark(on), space(off), mark, space... in microseconds,
//      starting with the carrier ON.
//   3. Flash this firmware; it replays the burst repeatedly so
//      the RTL-SDR (or the original receiver) sees it again.
// ============================================================

// ---- TTGO LoRa32 SX1278 pin map ----
// RST is 23 on T3 v2.1 (v1.6). Older v1.0 boards use 14 - change if init fails.
#define LORA_SCK    5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_NSS   18
#define LORA_RST   23
#define LORA_DIO0  26
#define LORA_DIO1  33
#define LED_PIN    25   // onboard LED (some variants: GPIO2)

#define FREQ_MHZ   433.92
#define TX_POWER   10   // dBm. Keep low on the bench - see README (RTL-SDR overload).

SX1278 radio = new Module(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1);

// ---- Captured OOK burst ----
// Alternating durations in microseconds: mark(on), space(off), mark, space...
// Placeholder = a trivial pattern. REPLACE with your rtl_433 -A capture.
static const uint16_t PULSES[] = {
  500, 1000, 1000, 500, 500, 1000, 1000, 500,
  500, 1000, 500, 1000, 1000, 500, 500, 1000
};
static const size_t   PULSE_COUNT    = sizeof(PULSES) / sizeof(PULSES[0]);
static const uint32_t GAP_BETWEEN_MS = 200;  // idle between repeats within a transmission
static const uint8_t  REPEATS        = 5;    // bursts per transmission
static const uint32_t PERIOD_MS      = 3000; // wait before the next transmission

static bool radioReady = false;

static void carrier(bool on) {
  if (on) { radio.transmitDirect(); digitalWrite(LED_PIN, HIGH); }
  else    { radio.standby();        digitalWrite(LED_PIN, LOW);  }
}

static void replayBurst() {
  for (size_t i = 0; i < PULSE_COUNT; i++) {
    carrier((i % 2) == 0);        // even index = mark (on), odd = space (off)
    delayMicroseconds(PULSES[i]);
  }
  carrier(false);                 // leave carrier off after the burst
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nTTGO LoRa32 SX1278 - 433.92 MHz OOK replay TX"));

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

  // FSK engine with OOK modulation enabled.
  int st = radio.beginFSK(FREQ_MHZ, 4.8, 5.0, 125.0, TX_POWER, 16, /*enableOOK=*/true);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("beginFSK failed: %d - check board variant / RST pin\n", st);
    return;
  }
  radio.setOOK(true);
  radio.setFrequency(FREQ_MHZ);
  radio.setOutputPower(TX_POWER);
  radio.standby();

  radioReady = true;
  Serial.printf("Ready. %u pulses/burst, %u repeats @ %.2f MHz\n",
                (unsigned)PULSE_COUNT, REPEATS, FREQ_MHZ);
}

void loop() {
  if (!radioReady) { delay(1000); return; }

  Serial.println(F("TX burst"));
  for (uint8_t r = 0; r < REPEATS; r++) {
    replayBurst();
    delay(GAP_BETWEEN_MS);
  }
  delay(PERIOD_MS);
}
