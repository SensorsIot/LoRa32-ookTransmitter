# Functional Specification — LoRa32 OOK Transmitter

## 1. System Overview

### 1.1 Purpose

The LoRa32 OOK Transmitter is a single-board firmware that reproduces ("replays")
433.92 MHz on-off-keyed (OOK) remote-control signals. Signals are first captured
from an original remote using an RTL-SDR receiver and `rtl_433`; the resulting
fixed codewords are stored in firmware and re-emitted on demand by a TTGO LoRa32
(SX1278) board. It is the transmit half of a capture-and-replay workflow, paired
with the RTL-SDR receiver on the Universal Embedded Workbench.

Its application is an **awning (Storen) controller**: it reproduces the awning
remote's **up** (retract) and **down** (extend) 433.92 MHz commands and implements
the awning's motion model, in which the motor runs in one direction until a
*counter-command* halts it. The two directions are therefore **asymmetric** — **down**
is a timed sequence (extend → wait → counter-stop, parking at a partial extension)
while **up** runs to the closed mechanical end-stop and self-stops. A safety
**emergency retract** (on demand and on a no-command watchdog) closes the awning fully.
This behaviour is specified in §2.5 and §4.1; its derivation from the legacy control
system is recorded in §10.4.

### 1.2 Goals

- Faithfully replay captured fixed-code OOK-PWM remote signals on 433.92 MHz via
  RadioLib (SX1278 DIO2 keying) — the legacy bit-banged encoding is not used.
- Control the awning: extend to a timed partial position (**down**), retract to the
  closed end-stop (**up**), and retract immediately on an emergency command.
- Use only the up/down codewords for motion; auto/manual are captured but unused.
- Remain reproducible and version-controlled so new remotes can be added easily.

### 1.3 Scope

In scope: RF generation of pre-captured fixed OOK-PWM codewords, serial control,
board bring-up. Out of scope: signal capture/decoding (performed externally by
`rtl_433`), rolling-code cracking, two-way RF, and any receive capability.

### 1.4 Definitions

| Term | Meaning |
|------|---------|
| OOK | On-Off Keying — carrier is switched fully on/off to encode data |
| PWM | Pulse-Width Modulation — bit value encoded by pulse duration |
| Codeword | The 18-bit fixed payload of one button press |
| Mark / Space | Carrier-on interval / carrier-off interval |
| Reset gap | Long inter-word gap that delimits repeated codewords |
| DUT | Device Under Test (here, the LoRa32 board on the workbench) |

## 2. System Architecture

### 2.1 Components

- **MCU / Radio**: TTGO LoRa32 v2.1 — ESP32 + Semtech SX1278 sub-GHz transceiver.
- **Firmware**: PlatformIO / Arduino framework using the RadioLib library.
- **Capture toolchain (external)**: RTL-SDR + `rtl_433` on the workbench Raspberry Pi.
- **Host/operator interface**: USB serial (CH9102) at 115200 baud.

### 2.2 Architecture Summary

```
  Original remote ──RF──► RTL-SDR + rtl_433 ──► codeword + timing (offline capture)
                                                        │
                                                        ▼  stored in BUTTONS[] / template
  Serial / auto-cycle ──► SX1278 OOK keying ──RF──► replayed 433.92 MHz codeword
```

The firmware holds each button's codeword and a shared OOK-PWM timing template.
On trigger, it reconstructs the pulse train bit-by-bit and keys the SX1278 carrier.

### 2.3 Signal Model

- Modulation: OOK, pulse-width coded. Bit `0` = long pulse (~2150 µs); bit `1` =
  short pulse (~416 µs). Each bit is one pulse followed by a gap.
- Codeword length: 18 bits, transmitted MSB-first.
- Word repetition: each codeword is repeated (default 12×); a ~15.6 ms reset gap
  separates repetitions.
- Codewords are **fixed** (not rolling), which is what makes replay valid.

### 2.4 Pin Mapping (SX1278 ↔ ESP32)

| SX1278 | GPIO | | SX1278 | GPIO |
|--------|------|-|--------|------|
| SCK | 5 | | NSS | 18 |
| MISO | 19 | | RST | 23 (v2.1) / 14 (v1.0) |
| MOSI | 27 | | DIO0 | 26 |
| — | — | | DIO1 | 33 |

Onboard LED on GPIO 25 mirrors carrier state.

### 2.5 Awning Motion Model

