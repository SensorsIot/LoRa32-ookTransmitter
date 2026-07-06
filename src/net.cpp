#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include "net.h"
#include "motion.h"

// ---- NVS-backed MQTT settings ----
static Preferences prefs;
static char     g_mqttHost[41] = "";
static uint16_t g_mqttPort      = MQTT_PORT_DEFAULT;
static char     g_mqttUser[33]  = "";
static char     g_mqttPass[65]  = "";

// ---- Clients ----
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);
static WebServer    server(80);

// ---- Status ----
static volatile bool s_portalActive = false;
static String        s_clientId;

// ---- Double-reset detection (RTC memory survives a reset button, not power loss) ----
RTC_NOINIT_ATTR static uint32_t s_drdFlag;
static const uint32_t DRD_MAGIC = 0xD0D01234UL;
static bool     s_drdWindow = false;
static uint32_t s_drdStart  = 0;

// ---- WiFiManager custom parameters (kept for the save callback) ----
static WiFiManagerParameter* pp_host;
static WiFiManagerParameter* pp_port;
static WiFiManagerParameter* pp_user;
static WiFiManagerParameter* pp_pass;

static void loadMqttSettings() {
  prefs.getString("mqtt_host", "").toCharArray(g_mqttHost, sizeof(g_mqttHost));
  g_mqttPort = prefs.getUShort("mqtt_port", MQTT_PORT_DEFAULT);
  prefs.getString("mqtt_user", "").toCharArray(g_mqttUser, sizeof(g_mqttUser));
  prefs.getString("mqtt_pass", "").toCharArray(g_mqttPass, sizeof(g_mqttPass));
}

static void saveMqttSettings() {
  prefs.putString("mqtt_host", g_mqttHost);
  prefs.putUShort("mqtt_port", g_mqttPort);
  prefs.putString("mqtt_user", g_mqttUser);
  prefs.putString("mqtt_pass", g_mqttPass);
}

static void saveParamsCallback() {
  strlcpy(g_mqttHost, pp_host->getValue(), sizeof(g_mqttHost));
  g_mqttPort = (uint16_t)atoi(pp_port->getValue());
  if (g_mqttPort == 0) g_mqttPort = MQTT_PORT_DEFAULT;
  strlcpy(g_mqttUser, pp_user->getValue(), sizeof(g_mqttUser));
  strlcpy(g_mqttPass, pp_pass->getValue(), sizeof(g_mqttPass));
  saveMqttSettings();
}

