#!/bin/bash
# Flash the TallyPad firmware with no hands on the board.
#
# Works AFTER the custom firmware is on the pad once: it opens a 6-second
# console window at every boot, and typing 'dl' in that window drops the
# chip into download mode. This script resets the chip, spams 'dl' into
# the window, then runs esptool.
#
# For the VERY FIRST flash (stock firmware still installed) see the
# README: you must hold IO9 to GND during power-up instead — the stock
# image has no console command.
#
# Requirements: python3 + pyserial + esptool (pip install esptool pyserial)
set -u
PORT="${TALLYPAD_PORT:-/dev/ttyUSB0}"
BUILD="${TALLYPAD_BUILD:-$(dirname "$0")/../firmware/build}"
ESPTOOL="${TALLYPAD_ESPTOOL:-esptool.py}"

echo "== stop anything holding $PORT (serial monitors, cat, loggers) first =="

python3 - "$PORT" <<'PY'
import sys, time
import serial
s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
s.dtr = False
s.rts = True; time.sleep(0.15); s.rts = False       # pulse EN: reboot
deadline = time.time() + 8
seen = b''
while time.time() < deadline:
    seen += s.read(512)
    s.write(b'dl\r\n'); s.flush()                    # spam the boot window
    time.sleep(0.25)
s.close()
print('boot text seen:', b'window open' in seen, '| download mode:', b'download mode' in seen)
PY

"$ESPTOOL" --port "$PORT" --baud 460800 --connect-attempts 3 --before no-reset \
  --after hard-reset --chip esp32c6 write-flash --flash-mode dio \
  --flash-size 4MB --flash-freq 80m \
  0x0 "$BUILD/bootloader/bootloader.bin" \
  0x8000 "$BUILD/partition_table/partition-table.bin" \
  0xf000 "$BUILD/ota_data_initial.bin" \
  0x20000 "$BUILD/talli_pad.bin"
