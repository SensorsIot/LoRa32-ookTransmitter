#include <RadioLib.h>
#include "ook.h"

static SX1278 radio = new Module(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1);
static bool s_ready = false;

static inline void carrier(bool on) {
  digitalWrite(LORA_DIO2, on ? HIGH : LOW);   // DIO2 = OOK DATA in direct mode
  digitalWrite(LED_PIN,   on ? HIGH : LOW);
}

// Assert OOK direct-transmit configuration on the chip. Split out of ookInit so
// it can be re-run after the RX gateway has reconfigured the same SX1278 for
// reception (the two share one radio; see rx433). Idempotent.
void ookBeginTx() {
  // FSK engine with OOK modulation enabled.
  radio.beginFSK(FREQ_MHZ, 4.8, 5.0, 125.0, TX_POWER, 16, /*enableOOK=*/true);
  radio.setOOK(true);
  radio.setFrequency(FREQ_MHZ);
  radio.setOutputPower(TX_POWER);

  // Continuous direct mode: the carrier is gated by the DIO2 DATA pin, which we
  // drive from the MCU for accurate microsecond timing.
  pinMode(LORA_DIO2, OUTPUT);
  digitalWrite(LORA_DIO2, LOW);
  radio.transmitDirect();
}

bool ookInit() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

  int st = radio.beginFSK(FREQ_MHZ, 4.8, 5.0, 125.0, TX_POWER, 16, /*enableOOK=*/true);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("beginFSK failed: %d - check board variant / RST pin\n", st);
    s_ready = false;
    return false;
  }
  ookBeginTx();

  s_ready = true;
  return true;
}

bool ookReady() { return s_ready; }

// One transmission = REPEATS words. Bits are read MSB-first from the top 18 of
// the 20-bit field (bits 19..2), long pulse = 0, short pulse = 1.
static void sendOnce(uint32_t code) {
  for (uint8_t r = 0; r < REPEATS; r++) {
    for (int8_t bit = 19; bit >= 2; bit--) {
      bool one = (code >> bit) & 0x1;
      carrier(true);
      delayMicroseconds(one ? SHORT_MARK : LONG_MARK);
      carrier(false);
      bool lastBit = (bit == 2);
      delayMicroseconds(lastBit ? RESET_GAP : (one ? SHORT_SPACE : LONG_SPACE));
    }
  }
  carrier(false);
}

void ookSend(uint8_t idx, uint8_t transmissions) {
  if (!s_ready || idx >= NUM_BUTTONS) return;
  uint32_t code = BUTTONS[idx].code;
  for (uint8_t t = 0; t < transmissions; t++) {
    sendOnce(code);
    // Yield between transmissions so the app core's network/OLED loop breathes.
    // The extra gap only lengthens the inter-word silence, which the receiver
    // tolerates.
    if (t + 1 < transmissions) vTaskDelay(pdMS_TO_TICKS(1));
  }
}