// ---------- Home Assistant MQTT discovery (FR-7.2) ----------
static void publishDiscovery() {
  JsonDocument dev;
  dev["ids"][0] = s_clientId;
  dev["name"]   = "Awning";
  dev["mdl"]    = "LoRa32 OOK Awning";
  dev["mf"]     = "SensorsIot";

  {
    JsonDocument d;
    d["name"]        = "Awning";
    d["uniq_id"]     = s_clientId + "_cover";
    d["dev_cla"]     = "awning";
    d["cmd_t"]       = T_CMD_SET;
    d["pl_open"]     = "OPEN";
    d["pl_cls"]      = "CLOSE";
    d["pl_stop"]     = "STOP";
    d["stat_t"]      = T_STAT_STATE;
    d["stat_open"]   = "open";
    d["stat_opening"] = "opening";
    d["stat_clsd"]   = "closed";
    d["stat_closing"] = "closing";
    d["pos_t"]       = T_STAT_POS;
    d["set_pos_t"]   = T_CMD_POSITION;
    d["pos_open"]    = 100;
    d["pos_clsd"]    = 0;
    d["avty_t"]      = T_STAT_AVAIL;
    d["dev"]         = dev;
    char buf[640]; size_t n = serializeJson(d, buf);
    mqtt.publish(HA_DISCOVERY_PREFIX "/cover/awning/awning/config", (const uint8_t*)buf, n, true);
  }
  {
    JsonDocument d;
    d["name"]    = "Awning emergency retract";
    d["uniq_id"] = s_clientId + "_emg";
    d["cmd_t"]   = T_CMD_EMERGENCY;
    d["pl_prs"]  = "PRESS";
    d["ic"]      = "mdi:alert";
    d["avty_t"]  = T_STAT_AVAIL;
    d["dev"]     = dev;
    char buf[400]; size_t n = serializeJson(d, buf);
    mqtt.publish(HA_DISCOVERY_PREFIX "/button/awning/emergency/config", (const uint8_t*)buf, n, true);
  }
  struct Sensor { const char* id; const char* name; const char* topic; const char* dcla; const char* unit; };
  static const Sensor sensors[] = {
    { "ip",     "Awning IP",       T_DIAG_IP,     nullptr,     nullptr },
    { "rssi",   "Awning RSSI",     T_DIAG_RSSI,   "signal_strength", "dBm" },
    { "uptime", "Awning uptime",   T_DIAG_UPTIME, "duration",  "s" },
    { "last",   "Awning last cmd", T_DIAG_LAST,   nullptr,     nullptr },
  };
  for (const auto& s : sensors) {
    JsonDocument d;
    d["name"]    = s.name;
    d["uniq_id"] = s_clientId + "_" + s.id;
    d["stat_t"]  = s.topic;
    if (s.dcla) d["dev_cla"] = s.dcla;
    if (s.unit) d["unit_of_meas"] = s.unit;
    d["avty_t"]  = T_STAT_AVAIL;
    d["ic"]      = "mdi:awning-outline";
    d["dev"]     = dev;
    char topic[96]; snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/sensor/awning/%s/config", s.id);
    char buf[420]; size_t n = serializeJson(d, buf);
    mqtt.publish(topic, (const uint8_t*)buf, n, true);
  }
  {
    JsonDocument d;
    d["name"]     = "Awning safety retract";
    d["uniq_id"]  = s_clientId + "_safety";
    d["stat_t"]   = T_DIAG_SAFETY;
    d["pl_on"]    = "ON";
    d["pl_off"]   = "OFF";
    d["dev_cla"]  = "safety";
    d["avty_t"]   = T_STAT_AVAIL;
    d["dev"]      = dev;
    char buf[400]; size_t n = serializeJson(d, buf);
    mqtt.publish(HA_DISCOVERY_PREFIX "/binary_sensor/awning/safety/config", (const uint8_t*)buf, n, true);
  }
}

// ---------- State reporting (FR-7.4/7.5) ----------
static const char* coverStateStr() {
  if (motionMoving()) return motionDir() == DIR_EXTEND ? "opening" : "closing";
  return motionPositionM() <= POS_EPSILON_M ? "closed" : "open";
}

static void publishState() {
  if (!mqtt.connected()) return;
  mqtt.publish(T_STAT_STATE, coverStateStr(), true);
  char pos[8]; snprintf(pos, sizeof(pos), "%d", (int)roundf(motionPositionPct()));
  mqtt.publish(T_STAT_POS, pos, true);
  mqtt.publish(T_DIAG_IP, WiFi.localIP().toString().c_str(), true);
  char rssi[8]; snprintf(rssi, sizeof(rssi), "%d", WiFi.RSSI());
  mqtt.publish(T_DIAG_RSSI, rssi, true);
  char up[12]; snprintf(up, sizeof(up), "%lu", millis() / 1000UL);
  mqtt.publish(T_DIAG_UPTIME, up, true);
  mqtt.publish(T_DIAG_LAST, motionLastCmd(), true);
  mqtt.publish(T_DIAG_SAFETY, motionSafetyRetract() ? "ON" : "OFF", true);
}

