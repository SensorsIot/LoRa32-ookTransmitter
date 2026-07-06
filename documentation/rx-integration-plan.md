# Build Plan — Add a 433 MHz RX gateway to the LoRa32 awning controller

Status: **DEPLOYED to production 2026-07-06.** Branch `feat/rx433-gateway`.
Board: TTGO LoRa32 **T3 v1.6.1** (ESP32 + SX1278), 433.92 MHz. Repo: `LoRa32-ookTransmitter`.

## Deployment outcome (2026-07-06)
- **On hardware (verified via the workbench):** flash (classic-ESP32 via `/api/flash`,
  RFC2217 auto-reset fails on the CH9102 → `Wrong boot mode`), clean boot, dual-radio
  coexistence init stable, WiFi+MQTT, **TX still works** (SDR decoded `Euromot-Awning-up
  7f454`), **RX decode** of the physical Euromot remote (`id 0x7F4`, button=auto,
  `code 1FD20`), HA auto-discovery bridge — all confirmed.
- **Onboarded to production HA:** the board is on home WiFi publishing to the production
  broker `192.168.0.203:1883`; the Awning cover + the RX gateway (Fineoffset-WHx080
  weather station etc.) appear in HA.
- **HA entity naming fix** shipped via **OTA** (ArduinoOTA / espota — relayed through a
  LAN host because a NAT'd container can't do the OTA reverse-connection): switched the
  discovery bridge to HA `has_entity_name` (clean `sensor.<model>_<id>_<field>`).
- **Weather-station HA sensors migrated history-safe:** the hand-configured `mqtt:`
  sensors (Roof Temp / Weather Wind / Gust / Rainrate) had their `state_topic` swapped
  from OMG's `OpenMQTT/433/RTL_433toMQTT/Fineoffset-WHx080/0/169` to the gateway's
  `rtl_433/Fineoffset-WHx080/169` — same `unique_id`, history preserved.
- **Ghost cleanup:** the full decoder set spawns false-decode devices (like OMG);
  `tools/ha_cleanup_rtl433.py` (whitelist, dry-run default) clears them safely.

Remaining: relocate the board to OMG's antenna spot for full sensor range, then
decommission OMG (power off + clear its stale `_0_169_` entities). RX is currently
served from the bench location.

## Build result (2026-07-05)
- `pio run -e ttgo-lora32` → **SUCCESS**. Flash **69.9 %** (1.374 MB / 1.966 MB,
  `min_spiffs.csv` partitions, OTA slots preserved), RAM **18.4 %** (60 KB).
- RadioLib **7.7.1** satisfies both the firmware TX (`beginFSK`/`setOOK`/`transmitDirect`)
  and rtl_433_ESP (`^7.2.1`) — single shared copy, no conflict.
- rtl_433_ESP **0.5.1** (git `NorthernMan54/rtl_433_ESP`), full OOK decoder set.

## What was built (vs the plan below)
- `platformio.ini`: rtl_433_ESP dep + `RF_SX1278` and the T3 pin/SPI build flags,
  `OOK_MODULATION=true`, `min_spiffs.csv`.
- `src/rx433.{h,cpp}`: rtl_433_ESP owns the SX1278 in OOK RX; radio **mutex**; TX hand-off
  (`rx433TxBegin`/`rx433TxEnd`); generic rtl_433→HA discovery bridge (numeric + string +
  battery fields; state on `rtl_433/<model>/<id>`, retained discovery, non-retained state;
  `s_seen` capped at 256 to bound RAM).
- `src/ook.{h,cpp}`: split out `ookBeginTx()` so TX config is re-asserted after each RX
  period (shared chip).
- `src/motion.cpp`: every transmitting command is bracketed by `rx433TxBegin/TxEnd`.
- `src/main.cpp`: `rx433EarlyInit()` (mutex, before the TX task) → `rx433Init()` (after net).
- `src/euromot_awning.c`: project-local compiled Euromot decoder, **injected at runtime via
  `register_protocol(&g_cfg, &euromot_awning, NULL)`** — no rtl_433_ESP fork. Codes derived
  from the controller's own TX table: id `0x7F4`, buttons up=0x15 down=0x17 auto=0x20
  manual=0x21; also emits the raw 18-bit code hex for on-hardware confirmation.

