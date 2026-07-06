# Home Assistant maintenance tools

Helpers for running the 433 MHz RX gateway (`src/rx433.*`) alongside / instead of
OpenMQTTGateway (OMG) and keeping Home Assistant tidy.

## Whitelist cleanup — `ha_cleanup_rtl433.py`

The gateway (like OMG) runs the full rtl_433 OOK decoder set, which mis-reads RF
noise as new "devices" (`Acurite-986-<random>`, `Oregon-CM180i-<random>`,
`Markisol`, `Secplus-v1`, `Generic-Motion/Remote`, …). Each spawns an MQTT device
card in HA that lingers even after its entities disappear.

`ha_cleanup_rtl433.py` keeps only the sensors listed in `ha_keep_sensors.json` and
removes every other rtl_433 device. It is **safe by construction**:

- It only considers MQTT devices that have an `mqtt` identifier **and** an rtl_433
  model name **and** no real manufacturer. BLE/iBeacon/Zigbee/ESPHome and any
  manufactured device are never touched. (Do **not** filter on "no manufacturer"
  alone — that also matches iBeacon devices such as Battery Monitors.)
- It is **dry-run by default**; pass `--delete` to actually remove.
- Removal uses the websocket command `config/device_registry/remove_config_entry`
  (HA 2026.x; `remove_config_entry_from_device` returns `unknown_command`).

```bash
source ~/.secrets/env                       # sets HA_URL + HA_TOKEN
pip install websockets
python3 ha_cleanup_rtl433.py                # dry run — shows what would go
python3 ha_cleanup_rtl433.py --delete       # remove the ghosts
```

Edit `ha_keep_sensors.json` (`keep_names`) to add/remove the sensors that must
survive.

## Migrating HA sensors from OMG to the gateway (history-preserving)

Hand-configured `mqtt:` sensors that read OMG topics keep their history if you
change only their **`state_topic`** (the `unique_id` / entity stays the same, so
the history stays). Swap:

```
OpenMQTT/433/RTL_433toMQTT/<Model>/<sub>/<id>   ->   rtl_433/<Model>/<id>
```

The gateway republishes the raw rtl_433 JSON, so `value_template`s
(`{{ value_json.temperature_C }}`, `wind_avg_km_h`, `wind_max_km_h`, `rain_mm`,
`humidity`, …) are unchanged. Reload with the `mqtt.reload` service — no restart,
no lost history. (Example: the Fineoffset-WHx080 weather station's Roof Temp /
Wind / Gust / Rainrate sensors were migrated this way.)
