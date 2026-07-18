# How I reverse-engineered the Dimplex Opti-myst (and how you can too)

I have a Dimplex Opti-myst Cassette 400 fire. It came with a Bluetooth remote and that's
it: no app, no API, nothing you can point Home Assistant at. So I picked the protocol
apart. This is roughly what I did, in order, including the bits that didn't work and the
places I got stuck.

If you just want the finished facts (UUIDs, handles, values), they're in
[PROTOCOL.md](PROTOCOL.md). This page is the messier account of how I got there, and what
you'd have to redo if your fire behaves differently from mine — a different pairing code
being the most likely difference.

## Figuring out what it is

I started with nRF Connect on my phone and a throwaway ESP32 sketch, just to see what the
fire looks like over Bluetooth. It advertises as `FI0514<Dimplex>` at `00:A0:50:D6:A5:14`,
and that address prefix belongs to Cypress/Infineon, so it's one of their BLE modules.

Dumping the GATT table showed one custom service (`00060000-F8CE-11E4-…`) with a
characteristic (`00060001`) I could write to. I assumed that was the control channel. It
wasn't — the actual commands go to plain attribute handles further down the table — but I
didn't know that yet. The ESP32 connected fine, but nothing I wrote had any effect, and the
remote just showed "NO PROD" when I tried to pair a second one. Something was clearly gating
everything.

## The wall

The fire reports "NOT BONDED", which had me convinced for a while that there was no
security to deal with. There is. Every read or write to the control attributes comes back
with error `0x05`, insufficient authentication. The fire only talks over an encrypted,
paired link: LE legacy pairing, passkey, MITM. So recording what the remote sends and
replaying it was never going to work. You have to genuinely pair. That's what finally
killed the "just emulate the fireplace" idea I'd been chasing.

## Sniffing the pairing

To pair I needed the passkey, and to get the passkey I had to watch the real remote pair.
I flashed a Nordic nRF52840 dongle with the nRF Sniffer firmware and captured in Wireshark.

This is where I lost the most time. The sniffer only follows one device, and you have to
select the fire from the "Device" dropdown in Wireshark *while it's advertising, before the
remote connects*. Get that wrong and you end up with a few thousand advertising packets and
none of the actual conversation. I did exactly that three or four times before it stuck.
When you've got it right, the packet list stops being a wall of `ADV_IND` and turns into a
stream of data packets the moment the remote connects.

## Cracking it

