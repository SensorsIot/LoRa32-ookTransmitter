# Functional Specification — LoRa32 OOK Awning Controller

## 1. System Overview

### 1.1 Purpose

The device is an awning (Storen) controller on a TTGO LoRa32 (SX1278) board. It drives
the awning by transmitting the awning remote's **up** (retract) and **down** (extend)
433.92 MHz OOK commands, and implements the awning's motion model: the motor runs in one
direction until a *counter-command* halts it. The two directions are asymmetric — **down**
extends and is stopped by a counter-command to park at a chosen position, while **up**
runs to the closed mechanical end-stop and self-stops. A safety **emergency retract**
closes the awning fully. Wind safety is delegated to Home Assistant; the device fails
closed if HA's liveness heartbeat stops.

### 1.2 Goals

- Position the awning by absolute target in metres, transmitting the up/down codewords
  via RadioLib (SX1278 DIO2 keying).
- Control and monitor over Home Assistant (MQTT), a local web page, and the OLED.
- Fail safe (fully retracted) on loss of the Home Assistant automation.

### 1.3 Scope

In scope: awning position control, RF generation of the fixed OOK codewords, MQTT/HTTP
control, OLED status, and the safety watchdog. Out of scope: signal capture/decoding
(done externally with `rtl_433`), two-way RF, and any receive capability.

### 1.4 Definitions

| Term | Meaning |
|------|---------|
| OOK | On-Off Keying — carrier switched fully on/off to encode data |
| Codeword | The 18-bit fixed payload of one remote button |
| Extend / Retract | Awning moving out (down) / in (up) |
| Counter-stop | Opposite command sent once to halt a moving awning at a partial position |
| Position | Awning extension in metres, 0 = fully retracted |
| Re-home | Full retract that drives past the end-stop and resets position to 0 |

## 2. System Architecture

### 2.1 Components

- **MCU / Radio**: TTGO LoRa32 T3 v1.6.1 — ESP32 + Semtech SX1278 sub-GHz transceiver.
- **Display**: onboard SSD1306 128×64 OLED (I2C).
- **Firmware**: PlatformIO / Arduino, RadioLib.
- **Wind-safety controller (external)**: Home Assistant, using the site weather-station
  wind sensor, runs the awning automation and publishes the liveness heartbeat the device
  watches (FR-4.6). The device has no local wind sensor.
- **Interfaces**: MQTT (Home Assistant), a local HTTP page, USB serial (CH9102, 115200),
  and the OLED.

### 2.2 Architecture Summary

```
  Command (MQTT / HTTP / serial) ─► motion state machine ─► SX1278 OOK keying ─RF─► awning
                                          ▲
  HA awning/watchdog heartbeat ───────────┘  (fail-safe retract on loss)
```

The firmware holds the up/down codewords and their timing template, reconstructs the
pulse train, and keys the SX1278 carrier via DIO2.

### 2.3 Signal Model

- Modulation: OOK, pulse-width coded. Bit `0` = long pulse (~2150 µs); bit `1` = short
  pulse (~416 µs). Each bit is one pulse followed by a gap.
- Codeword length: 18 bits, transmitted MSB-first.
- Word repetition: each codeword is repeated (default 12×); a ~15.6 ms reset gap separates
  repetitions. Each motion action sends its codeword `CMD_REPEATS` times (default 5).
- Codewords are fixed (not rolling).

### 2.4 Pin Mapping (SX1278 ↔ ESP32)

| SX1278 | GPIO | | SX1278 | GPIO |
|--------|------|-|--------|------|
| SCK | 5 | | NSS | 18 |
| MISO | 19 | | RST | 23 |
| MOSI | 27 | | DIO0 | 26 |
| — | — | | DIO1 | 33 |
| — | — | | **DIO2** | **32 — OOK keying** |

OLED (SSD1306, I2C): SDA 21, SCL 22, reset GPIO 16. Onboard LED on GPIO 25 mirrors
carrier state.

### 2.5 Awning Motion Model

