# LoRa32 OOK Transmitter

Record-and-replay of 433.92 MHz OOK signals. Capture an on-off-keyed transmission
(remote, doorbell, weather sensor, …) with the workbench RTL-SDR and `rtl_433`, then
retransmit the same pulse train from a TTGO LoRa32 (SX1278) board.

## How it works

```
  433 MHz remote  ──RF──►  RTL-SDR + rtl_433   ──►  pulse/gap timings (µs)
                            (workbench Pi)              │
                                                        ▼  transcribe into PULSES[]
  TTGO LoRa32 (SX1278)  ◄──────── src/main.cpp ─────────┘
        │
        └──RF──►  replayed 433.92 MHz OOK burst
```

The SX1278 runs in FSK/OOK mode. The firmware keys the carrier on/off according to a
timing array (`PULSES[]`), reproducing the captured waveform.

## 1. Capture with rtl_433 (on the workbench Pi)

```bash
# Decode known devices:
rtl_433 -f 433.92M

# Analyze an unknown signal - prints the pulse/gap table in microseconds:
rtl_433 -A -f 433.92M
```

Trigger your 433 MHz device while `-A` is running. `rtl_433` prints the mark/space
(pulse/gap) widths. Copy them into `PULSES[]` in `src/main.cpp`, alternating
`mark, space, mark, space, …`, starting with the carrier ON.

## 2. Build & flash

```bash
pio run                 # build
pio run -t upload       # flash over local USB
pio device monitor      # 115200 baud
```

Flash through the Universal Embedded Workbench (RFC2217) by overriding the port —
see the commented example in `platformio.ini`.

## 3. Verify the loop

Run `rtl_433 -A -f 433.92M` on the Pi again — it should now see the LoRa32's replayed
burst with the same timing you captured.

## Board wiring (TTGO LoRa32, SX1278)

| SX1278 | ESP32 GPIO |
|--------|-----------|
| SCK    | 5  |
| MISO   | 19 |
| MOSI   | 27 |
| NSS    | 18 |
| RST    | 23 (v2.1) / 14 (v1.0) |
| DIO0   | 26 |
| DIO1   | 33 |

Set the board version in `platformio.ini` (`ttgo-lora32-v21` / `ttgo-lora32-v1`). If
`beginFSK` fails, the `RST` pin is the usual culprit.

## Notes

- **RTL-SDR overload**: transmitting at close range into the RTL-SDR can saturate it.
  Keep `TX_POWER` low, add distance, or attenuate. The Pi is power-sensitive — don't
  transmit into it while flashing other devices.
- **Timing precision**: the carrier is keyed via SPI mode switches, so sub-~200 µs
  pulses are distorted. For microsecond-accurate replay, drive the SX1278 `DIO2` DATA
  pin directly in continuous mode (future enhancement).
- **433.92 MHz is licensed spectrum.** Only retransmit signals you are authorized to,
  at legal power/duty-cycle for your region.
