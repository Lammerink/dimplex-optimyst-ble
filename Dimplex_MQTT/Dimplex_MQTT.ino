/*
 * Dimplex CAS400LNH -> Home Assistant via MQTT (M5Stack Atom Lite / ESP32-PICO-D4)
 * Board: M5Atom.  Libraries: NimBLE-Arduino 2.x, PubSubClient, WiFiManager, built-in WiFi.
 *
 * BLE (NimBLE): pair w/ fixed passkey 584936, control characteristic handle 0x0040
 * (byte1 = flame level 0..6). Bridges to Home Assistant over MQTT auto-discovery.
 *
 * CONFIG PORTAL (WiFiManager): no need to re-flash for WiFi/MQTT changes.
 *   - First boot (or WiFi fails): starts an AP "Dimplex-Setup" (pass "dimplex123").
 *   - To reconfigure anytime: hold the Atom button while powering on (or hold it ~3s
 *     while running) -> opens the same setup portal.
 *   - Connect your phone to "Dimplex-Setup", a page opens to set WiFi + MQTT settings.
 *   - Settings are saved to flash (NVS) and survive reboots/power loss.
 */
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include "esp_gap_ble_api.h"

#define BTN_PIN 39
// Fixed BLE pairing passkey for this fire. 584936 worked on the author's CAS400LNH;
// it may be universal for this product line or per-unit. If pairing fails, find yours
// by sniffing the remote's pairing and cracking it (see docs/PROTOCOL.md).
static const uint32_t PASSKEY = 584936;
// Your fireplace's BLE address — change to match yours (see it in nRF Connect, or it
// advertises as "FI####<Dimplex>").
static NimBLEAddress FIRE_ADDR("00:a0:50:d6:a5:14", BLE_ADDR_PUBLIC);

// ---- MQTT settings (defaults) ----
// These are defaults; they can be overridden at runtime via the "Dimplex-Setup" WiFi
// portal (saved to flash). WiFi credentials are NEVER stored in source - only via the
// portal. NOTE: if you publish this repo, these MQTT creds become public - use a
// throwaway/local broker account (or blank them and use the portal).
static char cfg_host[40] = "homeassistant.local";  // MQTT broker: IP, hostname, or *.local (mDNS)
static char cfg_port[6]  = "1883";
static char cfg_user[32] = "dimplex";   // MQTT username
static char cfg_pass[40] = "dimplex";   // MQTT password

Preferences prefs;
WiFiManager wm;
WiFiManagerParameter p_host("host", "MQTT broker (IP or hostname, e.g. homeassistant.local)", cfg_host, 40);
WiFiManagerParameter p_port("port", "MQTT port", cfg_port, 6);
WiFiManagerParameter p_user("user", "MQTT username", cfg_user, 32);
WiFiManagerParameter p_pass("pass", "MQTT password", cfg_pass, 40);

// MQTT topics
static const char *T_AVAIL = "dimplex/status";
static const char *T_PWR_CMD = "dimplex/power/set", *T_PWR_ST = "dimplex/power/state";
static const char *T_LVL_CMD = "dimplex/level/set", *T_LVL_ST = "dimplex/level/state";
static const char *T_VOL_CMD = "dimplex/volume/set", *T_VOL_ST = "dimplex/volume/state";

WiFiClient g_wifi;
PubSubClient g_mqtt(g_wifi);

static NimBLEClient *g_client = nullptr;
static NimBLERemoteCharacteristic *g_ctrl = nullptr, *g_h12 = nullptr, *g_h87 = nullptr, *g_vol = nullptr;
static volatile bool g_connected = false, g_paired = false;
static bool g_ready = false, g_initDone = false;
static uint8_t g_lastLevel = 6;
static int g_pubLevel = -1, g_pubVol = -1;