The device drives the awning by transmitting the up and down codewords through the
RadioLib SX1278 OOK path (§2.3); a state machine layers the motion behaviour on top. The
motor runs in one direction until a counter-command stops it, so the two directions are
asymmetric:

```
  waiting ─(extend to target)─► extending ─(delta reached)─► send UP once (counter-stop) ─► waiting (E)
  waiting ─(retract to 0)─────► retracting ─(runs to end-stop, re-home)──────────────────► waiting (R)
  any state ─(emergency)──────► retracting (full close, re-home)
```

- **Down / extend:** transmit the down codeword, run for the delta time, then transmit the
  up codeword once as a counter-stop. The awning parks at the target.
- **Up / retract:** transmit the up codeword; the awning runs to its closed end-stop and
  self-stops.
- **Emergency retract:** a full retract regardless of state — on demand, on loss of the
  Home Assistant heartbeat, or on an unsafe heartbeat (FR-4.6). Retracted is the safe
  state; the device fails closed.

Awning state is tracked as extended (`E`), moving (`M`), or retracted (`R`).

**Position tracking (open-loop, in metres, self-homing).** With no encoder, the device
tracks position in **metres** (0 = fully retracted … `MAX_TRAVEL_M` = fully extended),
derived from motor run-time via **`SPEED_M_PER_S`** (metres per second):
`move_time = |target − current| / SPEED_M_PER_S`.

Commands carry an **absolute target position** (metres); the device moves only the
**delta** from the current position — extend if the target is greater, retract if less —
after clamping the target to `[0, MAX_TRAVEL_M]` (no motion if equal). At 1 m, a target of
2 m moves +1 m.

A move to 0 commands retract for the tracked distance **plus a margin**, so the awning
reaches the end-stop, then **resets position to 0** (re-home). On boot the device performs
a full retract (longer than the maximum extension) to reach the known-safe closed state
(0 m). The HA cover position (0–100 %) maps to 0…`MAX_TRAVEL_M`.

## 3. Requirements

### 3.1 Functional Requirements

**RF Generation**
- **FR-1.1** [Must]: The device shall transmit on 433.92 MHz using OOK modulation via the
  SX1278.
- **FR-1.2** [Must]: The device shall reconstruct each codeword's pulse train from a
  stored 18-bit value and a shared timing template (long/short pulse, gaps).
- **FR-1.3** [Must]: The device shall hold the four remote codewords — up (`0x7f454`),
  down (`0x7f45c`), auto (`0x7f480`), manual (`0x7f484`); motion uses up and down.
- **FR-1.4** [Must]: The device shall transmit each codeword MSB-first, 18 bits.
- **FR-1.5** [Should]: The device shall repeat each codeword a configurable number of
  times (default 12) with a ~15.6 ms reset gap between repetitions.

**Control & Operation**
- **FR-2.1** [Should]: The device shall accept a command on receipt of the matching serial
  key: `u` (up), `d` (down), `a`, `m`.
- **FR-2.2** [May]: The device shall mirror carrier state on the onboard LED.

**Initialization & Diagnostics**
- **FR-3.1** [Must]: The device shall initialize the SX1278 in FSK/OOK mode and, on
  initialization failure, halt transmission and emit a diagnostic message.
- **FR-3.2** [Should]: The device shall log readiness and each transmission over serial at
  115200 baud.

**Awning Motion Control**
- **FR-4.1** [Must]: The device shall drive the awning using the up (retract) and down
  (extend) codewords via the RadioLib SX1278 OOK path (§2.3, FR-1.x), transmitted
  `CMD_REPEATS` times per action (default 5).
- **FR-4.2** [Must]: To reach a target further open than the current position, the device
  shall transmit the down codeword, run for `delta / SPEED_M_PER_S`, then transmit the up
  codeword once as a counter-stop, parking at the target (state `E`).
- **FR-4.3** [Must]: On a retract command the device shall transmit the up codeword; the
  awning self-stops at its closed end-stop (state `R`).
