#include <Arduino.h>
#include <set>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <rtl_433_ESP.h>
#include "rx433.h"
#include "ook.h"
#include "net.h"

// ============================================================
// 433 MHz OOK RX gateway + generic rtl_433 -> Home Assistant bridge (FR-11)
// ============================================================

// rtl_433 internals for injecting the project-local Euromot decoder at runtime
// (see euromot_awning.c). Minimal forward declarations avoid pulling the heavy
// rtl_433 headers into this Arduino translation unit; we only take addresses.
struct r_cfg;
struct r_device;
extern "C" void register_protocol(struct r_cfg *cfg, struct r_device *dev, char *arg);
extern struct r_cfg      g_cfg;            // defined in the library
extern "C" struct r_device euromot_awning; // defined in euromot_awning.c

static rtl_433_ESP    rf;
static char           s_msg[512];                 // rtl_433 JSON callback buffer
static SemaphoreHandle_t s_mutex = nullptr;       // guards the shared SX1278
static volatile bool  s_rxActive = false;
static uint32_t       s_count    = 0;
static std::set<String> s_seen;                   // "<device>/<field>" discovery published

// ---- Field -> Home Assistant mapping ----
// Curated to the sensor fields Home Assistant treats as first-class (the ones
// the user's live OOK sensors publish: temperature, humidity, battery) plus the
// common weather fields. Any other numeric field falls through to a plain sensor.
struct FieldMap {
  const char* key;        // rtl_433 JSON key
  const char* name;       // HA entity name suffix
  const char* dev_cla;    // HA device_class (nullptr = none)
  const char* unit;       // unit_of_measurement (nullptr = none)
  const char* stat_cla;   // state_class (nullptr = none)
};

static const FieldMap FIELDS[] = {
  { "temperature_C",  "Temperature",    "temperature",   "°C", "measurement" },
  { "temperature_F",  "Temperature",    "temperature",   "°F", "measurement" },
  { "humidity",       "Humidity",       "humidity",      "%",       "measurement" },
  { "moisture",       "Moisture",       nullptr,         "%",       "measurement" },
  { "pressure_hPa",   "Pressure",       "pressure",      "hPa",     "measurement" },
  { "pressure_kPa",   "Pressure",       "pressure",      "kPa",     "measurement" },
  { "wind_avg_km_h",  "Wind speed",     "wind_speed",    "km/h",    "measurement" },
  { "wind_max_km_h",  "Wind gust",      "wind_speed",    "km/h",    "measurement" },
  { "wind_avg_m_s",   "Wind speed",     "wind_speed",    "m/s",     "measurement" },
  { "wind_dir_deg",   "Wind direction", nullptr,         "°",  "measurement" },
  { "rain_mm",        "Rain total",     "precipitation", "mm",      "total_increasing" },
  { "rain_rate_mm_h", "Rain rate",      "precipitation_intensity", "mm/h", "measurement" },
  { "light_lux",      "Illuminance",    "illuminance",   "lx",      "measurement" },
  { "uv",             "UV index",       nullptr,         nullptr,   "measurement" },
};

// String / categorical fields exposed as plain sensors (e.g. the Euromot remote's
// pressed button, or a generic remote's event/state).
static const char* const STRING_FIELDS[][2] = {
  { "button", "Button" },
  { "code",   "Code" },
  { "state",  "State" },
  { "event",  "Event" },
};

// Restrict an id to characters HA accepts in object/node ids.
static String sanitize(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    out += (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
  }
  return out;
}

// Publish one HA discovery config (retained), once per (device, field).
static void publishDiscovery(const String& comp, const String& dev,
                             const String& devName, const String& model,
                             const String& stateTopic, const char* key,
                             const char* name, const char* dev_cla,
                             const char* unit, const char* stat_cla,
                             const char* val_tpl) {
  String seenKey = dev + "/" + key;
  if (s_seen.count(seenKey)) return;
  // Bound memory: RF false-decodes invent new ids indefinitely. If the set of
  // seen (device,field) pairs grows large, drop it; discovery is retained and
  // idempotent, so real devices simply re-announce on their next packet.
  if (s_seen.size() > 256) s_seen.clear();

  PubSubClient& mqtt = netMqttClient();
  JsonDocument d;
  d["name"]    = name;                 // short field name; HA prefixes the device
  d["has_entity_name"] = true;         // -> "<device> <name>", clean entity_id
  d["uniq_id"] = dev + "_" + key;
  d["stat_t"]  = stateTopic;
  d["val_tpl"] = val_tpl;
  if (dev_cla)  d["dev_cla"]      = dev_cla;
  if (unit)     d["unit_of_meas"] = unit;
  if (stat_cla) d["stat_cla"]     = stat_cla;
  d["avty_t"]  = T_STAT_AVAIL;                    // tied to the gateway's LWT
  JsonObject dv = d["dev"].to<JsonObject>();
  dv["ids"][0] = dev;
  dv["name"]   = devName;
  dv["mdl"]    = model;
  dv["mf"]     = "rtl_433";

  char topic[128];
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/%s/%s/%s/config",
           comp.c_str(), dev.c_str(), key);
  char buf[512];
  size_t n = serializeJson(d, buf, sizeof(buf));
  if (mqtt.publish(topic, (const uint8_t*)buf, n, /*retained=*/true))
    s_seen.insert(seenKey);
}

