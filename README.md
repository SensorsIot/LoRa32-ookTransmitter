# LoRa32 OOK Transmitter

Record-and-replay of 433.92 MHz OOK signals. Capture an on-off-keyed transmission
(remote, doorbell, weather sensor, …) with the workbench RTL-SDR and `rtl_433`, then
retransmit the same pulse train from a TTGO LoRa32 (SX1278) board.

## How it works

```
  433 MHz remote  ──RF──►  RTL-SDR + rtl_433   ──►  pulse/gap timings (µs)
                            (workbench Pi)              │
                                                        ▼  codeword + timing → firmware
  TTGO LoRa32 (SX1278)  ◄──────── src/main.cpp ─────────┘
        │
        └──RF──►  replayed 433.92 MHz OOK burst
```

The SX1278 runs in FSK/OOK mode. The firmware keys the carrier on/off to reconstruct
each captured codeword from a shared timing template, reproducing the original waveform.

## Captured remote

A 4-button 433.92 MHz remote, captured with `rtl_433 -A` on the workbench RTL-SDR.
Each button is an 18-bit **fixed** OOK-PWM codeword (fixed, not rolling — so it
replays correctly):

| Button | Code (`{18}` hex) |
|--------|-------------------|
| up     | `0x7f454` |
| down   | `0x7f45c` |
| auto   | `0x7f480` |
| manual | `0x7f484` |

Timing template (measured, verified against rtl_433's bits): bit `0` = long pulse
~2150 µs, bit `1` = short pulse ~416 µs; each bit is one pulse + gap, and a word ends
with a ~15.6 ms reset gap. The firmware auto-cycles all four buttons, or triggers one
on a serial keypress: `u` / `d` / `a` / `m`.

## 1. Capture with rtl_433 (on the workbench Pi)

```bash
# Decode known devices:
rtl_433 -f 433.92M

# Analyze an unknown signal - prints the pulse/gap table in microseconds:
rtl_433 -A -f 433.92M
```

Trigger your 433 MHz device while `-A` is running. `rtl_433` prints the decoded
codeword (e.g. `{18}7f45c`) and the pulse/gap widths. Add the codeword to `BUTTONS[]`
in `src/main.cpp`; if the timing differs from this remote, adjust the pulse template
constants (`LONG_MARK`, `SHORT_MARK`, gaps, `RESET_GAP`).

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