## On-hardware test procedure (run when the Pi is back)
```bash
# 0. Confirm the Pi and find the LoRa32's RFC2217 port
curl -s http://192.168.0.87:8080/api/devices | python3 -m json.tool   # note tcp_port (4000/4001)
# 1. Flash over RFC2217 (replace 4000 with the slot port)
cd ~/LoRa32-ookTransmitter
pio run -e ttgo-lora32 -t upload --upload-port 'rfc2217://192.168.0.87:4000?ign_set_control'
# 2. Watch boot: expect "rx433: OOK receiver started on GPIO 32 @ 433.92 MHz"
pio device monitor --port 'rfc2217://192.168.0.87:4000?ign_set_control' -b 115200
# 3. RX: a live 433 OOK sensor (Bresser/Nexus/LaCrosse) appears in HA (source ~/.secrets/env)
curl -s -H "Authorization: Bearer $HA_TOKEN" "$HA_URL/api/states" | grep -i rtl_433
# 4. Euromot: press the physical remote -> entity "Euromot-Awning 2036 Button" (+ Code) in HA.
#    If NO Euromot decode appears, the bit-packing assumption needs one on-air sample:
#    temporarily relax the id!=0x7F4 guard in euromot_awning.c to log the raw code, press,
#    read the code, and lock the mapping.
# 5. TX still works + coexistence: fire an awning command over serial (u/d) or MQTT; the
#    workbench SDR must still hear 7f45x, and RX must resume (sensors reappear) afterwards.
# 6. Wedge check: cycle TX/RX many times (repeated moves) -> no radio lockup, RX keeps decoding.
```


## Goal
One board, two **unrelated** jobs (shared to save hardware cost):
1. **RX gateway, 24/7** — receive 433 MHz OOK sensors and publish to HA via auto-discovery, at **parity with the user's current OpenMQTTGateway** (OMG) setup.
2. **Awning TX, ~twice/day** — the existing controller (motion state machine, open-loop position, safety watchdog, HA `cover`), behaviour unchanged.

RX data does **not** feed the awning logic; the two functions are independent.

## Chosen direction
**Direction A** — add RX to the **existing awning firmware** (keep it), using the
**`rtl_433_ESP`** library (NorthernMan54) — the same rtl_433 decoder engine OMG
uses. *Not* Direction B (rebuilding the awning inside OMG): the user relaxed the
"OMG byte-identical" requirement to "same sensor data, easy HA auto-discovery",
so we don't reproduce OMG's discovery engine.

## Key facts established
- **rtl_433_ESP API:** `static initReceiver(byte inputPin, float freqMHz)`,
  `enableReceiver()`, `disableReceiver()`, `setCallback(cb, buf, size)` where
  `cb(char* json)`, `loop()`, `setRSSIThreshold()`, `setOOKThreshold()`,
  `setOOKModulation()/setFSKModulation()`. It owns its own RadioLib radio and
  does **not** expose the module for TX.
- **Half-duplex:** the SX1278 does OOK RX **or** TX, not both. TX today is
  `radio.transmitDirect()` bit-banging **DIO2 (GPIO32) as OUTPUT**; rtl_433_ESP
  receives by reading **that same DIO2 as INPUT**. Same pin, opposite direction.
- **OOK vs FSK:** the radio demodulates in hardware, so only one modulation is
  live at a time. OMG solves this with **separate board profiles**
  (`lilygo-rtl_433` OOK vs `lilygo-rtl_433-fsk`), never both at once.
- **The user's live HA sensors are all OOK** (Bresser-3CH, LaCrosse-TX, Nexus-TH,
  Acurite-609TXC updating within minutes). The only FSK entities (Cotech-367959,
  TPMS-Schrader) are **stale (1–2 days, `unknown`)** — not received now.
  → **OOK-only is full parity with today; no regression.**

## Architecture

### Radio coexistence (half-duplex, mutex-coordinated)
- **rtl_433_ESP owns the radio 24/7:** `initReceiver(LORA_DIO2=32, 433.92)` at
  boot, `enableReceiver()`, `rtl_433_ESP.loop()` called from the **main `loop()`**.
- **A FreeRTOS radio mutex** guards the chip. An awning TX burst (from the
  existing TX task in `motion.cpp`):
  1. take the mutex
  2. `rtl_433_ESP::disableReceiver()`
  3. re-init the SX1278 for **OOK `transmitDirect`** (existing `ookInit()` path),
     DIO2 → OUTPUT, bit-bang the codeword(s) as today
  4. `rtl_433_ESP::initReceiver(...)` + `enableReceiver()` to restore RX
  5. release the mutex
- The **main loop skips `rtl_433_ESP.loop()` while the mutex is held** by the TX
  task. RX is deaf only for the few-seconds TX burst, twice a day (accepted).
- **MQTT publish safety:** `rtl_433_ESP.loop()` runs in main-loop context, so the
  decode callback publishes via the existing `PubSubClient` with no cross-task
  hazard (the TX task only touches the *radio*, under the mutex).

