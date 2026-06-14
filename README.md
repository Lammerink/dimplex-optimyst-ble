# Dimplex Opti‑myst (Cassette 400 / CAS400LNH) → Home Assistant over BLE

Control a **Dimplex Opti‑myst Cassette electric fire** (the older Bluetooth‑remote
generation, BLE service `00060000‑f8ce‑11e4‑…`) from **Home Assistant**, using an
**M5Stack Atom Lite (ESP32)** as a BLE↔MQTT bridge.

This fire pairs with its remote using **authenticated Bluetooth pairing (LE legacy
passkey)** — so you *cannot* just replay commands; you have to pair. There is **no
public documentation or app** for this generation's protocol (the current Dimplex/Faber
apps speak a newer, different protocol). This repo is the result of reverse‑engineering
it from scratch. See **[docs/PROTOCOL.md](docs/PROTOCOL.md)** for the full protocol.

> ⚠️ The pairing passkey here (`584936`) is the one that worked on the author's unit.
> It may be universal for this product line or unique per unit — if pairing fails,
> [find yours](docs/PROTOCOL.md#finding-your-passkey).

## What you get in Home Assistant

Auto‑discovered MQTT device **"Dimplex CAS400LNH"** with:

| Entity | Type | Notes |
|---|---|---|
| **Fireplace** | switch | on / off |
| **Flame level** | number 0–6 | flame/mist intensity |
| **Volume** | number 0–6 | sound (0 = off) |
| **Flame level state** | sensor | current level |

State is polled every ~8 s, so changes made with the **physical remote** show up in HA too.

## Hardware

- **M5Stack Atom Lite** (ESP32‑PICO‑D4) — Arduino board type **`M5Atom`**.
- A Dimplex Opti‑myst Cassette fire of the BLE‑remote generation (advertises as
  `FI####<Dimplex>`, service `00060000‑f8ce‑11e4‑abf4‑0002a5d5c51c`).
- An MQTT broker reachable by the ESP (e.g. the Mosquitto add‑on in Home Assistant).

## Firmware

Two sketches (Arduino, ESP32 core 3.x):

- **[`Dimplex_MQTT/`](Dimplex_MQTT)** — the bridge you run day‑to‑day. NimBLE + MQTT
  auto‑discovery + a Wi‑Fi/MQTT setup portal.
- **[`Dimplex_Dump/`](Dimplex_Dump)** — a tool that pairs and dumps every readable
  register, for mapping new settings (see *Mapping more controls* below).

**Libraries:** `NimBLE-Arduino` (≥2.x), `PubSubClient`, `WiFiManager`.
**Why NimBLE:** the Bluedroid stack crashes when BLE + Wi‑Fi run together on the ESP32;
NimBLE coexists cleanly (and ESPHome's `ble_client` couldn't do the MITM pairing at all —
see PROTOCOL.md).

### Build & flash

Using the Arduino IDE: select board **M5Atom**, install the three libraries, open
`Dimplex_MQTT/Dimplex_MQTT.ino`, set your fire's BLE address (and passkey if needed),
upload.

Or with `arduino-cli`:

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "NimBLE-Arduino" "PubSubClient" "WiFiManager"
arduino-cli compile --fqbn esp32:esp32:m5stack_atom Dimplex_MQTT
arduino-cli upload  --fqbn esp32:esp32:m5stack_atom -p /dev/cu.usbserial-XXXX Dimplex_MQTT
```

## First‑run setup (the management interface)

No credentials are compiled in. On first boot (or when it can't reach Wi‑Fi) the Atom
opens a **captive‑portal setup page**:

1. The Atom starts a Wi‑Fi network **`Dimplex-Setup`** (password `dimplex123`).
2. Join it from your phone/laptop — a page opens (or browse to `192.168.4.1`).
3. Enter your **Wi‑Fi** network and your **MQTT broker** address (an IP **or** a hostname
   like `homeassistant.local` — `.local` names are resolved via mDNS), **port, username,
   password**.
4. Save → it reboots, connects, and the device appears in Home Assistant.

### Reaching it later

Once it's on your network, the bridge runs an always‑on management page at
**`http://dimplex-atom.local/`** (or its IP). From there you can see status (Info),
reconfigure Wi‑Fi + MQTT (Configure WiFi), and do a browser OTA firmware update (Update).

You can also reopen the full Wi‑Fi setup AP by **holding the Atom's button while powering
on**, or **holding it ~3 s while running**. Settings are stored in flash and survive power
loss. Wi‑Fi credentials are never written to source.

## Mapping more controls

The remote has settings we haven't exposed yet (timer, brightness, DST…). The reliable
way to decode any of them — without a BLE sniffer — is the **register‑diff method** with
`Dimplex_Dump`:

1. Flash `Dimplex_Dump`, power on → it prints a full register dump (snapshot **A**).
2. **Unplug the Atom** (frees the fire's single BLE connection).
3. Change **one** setting on the physical remote.
4. Plug the Atom back in → snapshot **B**.
5. `diff` A vs B → the register that changed is that setting.

This is exactly how Volume (`0x0076`) and the clock (`0x0010/0x0012`) were found.

## Tools (`tools/`)

Helper scripts used during reverse‑engineering (macOS):

- `serial_read.py` — read the ESP's USB serial (and send commands); no Arduino Serial
  Monitor needed.
- `crack_and_parse.sh` / `parse_att.py` — crack a sniffed pairing with
  [`crackle`](https://github.com/mikeryan/crackle) and decode the ATT writes.

## Credits & status

Reverse‑engineered and built from scratch; not affiliated with or endorsed by Dimplex /
Glen Dimplex. Provided as‑is. PRs welcome — especially confirming the passkey on other
units and mapping the remaining registers.

License: [MIT](LICENSE).
