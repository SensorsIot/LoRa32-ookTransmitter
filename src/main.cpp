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
#define LORA_DIO2  32   // SX1278 DIO2 = OOK DATA pin in direct mode
#define LED_PIN    25   // onboard LED (some variants: GPIO2)

#define FREQ_MHZ   433.92
#define TX_POWER   10   // dBm. Keep low on the bench - see README (RTL-SDR overload).

SX1278 radio = new Module(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1);

// ---- Captured remote (433.92 MHz OOK-PWM) ----
// Four buttons captured with rtl_433 -A on the workbench RTL-SDR. Each is an
// 18-bit FIXED codeword (fixed, not rolling -> replayable). The values below are
// exactly as rtl_433 prints them: {18} hex, i.e. the top 18 bits of this 20-bit
// field, first transmitted bit = MSB.
struct Button { const char* name; uint32_t code; };
static const Button BUTTONS[] = {
  { "up",     0x7f454 },
  { "down",   0x7f45c },
  { "auto",   0x7f480 },
  { "manual", 0x7f484 },
};
static const uint8_t NUM_BUTTONS = sizeof(BUTTONS) / sizeof(BUTTONS[0]);
static const uint8_t CODE_BITS   = 18;

// OOK-PWM timing template (microseconds), measured from the capture and verified
// against rtl_433's decoded bits: bit 0 = long pulse, bit 1 = short pulse. Each
// bit is one pulse + gap; the last bit of a word is followed by the reset gap.
static const uint16_t LONG_MARK  = 2150, LONG_SPACE  = 172;
static const uint16_t SHORT_MARK =  416, SHORT_SPACE = 1908;
static const uint16_t RESET_GAP  = 15588;   // inter-word reset (< 16383 us delayMicroseconds limit)

static const uint8_t  REPEATS   = 12;    // words per transmission (real remote sent ~50)
static const uint32_t PERIOD_MS = 3000;  // wait between buttons

static bool radioReady = false;

static inline void carrier(bool on) {
  digitalWrite(LORA_DIO2, on ? HIGH : LOW);   // DIO2 = OOK DATA in direct mode
  digitalWrite(LED_PIN,   on ? HIGH : LOW);
}

// Transmit one button's codeword REPEATS times, reconstructing the pulse train
// from the timing template. Bits are read MSB-first from the top 18 of the field.
static void sendButton(const Button& b) {
  for (uint8_t r = 0; r < REPEATS; r++) {
    for (int8_t bit = 19; bit >= 2; bit--) {
      bool one = (b.code >> bit) & 0x1;
      carrier(true);
      delayMicroseconds(one ? SHORT_MARK : LONG_MARK);
      carrier(false);
      bool lastBit = (bit == 2);
      delayMicroseconds(lastBit ? RESET_GAP : (one ? SHORT_SPACE : LONG_SPACE));
    }
  }
  carrier(false);
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

  // Direct OOK keying: in continuous direct mode the carrier is gated by the
  // SX1278 DIO2 DATA pin (GPIO32 on T3 v1.6.1). Enter direct TX once, then key
  // the carrier by driving DIO2 from the MCU for accurate microsecond timing.
  pinMode(LORA_DIO2, OUTPUT);
  digitalWrite(LORA_DIO2, LOW);
  radio.transmitDirect();

  radioReady = true;
  Serial.printf("Ready. %u buttons, %u repeats each @ %.2f MHz\n",
                NUM_BUTTONS, REPEATS, FREQ_MHZ);
  Serial.println(F("Serial: u=up d=down a=auto m=manual (or auto-cycles all)"));
}

static void transmitByKey(char c) {
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    if (c == BUTTONS[i].name[0]) {
      Serial.printf("TX %s (0x%05x)\n", BUTTONS[i].name, BUTTONS[i].code);
      sendButton(BUTTONS[i]);
      return;
    }
  }
}

void loop() {
  if (!radioReady) { delay(1000); return; }

  // Manual trigger over serial: u / d / a / m
  if (Serial.available()) {
    transmitByKey(Serial.read());
    return;
  }

  // Hands-free: cycle through every button so a receiver sees all four codes.
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    Serial.printf("TX %s (0x%05x)\n", BUTTONS[i].name, BUTTONS[i].code);
    sendButton(BUTTONS[i]);
    delay(PERIOD_MS);
  }
}