The device drives the awning by transmitting the **up** (retract) and **down**
(extend) codewords through the same RadioLib SX1278 OOK path as any other codeword
(§2.3); the motion behaviour is layered on top as a small state machine. The awning
motor runs continuously in one direction until a *counter-command* stops it, so the two
directions are asymmetric:

```
  waiting ──(down cmd)──► extending ──(after movement time)──► send UP once (counter-stop) ──► waiting (state E)
  waiting ──(up cmd)────► retracting ─(runs to closed end-stop)─────────────────────────────► waiting (state R)
  any state ─(emergency)─► retracting (full close)
```

- **Down / extend:** transmit the **down** codeword → wait a configurable **movement
  time** (seconds) → transmit the **up** codeword *once* as a counter-stop. The awning
  parks at the partial extension reached during that time.
- **Up / retract:** transmit the **up** codeword; the awning runs to its closed
  mechanical end-stop and self-stops. No counter-stop.
- **Emergency retract:** a full retract, triggered on demand or by a no-command
  watchdog, regardless of current state.

Each codeword is transmitted several times per action (default 5×, per the RF model in
§2.3). Awning state is tracked as extended (`E`), moving (`M`), or retracted (`R`). Only
the up/down codewords are used for motion; the auto/manual codewords (§10.1) are not.

## 3. Development Phases

### Phase 1 — Board & Radio Foundation
- **Scope**: Board bring-up, SPI init, SX1278 initialization in FSK/OOK mode,
  carrier on/off keying primitive.
- **Deliverables**: Firmware that initializes the radio at 433.92 MHz and can key
  the carrier; serial status output.
- **Exit criteria**: `beginFSK` returns success; carrier presence observable on an
  SDR; init failure halts with a diagnostic.
- **Dependencies**: RadioLib, correct board variant / RST pin.

### Phase 2 — Codeword Replay
- **Scope**: Codeword table (`BUTTONS[]`), timing template, pulse-train
  reconstruction, word repetition, hands-free auto-cycle, serial trigger.
- **Deliverables**: All four captured buttons replay correctly; serial `u/d/a/m`
  triggers; auto-cycle mode.
- **Exit criteria**: An RTL-SDR running `rtl_433` re-decodes each replayed codeword
  identically to the original capture.
- **Dependencies**: Phase 1; captured codewords.

### Phase 3 — Timing Precision & Extensibility (optional)
- **Scope**: Microsecond-accurate keying via SX1278 DIO2 direct data mode;
  additional remotes/protocols; adjustable TX power/attenuation.
- **Deliverables**: DIO2-based keying path; documented process for adding remotes.
- **Exit criteria**: Short intervals (<200 µs) reproduced within receiver tolerance.
- **Dependencies**: Phase 2; DIO2 availability on the board variant.

## 4. Requirements

### 4.1 Functional Requirements

**RF Generation**
- **FR-1.1** [Must]: The device shall transmit on 433.92 MHz using OOK modulation
  via the SX1278.
- **FR-1.2** [Must]: The device shall reconstruct each codeword's pulse train from a
  stored 18-bit value and a shared timing template (long/short pulse, gaps).
- **FR-1.3** [Must]: The device shall support the four captured buttons — up
  (`0x7f454`), down (`0x7f45c`), auto (`0x7f480`), manual (`0x7f484`).
- **FR-1.4** [Must]: The device shall transmit each codeword MSB-first, 18 bits.
- **FR-1.5** [Should]: The device shall repeat each codeword a configurable number of
  times (default 12) with a ~15.6 ms reset gap between repetitions.

**Control & Operation**
- **FR-2.1** [Must]: The device shall auto-cycle through all buttons, transmitting
  each in turn with a configurable inter-button delay (default 3 s), when no serial
  command is pending.
- **FR-2.2** [Should]: The device shall transmit a specific button on receipt of the
  matching serial key: `u`, `d`, `a`, `m`.
- **FR-2.3** [May]: The device shall mirror carrier state on the onboard LED.

**Awning Motion Control**
- **FR-4.1** [Must]: The device shall drive the awning using the **up** (retract) and
  **down** (extend) codewords transmitted via the existing RadioLib SX1278 OOK path
  (§2.3, FR-1.x) — not the legacy bit-banged encoding (§10.4). Each command shall be
  transmitted a configurable number of times per action (default 5).
- **FR-4.2** [Must]: On a **down/extend** command the device shall transmit the down
  codeword, wait a configurable **movement time**, then transmit the up codeword **once
  as a counter-stop**, leaving the awning at a partial extension (state `E`).
