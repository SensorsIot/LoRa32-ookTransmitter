# 🎚️ LoRa32 OOK Awning Controller

A smart-home controller for a 433.92 MHz remote-controlled awning (Storen), built on a
TTGO LoRa32 (SX1278) board. It drives the awning by transmitting the original remote's
**up** and **down** OOK codes, tracks the awning's position in metres, and exposes it to
Home Assistant over MQTT, to a phone via a LAN web page, and to the OLED — with a
fail-closed wind-safety watchdog.

## ✨ What it does

- 📏 **Positions the awning** to an absolute target in metres. Extending runs the motor and
  sends a counter-stop to park at the target; retracting runs to the closed end-stop and
  re-homes the position to 0.
- 🏠 **Home Assistant integration** via MQTT auto-discovery: the awning appears as a `cover`
  (open / close / stop / set-position), plus an emergency-retract button and diagnostic
  sensors.
- 📡 **433 MHz receive gateway.** The *same* SX1278 also receives 433 OOK sensors (weather
  stations, thermo-hygrometers, the Euromot remote) with the rtl_433 decoder engine and
  republishes them to Home Assistant via MQTT auto-discovery — a drop-in replacement for
  OpenMQTTGateway, sharing the radio half-duplex with the awning transmitter. See
  [`documentation/rx-integration-plan.md`](documentation/rx-integration-plan.md) and
  [`tools/`](tools/).
- 📱 **Local web page** on the LAN with up / down / stop / emergency controls, a
  target-position input, live state and progress, and the auto / manual remote buttons.
- 🛡️ **Fail-closed safety.** Home Assistant is the wind-safety authority: it publishes a
  liveness heartbeat, and if the heartbeat stops or signals an unsafe condition, the
  device fully retracts on its own.
- 🖥️ **OLED status**, captive-portal WiFi/MQTT provisioning, and OTA firmware updates.

## ⚙️ How it works

```
  Home Assistant ─MQTT─┐
  Web page (LAN)  ─────┼─► command queue ─► TX task ─► SX1278 OOK keying ─RF─► awning
  USB serial      ─────┘         ▲
  HA awning/watchdog heartbeat ──┘  (fail-closed retract on loss)
```

The awning has no position feedback, so the device tracks position **open-loop**: motor
run-time × a calibrated speed (`SPEED_M_PER_S`). A dedicated FreeRTOS task on the
application core keys the SX1278 and runs the motion sequences; WiFi, MQTT, HTTP, and OTA
run alongside on the other core. Every full retract drives past the end-stop and resets
the tracked position to 0, so drift cannot accumulate. On boot the device performs a full
retract to reach the known-safe closed state.

The radio side is unchanged from a plain OOK replay: the SX1278 runs in continuous direct
mode and the carrier is keyed by driving `DIO2` (GPIO32) from the MCU, reconstructing each
codeword from a shared timing template.

## 📡 Remote codes

A 4-button 433.92 MHz remote, captured with `rtl_433 -A` on the workbench RTL-SDR. Each
button is an 18-bit fixed OOK-PWM codeword:

| Button | Code (`{18}` hex) | Use |
|--------|-------------------|-----|
| up     | `0x7f454` | retract |
| down   | `0x7f45c` | extend |
| auto   | `0x7f480` | web-page button |
| manual | `0x7f484` | web-page button |

Timing template: bit `0` = long pulse ~2150 µs, bit `1` = short pulse ~416 µs; each bit is
one pulse + gap, and a word ends with a ~15.6 ms reset gap.

## 🚀 Setup

### 1. 🔨 Build & flash

```bash
pio run                 # build
pio run -t upload       # flash over local USB
pio device monitor      # 115200 baud
```

Flash through the Universal Embedded Workbench (RFC2217) by overriding the upload port —
see the commented example in `platformio.ini`. After the first USB flash, subsequent
updates can go over WiFi (ArduinoOTA, hostname `awning`).

### 2. 📶 Provision WiFi + MQTT

On first boot (or after a double reset within 10 s) the device starts a captive-portal
access point **`Awning-Setup`**. Join it and enter the WiFi credentials and the MQTT
broker host / port / user / password; they are stored in NVS.

### 3. 📐 Calibrate

Measure a real run and set the two constants in `src/config.h`:

- `SPEED_M_PER_S` — metres of awning travel per second of motor run.
- `MAX_TRAVEL_M` — full extension in metres (maps to Home Assistant's 100 % position).

### 4. 🏠 Home Assistant

The awning appears automatically via MQTT discovery. The site's wind/lux/temperature
automation and the required `awning/watchdog` heartbeat are provided in
[`homeassistant/awning.yaml`](homeassistant/awning.yaml). The device only allows the
awning to stay open while that automation is alive.

## 🔌 Board wiring (TTGO LoRa32 T3 v1.6.1, SX1278)

| SX1278 | ESP32 GPIO | Role |
|--------|-----------|------|
| SCK    | 5  | SPI clock |
| MISO   | 19 | SPI in |
| MOSI   | 27 | SPI out |
| NSS    | 18 | SPI chip select |
| RST    | 23 | Radio reset |
| DIO0   | 26 | RadioLib IRQ |
| DIO1   | 33 | RadioLib IRQ |
| DIO2   | 32 | **OOK DATA — carrier keying** |

OLED (SSD1306): SDA 21, SCL 22, reset GPIO 16. Onboard LED (GPIO 25) mirrors the carrier.
If `beginFSK` fails, the `RST` pin is the usual culprit; if the radio initializes but no
RF is emitted, `DIO2` is not reaching GPIO32.

## 🎛️ Adding or recapturing a code

```bash
rtl_433 -A -f 433.92M       # on the workbench Pi; trigger the button
```

`rtl_433` prints the decoded codeword (e.g. `{18}7f45c`) and the pulse/gap widths. Set the
value in `BUTTONS[]` in `src/config.cpp`; if the timing differs, adjust the template
constants in `src/config.h`.

## ⚠️ Notes

- **RTL-SDR overload**: transmitting at close range into the RTL-SDR can saturate it. Keep
  `TX_POWER` low, add distance, or attenuate.
- **433.92 MHz is licensed spectrum.** Operate within legal power / duty-cycle limits for
  your region.

## 📚 Documentation

- [`documentation/ookTransmitter-fsd.md`](documentation/ookTransmitter-fsd.md) — full
  functional specification.
- [`homeassistant/awning.yaml`](homeassistant/awning.yaml) — Home Assistant automation +
  watchdog heartbeat.