- **FR-4.4** [Must]: The device shall accept an **absolute target position in metres** and
  move only the **delta** from the current position (extend if greater, retract if less),
  converting distance↔time via `SPEED_M_PER_S`. Targets are clamped to `[0, MAX_TRAVEL_M]`;
  no motion if already at the target.
- **FR-4.5** [Must]: The device shall provide an emergency-retract command that immediately
  performs a full retract regardless of current state.
- **FR-4.6** [Must]: The device shall monitor the Home Assistant automation heartbeat
  (topic `awning/watchdog`, ~30 s interval). If no heartbeat arrives within
  `EMERGENCY_TIMEOUT_S` (default 120 s), or a heartbeat signals an unsafe condition, the
  device shall emergency-retract. Only the heartbeat resets the watchdog — ordinary command
  traffic does not — so the timer proves the wind-safety automation is alive.
- **FR-4.7** [Should]: The device shall track and report awning state — extended (`E`),
  moving (`M`), retracted (`R`).
- **FR-4.8** [Should]: The device shall ignore a command that matches the current position
  (no redundant motion).
- **FR-4.9** [Must]: The device shall track awning position in metres open-loop
  (accumulated motor run-time × `SPEED_M_PER_S`). Extending increases it, retracting
  decreases it, and a full retract resets it to 0.
- **FR-4.10** [Must]: Every full / emergency / boot retract shall drive the motor for the
  tracked position plus `RETRACT_MARGIN_S` to guarantee the closed end-stop and re-home the
  position to 0. At boot the device shall perform a full retract (`FULL_RETRACT_S`) to reach
  the known-safe closed state.

**Status Display (OLED)**
- **FR-5.1** [Should]: The device shall drive the onboard SSD1306 128×64 OLED (I2C,
  SDA 21 / SCL 22) to present operating status.
- **FR-5.2** [Should]: The display shall show the awning state prominently — `RETRACTED`,
  `MOVING`, or `EXTENDED` — with a direction indicator (§5.4).
- **FR-5.3** [Should]: During a down movement the display shall show a progress bar and
  countdown to the counter-stop (FR-4.2).
- **FR-5.4** [Should]: The display shall show connectivity status — WiFi (SSID/IP, or
  AP-provisioning) and MQTT (connected/disconnected).
- **FR-5.5** [May]: The display shall show the last command and its result.
- **FR-5.6** [May]: The display shall blank or dim after a configurable idle period, waking
  on any state change.

### 3.2 Non-Functional Requirements

- **NFR-1** [Must]: Reproduced pulse widths shall fall within the target receiver's timing
  tolerance (~±690 µs) so the awning receiver accepts them.
- **NFR-2** [Should]: TX power shall be modest to avoid overloading a nearby RTL-SDR during
  bench checks.
- **NFR-3** [Must]: Operation shall stay within regional 433 MHz ISM power/duty-cycle
  limits.
- **NFR-4** [May]: Firmware shall fit within the board's flash and RAM.

## 4. Risks, Assumptions & Dependencies

| ID | Type | Description | Mitigation |
|----|------|-------------|------------|
| R-1 | Risk | Wrong `LORA_RST` for the board prevents radio init. | `LORA_RST` set to 23; diagnostic on `beginFSK` failure. |
| R-2 | Risk | Transmitting at close range overloads a nearby RTL-SDR. | Low TX power, distance, or attenuation (NFR-2). |
| R-3 | Risk | Open-loop position drifts while extending. | Every full retract re-homes to 0 (FR-4.10). |
| R-4 | Risk | A fast gust while HA is alive but slow to react leaves the awning exposed; no local sensor can beat HA's reaction. | Conservative wind threshold and prompt retract in the HA automation. |
| A-1 | Assumption | The awning remote uses fixed (non-rolling) codes. | — |
| A-2 | Assumption | Wind data exists only in Home Assistant; the device delegates wind safety to HA. | Heartbeat watchdog retracts on HA/automation/link loss (FR-4.6). |
| D-1 | Dependency | RadioLib SX127x driver (OOK direct mode). | Pinned in `platformio.ini`. |
| D-2 | Dependency | Home Assistant awning automation publishing `awning/watchdog`. | FR-4.6, §9.4. |