// ---------------- config persistence ----------------
static void loadConfig() {
  prefs.begin("cfg", true);
  // getString(key, default) returns the saved value, or the current default if unset
  strlcpy(cfg_host, prefs.getString("host", cfg_host).c_str(), sizeof(cfg_host));
  strlcpy(cfg_port, prefs.getString("port", cfg_port).c_str(), sizeof(cfg_port));
  strlcpy(cfg_user, prefs.getString("user", cfg_user).c_str(), sizeof(cfg_user));
  strlcpy(cfg_pass, prefs.getString("pass", cfg_pass).c_str(), sizeof(cfg_pass));
  prefs.end();
}
static void saveParamsCallback() {
  strncpy(cfg_host, p_host.getValue(), sizeof(cfg_host));
  strncpy(cfg_port, p_port.getValue(), sizeof(cfg_port));
  strncpy(cfg_user, p_user.getValue(), sizeof(cfg_user));
  strncpy(cfg_pass, p_pass.getValue(), sizeof(cfg_pass));
  prefs.begin("cfg", false);
  prefs.putString("host", cfg_host); prefs.putString("port", cfg_port);
  prefs.putString("user", cfg_user); prefs.putString("pass", cfg_pass);
  prefs.end();
  Serial.printf("[cfg] saved: host=%s port=%s user=%s\n", cfg_host, cfg_port, cfg_user);
}

// ---------------- BLE (NimBLE) ----------------
class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *) override { g_connected = true; Serial.println("[ble] connected"); }
  void onDisconnect(NimBLEClient *, int reason) override {
    Serial.printf("[ble] disconnected (%d)\n", reason);
    g_connected = g_paired = g_ready = g_initDone = false; g_ctrl = g_h12 = g_h87 = nullptr;
  }
  void onPassKeyEntry(NimBLEConnInfo &ci) override { NimBLEDevice::injectPassKey(ci, PASSKEY); }
  void onConfirmPasskey(NimBLEConnInfo &ci, uint32_t) override { NimBLEDevice::injectConfirmPasskey(ci, true); }
  void onAuthenticationComplete(NimBLEConnInfo &ci) override {
    Serial.printf("[sec] auth complete enc=%d\n", ci.isEncrypted()); g_paired = ci.isEncrypted();
  }
};
static bool resolveChars() {
  g_ctrl = g_h12 = g_h87 = g_vol = nullptr;
  for (auto *s : g_client->getServices(true))
    for (auto *c : s->getCharacteristics(true)) {
      uint16_t h = c->getHandle();
      if (h == 0x0040) g_ctrl = c; else if (h == 0x0012) g_h12 = c;
      else if (h == 0x0087) g_h87 = c; else if (h == 0x0076) g_vol = c;
    }
  return g_ctrl != nullptr;
}
static void sendInit() {
  uint8_t i12[] = {0x14,0x13,0x01,0x01,0x00,0x0a,0x01,0x07}, a87[] = {0x01,0x01,0x00}, b87[] = {0x00,0x00,0x00};
  if (g_h12) g_h12->writeValue(i12, sizeof(i12), true);
  if (g_h87) { g_h87->writeValue(a87, sizeof(a87), true); delay(120); g_h87->writeValue(b87, sizeof(b87), true); }
  g_initDone = true;
}
static void bleSetFlame(uint8_t level) {
  if (!g_ready || !g_ctrl) return;
  if (!g_initDone) sendInit();
  if (level > 6) level = 6;
  if (level > 0) g_lastLevel = level;
  uint8_t v = level; g_ctrl->writeValue(&v, 1, true);
  Serial.printf("[ble] flame -> %s (0x%02x)\n", level ? "ON" : "OFF", level);
}
static void bleSetVolume(uint8_t v) {
  if (!g_ready || !g_vol) return;
  if (v > 6) v = 6;
  g_vol->writeValue(&v, 1, true);
  Serial.printf("[ble] volume -> %u\n", v);
}
static void bleConnect() {
  if (g_connected) return;
  if (!g_client) { g_client = NimBLEDevice::createClient(); g_client->setClientCallbacks(new ClientCB(), false); }
  if (!g_client->connect(FIRE_ADDR)) { Serial.println("[ble] connect failed"); return; }
  if (!g_client->secureConnection()) { Serial.println("[ble] secure failed"); return; }
}