LE legacy pairing is weak. The passkey is effectively 20 bits, so once you've captured the
pairing handshake you can brute-force it offline. [crackle](https://github.com/mikeryan/crackle)
does exactly that:

    crackle -i capture.pcapng -o decrypted.pcap

A couple of seconds later it printed `TK found: 584936`. That's the passkey. It decrypts the
rest of the captured session at the same time.

Small trap: crackle leaves a flag set that makes Wireshark believe the decrypted packets are
still encrypted, so it shows "bad MIC" and refuses to parse them. The plaintext is right
there, Wireshark is just being fussy. I wrote a little script, `tools/parse_att.py`, to walk
the packets and print the ATT reads and writes; `tools/crack_and_parse.sh` does both steps
in one shot. That's where the real protocol showed up: a couple of setup writes on connect,
then commands to raw handles like `0x0040` and `0x0042`, not the `00060001` characteristic
I'd been staring at.

## Getting an ESP32 to pair

Two false starts here that are worth passing on.

I tried ESPHome first, since Home Assistant was the whole point. Its `ble_client` connects
and even completes pairing, but I could never get the writes to go through — they kept
failing with insufficient authentication, and I burned a fair bit of time before writing it
off.

Then I used the Arduino BLE stack (Bluedroid) and hit a different problem: as soon as WiFi
and BLE were both up, it crashed in a FreeRTOS assert and sat in a reboot loop. Bluedroid
and WiFi fighting over the one radio is apparently a well-known headache.

Switching the BLE side to NimBLE fixed both. It's much lighter, it coexists with WiFi
without falling over, and it actually establishes a proper authenticated link. What worked:
no bonding, MITM on, legacy pairing, keyboard/display IO capability, hand the passkey back
when it's asked for, connect, secure the connection, then write.

## Mapping the rest of the buttons

I had on/off and a rough idea where flame lived, but not volume, the clock, or how the
intensity scale actually worked. Sniffing each remote press was miserable because the remote
only connects for a split second and the sniffer kept missing it.

So I gave up on the sniffer for this part and used the ESP instead. Only one thing can hold
the connection at a time, which turns into a decent trick: connect the ESP, dump every
readable register, unplug it, change one setting on the remote, plug it back in, dump again,
and diff the two. Whatever moved is the setting you just changed.

That found volume (`0x0076`), the clock (`0x0010`/`0x0012`, literally the date and time as
bytes), and — after I got it wrong the first time — the fact that the flame level is handle
`0x0042`, not `0x0040`. `0x0040` is only on/off. I'd been writing the level into the on/off
register, which is why only some of the values appeared to do anything.

## Things that don't work, so you can skip them

Replaying captured packets or pretending to be the fireplace: no, it's encrypted, you have
to pair. ESPHome's `ble_client` for this fire: it won't hold the authenticated link.
Bluedroid plus WiFi on an ESP32: use NimBLE instead. And the official apps (Flame Connect,
Faber ITC): I decompiled both, and they speak a newer protocol (`713d…`) for newer fires and
know nothing about this generation. As far as I can find, this one isn't documented anywhere
public.

---

# Doing this on your own fire

If my passkey (`584936`) doesn't pair your unit, or the handles come out different, here's
the short version of how to redo it. I only had one fire to test, so the passkey may well be
per-unit.

You'll need a Nordic nRF52840 dongle with Wireshark and the nRF Sniffer plugin, crackle
(clone and `make` it — it's GPL, so it's not bundled here), an ESP32 (I used an M5Stack Atom
Lite), and the scripts in `tools/`.

1. Scan for your fire (nRF Connect or the ESP) and note its address. Put it in `FIRE_ADDR`
   in `Dimplex_MQTT.ino`.

2. Sniff a pairing. Start the nRF Sniffer capture, pick your fire in the Device dropdown
   while it's advertising and the remote is idle, then press some buttons on the remote.
   Check that data packets actually show up, not just adverts. Stop and save.

3. Crack your passkey: `crackle -i capture.pcapng -o decrypted.pcap`, read the `TK found`
   line, and drop that number into `PASSKEY`. (Or just run
   `tools/crack_and_parse.sh capture.pcapng`, which cracks it and prints the decoded writes.)

4. Look at the decoded writes for on/off and flame. If the handles match mine (`0x0040`
   power, `0x0042` level, `0x0076` volume), flash `Dimplex_MQTT` and you're done.

5. If they don't match, map them with the dump tool. Flash `Dimplex_Dump`, let it print a
   full register dump, unplug the ESP, change one setting on the remote, plug it back in,
   dump again, and diff. Repeat per control and update the handles in `Dimplex_MQTT.ino`.

## When it goes wrong

If your capture is nothing but `ADV_IND`, you didn't follow the device — select it in the
Device dropdown before the remote connects. If crackle reports zero connections, it didn't
catch the pairing handshake (same cause), or the remote was already connected when you
started following. If Wireshark shows "bad MIC" on the decrypted file, that's expected;
read it with `parse_att.py`. If the ESP pairs but writes still fail with `0x05`, the link
isn't authenticated — use NimBLE and let the GATT op carry the auth requirement rather than
encrypting out of band. If the ESP reboots whenever WiFi and BLE are both on, you're on
Bluedroid; switch to NimBLE. If only some flame levels seem to work, you're probably writing
the level into the on/off register instead of the separate level handle. And if the fire
beeps twice from idle (not just from an extra command), that's the low-water warning —
refill and power-cycle it.

---

*Not affiliated with Dimplex or Glen Dimplex. I reverse-engineered this for a fire I own, so
I could use it with Home Assistant.*