## 5. Interfaces

### 5.1 RF Interface
- Frequency: 433.92 MHz. Modulation: OOK, PWM-coded.
- Timing template: long pulse ≈2150 µs / space ≈172 µs; short pulse ≈416 µs / space
  ≈1908 µs; inter-word reset ≈15.6 ms.
- Payload: 18-bit fixed codeword, MSB-first.

### 5.2 Serial Interface
- 115200 baud, 8N1, USB CDC (CH9102).
- Input keys: `u` = up, `d` = down, `a`, `m`.
- Output: readiness banner and per-transmission log lines.

### 5.3 Configuration Constants (firmware)
| Constant | Default | Meaning |
|----------|---------|---------|
| `FREQ_MHZ` | 433.92 | Carrier frequency |
| `TX_POWER` | 10 dBm | SX1278 output power |
| `REPEATS` | 12 | Word repetitions per transmission |
| `BUTTONS[]` | 4 codewords | Remote codes |
| `CMD_REPEATS` | 5 | Codeword transmissions per motion action (FR-4.1) |
| `SPEED_M_PER_S` | calibrate | Awning speed: metres per second of motor run (FR-4.4, FR-4.9) |
| `MAX_TRAVEL_M` | calibrate | Full extension in metres (cover 100 % / open) (FR-4.4) |
| `EMERGENCY_TIMEOUT_S` | 120 | Heartbeat-loss watchdog before auto-retract (FR-4.6) |
| `WATCHDOG_TOPIC` | `awning/watchdog` | HA automation heartbeat the watchdog monitors (FR-4.6) |
| `RETRACT_MARGIN_S` | 5 | Extra retract time beyond tracked position to guarantee end-stop / re-home (FR-4.10) |
| `FULL_RETRACT_S` | 40 | Boot/emergency full-close drive time (FR-4.10) |

### 5.4 OLED Status Display

128×64 SSD1306 over I2C (SDA 21 / SCL 22): a header with device name and connectivity
indicators, a large centred awning state with a direction arrow, and a footer with the
last command and IP. During a down movement the state area shows a progress bar and
countdown to the counter-stop.

```
 Idle / at rest                     During a down movement
 +----------------------+          +----------------------+
 | Awning      wifi mqtt |          | Awning      wifi mqtt |
 |                      |          |  EXTENDING           |
 |   ^   RETRACTED      |          |  [########----] 1.5m |
 |                      |          |  stop at 1.5 m       |
 | 192.168.0.x          |          | 192.168.0.x          |
 +----------------------+          +----------------------+
```

- State glyphs: `^` retracted (in), `v` extended (out), animated during `MOVING`.
- Library: U8g2, or Adafruit SSD1306 + GFX.

## 6. Operational Procedures

### 6.1 Build & Flash
- Build: `pio run`.
- Flash (local USB): `pio run -t upload`.
- Flash (workbench RFC2217): `pio run -t upload --upload-port 'rfc2217://<host>:<port>'`.
  If the board does not enter download mode over RFC2217, flash locally via the workbench
  `POST /api/flash`.

### 6.2 Normal Operation
- On power-up the firmware initializes the radio and performs a full retract to the
  known-safe closed state (position 0).
- The device positions the awning on command from Home Assistant (MQTT), the web page, or
  serial.

### 6.3 Adding / Recapturing a Remote Code
1. Capture on the RTL-SDR: `rtl_433 -A -f 433.92M`, trigger the button.
2. Read the decoded `{18}` codeword.
3. Set the value in `BUTTONS[]`; adjust the timing template if the pulse widths differ.
4. Rebuild, flash, and verify (Section 7).

### 6.4 Recovery
- Radio init failure: check `LORA_RST` and wiring; re-flash.
- No RF: check frequency, antenna, and TX power; confirm the 433 MHz SX1278.

## 7. Verification & Validation

### 7.1 Test Environment
Workbench RTL-SDR running `rtl_433 -f 433.92M` (or `-A`), LoRa32 with antenna at bench
distance and reduced TX power.