// ---------- MQTT command handling (FR-7.3) ----------
static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char msg[32];
  size_t n = len < sizeof(msg) - 1 ? len : sizeof(msg) - 1;
  memcpy(msg, payload, n); msg[n] = '\0';

  if (strcmp(topic, WATCHDOG_TOPIC) == 0) {
    motionFeedWatchdog(strcmp(msg, WATCHDOG_UNSAFE) == 0);
  } else if (strcmp(topic, T_CMD_SET) == 0) {
    if      (strcmp(msg, "OPEN")  == 0) motionOpen();
    else if (strcmp(msg, "CLOSE") == 0) motionClose();
    else if (strcmp(msg, "STOP")  == 0) motionStop();
  } else if (strcmp(topic, T_CMD_POSITION) == 0) {
    motionCommandPct(atof(msg));
  } else if (strcmp(topic, T_CMD_EMERGENCY) == 0) {
    motionEmergency();
  }
}

static void mqttEnsure() {
  static uint32_t lastTry = 0;
  if (mqtt.connected() || strlen(g_mqttHost) == 0) return;
  if (millis() - lastTry < 5000) return;
  lastTry = millis();
  bool ok = mqtt.connect(s_clientId.c_str(), g_mqttUser, g_mqttPass,
                         T_STAT_AVAIL, 0, true, "offline");
  if (!ok) return;
  mqtt.publish(T_STAT_AVAIL, "online", true);
  mqtt.subscribe(WATCHDOG_TOPIC);
  mqtt.subscribe(T_CMD_SET);
  mqtt.subscribe(T_CMD_POSITION);
  mqtt.subscribe(T_CMD_EMERGENCY);
  publishDiscovery();
  publishState();
}