- **FR-4.3** [Must]: On an **up/retract** command the device shall transmit the up
  codeword; the awning self-stops at its closed end-stop (state `R`). No counter-stop is
  sent.
- **FR-4.4** [Must]: The movement time shall be configurable **in seconds** and shall
  not be restricted to the legacy single-digit `dur × 5 s` encoding. Default 30 s (the
  legacy `X6` value).
- **FR-4.5** [Must]: The device shall provide an **emergency-retract command** that
  immediately performs a full retract regardless of current state (wind/storm safety).
- **FR-4.6** [Should]: The device shall perform an **automatic emergency retract** if no
  command is received within a configurable watchdog timeout (default 120 s).
- **FR-4.7** [Should]: The device shall track and report awning state — extended (`E`),
  moving (`M`), retracted (`R`).
- **FR-4.8** [Should]: The device shall ignore a direction command that conflicts with
  the current state (e.g. extend while already extended) to avoid redundant motion.

**Initialization & Diagnostics**
- **FR-3.1** [Must]: The device shall initialize the SX1278 in FSK/OOK mode and, on
  initialization failure, halt further transmission and emit a diagnostic message.
- **FR-3.2** [Should]: The device shall log readiness and each transmission (button
  name and code) over serial at 115200 baud.

> **Note:** the hands-free auto-cycle of FR-2.1 is a bring-up/test behaviour only; it
> must not run in awning-control operation, where motion occurs solely on command.

### 4.2 Non-Functional Requirements

- **NFR-1** [Must]: Reproduced pulse widths shall fall within the target receiver's
  timing tolerance (~±690 µs observed) so that the original receiver accepts them.
- **NFR-2** [Should]: Default TX power shall be modest to avoid overloading a
  nearby RTL-SDR during bench verification.
- **NFR-3** [Must]: Operation shall be limited to signals the operator is authorized
  to transmit, within regional 433 MHz ISM power/duty-cycle limits.
- **NFR-4** [May]: Firmware shall fit comfortably within board resources (baseline:
  ~23% flash, ~7% RAM).

## 5. Risks, Assumptions & Dependencies

| ID | Type | Description | Mitigation |
|----|------|-------------|------------|
| R-1 | Risk | Carrier keyed via SPI mode switches (`transmitDirect`/`standby`) has latency; short intervals (172 µs) may be distorted. | Large receiver tolerance absorbs it; Phase 3 DIO2 direct keying for precision. |
| R-2 | Risk | Board variant RST pin differs (23 on v2.1, 14 on v1.0); wrong pin → init failure. | Documented `LORA_RST` define; diagnostic on `beginFSK` failure. |
| R-3 | Risk | Transmitting at close range overloads the RTL-SDR receiver. | Low TX power, physical distance, or attenuation (NFR-2). |
| A-1 | Assumption | Target remote uses fixed (non-rolling) codes. | Verified: identical codeword across all presses. |
| A-2 | Assumption | All four buttons share one timing template. | Verified: identical timing across captures. |
| D-1 | Dependency | RadioLib SX127x driver (OOK direct mode). | Pinned in `platformio.ini`. |
| D-2 | Dependency | External capture via `rtl_433` for any new remote. | Documented workflow. |

## 6. Interfaces

### 6.1 RF Interface
- Frequency: 433.92 MHz. Modulation: OOK, PWM-coded.
- Timing template: long pulse ≈2150 µs / space ≈172 µs; short pulse ≈416 µs /
  space ≈1908 µs; inter-word reset ≈15.6 ms.
- Payload: 18-bit fixed codeword, MSB-first.

### 6.2 Serial Interface
- 115200 baud, 8N1, USB CDC (CH9102).
- Input commands (single character): `u` = up, `d` = down, `a` = auto, `m` = manual.
- Output: readiness banner and per-transmission log lines `TX <name> (0x<code>)`.

### 6.3 Configuration Constants (firmware)
| Constant | Default | Meaning |
|----------|---------|---------|
| `FREQ_MHZ` | 433.92 | Carrier frequency |
| `TX_POWER` | 10 dBm | SX1278 output power |
| `REPEATS` | 12 | Word repetitions per transmission |
| `PERIOD_MS` | 3000 | Delay between buttons in auto-cycle (test mode) |
| `BUTTONS[]` | 4 codewords | Captured remote codes |
| `CMD_REPEATS` | 5 | Codeword transmissions per motion action (FR-4.1) |
| `MOVEMENT_TIME_S` | 30 | Default down extend→counter-stop delay, seconds (FR-4.4) |
| `EMERGENCY_TIMEOUT_S` | 120 | No-command watchdog before auto-retract (FR-4.6) |