### 7.2 Functional Tests

| ID | Objective | Steps | Expected Result |
|----|-----------|-------|-----------------|
| TC-1 | Radio init | Power up; observe serial | Readiness banner; no init-failure diagnostic (FR-3.1, FR-3.2) |
| TC-2 | Carrier on 433.92 MHz | Trigger a command; observe SDR | Carrier bursts centred on 433.92 MHz (FR-1.1) |
| TC-3 | Down codeword | Send `d`; run `rtl_433` | Decodes `0x7f45c` (FR-1.2, FR-1.3, FR-1.4) |
| TC-4 | Up codeword | Send `u`; run `rtl_433` | Decodes `0x7f454` (FR-1.3) |
| TC-5 | Word repetition | Capture one transmission | Codeword repeats ~12× with ~15.6 ms reset gaps (FR-1.5) |
| TC-6 | Serial trigger | Send `u`/`d` | Matching code transmitted; log line printed (FR-2.1, FR-3.2) |
| TC-7 | LED mirror | Observe LED during TX | LED follows carrier activity (FR-2.2) |
| TC-8 | Timing tolerance | Compare replayed vs original pulse widths | Within receiver tolerance; awning responds (NFR-1) |
| TC-9 | Init-failure path | Set wrong `LORA_RST`; power up | Diagnostic emitted; no transmission (FR-3.1) |
| TC-10 | Extend to target | From 0, command extend to 1 m | Down codeword sent, up counter-stop after ~(1 m / `SPEED_M_PER_S`); parks near 1 m; state `E` (FR-4.1, FR-4.2) |
| TC-11 | Absolute-position delta | At 1 m, command extend to 2 m | Moves only +1 m, not a full 2 m (FR-4.4) |
| TC-12 | Target clamp / no-op | Command a target > `MAX_TRAVEL_M`, then one equal to current | Clamped to max; no motion when at target (FR-4.4) |
| TC-13 | Retract | Command retract to 0 | Up codeword sent; awning to end-stop; state `R`; position 0 (FR-4.1, FR-4.3) |
| TC-14 | Emergency retract | Issue emergency while extended | Immediate full retract regardless of state (FR-4.5) |
| TC-15 | Heartbeat watchdog | Stop `awning/watchdog` past `EMERGENCY_TIMEOUT_S` while still sending ordinary commands | Retract fires on heartbeat loss; ordinary commands do not reset it (FR-4.6) |
| TC-16 | State reporting | Drive up/down; read state | E → M → R / R → M → E reported (FR-4.7) |
| TC-17 | No-op command | Command the current position | Ignored; no motion (FR-4.8) |
| TC-18 | Position / re-home | Extend, then retract; read position | Position rises while open; retract overshoots and resets to 0 (FR-4.9, FR-4.10) |
| TC-19 | Boot homing | Power up with the awning partly open | Full retract to closed, position 0 (FR-4.10) |
| TC-20 | OLED state | Power up; drive up/down | OLED shows RETRACTED/MOVING/EXTENDED with glyph (FR-5.1, FR-5.2) |
| TC-21 | OLED countdown | Extend to a target | OLED shows progress bar + countdown to counter-stop (FR-5.3) |
| TC-22 | OLED connectivity | Observe header | Shows WiFi/MQTT indicators (FR-5.4) |

### 7.3 Acceptance Criteria
All **Must** FRs pass; each codeword re-decodes identically on the RTL-SDR; the awning
reaches commanded positions and fully retracts on emergency and on heartbeat loss.

### 7.4 Traceability Matrix

