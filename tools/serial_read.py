#!/usr/bin/env python3
"""Read the ESP32 serial output (and optionally send a command).
Usage: serial_read.py [seconds] [command_to_send] [send_after_seconds]
Resets the board on open so we capture the full boot log."""
import sys, time, serial, glob
try: sys.stdout.reconfigure(line_buffering=True)   # flush each line (live log when redirected)
except Exception: pass

dur   = float(sys.argv[1]) if len(sys.argv) > 1 else 25
cmd   = sys.argv[2] if len(sys.argv) > 2 else None
after = float(sys.argv[3]) if len(sys.argv) > 3 else 8

ports = glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/cu.SLAB*")
if not ports:
    print("NO SERIAL PORT FOUND (is the Atom plugged in? Arduino Serial Monitor closed?)")
    sys.exit(1)
port = ports[0]
print(f"# opening {port} @115200 for {dur}s" + (f", will send {cmd!r} at {after}s" if cmd else ""))

try:
    ser = serial.Serial(port, 115200, timeout=0.2)
except Exception as e:
    print(f"# COULD NOT OPEN PORT: {e}\n# (close the Arduino IDE Serial Monitor first)")
    sys.exit(1)

# Reset the ESP32 into run mode (pulse EN via RTS) unless "noreset" requested
if "noreset" not in sys.argv:
    ser.setDTR(False); ser.setRTS(True); time.sleep(0.12); ser.setRTS(False)

t0 = time.time(); sent = False
while time.time() - t0 < dur:
    if cmd and not sent and time.time() - t0 >= after:
        ser.write((cmd + "\n").encode()); ser.flush()
        print(f"# --- sent: {cmd!r} ---")
        sent = True
    data = ser.readline()
    if data:
        print(data.decode("utf-8", "replace").rstrip())
ser.close()
print("# done")