// ---------------- MQTT ----------------
static void publishDiscovery() {
  const char *dev = "\"dev\":{\"ids\":[\"dimplex_f000b540\"],\"name\":\"Dimplex CAS400LNH\",\"mf\":\"Dimplex\",\"mdl\":\"CAS400LNH\"}";
  char b[480];
  snprintf(b, sizeof(b), "{\"name\":\"Fireplace\",\"uniq_id\":\"dimplex_power\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"avty_t\":\"%s\",\"ic\":\"mdi:fireplace\",%s}", T_PWR_CMD, T_PWR_ST, T_AVAIL, dev);
  g_mqtt.publish("homeassistant/switch/dimplex/power/config", b, true);
  snprintf(b, sizeof(b), "{\"name\":\"Flame level\",\"uniq_id\":\"dimplex_level\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"min\":0,\"max\":6,\"step\":1,\"mode\":\"slider\",\"avty_t\":\"%s\",\"ic\":\"mdi:fire\",%s}", T_LVL_CMD, T_LVL_ST, T_AVAIL, dev);
  g_mqtt.publish("homeassistant/number/dimplex/level/config", b, true);
  snprintf(b, sizeof(b), "{\"name\":\"Flame level state\",\"uniq_id\":\"dimplex_levelstate\",\"stat_t\":\"%s\",\"avty_t\":\"%s\",\"ic\":\"mdi:fire\",%s}", T_LVL_ST, T_AVAIL, dev);
  g_mqtt.publish("homeassistant/sensor/dimplex/level/config", b, true);
  snprintf(b, sizeof(b), "{\"name\":\"Volume\",\"uniq_id\":\"dimplex_volume\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"min\":0,\"max\":6,\"step\":1,\"mode\":\"slider\",\"avty_t\":\"%s\",\"ic\":\"mdi:volume-high\",%s}", T_VOL_CMD, T_VOL_ST, T_AVAIL, dev);
  g_mqtt.publish("homeassistant/number/dimplex/volume/config", b, true);
  // ensure the old (removed) water sensor stays deleted
  g_mqtt.publish("homeassistant/binary_sensor/dimplex/water/config", "", true);
  Serial.println("[mqtt] discovery published");
}
static void publishState(uint8_t level) {
  g_mqtt.publish(T_PWR_ST, level ? "ON" : "OFF", true);
  char b[4]; snprintf(b, sizeof(b), "%u", level); g_mqtt.publish(T_LVL_ST, b, true);
}
static void mqttCallback(char *topic, byte *payload, unsigned int len) {
  String t(topic), p; for (unsigned i = 0; i < len; i++) p += (char) payload[i];
  if (t == T_VOL_CMD) {            // volume control
    int vol = p.toInt(); if (vol < 0) vol = 0; if (vol > 6) vol = 6;
    bleSetVolume((uint8_t) vol);
    char vb[4]; snprintf(vb, sizeof(vb), "%d", vol); g_mqtt.publish(T_VOL_ST, vb, true);
    g_pubVol = vol;
    return;
  }
  int cmd = -1;
  if (t == T_PWR_CMD) cmd = (p == "ON") ? g_lastLevel : 0;
  else if (t == T_LVL_CMD) cmd = (uint8_t) p.toInt();
  if (cmd < 0) return;
  if (cmd > 6) cmd = 6;
  bleSetFlame((uint8_t) cmd);
  publishState((uint8_t) cmd);   // instant confirm (no HA switch revert)
  g_pubLevel = cmd;
}
static bool g_mdnsStarted = false, g_serverSet = false;
// Resolve the configured broker and point PubSubClient at it. Handles a numeric IP,
// a *.local mDNS name (resolved to an IP), or a plain DNS hostname.
static void resolveAndSetServer() {
  IPAddress ip;
  String h = cfg_host;
  if (ip.fromString(h)) {                          // already a numeric IP
    g_mqtt.setServer(ip, (uint16_t) atoi(cfg_port)); g_serverSet = true; return;
  }
  if (h.endsWith(".local")) {                      // mDNS name -> resolve to an IP
    IPAddress mip = MDNS.queryHost(h.substring(0, h.length() - 6), 2000);
    if ((uint32_t) mip != 0) {
      Serial.printf("[mdns] %s -> %s\n", cfg_host, mip.toString().c_str());
      g_mqtt.setServer(mip, (uint16_t) atoi(cfg_port)); g_serverSet = true;
    } else {
      Serial.printf("[mdns] could not resolve %s (retrying)\n", cfg_host);
      g_serverSet = false;
    }
    return;
  }
  g_mqtt.setServer(cfg_host, (uint16_t) atoi(cfg_port)); g_serverSet = true;  // plain DNS hostname
}
static void mqttReconnect() {
  static uint32_t last = 0;
  if (g_mqtt.connected() || millis() - last < 3000) return;
  last = millis();
  if (g_mqtt.connect("dimplex-atom", cfg_user, cfg_pass, T_AVAIL, 0, true, "offline")) {
    Serial.println("[mqtt] connected");
    g_mqtt.publish(T_AVAIL, "online", true);
    g_mqtt.subscribe(T_PWR_CMD); g_mqtt.subscribe(T_LVL_CMD); g_mqtt.subscribe(T_VOL_CMD);
    publishDiscovery();
    g_pubLevel = -1; g_pubVol = -1;
  } else {
    Serial.printf("[mqtt] connect failed rc=%d\n", g_mqtt.state());
    static uint8_t fails = 0;
    if (++fails >= 3) { fails = 0; g_serverSet = false; }  // re-resolve (broker IP may have changed)
  }
}
static void pollAndPublish() {
  if (!g_ready || !g_ctrl) return;
  NimBLEAttValue v = g_ctrl->readValue();
  if (v.length() >= 1) {
    uint8_t level = v[0];
    if (level != g_pubLevel) { publishState(level); g_pubLevel = level; Serial.printf("[pub] level=%u\n", level); }
  }
  if (g_vol) {                       // poll volume too
    NimBLEAttValue vv = g_vol->readValue();
    if (vv.length() >= 1 && vv[0] != g_pubVol) {
      char vb[4]; snprintf(vb, sizeof(vb), "%u", vv[0]); g_mqtt.publish(T_VOL_ST, vb, true);
      g_pubVol = vv[0]; Serial.printf("[pub] volume=%u\n", vv[0]);
    }
  }
}

