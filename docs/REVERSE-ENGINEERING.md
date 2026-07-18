# How I reverse-engineered the Dimplex Opti-myst

My Dimplex Opti-myst Cassette 400 has a Bluetooth remote and nothing else. No app, no API.
I wanted it in Home Assistant, so I worked out the protocol myself. Here's how it went,
warts and all. If you only want the end result (the UUIDs and handles), read
[PROTOCOL.md](PROTOCOL.md). This is the how.

> A note on how this was written: I did the actual reverse-engineering — the sniffing,
> the cracking, the ESP work, the dead ends. But I wrote most of this up with Claude
> (Anthropic's AI), which also helped a lot along the way. So parts of it will read a bit
> AI. I've left it that way on purpose rather than fighting the wording forever, because
> the steps and the numbers are right and I'd rather it be out here helping someone than
> sitting polished on my drive.

## Poking at it

I scanned it with nRF Connect and a small ESP32 sketch. It shows up as `FI0514<Dimplex>` on
`00:A0:50:D6:A5:14`. That address prefix is Cypress/Infineon, so it's one of their BLE
modules inside.

There's one custom service, `00060000-F8CE-11E4-…`, with a writable characteristic
(`00060001`). I figured that was the control channel. Wrong. The commands actually go to
bare attribute handles further down, but I didn't know that yet. For now the ESP32
connected fine, my writes did nothing, and a spare remote just said "NO PROD". Something was
blocking me.

## It's encrypted

The fire says "NOT BONDED", and I wasted a while assuming that meant no security. It
doesn't. Every read or write to a control attribute comes back with error `0x05`,
insufficient authentication. The link has to be paired and encrypted: LE legacy pairing, a
passkey, MITM. So you can't record the remote and replay it. You have to pair for real.
That's where I gave up on faking the fireplace.

## Catching the passkey

No passkey, no access, and the only way to get it was to watch the remote pair. I put nRF
Sniffer firmware on a Nordic nRF52840 dongle and captured in Wireshark.

This is what ate my afternoon. The sniffer follows one device, and you pick it from the
"Device" box in Wireshark while the fire is advertising, before the remote connects. Miss
that window and all you get is advertising traffic, none of the actual pairing. I got it
wrong four times. When it's right, the packet list stops scrolling `ADV_IND` and fills with
data packets the second the remote links up.

## Breaking it

Legacy pairing is about 20 bits of key. Weak. Once you've got the handshake on tape, crackle
brute-forces it in seconds:

    crackle -i capture.pcapng -o decrypted.pcap

It spat out `TK found: 584936`. There's the passkey. It decrypts the rest of the capture
while it's at it.

Annoyingly, crackle leaves a flag on the packets that makes Wireshark think they're still
encrypted, so it prints "bad MIC" and won't decode them. The data is fine. I wrote
`tools/parse_att.py` to read the packets myself, and `tools/crack_and_parse.sh` runs crackle
and the parser together. That's when the real commands showed up: a bit of setup on
connect, then writes to handles like `0x0040` and `0x0042`. Not the characteristic I'd been
looking at.

## Making the ESP32 pair

Two dead ends first, so you can skip them.

ESPHome was my first try, because Home Assistant. Its `ble_client` connects and pairs, but
my writes always came back as insufficient auth. Couldn't get past it, dropped it.

Then I used Arduino's Bluedroid stack. It paired, but the moment WiFi and Bluetooth were
both running it crashed on a FreeRTOS assert and rebooted, over and over. The two of them
fighting over the radio is a known mess.

NimBLE fixed it. Lighter, plays nicely with WiFi, and it gets a properly authenticated link.
The settings that worked: no bonding, MITM on, legacy (no secure connections),
keyboard/display IO capability, feed the passkey back when it asks, connect, secure the
connection, write.

## The rest of the buttons

I had power, and roughly where flame was. Volume, the clock, the intensity scale, no idea.
And sniffing each remote press was hopeless, because the remote connects for a fraction of a
second and the sniffer kept missing it.

So I did it from the ESP instead. Only one device can hold the connection at a time, and
that turns out to be handy: connect the ESP, read every register, unplug it, change one
thing on the remote, plug it back in, read everything again, compare. Whatever changed is
the thing you touched.

That gave me volume on `0x0076`, the clock on `0x0010` and `0x0012` (just the date and time
written out as bytes), and the flame level on `0x0042`. I'd assumed flame was `0x0040`, but
`0x0040` is only on/off. I'd been writing the level into the power register, which is why
some values worked and some didn't.

## Don't bother with

Replaying packets or emulating the fire. It's encrypted, you have to pair. ESPHome's
`ble_client` for this fire, it won't hold the authenticated link. Bluedroid plus WiFi on an
ESP32, use NimBLE. The Dimplex and Faber apps: I pulled both apart, and they run a newer
protocol (`713d…`) for newer fires, nothing about this one. I couldn't find this generation
documented anywhere.

---

# Doing it on your fire

First try the easy path: just flash `Dimplex_MQTT` and set up WiFi/MQTT in the portal. It
auto-detects the fireplace by name and pairs with my passkey (`584936`) by default, so for
most people that's the whole job. The rest of this section is only for when that *doesn't*
work — a different passkey, or different register handles. I only had one fire to test, so
the passkey could easily be per-unit.

You need a Nordic nRF52840 dongle, Wireshark with the nRF Sniffer plugin, crackle (clone and
`make` it, it's GPL so it isn't bundled here), an ESP32 (mine is an M5Stack Atom Lite), and
the `tools/` scripts.

1. Sniff a pairing. Start the capture, select your fire in the Device box while it's
   advertising and the remote is idle, then press buttons on the remote. Make sure you
   actually see data packets, not just adverts. Stop, save.

2. Crack it: `crackle -i capture.pcapng -o decrypted.pcap`, read the `TK found` line — that's
   your passkey. Or run `tools/crack_and_parse.sh capture.pcapng`, which cracks it and prints
   the decoded writes for you.

3. Put that passkey in the setup portal (the "Pairing passkey" field), or in `DEFAULT_PASSKEY`
   in `Dimplex_MQTT.ino` if you'd rather bake it in. No need to touch the BLE address —
   the firmware finds the fire by name on its own.

4. Check the handles in those decoded writes. If on/off, flame and volume land on the same
   handles as mine (`0x0040`, `0x0042`, `0x0076`), you're done.

5. If they don't match, map yours with the dump tool. Flash `Dimplex_Dump`, let it print a
   full register dump, unplug the ESP, change one setting on the remote, plug it back in,
   dump again, diff the two. One control at a time. Then put your handles in
   `Dimplex_MQTT.ino`.

## If it won't work

**Capture is all `ADV_IND`, no data.** You didn't follow the device. Select it in the Device
box before the remote connects.

**crackle says zero connections.** It didn't catch the handshake (same reason), or the
remote was already connected when you started the capture.

**Wireshark shows "bad MIC".** Normal on crackle's output. Read it with `parse_att.py`.

**ESP pairs but writes fail with `0x05`.** The link isn't authenticated. Use NimBLE and let
the write itself carry the auth requirement, don't encrypt the connection separately.

**ESP reboots when WiFi and BLE are both on.** You're on Bluedroid. Switch to NimBLE.

**Only some flame levels do anything.** You're writing the level into the on/off handle
instead of the level handle.

**Two beeps from idle (not after a command).** Low water. Refill and power-cycle.

---

*Not affiliated with Dimplex or Glen Dimplex. I did this for a fire I own, so I could use it
with Home Assistant.*