## 7. Operational Procedures

### 7.1 Build & Flash
- Build: `pio run`.
- Flash (local USB): `pio run -t upload`.
- Flash (workbench RFC2217): `pio run -t upload --upload-port
  'rfc2217://<host>:<port>'`. If the board does not enter download mode via
  DTR/RTS auto-reset, force download mode (hold GPIO0 low during reset) — see
  Section 9.

### 7.2 Normal Operation
- On power-up the firmware initializes the radio and prints a readiness banner.
- With no serial input, it auto-cycles all four buttons.
- Sending `u`/`d`/`a`/`m` transmits the corresponding button immediately.

### 7.3 Adding a New Remote / Button
1. Capture on the RTL-SDR: `rtl_433 -A -f 433.92M`, trigger the button.
2. Record the decoded `{18}` codeword; confirm it is identical across presses.
3. Add `{ "name", 0x<code> }` to `BUTTONS[]`. Adjust the timing template constants
   only if the new remote's pulse widths differ.
4. Rebuild, flash, and verify (Section 8).

### 7.4 Recovery
- Radio init failure: check board variant `LORA_RST` and wiring; re-flash.
- No RF observed: verify frequency, antenna, and TX power; confirm SX1278 (433 MHz
  variant, not 868/915).

## 8. Verification & Validation

### 8.1 Test Environment
Workbench RTL-SDR running `rtl_433 -f 433.92M` (or `-A`), LoRa32 DUT with antenna
at bench distance and reduced TX power.

### 8.2 Functional Tests

| ID | Objective | Steps | Expected Result |
|----|-----------|-------|-----------------|
| TC-1 | Radio init | Power up DUT; observe serial | Readiness banner printed; no init-failure diagnostic (FR-3.1, FR-3.2) |
| TC-2 | Carrier on 433.92 MHz | Trigger any button; observe SDR spectrum | Carrier bursts appear centered on 433.92 MHz (FR-1.1) |
| TC-3 | Down replay | Send `d`; run `rtl_433` | Decodes `0x7f45c` (FR-1.2, FR-1.3, FR-1.4) |
| TC-4 | Up replay | Send `u`; run `rtl_433` | Decodes `0x7f454` (FR-1.3) |
| TC-5 | Auto replay | Send `a`; run `rtl_433` | Decodes `0x7f480` (FR-1.3) |
| TC-6 | Manual replay | Send `m`; run `rtl_433` | Decodes `0x7f484` (FR-1.3) |
| TC-7 | Word repetition | Capture one transmission | Codeword repeats ~12× with ~15.6 ms reset gaps (FR-1.5) |
| TC-8 | Auto-cycle | Leave DUT idle; observe serial + SDR | All four codes transmitted in sequence with ~3 s spacing (FR-2.1) |
| TC-9 | Serial trigger | Send each of `u/d/a/m` | Matching code transmitted; log line printed (FR-2.2, FR-3.2) |
| TC-10 | LED mirror | Observe LED during TX | LED follows carrier activity (FR-2.3) |
| TC-11 | Timing tolerance | Compare replayed vs original pulse widths | Within receiver tolerance; original receiver actuates (NFR-1) |
| TC-12 | Init-failure path | Set wrong `LORA_RST`; power up | Diagnostic emitted; no transmission (FR-3.1) |
| TC-13 | Down sequence | Issue down (movement time e.g. 5 s); observe RF + awning | Down codeword sent, up codeword sent once ~5 s later; awning parks partially open; state `E` (FR-4.1, FR-4.2, FR-4.4) |
| TC-14 | Up sequence | Issue up; observe RF + awning | Up codeword sent; awning runs to closed end-stop, no counter-stop; state `R` (FR-4.1, FR-4.3) |
| TC-15 | Emergency retract command | Issue emergency command while extended | Immediate full retract regardless of state (FR-4.5) |
| TC-16 | Emergency watchdog | Send no command past the watchdog timeout | Automatic full retract occurs (FR-4.6) |
| TC-17 | State reporting | Drive up/down; read reported state | State transitions E → M → R / R → M → E reported (FR-4.7) |
| TC-18 | Conflicting command ignored | Issue down while already extended | Command ignored; no motion (FR-4.8) |