// ---------------- config portal ----------------
static void runConfigPortal(bool forced) {
  wm.setConfigPortalTimeout(180);
  wm.setSaveParamsCallback(saveParamsCallback);
  // refresh param defaults to current values so the page pre-fills
  p_host.setValue(cfg_host, 40); p_port.setValue(cfg_port, 6);
  p_user.setValue(cfg_user, 32); p_pass.setValue(cfg_pass, 40);
  Serial.println(forced ? "[cfg] opening setup portal (join WiFi 'Dimplex-Setup' / pass 'dimplex123')"
                        : "[wifi] connecting to saved network (opens 'Dimplex-Setup' portal only if none saved / connect fails)");
  if (forced) wm.startConfigPortal("Dimplex-Setup", "dimplex123");
  else        wm.autoConnect("Dimplex-Setup", "dimplex123");
  loadConfig();   // pick up any changes
  g_serverSet = false;   // re-resolve broker with the (possibly new) host
}

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\nDimplex MQTT bridge (NimBLE + config portal) starting...");
  pinMode(BTN_PIN, INPUT);

  loadConfig();
  Serial.printf("[cfg] mqtt host=%s port=%s user=%s\n", cfg_host, cfg_port, cfg_user);

  NimBLEDevice::init("dimplex-atom");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(false, true, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);

  wm.addParameter(&p_host); wm.addParameter(&p_port);
  wm.addParameter(&p_user); wm.addParameter(&p_pass);

  bool buttonHeld = (digitalRead(BTN_PIN) == LOW);   // hold button at power-on -> portal
  runConfigPortal(buttonHeld);

  g_mqtt.setCallback(mqttCallback);
  g_mqtt.setBufferSize(512);
  // MQTT server is resolved + set in loop() once WiFi (and mDNS for .local) is up.
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!g_mdnsStarted) { MDNS.begin("dimplex-atom"); g_mdnsStarted = true; }
    static uint32_t lastResolve = 0;
    if (!g_serverSet && millis() - lastResolve > 4000) { lastResolve = millis(); resolveAndSetServer(); }
    if (g_serverSet) { mqttReconnect(); g_mqtt.loop(); }
  }

  static uint32_t lastBle = 0;
  if (!g_connected && millis() - lastBle > 5000) { lastBle = millis(); bleConnect(); }
  if (g_connected && g_paired && !g_ready) { if (resolveChars()) { g_ready = true; sendInit(); Serial.println("[ble] ready"); } }

  static uint32_t lastPoll = 0;
  if (g_ready && millis() - lastPoll > 8000) { lastPoll = millis(); pollAndPublish(); }

  // hold the button ~3s while running -> open setup portal to reconfigure
  static uint32_t pressStart = 0;
  if (digitalRead(BTN_PIN) == LOW) {
    if (pressStart == 0) pressStart = millis();
    else if (millis() - pressStart > 3000) { pressStart = 0; runConfigPortal(true); }
  } else pressStart = 0;
}