| Requirement | Priority | Tests | Status |
|-------------|----------|-------|--------|
| FR-1.1 | Must | TC-2 | Covered |
| FR-1.2 | Must | TC-3, TC-8 | Covered |
| FR-1.3 | Must | TC-3, TC-4 | Covered |
| FR-1.4 | Must | TC-3 | Covered |
| FR-1.5 | Should | TC-5 | Covered |
| FR-2.1 | Should | TC-6 | Covered |
| FR-2.2 | May | TC-7 | Covered |
| FR-3.1 | Must | TC-1, TC-9 | Covered |
| FR-3.2 | Should | TC-1, TC-6 | Covered |
| FR-4.1 | Must | TC-10, TC-13 | Covered |
| FR-4.2 | Must | TC-10 | Covered |
| FR-4.3 | Must | TC-13 | Covered |
| FR-4.4 | Must | TC-11, TC-12 | Covered |
| FR-4.5 | Must | TC-14 | Covered |
| FR-4.6 | Must | TC-15 | Covered |
| FR-4.7 | Should | TC-16 | Covered |
| FR-4.8 | Should | TC-17 | Covered |
| FR-4.9 | Must | TC-18 | Covered |
| FR-4.10 | Must | TC-18, TC-19 | Covered |
| FR-5.1 | Should | TC-20 | Covered |
| FR-5.2 | Should | TC-20 | Covered |
| FR-5.3 | Should | TC-21 | Covered |
| FR-5.4 | Should | TC-22 | Covered |
| FR-5.5 | May | TC-20 | Covered |
| FR-5.6 | May | — | GAP |
| NFR-1 | Must | TC-8 | Covered |
| NFR-2 | Should | TC-2 | Covered |
| NFR-3 | Must | Operational control (Section 6) | Procedural |
| NFR-4 | May | Build size report | Covered |

## 8. Troubleshooting

| Symptom | Likely Cause | Action |
|---------|--------------|--------|
| `beginFSK failed` on boot | Wrong `LORA_RST` | Set RST to 23; re-flash |
| No RF on any command | Wrong band SX1276, or no antenna | Confirm 433 MHz SX1278; attach antenna |
| RTL-SDR overloads | TX too strong at close range | Lower `TX_POWER`; add distance/attenuation |
| Awning doesn't reach position | `SPEED_M_PER_S` miscalibrated | Recalibrate speed against a measured run |
| Awning retracts unexpectedly | `awning/watchdog` heartbeat missing | Check the HA automation and MQTT link |
| Flash fails: `Wrong boot mode (0x13)` | RFC2217 auto-reset can't enter bootloader | Flash locally via workbench `POST /api/flash` |

## 9. Appendix

### 9.1 Remote Codewords
| Button | Code (`{18}` hex) | Use |
|--------|-------------------|-----|
| up | `0x7f454` | retract |
| down | `0x7f45c` | extend |
| auto | `0x7f480` | unused |
| manual | `0x7f484` | unused |

### 9.2 Toolchain
- PlatformIO (`espressif32`), Arduino, RadioLib; U8g2 (or Adafruit SSD1306 + GFX).
- Board: `ttgo-lora32-v21`.
- Capture: `rtl_433` on the workbench RTL-SDR (RTL2838, R820T tuner).

### 9.3 Repository Layout
- `src/main.cpp` — firmware (codewords, timing template, keying, motion, control).
- `platformio.ini` — build/upload configuration.
- `README.md` — quick-start and capture workflow.
- `documentation/ookTransmitter-fsd.md` — this document.

### 9.4 Home Assistant Awning Automation

Home Assistant is the wind-safety authority. Its awning automation commands the device
over MQTT and publishes the `awning/watchdog` heartbeat (FR-4.6). Automation parameters,
driven by the weather-station wind, lux, and temperature:

- Extend when `Lux ≥ 3.0` **and** `temp ≥ 20 °C` **and** `wind ≤ 20`.
- Retract when `Lux < 1.0` **or** `temp < 18 °C`.
- Emergency retract when `wind > 25`.
- Move at most once per 10 minutes.

MQTT topics: `awning/command` (targets to the device), `awning/status` (device state),
`awning/watchdog` (HA liveness heartbeat).

## 10. Related

- [[Embedded-Workbench-FSD]] — the workbench hosting the RTL-SDR receiver and flashing the
  device.
- [[signal-generator]] — the workbench RF transmit source (Si5351/GPCLK).