### 8.3 Acceptance Criteria
All **Must** FRs pass; each replayed codeword re-decodes identically to its original
capture; the physical target device responds to at least one replayed button.

### 8.4 Traceability Matrix

| Requirement | Priority | Tests | Status |
|-------------|----------|-------|--------|
| FR-1.1 | Must | TC-2 | Covered |
| FR-1.2 | Must | TC-3, TC-11 | Covered |
| FR-1.3 | Must | TC-3, TC-4, TC-5, TC-6 | Covered |
| FR-1.4 | Must | TC-3, TC-7 | Covered |
| FR-1.5 | Should | TC-7 | Covered |
| FR-2.1 | Must | TC-8 | Covered |
| FR-2.2 | Should | TC-9 | Covered |
| FR-2.3 | May | TC-10 | Covered |
| FR-3.1 | Must | TC-1, TC-12 | Covered |
| FR-3.2 | Should | TC-1, TC-9 | Covered |
| FR-4.1 | Must | TC-13, TC-14 | Covered |
| FR-4.2 | Must | TC-13 | Covered |
| FR-4.3 | Must | TC-14 | Covered |
| FR-4.4 | Must | TC-13 | Covered |
| FR-4.5 | Must | TC-15 | Covered |
| FR-4.6 | Should | TC-16 | Covered |
| FR-4.7 | Should | TC-17 | Covered |
| FR-4.8 | Should | TC-18 | Covered |
| NFR-1 | Must | TC-11 | Covered |
| NFR-2 | Should | TC-2 (bench setup) | Covered |
| NFR-3 | Must | Operational control (Section 7) | Procedural |
| NFR-4 | May | Build size report | Covered |

## 9. Troubleshooting

| Symptom | Likely Cause | Action |
|---------|--------------|--------|
| `beginFSK failed` on boot | Wrong `LORA_RST` for board variant | Set RST to 23 (v2.1) or 14 (v1.0); re-flash |
| No RF on any button | 868/915 MHz SX1276 fitted, or no antenna | Confirm 433 MHz SX1278 variant; attach antenna |
| RTL-SDR shows garbage / overload | TX too strong at close range | Lower `TX_POWER`; add distance/attenuation |
| Original receiver ignores replay | Short-pulse timing distorted by SPI keying | Reduce interference; use Phase 3 DIO2 direct keying |
| Flash fails: `Wrong boot mode (0x13)` | DTR/RTS auto-reset not entering bootloader over RFC2217 | Force download mode (GPIO0 low at reset), then upload |

## 10. Appendix

### 10.1 Captured Codewords
| Button | Code (`{18}` hex) | Notes |
|--------|-------------------|-------|
| up | `0x7f454` | Fixed |
| down | `0x7f45c` | Fixed |
| auto | `0x7f480` | Fixed |
| manual | `0x7f484` | Fixed; differs from up by one bit |

### 10.2 Toolchain
- PlatformIO (`espressif32`), Arduino framework, RadioLib.
- Board: `ttgo-lora32-v21`.
- Capture: `rtl_433` v25.02 on the workbench RTL-SDR (RTL2838, R820T tuner).

### 10.3 Repository Layout
- `src/main.cpp` — firmware (codeword table, timing template, keying, control).
- `platformio.ini` — build/upload configuration.
- `README.md` — quick-start and capture workflow.
- `documentation/ookTransmitter-fsd.md` — this document.

### 10.4 Legacy Awning Control (Node-RED, disabled)

The 433.92 MHz remote replayed by this device is an **awning** controller. A prior,
now-**disabled** Node-RED flow named **"Awning"** (tab `e1c2ca1342eeb2fd`, in
`flows.json` on the IOTstack hub `192.168.0.203`) drove the awning over MQTT before
this device existed. It is captured here as background for the intended MQTT/auto
behaviour of the new firmware.

**Control model:** the flow published short codes to MQTT topics `awning/command`
and `curtain/command`. The letter is direction, the trailing number is a
sequence/parameter:

- **`X#` = eXtend** (awning out — corresponds to **down**)
- **`R#` = Retract** (awning in — corresponds to **up**)

| Origin | Code | Meaning |
|--------|------|---------|
| Telegram `e` (manual) | `X6` | extend |
| Telegram `r` (manual) | `R7` | retract |
| Auto "Commander" | `X7` | extend |
| Auto "Commander" | `R9` | retract / emergency |

Distinct codes observed across the flows: `X1, X6, X7, R7, R9`.