// ---------- HTTP interface (FR-8.x) ----------
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Awning</title><style>
body{font-family:sans-serif;margin:0;background:#111;color:#eee;text-align:center}
h1{font-size:1.2em;padding:.5em;margin:0;background:#222}
#st{font-size:1.6em;margin:.4em}#pos{color:#8cf}
button{font-size:1.1em;margin:.3em;padding:.6em 1em;border:0;border-radius:.4em;background:#345;color:#fff}
button.e{background:#a22}.bar{height:14px;background:#333;border-radius:7px;margin:.5em 2em}
.bar>i{display:block;height:100%;background:#8cf;border-radius:7px;width:0}
input{font-size:1.1em;width:4em;padding:.3em}small{color:#888}
</style></head><body><h1>Awning Controller</h1>
<div id=st>...</div><div class=bar><i id=pb></i></div>
<div><button onclick=c('down')>Down &#9660;</button>
<button onclick=c('up')>Up &#9650;</button>
<button onclick=c('stop')>Stop</button>
<button class=e onclick=c('emergency')>EMERGENCY</button></div>
<div><input id=m type=number step=0.1 min=0 value=1.0> m
<button onclick="c('target','&m='+document.getElementById('m').value)">Go</button></div>
<div><button onclick=c('auto')>Auto</button><button onclick=c('manual')>Manual</button></div>
<div><small id=info></small></div>
<script>
function c(a,x){fetch('/api/cmd?do='+a+(x||'')).then(u)}
function u(){fetch('/api/status').then(r=>r.json()).then(s=>{
document.getElementById('st').innerHTML=s.state.toUpperCase()+
' <span id=pos>'+s.pos.toFixed(2)+'m</span>'+(s.safety?' ⚠':'');
document.getElementById('pb').style.width=(s.moving?s.progress*100:s.pct)+'%';
document.getElementById('info').textContent=
'wifi:'+(s.wifi?s.rssi+'dBm':'-')+' mqtt:'+(s.mqtt?'up':'down')+' | '+s.last;})}
setInterval(u,1000);u();
</script></body></html>)HTML";

static void handleRoot() { server.send_P(200, "text/html", PAGE); }

static void handleStatus() {
  JsonDocument d;
  d["state"]    = coverStateStr();
  d["pos"]      = motionPositionM();
  d["pct"]      = (int)roundf(motionPositionPct());
  d["target"]   = motionTargetM();
  d["moving"]   = motionMoving();
  d["progress"] = motionProgress();
  d["safety"]   = motionSafetyRetract();
  d["wifi"]     = netWifiConnected();
  d["mqtt"]     = mqtt.connected();
  d["rssi"]     = WiFi.RSSI();
  d["ip"]       = WiFi.localIP().toString();
  d["last"]     = motionLastCmd();
  String out; serializeJson(d, out);
  server.send(200, "application/json", out);
}

static void handleCmd() {
  String a = server.arg("do");
  if      (a == "up")        motionClose();
  else if (a == "down")      motionOpen();
  else if (a == "stop")      motionStop();      // interrupts an in-progress move (FR-8.3)
  else if (a == "emergency") motionEmergency();
  else if (a == "auto")      motionRaw(BTN_AUTO);
  else if (a == "manual")    motionRaw(BTN_MANUAL);
  else if (a == "target")    motionCommandTarget(server.arg("m").toFloat());
  server.send(200, "application/json", "{\"ok\":true}");
}

static void setupWeb() {
  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/cmd", handleCmd);
  server.begin();
}

// ---------- Setup / loop ----------
void netSetup() {
  prefs.begin("awning", false);
  loadMqttSettings();

  bool doubleReset = (s_drdFlag == DRD_MAGIC);
  s_drdFlag = DRD_MAGIC;                     // arm; cleared after the window in netLoop
  s_drdWindow = true; s_drdStart = millis();

  s_clientId = "awning-" + WiFi.macAddress();
  s_clientId.replace(":", "");

  static char portBuf[8]; snprintf(portBuf, sizeof(portBuf), "%u", g_mqttPort);
  static WiFiManagerParameter ph("host", "MQTT host", g_mqttHost, 40);
  static WiFiManagerParameter pt("port", "MQTT port", portBuf, 6);
  static WiFiManagerParameter pu("user", "MQTT user", g_mqttUser, 32);
  static WiFiManagerParameter pw("pass", "MQTT pass", g_mqttPass, 64);
  pp_host = &ph; pp_port = &pt; pp_user = &pu; pp_pass = &pw;

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.addParameter(&ph); wm.addParameter(&pt); wm.addParameter(&pu); wm.addParameter(&pw);
  wm.setSaveParamsCallback(saveParamsCallback);
  wm.setAPCallback([](WiFiManager*) { s_portalActive = true; });
  if (doubleReset) { Serial.println(F("Double reset - clearing WiFi credentials")); wm.resetSettings(); }

  bool ok = wm.autoConnect(AP_SSID);        // captive portal if unprovisioned (FR-6.2)
  s_portalActive = false;
  if (!ok) { Serial.println(F("WiFi connect failed - restarting")); delay(1000); ESP.restart(); }
  Serial.printf("WiFi up: %s  IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  mqtt.setServer(g_mqttHost, g_mqttPort);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(mqttCallback);

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.begin();                        // FR-9.1

  setupWeb();
  WiFi.setAutoReconnect(true);               // FR-6.4
}

void netLoop() {
  if (s_drdWindow && millis() - s_drdStart > DRD_WINDOW_S * 1000UL) {
    s_drdFlag = 0; s_drdWindow = false;       // no second reset -> disarm DRD
  }
  ArduinoOTA.handle();
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED) {
    mqttEnsure();
    mqtt.loop();
    static uint32_t lastPub = 0;
    if (millis() - lastPub > 2000) { lastPub = millis(); publishState(); }
  }
}

// ---------- Status getters ----------
bool   netWifiConnected() { return WiFi.status() == WL_CONNECTED; }
bool   netMqttConnected() { return mqtt.connected(); }
PubSubClient& netMqttClient() { return mqtt; }
bool   netPortalActive()  { return s_portalActive; }
String netIP()            { return WiFi.localIP().toString(); }
int    netRSSI()          { return WiFi.RSSI(); }
String netSSID()          { return WiFi.SSID(); }
