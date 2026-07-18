/*
 * Dimplex register-dump tool (M5Stack Atom Lite, NimBLE) — for empirically mapping
 * what the remote changes. On each boot it pairs and dumps EVERY readable register.
 *
 * Workflow to identify a setting (e.g. volume):
 *   1. Power on Atom -> it prints "===DUMP===" with all registers (snapshot A).
 *   2. Unplug the Atom (frees the fire's BLE connection).
 *   3. On the REMOTE, change ONE setting (e.g. volume 03 -> 06).
 *   4. Plug the Atom back in -> it prints snapshot B.
 *   5. Diff A vs B -> the register(s) that changed = that setting.
 */
#include <NimBLEDevice.h>

static const uint32_t PASSKEY = 584936;
#define FP_NAME_MATCH "Dimplex"        // auto-detect the fire by advertised name ("FI####<Dimplex>")
static NimBLEClient *g_client = nullptr;
static volatile bool g_connected = false, g_paired = false;
static bool g_dumped = false;

class CB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *) override { g_connected = true; }
  void onDisconnect(NimBLEClient *, int) override { g_connected = g_paired = false; g_dumped = false; }
  void onPassKeyEntry(NimBLEConnInfo &ci) override { NimBLEDevice::injectPassKey(ci, PASSKEY); }
  void onConfirmPasskey(NimBLEConnInfo &ci, uint32_t) override { NimBLEDevice::injectConfirmPasskey(ci, true); }
  void onAuthenticationComplete(NimBLEConnInfo &ci) override { g_paired = ci.isEncrypted(); }
};

static String hx(const NimBLEAttValue &v) {
  String s; for (size_t i = 0; i < v.length(); i++) { char b[3]; sprintf(b, "%02x", v[i]); s += b; } return s;
}

// Scan for a fireplace by advertised name (so no hardcoded per-unit address).
static bool discoverFireplace(NimBLEAddress &out) {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
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

static void dumpAll() {
  Serial.println("===DUMP START===");
  for (auto *s : g_client->getServices(true)) {
    for (auto *c : s->getCharacteristics(true)) {
      if (!c->canRead()) continue;
      NimBLEAttValue v = c->readValue();
      Serial.printf("h=0x%04x uuid=%s val=%s\n", c->getHandle(), c->getUUID().toString().c_str(), hx(v).c_str());
    }
  }
  Serial.println("===DUMP END===");
}

void setup() {
  Serial.begin(115200); delay(600);
  Serial.println("\nDimplex register-dump tool starting...");
  NimBLEDevice::init("dimplex-dump");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(false, true, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);
}

void loop() {
  if (!g_connected) {
    g_client = g_client ? g_client : NimBLEDevice::createClient();
    g_client->setClientCallbacks(new CB(), false);
    NimBLEAddress target;
    if (!discoverFireplace(target)) { Serial.println("[ble] no fireplace found, retrying"); delay(2000); return; }
    Serial.printf("[ble] connecting to %s...\n", target.toString().c_str());
    if (g_client->connect(target) && g_client->secureConnection()) {
      Serial.println("[ble] paired");
    } else { Serial.println("[ble] connect/pair failed, retrying"); delay(3000); return; }
  }
  if (g_connected && g_paired && !g_dumped) { dumpAll(); g_dumped = true; Serial.println("[ble] dump complete - unplug to let the remote change a setting, then replug"); }
  delay(200);
}