**Auto logic ("Commander" function):** driven by global vars `wind`,
`SolarIntensity` (Lux), `temp`:
- Extend when `Lux ≥ 3.0` **and** `temp ≥ 20 °C` **and** `wind ≤ 20`.
- Retract when `Lux < 1.0` **or** `temp < 18 °C`.
- **Emergency retract when `wind > 25`.**
- Minimum 10 minutes between movements.

The flow also had a Telegram bot for manual `e`/`r` control and status messages, and
a `Store Command` node tracking a `commandSent` flag (`X6`→1, `R7`→0).

**Legacy device firmware (resolves the sequence).** An ESP32 subscribed to
`awning/command`, parsed `command = payload[0]` (`X`/`R`) and `dur = payload[1] − '0'`
(a single digit 0–9), and keyed a 433 MHz OOK transmitter on GPIO4. This resolves the
previously open items:

**1. The numeric suffix is `dur`, a movement *time*** — not a repeat count or
position. Movement time = **`dur × 5 s`**; the awning simply runs for that long. The
distance opened is only the emergent result of the run time.

| Command | `dur` | Move time |
|---------|-------|-----------|
| `X6` (normal manual **down**) | 6 | **30 s** |
| `X7` (auto down) | 7 | 35 s |
| `X1` | 1 | 5 s |
| `R7` (manual **up**) | 7 | 35 s |
| `R9` (auto / emergency up) | 9 | 45 s |
| boot default (`dur = 5`) | 5 | 25 s |

Single digit ⇒ `dur × 5` caps at 45 s.

**2. Motion model — the "sequence".** The awning motor runs in one direction until a
**counter-command** stops it (per the firmware's header comment). The two directions
are therefore asymmetric:

- **Down / extend (`X#`):** send `extendCommand` → wait **`dur × 5 s`** → send
  `retractCommand` **once as a stop** → state `E` (parked at a partial extension set by
  the run time).
- **Up / retract (`R#`):** send `retractCommand` → runs to the closed mechanical
  end-stop and **self-stops** → state `R`. No counter-command; its `dur × 5 s` is only a
  "closed" status timer.
- **Emergency:** no command for 120 s → auto-retract.

**3. RF codes = the captured buttons.** `extendCommand` / `retractCommand` are the
**down** / **up** codes of §10.1 (they differ by one bit, matching `down 0x7f45c` vs
`up 0x7f454`). `manCommand` / `autoCommand` are defined but **never used** — only
extend/retract drove the awning. Each command is transmitted **5×** per action.

**4. Legacy RF encoding (reference only).** The legacy device bit-banged these
symbolic pulse strings on a plain OOK transmitter (GPIO4), reverse-engineered with
**URH** (the author notes `rtl_433` does not decode it reliably). **The new firmware
does NOT use this bit-banged encoding** — it reproduces the same up/down RF from the
§10.1 captured codewords via **RadioLib SX1278 DIO2 keying**, which is verified to move
the awning. The strings are kept here only as the legacy reference:

| Symbol | Meaning | Timing |
|--------|---------|--------|
| `L` | long pulse (on) | 2000 µs |
| `S` | short pulse (on) | 350 µs |
| `G` | short gap (off) | 250 µs |
| `P` | long gap (off) | 2000 µs |
| `N` | end-of-frame gap | 16000 µs |

- `extendCommand`  = `LGSPSPSPSPSPSPSPLGSPLGLGLGSPLGSPSPSN`
- `retractCommand` = `LGSPSPSPSPSPSPSPLGSPLGLGLGSPLGSPLGSN`

These legacy timings sit within the receiver's tolerance of the §10.1 captured values
that the new firmware transmits via RadioLib.

**Design note for the new firmware.** The new device is **not** bound to the old
single-digit `dur × 5 s` MQTT encoding — it should accept a **movement time in seconds
directly** (finer, and beyond 45 s). Concretely:

- **Down** takes a stop-delay parameter in seconds (default = the legacy `X6` value,
  **30 s**): extend → wait → counter-stop.
- **Up** is fire-and-forget (retract to the end-stop); no parameter.
- Only **up/down** are required; the auto/manual RF codes are unused by the actuator.

## 11. Related

- [[Embedded-Workbench-FSD]] — the workbench that hosts the RTL-SDR receiver and
  flashes this DUT.
- [[signal-generator]] — the workbench's RF transmit source (Si5351/GPCLK), a
  complementary TX path.
