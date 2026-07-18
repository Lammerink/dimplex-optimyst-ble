/*
 * Dimplex CAS400LNH -> Home Assistant via MQTT (M5Stack Atom Lite / ESP32-PICO-D4)
 * Board: M5Atom.  Libraries: NimBLE-Arduino 2.x, PubSubClient, WiFiManager, built-in WiFi.
 *
 * BLE (NimBLE): auto-detects the fireplace by advertised name ("FI####<Dimplex>") and
 * pairs with a passkey (default 584936). Control via raw handles (0x0040 power, 0x0042
 * flame, 0x0076 volume). Bridges to Home Assistant over MQTT auto-discovery. The BLE
 * address and passkey can be overridden in the setup portal (with a scan-to-pick list).
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
#include <WebServer.h>
#include <NimBLEDevice.h>
#include "esp_gap_ble_api.h"

#define BTN_PIN 39

// ---- Fireplace identity (auto-detected by default; override in the setup portal) ----
// The bridge finds your fireplace by scanning for a BLE device whose advertised name
// contains "Dimplex" (they show up as "FI####<Dimplex>"), so normally you set nothing.
// If you have more than one, or want to pin a specific unit, enter its BLE address in the
// "Dimplex-Setup" portal (there's a scan-to-pick list). The passkey 584936 worked on the
// author's CAS400LNH; it may be universal for this product line or per-unit — if pairing
// fails, find yours by sniffing + cracking the remote's pairing (see docs/PROTOCOL.md).
#define FP_NAME_MATCH  "Dimplex"        // advertised-name substring used for auto-detect
#define DEFAULT_PASSKEY "584936"        // default pairing passkey (portal-overridable)
static char cfg_addr[20] = "";          // "" = auto-detect; else a pinned BLE MAC
static char cfg_pkey[12] = DEFAULT_PASSKEY;
static uint32_t g_passkey = 584936;     // parsed from cfg_pkey at load time

// ---- MQTT settings (defaults) ----
// >>> USE YOUR OWN MQTT CREDENTIALS <<<
// The user/pass below ("dimplex"/"dimplex") are the author's LOCAL broker account, kept
// only as an example. Replace them with your own broker's username/password - either edit
// them here, or (recommended) leave them and set your own at runtime via the "Dimplex-Setup"
// WiFi portal, which saves to flash and overrides these. Wi-Fi credentials are NEVER stored
// in source; they are entered only through the portal.
static char cfg_host[40] = "homeassistant.local";  // MQTT broker: IP, hostname, or *.local (mDNS)
static char cfg_port[6]  = "1883";
static char cfg_user[32] = "dimplex";   // <-- change to your MQTT username (or set via portal)
static char cfg_pass[40] = "dimplex";   // <-- change to your MQTT password (or set via portal)

Preferences prefs;
WiFiManager wm;
WiFiManagerParameter p_host("host", "MQTT broker (IP or hostname, e.g. homeassistant.local)", cfg_host, 40);
WiFiManagerParameter p_port("port", "MQTT port", cfg_port, 6);
WiFiManagerParameter p_user("user", "MQTT username", cfg_user, 32);
WiFiManagerParameter p_pass("pass", "MQTT password", cfg_pass, 40);
// Fireplace picker: a <select> filled from a BLE scan when the portal opens; choosing an
// entry fills the address field below. Leave the address blank to auto-detect.
static char g_scanHtml[1400] = "";
WiFiManagerParameter p_scan(g_scanHtml);
WiFiManagerParameter p_addr("fpaddr", "Fireplace BLE address (blank = auto-detect)", cfg_addr, 19);
WiFiManagerParameter p_pkey("pkey", "Pairing passkey", cfg_pkey, 11);

// MQTT topics
static const char *T_AVAIL = "dimplex/status";
static const char *T_PWR_CMD = "dimplex/power/set", *T_PWR_ST = "dimplex/power/state";
static const char *T_LVL_CMD = "dimplex/level/set", *T_LVL_ST = "dimplex/level/state";
static const char *T_VOL_CMD = "dimplex/volume/set", *T_VOL_ST = "dimplex/volume/state";

WiFiClient g_wifi;
PubSubClient g_mqtt(g_wifi);
WebServer g_web(80);              // control page on the LAN (http://dimplex-atom.local/)
static bool g_webStarted = false;

static NimBLEClient *g_client = nullptr;
static NimBLERemoteCharacteristic *g_ctrl = nullptr, *g_lvl = nullptr, *g_h12 = nullptr, *g_h87 = nullptr, *g_vol = nullptr;
static volatile bool g_connected = false, g_paired = false;
static bool g_ready = false, g_initDone = false;
static bool g_haveTarget = false;        // whether g_target holds a resolved address
static NimBLEAddress g_target;           // the fireplace we connect to (pinned or auto-detected)
static char g_detaddr[20] = "";          // last auto-detected address, remembered in NVS
static uint8_t g_lastLevel = 6;
static int g_pubPower = -1, g_pubLevel = -1, g_pubVol = -1;

// ---------------- config persistence ----------------
static void loadConfig() {
  prefs.begin("cfg", true);
  // getString(key, default) returns the saved value, or the current default if unset
  strlcpy(cfg_host, prefs.getString("host", cfg_host).c_str(), sizeof(cfg_host));
  strlcpy(cfg_port, prefs.getString("port", cfg_port).c_str(), sizeof(cfg_port));
  strlcpy(cfg_user, prefs.getString("user", cfg_user).c_str(), sizeof(cfg_user));
  strlcpy(cfg_pass, prefs.getString("pass", cfg_pass).c_str(), sizeof(cfg_pass));
  strlcpy(cfg_addr, prefs.getString("addr", cfg_addr).c_str(), sizeof(cfg_addr));
  strlcpy(cfg_pkey, prefs.getString("pkey", cfg_pkey).c_str(), sizeof(cfg_pkey));
  strlcpy(g_detaddr, prefs.getString("detaddr", "").c_str(), sizeof(g_detaddr));
  prefs.end();
  unsigned long k = strtoul(cfg_pkey, nullptr, 10);
  g_passkey = (k > 0 && k <= 999999) ? (uint32_t) k : 584936;   // 6-digit legacy passkey
}
// Remember (or clear) the auto-detected address so we skip scanning on later boots.
static void saveDetectedAddr(const char *mac) {
  strlcpy(g_detaddr, mac ? mac : "", sizeof(g_detaddr));
  prefs.begin("cfg", false); prefs.putString("detaddr", g_detaddr); prefs.end();
}
static void saveParamsCallback() {
  strncpy(cfg_host, p_host.getValue(), sizeof(cfg_host));
  strncpy(cfg_port, p_port.getValue(), sizeof(cfg_port));
  strncpy(cfg_user, p_user.getValue(), sizeof(cfg_user));
  strncpy(cfg_pass, p_pass.getValue(), sizeof(cfg_pass));
  strncpy(cfg_addr, p_addr.getValue(), sizeof(cfg_addr));
  strncpy(cfg_pkey, p_pkey.getValue(), sizeof(cfg_pkey));
  prefs.begin("cfg", false);
  prefs.putString("host", cfg_host); prefs.putString("port", cfg_port);
  prefs.putString("user", cfg_user); prefs.putString("pass", cfg_pass);
  prefs.putString("addr", cfg_addr); prefs.putString("pkey", cfg_pkey);
  prefs.putString("detaddr", "");   // forget any remembered auto-detect; resolve fresh
  prefs.end();
  g_detaddr[0] = 0;
  unsigned long k = strtoul(cfg_pkey, nullptr, 10);
  g_passkey = (k > 0 && k <= 999999) ? (uint32_t) k : 584936;
  g_haveTarget = false;   // re-resolve the fireplace with the (possibly new) address
  Serial.printf("[cfg] saved: host=%s port=%s user=%s addr=%s\n",
                cfg_host, cfg_port, cfg_user, strlen(cfg_addr) ? cfg_addr : "(auto)");
}

// ---------------- BLE (NimBLE) ----------------
class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *) override { g_connected = true; Serial.println("[ble] connected"); }
  void onDisconnect(NimBLEClient *, int reason) override {
    Serial.printf("[ble] disconnected (%d)\n", reason);
    g_connected = g_paired = g_ready = g_initDone = false; g_ctrl = g_lvl = g_h12 = g_h87 = g_vol = nullptr;
  }
  void onPassKeyEntry(NimBLEConnInfo &ci) override { NimBLEDevice::injectPassKey(ci, g_passkey); }
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
      if (h == 0x0040) g_ctrl = c; else if (h == 0x0042) g_lvl = c; else if (h == 0x0012) g_h12 = c;
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
static void blePower(bool on) {            // 0x0040 = power on/off (single write; fire keeps its own level)
  if (!g_ready || !g_ctrl) return;
  if (!g_initDone) sendInit();
  uint8_t v = on ? 0x06 : 0x00; g_ctrl->writeValue(&v, 1, true);
  Serial.printf("[ble] power -> %s\n", on ? "ON" : "OFF");
}
static void bleSetLevel(uint8_t level) {   // 0x0042 = flame intensity 1..6 (independent of power)
  if (!g_ready || !g_lvl) return;
  if (!g_initDone) sendInit();
  if (level < 1) level = 1; if (level > 6) level = 6;
  g_lastLevel = level;
  uint8_t L = level; g_lvl->writeValue(&L, 1, true);
  Serial.printf("[ble] flame intensity -> %u\n", level);
}
static void bleSetVolume(uint8_t v) {
  if (!g_ready || !g_vol) return;
  if (v > 6) v = 6;
  g_vol->writeValue(&v, 1, true);
  Serial.printf("[ble] volume -> %u\n", v);
}
// Scan for a fireplace by advertised name. Returns true and sets `out` on the first match.
static bool discoverFireplace(NimBLEAddress &out) {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);                       // active scan -> we get scan-response names
  NimBLEScanResults res = scan->getResults(4000, false);
  bool found = false;
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice *d = res.getDevice(i);
    if (String(d->getName().c_str()).indexOf(FP_NAME_MATCH) < 0) continue;
    out = d->getAddress(); found = true; break;
  }
  scan->clearResults();
  return found;
}
// Scan and build the portal's <select> of found fireplaces (choosing one fills #fpaddr).
static void refreshScanParam() {
  Serial.println("[ble] scanning for fireplaces (setup portal)...");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(4000, false);
  String o = "<label for='fpaddr'>Fireplace (from scan)</label>"
             "<select onchange=\"var e=document.getElementById('fpaddr');if(e)e.value=this.value;\" "
             "style='width:100%;padding:5px;margin:2px 0'>"
             "<option value=''>Auto-detect (recommended)</option>";
  int n = 0;
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice *d = res.getDevice(i);
    String name = d->getName().c_str();
    if (name.indexOf(FP_NAME_MATCH) < 0) continue;
    String mac = d->getAddress().toString().c_str();
    name.replace("<", "&lt;"); name.replace(">", "&gt;");     // "<Dimplex>" would break the HTML
    o += "<option value='" + mac + "'>" + name + " &mdash; " + mac + "</option>";
    n++;
  }
  o += "</select>";
  scan->clearResults();
  strlcpy(g_scanHtml, o.c_str(), sizeof(g_scanHtml));
  Serial.printf("[ble] scan found %d fireplace(s)\n", n);
}
static void bleConnect() {
  if (g_connected) return;
  if (!g_client) { g_client = NimBLEDevice::createClient(); g_client->setClientCallbacks(new ClientCB(), false); }
  if (!g_haveTarget) {                               // resolve which fireplace to talk to
    if (strlen(cfg_addr) >= 17) {                    // pinned address from the portal
      g_target = NimBLEAddress(cfg_addr, BLE_ADDR_PUBLIC); g_haveTarget = true;
    } else if (strlen(g_detaddr) >= 17) {            // remembered from a previous auto-detect
      g_target = NimBLEAddress(g_detaddr, BLE_ADDR_PUBLIC); g_haveTarget = true;
      Serial.printf("[ble] using remembered fireplace %s\n", g_detaddr);
    } else if (discoverFireplace(g_target)) {        // fresh auto-detect by name
      g_haveTarget = true;
      saveDetectedAddr(g_target.toString().c_str());
      Serial.printf("[ble] auto-detected fireplace %s (remembered)\n", g_detaddr);
    } else {
      Serial.println("[ble] no fireplace found in scan (will retry)"); return;
    }
  }
  if (!g_client->connect(g_target)) {
    Serial.println("[ble] connect failed");
    if (!strlen(cfg_addr)) {                         // auto mode: drop target, and if it was a
      g_haveTarget = false;                          // remembered address, forget it and re-scan
      if (strlen(g_detaddr)) { Serial.println("[ble] forgetting remembered address; will re-scan"); saveDetectedAddr(""); }
    }
    return;
  }
  if (!g_client->secureConnection()) { Serial.println("[ble] secure failed"); return; }
}

// ---------------- MQTT ----------------
static void publishDiscovery() {
  const char *dev = "\"dev\":{\"ids\":[\"dimplex_cas400\"],\"name\":\"Dimplex CAS400LNH\",\"mf\":\"Dimplex\",\"mdl\":\"CAS400LNH\"}";
  char b[480];
  snprintf(b, sizeof(b), "{\"name\":\"Fireplace\",\"uniq_id\":\"dimplex_power\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"avty_t\":\"%s\",\"ic\":\"mdi:fireplace\",%s}", T_PWR_CMD, T_PWR_ST, T_AVAIL, dev);
  g_mqtt.publish("homeassistant/switch/dimplex/power/config", b, true);
  snprintf(b, sizeof(b), "{\"name\":\"Flame intensity\",\"uniq_id\":\"dimplex_level\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"min\":1,\"max\":6,\"step\":1,\"mode\":\"slider\",\"avty_t\":\"%s\",\"ic\":\"mdi:fire\",%s}", T_LVL_CMD, T_LVL_ST, T_AVAIL, dev);
  g_mqtt.publish("homeassistant/number/dimplex/level/config", b, true);
  snprintf(b, sizeof(b), "{\"name\":\"Flame intensity state\",\"uniq_id\":\"dimplex_levelstate\",\"stat_t\":\"%s\",\"avty_t\":\"%s\",\"ic\":\"mdi:fire\",%s}", T_LVL_ST, T_AVAIL, dev);
  g_mqtt.publish("homeassistant/sensor/dimplex/level/config", b, true);
  snprintf(b, sizeof(b), "{\"name\":\"Volume\",\"uniq_id\":\"dimplex_volume\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"min\":0,\"max\":6,\"step\":1,\"mode\":\"slider\",\"avty_t\":\"%s\",\"ic\":\"mdi:volume-high\",%s}", T_VOL_CMD, T_VOL_ST, T_AVAIL, dev);
  g_mqtt.publish("homeassistant/number/dimplex/volume/config", b, true);
  // ensure the old (removed) water sensor stays deleted
  g_mqtt.publish("homeassistant/binary_sensor/dimplex/water/config", "", true);
  Serial.println("[mqtt] discovery published");
}
static void publishPower(bool on) { g_mqtt.publish(T_PWR_ST, on ? "ON" : "OFF", true); }
static void publishLevel(uint8_t lvl) { char b[4]; snprintf(b, sizeof(b), "%u", lvl); g_mqtt.publish(T_LVL_ST, b, true); }
static void mqttCallback(char *topic, byte *payload, unsigned int len) {
  String t(topic), p; for (unsigned i = 0; i < len; i++) p += (char) payload[i];
  if (t == T_VOL_CMD) {            // volume control
    int vol = p.toInt(); if (vol < 0) vol = 0; if (vol > 6) vol = 6;
    bleSetVolume((uint8_t) vol);
    char vb[4]; snprintf(vb, sizeof(vb), "%d", vol); g_mqtt.publish(T_VOL_ST, vb, true);
    g_pubVol = vol;
    return;
  }
  if (t == T_PWR_CMD) {            // power on/off
    bool on = (p == "ON"); blePower(on); publishPower(on); g_pubPower = on; return;
  }
  if (t == T_LVL_CMD) {            // flame intensity 1..6
    int n = p.toInt(); if (n < 1) n = 1; if (n > 6) n = 6;
    bleSetLevel((uint8_t) n); publishLevel(n); g_pubLevel = n; return;
  }
}
static bool g_serverSet = false;
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
  NimBLEAttValue v = g_ctrl->readValue();          // 0x0040 byte0 != 0 => on
  bool on = (v.length() >= 1 && v[0] != 0);
  if ((int) on != g_pubPower) { publishPower(on); g_pubPower = on; Serial.printf("[pub] power=%d\n", on); }
  if (g_lvl) {                                     // 0x0042 = flame intensity (independent of power)
    NimBLEAttValue lv = g_lvl->readValue();
    if (lv.length() >= 1 && lv[0] != g_pubLevel) { publishLevel(lv[0]); g_pubLevel = lv[0]; Serial.printf("[pub] level=%u\n", lv[0]); }
  }
  if (g_vol) {                       // poll volume too
    NimBLEAttValue vv = g_vol->readValue();
    if (vv.length() >= 1 && vv[0] != g_pubVol) {
      char vb[4]; snprintf(vb, sizeof(vb), "%u", vv[0]); g_mqtt.publish(T_VOL_ST, vb, true);
      g_pubVol = vv[0]; Serial.printf("[pub] volume=%u\n", vv[0]);
    }
  }
}

// ---------------- web control page ----------------
static String btnRow(const char *kind, int cur, int start) {
  String s;
  for (int i = start; i <= 6; i++) {
    s += "<a class='b"; s += (i == cur) ? " on'" : "'";
    s += " href='/set?"; s += kind; s += "="; s += i; s += "'>";
    s += (i == 0) ? "Off" : String(i); s += "</a> ";
  }
  return s;
}
static void handleRoot() {
  String h = "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
             "<title>Dimplex Fireplace</title><style>body{font-family:sans-serif;max-width:480px;margin:18px auto;"
             "padding:0 12px;color:#222}h2{color:#e25822}.b{display:inline-block;min-width:34px;text-align:center;"
             "padding:10px 12px;margin:3px;background:#eee;border-radius:8px;text-decoration:none;color:#222}"
             ".b.on{background:#e25822;color:#fff}</style></head><body><h2>&#128293; Dimplex Fireplace</h2>";
  h += "<p><b>Power:</b> " + String(g_pubPower == 1 ? "On" : (g_pubPower == 0 ? "Off" : "?")) +
       " &nbsp;&nbsp; <b>Flame:</b> " + String(g_pubLevel < 1 ? 1 : g_pubLevel) +
       " &nbsp;&nbsp; <b>Volume:</b> " + String(g_pubVol < 0 ? 0 : g_pubVol) + "</p>";
  h += "<h3>Power</h3><a class='b"; h += (g_pubPower == 1) ? " on'" : "'"; h += " href='/set?power=on'>On</a> ";
  h += "<a class='b"; h += (g_pubPower == 0) ? " on'" : "'"; h += " href='/set?power=off'>Off</a>";
  h += "<h3>Flame intensity</h3>" + btnRow("flame", g_pubLevel, 1);
  h += "<h3>Volume</h3>" + btnRow("vol", g_pubVol, 0);
  h += "<p><a class=b href='/'>&#8635; Refresh</a></p><p style='color:#888;font-size:90%'>";
  h += g_ready ? "Connected &amp; paired to the fireplace." : "Connecting to the fireplace&hellip;";
  h += "<br>Fireplace: ";
  h += strlen(cfg_addr) ? cfg_addr : "auto-detect";
  if (g_haveTarget) { h += " ("; h += g_target.toString().c_str(); h += ")"; }
  h += "<br>To change WiFi/MQTT/fireplace: hold the device button ~3s (opens the &lsquo;Dimplex-Setup&rsquo; network).</p>"
       "</body></html>";
  g_web.send(200, "text/html", h);
}
static void handleSet() {
  if (g_web.hasArg("power")) { bool on = (g_web.arg("power") == "on"); blePower(on); publishPower(on); g_pubPower = on; }
  if (g_web.hasArg("flame")) { int n = g_web.arg("flame").toInt(); n = n<1?1:(n>6?6:n);
    bleSetLevel(n); publishLevel(n); g_pubLevel = n; }
  if (g_web.hasArg("vol"))   { int n = g_web.arg("vol").toInt(); n = n<0?0:(n>6?6:n);
    bleSetVolume(n); char vb[4]; snprintf(vb, sizeof(vb), "%d", n); g_mqtt.publish(T_VOL_ST, vb, true); g_pubVol = n; }
  g_web.sendHeader("Location", "/"); g_web.send(303);
}
static void startWeb() { g_web.on("/", handleRoot); g_web.on("/set", handleSet); g_web.begin(); g_webStarted = true; }

// ---------------- config portal ----------------
static void runConfigPortal(bool forced) {
  if (g_webStarted) { g_web.stop(); g_webStarted = false; }   // free port 80 for the AP portal
  wm.setConfigPortalTimeout(180);
  wm.setSaveParamsCallback(saveParamsCallback);
  // refresh param defaults to current values so the page pre-fills
  p_host.setValue(cfg_host, 40); p_port.setValue(cfg_port, 6);
  p_user.setValue(cfg_user, 32); p_pass.setValue(cfg_pass, 40);
  p_addr.setValue(cfg_addr, 19); p_pkey.setValue(cfg_pkey, 11);
  // scan for nearby fireplaces to fill the picker, but only when the portal will show
  // (forced by the button, or first boot with no saved WiFi) — avoids delaying normal boots
  if (forced || !wm.getWiFiIsSaved()) refreshScanParam();
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

  wm.addParameter(&p_scan); wm.addParameter(&p_addr); wm.addParameter(&p_pkey);
  wm.addParameter(&p_host); wm.addParameter(&p_port);
  wm.addParameter(&p_user); wm.addParameter(&p_pass);
  wm.setHostname("dimplex-atom");          // STA hostname -> reachable as dimplex-atom.local
  // (blocking config portal: on boot, if WiFi can't connect it opens the Dimplex-Setup AP
  //  and waits there for you to set WiFi.)

  bool buttonHeld = (digitalRead(BTN_PIN) == LOW);   // hold button at power-on -> portal
  runConfigPortal(buttonHeld);

  g_mqtt.setCallback(mqttCallback);
  g_mqtt.setBufferSize(512);
  // MQTT server is resolved + set in loop() once WiFi (and mDNS for .local) is up.
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!g_webStarted) {
      MDNS.begin("dimplex-atom");
      MDNS.addService("http", "tcp", 80);
      startWeb();
      Serial.printf("[web] control UI at http://dimplex-atom.local/  (or http://%s/)\n",
                    WiFi.localIP().toString().c_str());
    }
    g_web.handleClient();            // serve the control page
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
