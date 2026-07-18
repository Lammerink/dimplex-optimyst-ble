# How this was reverse‑engineered (and how to do it for *your* fire)

This is the story of how the Dimplex Opti‑myst BLE protocol was worked out from nothing,
followed by a **step‑by‑step guide to replicate it** — useful if your unit differs (e.g.
a **different pairing passkey**, or different control registers).

For the *facts* (UUIDs, handles, values) see [PROTOCOL.md](PROTOCOL.md). This page is the
*method*.

---

## Part 1 — The journey

### 1. Recon: what is this thing?
- Scanned with **nRF Connect** and an ESP32. The fire advertises as `FI####<Dimplex>`,
  address `00:A0:50:D6:A5:14` (OUI `00:A0:50` = Cypress/Infineon BLE module).
- Dumped its GATT: one custom service `00060000-F8CE-11E4-…` with characteristic
  `00060001` (WRITE/NOTIFY). Looked like the control channel — it wasn't (the real
  controls are raw attribute handles, see below).
- An ESP32 could **connect** but the remote showed **`NO PROD`**, and writes did nothing.

### 2. The wall: it's encrypted
- The fire shows `CONNECTED / NOT BONDED`, which first suggested "no security." Wrong.
- Reading/writing the control attributes returned **`0x05 INSUF_AUTHENTICATION`** — the
  link must be **encrypted**. The fire uses **LE Legacy pairing, Passkey Entry, MITM**.
  You **cannot replay commands** — you must pair. That's why cloning/advertising failed.

### 3. Capture the pairing (nRF Sniffer)
- Flashed a **Nordic nRF52840 dongle** with **nRF Sniffer for Bluetooth LE** and captured
  in **Wireshark**.
- **The one critical trick:** in Wireshark's nRF Sniffer toolbar, select the fire in the
  **"Device" dropdown while it is advertising (remote idle)** so the sniffer *follows the
  connection*. Miss this and you only capture advertising packets — no commands. This is
  the single most common failure.
- Pressed buttons on the original remote to capture: `CONNECT_IND`, the SMP pairing
  handshake, and the encrypted ATT writes.

### 4. Crack the passkey (crackle)
- LE Legacy passkey pairing is only **20 bits** of entropy, so it cracks offline in
  seconds with [`crackle`](https://github.com/mikeryan/crackle):
  ```
  crackle -i capture.pcapng -o decrypted.pcap
  ```
  → `TK found: 584936` — the pairing passkey — and it decrypts the session.

### 5. Read the commands
- crackle leaves the Nordic "encrypted" flag set, so Wireshark shows *"bad MIC"* even
  though the plaintext is there. `tools/parse_att.py` walks the link layer and prints the
  decrypted ATT reads/writes; `tools/crack_and_parse.sh` chains crackle + the parser.
- This revealed: on‑connect init writes, and control by **raw attribute handles**
  (`0x0040`, `0x0042`, …) — not the "obvious" `00060001` characteristic.

### 6. Build & pair from an ESP32
- Key lesson: **use NimBLE, not Bluedroid.** Bluedroid + Wi‑Fi crashes on the ESP32
  (`xQueueGenericSend` assert). NimBLE is light and coexists cleanly, and it achieved a
  proper **MITM‑authenticated** link. (ESPHome's `ble_client` could not pair with the
  fixed passkey at all — documented in PROTOCOL.md.)
- NimBLE recipe: `setSecurityAuth(false, true, false)` (no‑bond, MITM, legacy),
  `setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY)`, reply to `onPassKeyEntry` with the
  passkey, then `connect()` + `secureConnection()`, and write raw handles.

### 7. Map the rest without a sniffer (register‑diff)
- Following the remote's brief connections in the sniffer was flaky, so most controls were
  mapped with a **register‑diff**: dump every readable register, change **one** setting on
  the remote (ESP disconnected), dump again, `diff`. That's how **Volume (`0x0076`)**, the
  **clock (`0x0010/0x0012`)** and the real **flame level (`0x0042`, *not* `0x0040`)** were
  found. `Dimplex_Dump/` is that tool.

### Dead‑ends worth knowing
- Emulating the fire / replaying raw packets — impossible (authenticated encryption).
- ESPHome `ble_client` — pairs but never gets an authenticated link for writes.
- Bluedroid + Wi‑Fi — reboot loop; NimBLE fixed it.
- Vendor apps (*Flame Connect*, *Faber ITC*) — a **newer** protocol (`713d…`), not this
  one; decompiling them didn't help. This generation is undocumented anywhere public.

---

## Part 2 — Replicate it for your own fire

Do this if the defaults don't work — most likely a **different passkey** (it may be
per‑unit) or **different registers** on your model.

