#!/usr/bin/env python3
"""Whitelist-based Home Assistant rtl_433 device cleanup.

OpenMQTTGateway / rtl_433 with the full decoder set continuously mis-reads RF
noise as new "devices" (Acurite-986-<random>, Oregon-CM180i-<random>, Markisol,
Secplus-v1, Generic-Motion/Remote, ...). Each spawns an MQTT device card in HA
that lingers even after its entities are gone.

This script keeps ONLY the sensors listed in ha_keep_sensors.json and removes
every other rtl_433 device. It is SAFE by construction:

  * It only ever considers MQTT devices whose manufacturer is empty / "<unknown>"
    -- that is exactly the set of rtl_433 devices. Anything with a real
    manufacturer (Xiaomi, IKEA, TuYa, esphome, OMG_community, SensorsIot, ...)
    is never touched, so BLE / Zigbee / ESPHome devices are safe.
  * Of those, it keeps the whitelisted names and deletes the rest.
  * It is DRY-RUN by default; pass --delete to actually remove.

Env: HA_URL (e.g. http://192.168.0.202:8123) and HA_TOKEN (long-lived token).
Deps: pip install websockets

Usage:
  python3 ha_cleanup_rtl433.py                 # dry run: show what would go
  python3 ha_cleanup_rtl433.py --delete        # actually delete
  python3 ha_cleanup_rtl433.py --keep other.json
"""
import argparse
import asyncio
import json
import os
import re
import sys

try:
    import websockets
except ImportError:
    sys.exit("Install the websocket client first:  pip install websockets")

UNKNOWN_MFR = (None, "", "<unknown>")

# rtl_433 device model prefixes. Used as a safety belt so we only ever consider
# devices that are actually rtl_433 (an MQTT device with no manufacturer could
# otherwise be an iBeacon, a custom sensor, etc.). Add models here if a real
# rtl_433 sensor of yours is not being seen.
RTL433 = re.compile(
    r"^(Acurite|Ambient|Bresser|Cotech|Digitech|Fineoffset|Generic-|GT-WT|GT-|"
    r"inFactory|LaCrosse|Markisol|Nexus|Oregon|Prologue|Rubicson|Secplus|"
    r"Springfield|TFA|TPMS|Visonic|Waveman|WT02|Interlogix|Skylink|Megacode|"
    r"HT680|Telldus|Revolt|Regency|Burnhard|Chuango|Kerui|Smoke-GS558|X10|"
    r"Honeywell|Nice-Flor|Cardin|DSC|Elro|Somfy|Silvercrest|Emos|Klimalogg|"
    r"Thermopro|Vauno|Rojaflex|Kedsum|Solight|Auriol|Eurochron|Mebus|Hideki|"
    r"Esperanza|Efergy|Steelmate|Inkbird|Maverick|Ecowitt|Holman|Vevor)",
    re.I,
)


def is_rtl433(dev):
    """True only for OMG/rtl_433 MQTT devices: no manufacturer, an 'mqtt'
    identifier, and an rtl_433 model name. Excludes iBeacon/BLE/Zigbee/etc."""
    if dev.get("manufacturer") not in UNKNOWN_MFR:
        return False  # has a real manufacturer -> not rtl_433
    idents = dev.get("identifiers") or []
    if not any(i and i[0] == "mqtt" for i in idents):
        return False  # not MQTT-sourced (e.g. ibeacon) -> skip
    name = dev.get("name_by_user") or dev.get("name") or ""
    model = dev.get("model") or ""
    return bool(RTL433.search(name) or RTL433.search(model))


async def _ws_call(ws, _id, payload):
    payload["id"] = _id[0]
    _id[0] += 1
    await ws.send(json.dumps(payload))
    while True:
        msg = json.loads(await ws.recv())
        if msg.get("id") == payload["id"]:
            return msg


async def run(url, token, keep_names, do_delete):
    ws_url = url.replace("http", "ws").rstrip("/") + "/api/websocket"
    async with websockets.connect(ws_url, max_size=None) as ws:
        await ws.recv()
        await ws.send(json.dumps({"type": "auth", "access_token": token}))
        auth = json.loads(await ws.recv())
        if auth.get("type") != "auth_ok":
            sys.exit("HA auth failed -- check HA_TOKEN")

        _id = [1]
        devs = (await _ws_call(ws, _id, {"type": "config/device_registry/list"}))["result"]
        ents = (await _ws_call(ws, _id, {"type": "config/entity_registry/list"}))["result"]
        ecount = {}
        for e in ents:
            ecount[e.get("device_id")] = ecount.get(e.get("device_id"), 0) + 1

        keep, delete = [], []
        for d in devs:
            if not is_rtl433(d):
                continue  # not an rtl_433 device -> never touch
            name = d.get("name_by_user") or d.get("name") or ""
            (keep if name in keep_names else delete).append((d, name, ecount.get(d["id"], 0)))

        print(f"rtl_433 MQTT devices: {len(keep) + len(delete)}")
        print(f"  KEEP (whitelisted): {len(keep)}")
        for _, n, ec in sorted(keep, key=lambda x: x[1]):
            print(f"      {n or '(unnamed)'}  [{ec} entities]")
        print(f"  {'DELETING' if do_delete else 'WOULD DELETE'}: {len(delete)}")
        for _, n, ec in sorted(delete, key=lambda x: x[1]):
            print(f"      {n or '(unnamed orphan '+str(_[0].get('identifiers'))+')'}  [{ec} entities]")

        if not do_delete:
            print("\nDry run. Re-run with --delete to remove the above.")
            return
        removed = 0
        for d, n, _ec in delete:
            ce = d.get("config_entries") or []
            if not ce:
                print(f"  SKIP (no config entry): {n}")
                continue
            r = await _ws_call(ws, _id, {
                "type": "config/device_registry/remove_config_entry",
                "device_id": d["id"], "config_entry_id": ce[0]})
            if r.get("success"):
                removed += 1
            else:
                print(f"  FAIL {n}: {(r.get('error') or {}).get('code')}")
        print(f"\nDeleted {removed} device(s).")


def main():
    ap = argparse.ArgumentParser(description="Whitelist HA rtl_433 device cleanup")
    ap.add_argument("--keep", default=os.path.join(os.path.dirname(__file__), "ha_keep_sensors.json"))
    ap.add_argument("--url", default=os.environ.get("HA_URL"))
    ap.add_argument("--token", default=os.environ.get("HA_TOKEN"))
    ap.add_argument("--delete", action="store_true", help="actually delete (default: dry run)")
    args = ap.parse_args()
    if not args.url or not args.token:
        sys.exit("Set HA_URL and HA_TOKEN (env or --url/--token). e.g. source ~/.secrets/env")
    with open(args.keep) as f:
        keep_names = set(json.load(f)["keep_names"])
    asyncio.run(run(args.url, args.token, keep_names, args.delete))


if __name__ == "__main__":
    main()