// rtl_433 decode callback. Runs in the loop() context that calls rf.loop()
// (rx433Loop), i.e. the same task as the MQTT client, so publishing here is
// safe. `json` is the decoded packet as a JSON string.
static void onMessage(char* json) {
#ifdef RX433_DEBUG
  Serial.printf("[rx433] msg: %s\n", json);   // every decoded packet
#endif
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;

  const char* model = doc["model"] | "";
  if (!model[0]) return;                          // status / non-device messages

  // Device identity: id, else channel, else 0.
  String idStr;
  if (!doc["id"].isNull())          idStr = doc["id"].as<String>();
  else if (!doc["channel"].isNull()) idStr = "ch" + doc["channel"].as<String>();
  else                               idStr = "0";

  if (!netMqttConnected()) return;
  PubSubClient& mqtt = netMqttClient();

  String dev       = sanitize(String(model) + "-" + idStr);
  String devName   = String(model) + " " + idStr;
  String stateTopic = "rtl_433/" + sanitize(model) + "/" + sanitize(idStr);

  // State (not retained): sensors refresh on their next packet, and this avoids
  // replaying one-shot events (e.g. a remote button press) on an HA restart.
  mqtt.publish(stateTopic.c_str(), (const uint8_t*)json, strlen(json), false);

  // Discovery for each mapped field present.
  for (const auto& f : FIELDS) {
    if (doc[f.key].isNull()) continue;
    String tpl = String("{{ value_json.") + f.key + " }}";
    publishDiscovery("sensor", dev, devName, model, stateTopic, f.key,
                     f.name, f.dev_cla, f.unit, f.stat_cla, tpl.c_str());
  }

  // String / categorical fields (Euromot button/code, generic remote events).
  for (const auto& sf : STRING_FIELDS) {
    if (doc[sf[0]].isNull()) continue;
    String tpl = String("{{ value_json.") + sf[0] + " }}";
    publishDiscovery("sensor", dev, devName, model, stateTopic, sf[0],
                     sf[1], nullptr, nullptr, nullptr, tpl.c_str());
  }

  // Battery: rtl_433 emits battery_ok (1 = OK). Represent as a HA battery
  // binary_sensor where ON = low battery.
  if (!doc["battery_ok"].isNull()) {
    publishDiscovery("binary_sensor", dev, devName, model, stateTopic,
                     "battery_ok", "Battery", "battery",
                     nullptr, nullptr,
                     "{{ 'OFF' if value_json.battery_ok == 1 else 'ON' }}");
  }

  s_count++;
}

void rx433EarlyInit() {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

void rx433Init() {
  if (s_rxActive) return;
  if (!s_mutex) rx433EarlyInit();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  rf.setCallback(onMessage, s_msg, sizeof(s_msg));
  rtl_433_ESP::initReceiver(RF_MODULE_RECEIVER_GPIO, FREQ_MHZ);
  // Inject the Euromot awning remote decoder into the built decoder list (the
  // built-ins were just registered by initReceiver -> rtlSetup). One extra
  // append; the fixed device array is untouched (FR-11).
  register_protocol(&g_cfg, &euromot_awning, nullptr);
  rtl_433_ESP::enableReceiver();
  s_rxActive = true;
  xSemaphoreGive(s_mutex);
  Serial.printf("rx433: OOK receiver started on GPIO %d @ %.2f MHz\n",
                RF_MODULE_RECEIVER_GPIO, FREQ_MHZ);
}

void rx433Loop() {
#ifdef RX433_DEBUG
  static uint32_t lastStat = 0;   // heartbeat: signals heard vs decoded
  if (millis() - lastStat > 4000) {
    lastStat = millis();
    Serial.printf("[rx433] rxActive=%d signals=%d msgs=%d bridged=%u free=%d\n",
                  (int)s_rxActive, rtl_433_ESP::totalSignals,
                  rtl_433_ESP::messageCount, (unsigned)s_count,
                  (int)ESP.getFreeHeap());
  }
#endif
  if (!s_rxActive) return;
  if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
    rf.loop();
    xSemaphoreGive(s_mutex);
  }
}

void rx433TxBegin() {
  if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (s_rxActive) rtl_433_ESP::disableReceiver();  // stops ISR + core-0 RSSI reads
  ookBeginTx();                                    // chip -> OOK transmit mode
}

void rx433TxEnd() {
  if (s_rxActive) {
    rtl_433_ESP::initReceiver(RF_MODULE_RECEIVER_GPIO, FREQ_MHZ);
    rtl_433_ESP::enableReceiver();
  }
  if (s_mutex) xSemaphoreGive(s_mutex);
}

bool     rx433Active() { return s_rxActive; }
uint32_t rx433Count()  { return s_count; }