### You'll need
- A **Nordic nRF52840** dongle (BLE sniffer) + **Wireshark** with the **nRF Sniffer for
  Bluetooth LE** extcap plugin.
- **[crackle](https://github.com/mikeryan/crackle)** (build from source: `git clone … && make`).
- An **ESP32** (this repo targets M5Stack Atom Lite) + `arduino-cli`.
- This repo's `tools/` scripts.

### Step 1 — Find your fire
Scan (nRF Connect, or the ESP). Note its **BLE address** and advertised name
(`FI####<Dimplex>`). Put the address in `FIRE_ADDR` in `Dimplex_MQTT.ino`.

### Step 2 — Sniff a pairing
1. Fire powered, **remote idle** (fire advertising).
2. Wireshark → start the **nRF Sniffer** capture.
3. **Select your fire in the "Device" dropdown *before* touching the remote** (so it
   follows the connection — confirm the packet list fills with *data* packets, not just
   `ADV_IND`, once the remote connects).
4. Press remote buttons; **Stop**; **Save As** `capture.pcapng`.

### Step 3 — Crack your passkey
```
./crackle/crackle -i capture.pcapng -o decrypted.pcap
```
Read the `TK found: NNNNNN` line — **that is your passkey.** Put it in `PASSKEY` in
`Dimplex_MQTT.ino`. (Or run `tools/crack_and_parse.sh capture.pcapng` to crack **and**
print the decrypted ATT writes in one go.)

### Step 4 — Confirm the control handles
In the decrypted output, find the `WRITE_REQ` to raw handles when you pressed on/off and
flame. On this unit: `0x0040` = power (`0x06`/`0x00`), `0x0042` = flame level `1–6`,
`0x0076` = volume. If yours match, you're done — flash `Dimplex_MQTT`.

### Step 5 — If your registers differ: register‑diff
If the handles/values aren't the same, map them empirically with `Dimplex_Dump/`:
1. Flash `Dimplex_Dump` (set your `PASSKEY`/`FIRE_ADDR`), power on → it prints a full dump
   (snapshot **A**).
2. **Unplug the ESP** (frees the fire's single BLE connection).
3. Change **one** setting on the remote (e.g. flame up, or volume).
4. Plug the ESP back in → snapshot **B**.
5. `diff` A vs B → the register that changed is that control. Repeat per setting.
6. Update the handles in `Dimplex_MQTT.ino` accordingly.

---

## Part 3 — Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Sniffer capture has only `ADV_IND`, no data | You didn't **follow the device** — select it in the "Device" dropdown *before* the remote connects (Step 2.3). |
| `crackle` says "0 connections" | The pairing handshake wasn't captured (see above), or the remote was already connected when you started following. |
| crackle output shows "bad MIC" in Wireshark | Expected — the plaintext is there; use `tools/parse_att.py` to read it. |
| ESP pairs (`auth complete success=1`) but writes fail `0x05` | Link isn't MITM‑authenticated. Use NimBLE with `AUTH_REQ` on the op, not out‑of‑band encryption (see Part 1.6). |
| ESP reboots when Wi‑Fi + BLE both on | You're on Bluedroid — switch to **NimBLE**. |
| Pairing fails with your passkey | It may be per‑unit — crack **your** capture (Step 3). |
| Only some flame levels "work" | You're writing the level to the on/off register — the level is a **separate** handle (`0x0042` here). |
| Fire beeps twice on power‑on | Each accepted write beeps once; don't send a redundant second write. Two beeps *from the remote/idle* instead means **low water** (refill + power‑cycle). |

---

*Not affiliated with Dimplex / Glen Dimplex. Reverse‑engineered for interoperability of a
device the author owns.*
