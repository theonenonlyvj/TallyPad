# TallyPad: custom firmware for the Talli baby tracker button pad

**Flash your Talli pad to work with whatever you want. Fully local, no cloud,
no subscription, and the buttons mean whatever you decide.**

TallyPad is open-source replacement firmware for the **Talli Baby tracker**
(the 8-button Wi-Fi "Talli pad" from Talli / BabyLogger). If your Talli
stopped syncing, the app no longer works for you, or you just want your baby
log to live on your own network, this repo turns the pad back into a useful
one-press logger for pee, poo, diaper changes, feeds, nursing, pumping, or
anything else.

Each press is sent over your home Wi-Fi to a tiny program (the "listener")
that you run on any computer at home. The listener turns presses into a
plain line of text like `pad press 21:07: pee + poo + cloth diaper` and can
forward it anywhere: Home Assistant, a chat webhook (Slack, Mattermost), a
spreadsheet through n8n or Zapier, Node-RED, or just a log.

> **No experience needed.** If you have never flashed a microcontroller
> before, follow "Step by step" below in order. Every command is written
> out. Plan on about an hour the first time, mostly waiting for downloads.

---

## Contents

- [What you need](#what-you-need)
- [Step by step](#step-by-step)
- [Choosing what each button means](#choosing-what-each-button-means)
- [Sending presses somewhere useful](#sending-presses-somewhere-useful)
- [What the lights mean](#what-the-lights-mean)
- [Troubleshooting](#troubleshooting)
- [FAQ](#faq)
- [How it works (hardware)](#how-it-works-hardware)
- [How this was reverse engineered](#how-this-was-reverse-engineered)
- [Making your own changes](docs/DEVELOPING.md)
- [Safety, legality, warranty](#safety-legality-warranty)

---

## What you need

| Thing | Notes |
|---|---|
| A Talli pad | The 8-button model with a micro-USB port. Inside is an ESP32-C6 chip. |
| A **micro-USB data cable** | Many cheap cables only charge. If your computer never sees the pad, try another cable first. |
| A computer | Mac, Windows, or Linux, to build and flash the firmware once. |
| An always-on computer at home for the listener | A Raspberry Pi, a NAS, an old laptop, or the same computer. It only needs Python 3. |
| For the very first flash only: a short wire | A jumper wire, a paperclip, or metal tweezers, to touch two points for a second. |

You do **not** need to solder anything.

---

## Step by step

### Step 1. Install the tools on your computer

You need **ESP-IDF** (Espressif's free toolkit for building ESP32 firmware)
and **esptool** (the program that copies firmware onto the chip).

1. Install ESP-IDF **version 5.x** with Espressif's official guide:
   - Windows: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/windows-setup.html>
   - Mac / Linux: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/linux-macos-setup.html>
2. Open a terminal with ESP-IDF loaded.
   - Windows: open "ESP-IDF PowerShell" from the Start menu.
   - Mac / Linux: run `. $HOME/esp/esp-idf/export.sh` (use the folder where
     you installed it).
3. Check it worked: `idf.py --version` prints a version number.
4. Install the flashing helpers: `pip install esptool pyserial`

### Step 2. Download this repo

```sh
git clone https://github.com/theonenonlyvj/TallyPad.git
cd TallyPad
```

No git? On GitHub click the green **Code** button, choose **Download ZIP**,
and unzip it.

### Step 3. Find your listener computer's address

The pad needs to know which computer to send presses to. On the computer
that will run the listener, find its local IP address (it looks like
`192.168.1.23`):

- Windows: run `ipconfig` and look for "IPv4 Address".
- Mac: System Settings, Wi-Fi, Details, "IP address".
- Linux / Raspberry Pi: run `hostname -I`.

Tip: in your router's settings, give that computer a reserved ("static")
address so it never changes.

### Step 4. Put that address into the firmware

Copy `firmware/main/local_config.h.example` to `firmware/main/local_config.h`
and change the address to yours (keep `:4180/press` on the end):

```c
#define INGEST_URL "http://192.168.1.23:4180/press"
```

`local_config.h` is git-ignored, so your address never ends up in a commit
and `git pull` never conflicts with it.

### Step 5. Build the firmware

In the ESP-IDF terminal, from the TallyPad folder:

```sh
cd firmware
idf.py set-target esp32c6
idf.py build
cd ..
```

The first build takes a few minutes and ends with "Project build complete".

### Step 6. Find the pad's USB port

Plug the pad into your computer. Its **CH340** USB chip shows up as a
serial port:

- Windows: Device Manager, "Ports (COM & LPT)", something like `COM3`. If
  nothing appears, install the CH340 driver from the chip maker (WCH).
- Mac: run `ls /dev/cu.*` and look for `/dev/cu.usbserial-...` or
  `/dev/cu.wchusbserial...`.
- Linux: usually `/dev/ttyUSB0`. If you get "permission denied", run
  `sudo usermod -aG dialout $USER`, then log out and back in.

Use your port wherever the commands below say `/dev/ttyUSB0`. Then unplug
the pad again.

### Step 7. Put the pad in download mode (first time only)

The original firmware can't be told to accept new firmware, so the first
time you hold the chip in "download mode" by hand:

1. Unplug the pad, take out any batteries, and open the case.
2. Find the **IO9** contact: the corner contact of the silver ESP32 module
   that is **farthest from the antenna**, on the bottom row, in line with
   the printed label `TP1`. See the photo:
   [`docs/images/io9-first-flash-pad.png`](docs/images/io9-first-flash-pad.png).
3. Touch one end of your wire to IO9 and the other end to ground (the metal
   shell of the USB socket works).
4. **While holding the wire there, plug in the USB cable.** Then let go.

The pad is now waiting for a computer to talk to it.

### Step 8. Back up the original firmware, then flash TallyPad

First, a backup, so you can put the stock Talli firmware back later:

```sh
esptool.py --port /dev/ttyUSB0 --chip esp32c6 read-flash 0 0x400000 stock-backup.bin
```

**Keep `stock-backup.bin` private.** It contains your pad's own cloud key.

Then flash TallyPad (if esptool says it can't connect, redo Step 7 first):

```sh
esptool.py --port /dev/ttyUSB0 --chip esp32c6 write-flash \
  0x0 firmware/build/bootloader/bootloader.bin \
  0x8000 firmware/build/partition_table/partition-table.bin \
  0xf000 firmware/build/ota_data_initial.bin \
  0x20000 firmware/build/talli_pad.bin
```

(Windows PowerShell: put it all on one line and drop the `\` characters.)

When it says "Hard resetting", unplug and replug the pad and close the case.
You never need to open it again: later updates go over the cable with
`tools/flash.sh`.

### Step 9. Start the listener

On your always-on computer, in the TallyPad folder:

```sh
cd listener
cp buttons.example.json buttons.json
cp config.example.json config.json
python3 ingest.py
```

(Windows: use `copy` instead of `cp`, and `python` instead of `python3`.)

Leave it running. To start it automatically on a Raspberry Pi or other
Linux box, copy `tallypad-listener.service.example` to
`/etc/systemd/system/tallypad-listener.service`, fix the path inside it,
then run `sudo systemctl enable --now tallypad-listener`.

If that computer has a firewall, allow incoming connections on port **4180**.

### Step 10. Connect the pad to Wi-Fi

1. Power the pad (USB is best; see the FAQ about batteries).
2. On your phone, open Wi-Fi settings and join **`BabyPad-Setup`**.
3. In your phone's browser, go to `http://192.168.4.1`.
4. Enter your home Wi-Fi name and password (it must be a **2.4 GHz**
   network) and save. The pad restarts and joins your Wi-Fi.

### Step 11. Press a button

The button lights up and the status light winks green. In the listener
window you'll see:

```
EVENT: pad press 21:07: pee
```

That's it. You now own your baby log.

---

## Choosing what each button means

Edit `listener/buttons.json`. The number is the button: 1 to 4 across the
top row left to right, 5 to 8 across the bottom row. Changes apply on the
next press, with no restart and no reflash.

```json
{
  "1": "pee",
  "2": "issue, ask for details later",
  "3": "nursing left start/stop",
  "4": "a feed took place",
  "5": "poo",
  "6": "cloth diaper",
  "7": "IGNORE",
  "8": "pumped a bottle"
}
```

- Presses within **10 seconds** of each other become one event,
  `pee + poo + cloth diaper`, stamped with the time of the first press.
- `null` means unassigned. The press is still recorded as "button N
  (unmapped button)".
- `"IGNORE"` drops the press entirely. Handy for a test button.
- The 9th ("sync") button is wired as a hardware reset. It restarts the pad
  and never sends anything.

**Instant buttons.** For start/stop toggles where exact order and time
matter (like nursing left and right), list them in `config.json` and they
skip the 10-second window:

```json
{ "immediate_buttons": [3, 7] }
```

---

## Sending presses somewhere useful

Set `webhook_url` in `listener/config.json`. Every event is sent as an HTTP
POST with the body `{"text": "pad press 21:07: pee"}`.

```json
{
  "webhook_url": "https://your-endpoint.example/hook",
  "webhook_headers": { "Authorization": "Bearer YOUR-TOKEN" },
  "immediate_buttons": []
}
```

This works with anything that accepts a webhook: Home Assistant (an
automation with a webhook trigger), Slack or Mattermost incoming webhooks,
n8n, Node-RED, Zapier, IFTTT, or your own script. Discord expects `content`
instead of `text`, so put a small relay in between or edit `deliver()` in
`ingest.py`. Leave `webhook_url` empty to only log.

---

## What the lights mean

- The pressed button's **white backlight flashes** and the status light
  **winks green**: the press was received.
- **Double red** wink: the pad couldn't reach the listener (listener not
  running, wrong address in Step 4, or Wi-Fi down).
- Dark when idle, on purpose. It lives in a nursery at 3 a.m.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Computer doesn't see the pad | Try another USB cable (charge-only cables are common). On Windows, install the CH340 driver. |
| esptool: "Failed to connect" on the first flash | Redo Step 7: IO9 must touch ground **while** the cable is plugged in. Check the corner against the photo. |
| Linux: "Permission denied" on `/dev/ttyUSB0` | `sudo usermod -aG dialout $USER`, then log out and in. |
| `BabyPad-Setup` never appears | The pad already has Wi-Fi saved. Open a serial monitor (`idf.py -p /dev/ttyUSB0 monitor` from the `firmware` folder), replug the pad, and type `wificlear` within 6 seconds. |
| Pad won't join Wi-Fi | It needs 2.4 GHz. Many routers have a separate 2.4 GHz network name or a setting to split the bands. |
| Double red wink on every press | Check the listener is running, the address in `main.c` matches, and port 4180 is allowed through the firewall. After changing the address, rebuild and reflash. |
| Wrong time on events | The time comes from the listener computer's clock and time zone. |
| Reflash after changes | `tools/flash.sh` (set `TALLYPAD_PORT` if your port isn't `/dev/ttyUSB0`). No case opening. |

---

## FAQ

**Is this official?** No. TallyPad is an independent, unofficial project,
not affiliated with Talli or BabyLogger.

**Does the Talli app still work afterwards?** No. Presses go to your
listener instead of the Talli cloud. To go back, write your backup:
`esptool.py --chip esp32c6 write-flash 0x0 stock-backup.bin` (after Step 7).

**Batteries or USB?** Either. Since v31 the firmware copies the stock
power design: between presses the chip is in deep sleep with the Wi-Fi
radio off, and a press wakes it. The pressed buttons light white right
away, the pad joins Wi-Fi and sends them, then one green flash means sent
and red blinks mean something failed. It also wakes every 30 minutes on
a safety timer and goes straight back to sleep if nothing was pressed.
v31 and v32 had a bug: after a wake, one press could turn into presses
on every button. The real cause (measured on the pad): while any button
lamp is lit, all eight keys read as pressed, and those versions read the
keys during Wi-Fi join with the wake lamp lit. v34 reads the keys only
with the lamp gate off for one read. v34 passed a hands-on test on the
pad (single presses, a wake press, and multi-button batches, with no
extra presses) and is the recommended build. If you
want the proven always-on behaviour, build tag v29.
The trade-off is a short delay after the first press while Wi-Fi
reconnects. The earlier always-on firmware emptied fresh AA cells in
under two days; how long batteries last on v31 has not been measured yet.

**Does anything go to the internet?** Only what you send there yourself
with `webhook_url`. The pad talks to your listener on your home network.

**Can I use it for something other than a baby?** Yes. It's an 8-button
Wi-Fi remote: medication log, pet feeding, chores, a Home Assistant scene
controller.

---

## How it works (hardware)

Inside the pad: an **ESP32-C6-WROOM-1** module, a **CH340G USB-serial**
chip wired to the micro-USB jack (console and flashing work with just a
cable), and **U3, an I²C keypad/LED expander at address `0x20`** (SDA =
GPIO22, SCL = GPIO23) that owns the 8 buttons and their white backlights.
Register 0 bits 8 to 15 are buttons 1 to 8 in reading order (active high);
register 2 is per-lamp control (**active-low**); register `0x48` is a
brightness gate that must be nonzero or nothing ever lights. ⚠️ **While
`0x48` is on, all eight buttons read as pressed.** Only read the buttons
with it off (see [`docs/DEVELOPING.md`](docs/DEVELOPING.md)). The 9th
("sync") button is wired into power/reset. The status LED is a WS2812-style
addressable LED on GPIO10.

⚠️ **Don't drive GPIO3, GPIO11, or GPIO18 high** if you write your own
firmware. They sit in the power path and power-cycle the board.

Full pinout, register map, and dead ends: [`docs/hardware.md`](docs/hardware.md).

### What's in this repo

- `firmware/`: the ESP-IDF firmware. Reads the buttons, lights the
  backlights, runs the `BabyPad-Setup` Wi-Fi page, and POSTs
  `{"button": N}` to the listener. A 6-second console window at every boot
  accepts `dl` (download mode for cable reflashing), `ping`, and
  `wificlear`.
- `listener/`: `ingest.py` (plain Python 3, no extra packages), example
  settings, a systemd unit, and tests (`python3 -m unittest test_ingest.py`).
- `tools/flash.sh`: reflash over the cable with no hands on the board.
- `tools/bench-sink.py`: a stand-in listener for testing new builds safely.
- `docs/`: hardware notes and the IO9 photo.

---

## How this was reverse engineered

Everything was worked out from one pad in one day, with no schematic and no
datasheet for U3 (its `L16A / ZSD332A` marking matches nothing public):

1. Dumped the stock flash over the CH340 and disassembled it: an
   Arduino-core sketch that POSTs to the vendor cloud. The linker map showed
   addressable-LED and I²C use, which pointed to an expander instead of GPIO
   buttons.
2. Six probe firmware generations mapped every module pin live, and found
   the power-path traps the hard way.
3. The stock disassembly gave up SDA/SCL = GPIO22/23 and address `0x20`; a
   register scan while pressing buttons produced the key map.
4. The backlights fell to three presses: one lit everything (the brightness
   gate `0x48`), one lit every lamp *except* the pressed one (register 2 is
   active-low), and the third lit exactly the right lamp.
5. Battery life came from copying the stock deep sleep. The first two
   sleep builds (v31, v32) turned one press into presses on every button.
   Listing every register the stock firmware writes showed it never raises
   `0x48`; two small diagnostic builds then measured that `0x48` forces all
   keys to read pressed, instantly and every time. v34 reads keys only with
   it off, and passed a hands-on test first sent to a bench sink.

**Want to change the firmware yourself?** The full workflow (versions,
diagnostic builds, disassembly, bench testing, flashing) is in
[`docs/DEVELOPING.md`](docs/DEVELOPING.md).

---

## Safety, legality, warranty

This is your own device and your own risk. Replacing the firmware removes
the vendor cloud pairing and likely any warranty. Nothing here touches
Talli's servers or anyone else's device. TallyPad is a logging convenience,
not a medical device or a baby monitor.

## License

MIT. See `LICENSE`.

---

<sub>Keywords: Talli baby tracker, Talli pad, Talli button, Talli firmware,
Talli hack, Talli not working, Talli alternative, Talli Home Assistant,
BabyLogger, baby tracker button, one-touch baby log, diaper tracker, feeding
tracker, nursing tracker, breastfeeding log, pumping log, newborn tracker,
ESP32-C6, ESP32 custom firmware, ESP-IDF, CH340, reflash, repurpose,
local-first, no cloud, self-hosted, open source, Wi-Fi button, webhook.</sub>
