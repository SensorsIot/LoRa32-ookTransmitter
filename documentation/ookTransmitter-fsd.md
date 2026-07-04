# Functional Specification — LoRa32 OOK Transmitter

## 1. System Overview

### 1.1 Purpose

The LoRa32 OOK Transmitter is a single-board firmware that reproduces ("replays")
433.92 MHz on-off-keyed (OOK) remote-control signals. Signals are first captured
from an original remote using an RTL-SDR receiver and `rtl_433`; the resulting
fixed codewords are stored in firmware and re-emitted on demand by a TTGO LoRa32
(SX1278) board. It is the transmit half of a capture-and-replay workflow, paired
with the RTL-SDR receiver on the Universal Embedded Workbench.

### 1.2 Goals

- Faithfully replay captured fixed-code OOK-PWM remote signals on 433.92 MHz.
- Support the four buttons of the target remote: up, down, auto, manual.
- Provide both hands-free (auto-cycle) and interactive (serial-triggered) operation.
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

**Initialization & Diagnostics**
- **FR-3.1** [Must]: The device shall initialize the SX1278 in FSK/OOK mode and, on
  initialization failure, halt further transmission and emit a diagnostic message.
- **FR-3.2** [Should]: The device shall log readiness and each transmission (button
  name and code) over serial at 115200 baud.

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
| `PERIOD_MS` | 3000 | Delay between buttons in auto-cycle |
| `BUTTONS[]` | 4 codewords | Captured remote codes |

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

## 11. Related

- [[Embedded-Workbench-FSD]] — the workbench that hosts the RTL-SDR receiver and
  flashes this DUT.
- [[signal-generator]] — the workbench's RF transmit source (Si5351/GPCLK), a
  complementary TX path.