### Decoding (OOK, curated)
Enable via `MY_DEVICES` only the decoders for the user's live devices, plus Euromot:
`nexus`, `acurite` (609TXC / 986 / Rain), `bresser_3ch`, `oregon_scientific`
(SL109H / THGR810 / CM180i), `fineoffset_wh1080` (WHx080), `lacrosse` (TX-118).
FSK (`fineoffset`/WH24 = Cotech, `tpms_schrader`) **out of scope for v1**.

### Euromot Awning decoder (the 4 keys)
rtl_433_ESP uses **compiled r_device decoders**, not the desktop `.conf` flex the
workbench uses — so add a small device decoder to the library's device set.
- OOK_PWM, **received** timing `s≈340, l≈2068, r≈13936` (workbench-measured on the
  physical remote; generous tolerance). *(Note the firmware's own TX uses
  s=416,l=2150,r=15588 — slightly different; the decoder should key off the
  physical remote's timing and tolerate both.)*
- 18-bit codewords, map: `7f454`→up, `7f45c`→down, `7f480`→auto, `7f484`→manual;
  fixed device id `0x7F4`.
- Implementation options to investigate: (a) a local decoder file registered with
  rtl_433_ESP without forking the lib, or (b) match-style entries. Confirm how
  rtl_433_ESP lets you inject a custom `r_device`.

### Output — generic rtl_433 → HA auto-discovery bridge (new `rx433.cpp/.h`)
On **first-seen** device (`model`+`id`): publish HA **discovery** config per JSON
field, then publish state; reuse the existing `PubSubClient` from `net.cpp`.
- Topic namespace separate from `awning/…`, e.g. `rtl_433/<model>/<id>`.
- Field → HA `device_class`/unit map: `temperature_C`→temperature/°C,
  `humidity`→humidity/%, `battery_ok`→battery, `wind_avg*`→wind_speed,
  `rain*`→precipitation/mm, `pressure*`→pressure, `rssi`→signal_strength/dB;
  unknown fields → plain sensor. Retained discovery + state.
- **Not** an OMG byte-clone — fresh entities appear in HA (user accepted this).

## Files to add / change
| File | Change |
|------|--------|
| `platformio.ini` | add `rtl_433_ESP` (+ compatible RadioLib — **verify vs existing @7.1.0**); build_flags `RF_SX1278`, `RF_MODULE_FREQUENCY=433.92`, `OOK_MODULATION`, pin defines, `MY_DEVICES`, `RSSI_THRESHOLD`, OOK threshold |
| `src/rx433.h/.cpp` (new) | rtl_433_ESP init, callback→HA-discovery bridge, radio mutex, mode-switch helpers, `rx433Loop()` |
| `src/ook.cpp` | refactor TX to run as "acquire radio mutex → transmitDirect → bit-bang → restore RX → release", coordinated with rx433 |
| `src/net.cpp` | expose a publish helper (or share the `mqtt` client) for `rx433` |
| `src/main.cpp` | `rx433Init()` in `setup()`, `rx433Loop()` in `loop()` |
| rtl_433_ESP custom decoder | `euromot_awning` r_device enabled via `MY_DEVICES` |

## Pins (from `config.h`, T3 v1.6.1)
`SCK=5 MISO=19 MOSI=27 NSS=18 RST=23 DIO0=26 DIO1=33 DIO2=32 (OOK DATA/RX pin) LED=25`

## Open implementation risks (resolve during build)
1. **RadioLib version** — rtl_433_ESP may require a specific RadioLib; the firmware
   pins `@7.1.0`. Pin a compatible version for both.
2. **Custom decoder injection** — confirm how to add the Euromot `r_device` to
   rtl_433_ESP without maintaining a fork.
3. **Mode-switch robustness** — RX↔TX both re-init the SX1278; verify many cycles
   don't wedge the radio (mirror the workbench's "watch for wedge" lesson).
4. **Resource fit** — OMG runs the full stack on this board, so curated decoders +
   the awning stack should fit; check flash/RAM after first build.

## Test plan
1. Flash; confirm live OOK sensors (Bresser/LaCrosse/Nexus) decode → entities
   auto-appear in HA.
2. Press the physical Euromot remote → decodes as `Euromot-Awning-*` → entity in HA.
3. Fire an awning command → RX pauses, TX bit-bangs (awning moves; workbench SDR
   hears `7f45x`), RX resumes cleanly.
4. Cycle TX/RX many times → no radio wedge.

## Explicitly out of scope (v1)
FSK (Cotech/TPMS); OMG byte-identical discovery; RF replay; using RX to close the
awning's open-loop position. (OMG-style runtime OOK↔FSK switching is a possible v2.)
