# TallyPad hardware notes

Everything below was measured on a real board (`Talli (tm) BL-V1-001 Rev 2.0`,
`(c) 2018,2019 BabyLogger`, `DATE:20240718`) — not inferred from a schematic,
because there isn't one public.

## Board tour

- **ESP32-C6-WROOM-1** module (single-core RISC-V, Wi-Fi 6 capable; we use
  plain 2.4 GHz STA/AP).
- **U6 = CH340G** USB-serial bridge + 12 MHz crystal + micro-USB jack `J1`.
  The board flashes and logs over a plain USB cable at 115200 baud. RTS pulses
  the chip's EN (reset); that's what `tools/flash.sh` uses.
- **U3** — 16-pin TSSOP marked `L16A / 0205 / ZSD332A`. No public datasheet or
  marking-database hit. Behaviorally it is an I²C keypad + LED expander at
  address **`0x20`**, SDA = **GPIO22**, SCL = **GPIO23** (pin assignment
  recovered from the stock firmware disassembly).
- Transistor bank Q1–Q7 next to U3 drives the button backlights.
- 8 silicone light-pipe buttons on the top side, each a small SMD tactile
  switch; a 9th switch (`SW10`) is the "sync" button.
- Status LED: WS2812-style addressable chain on **GPIO10** (stock firmware
  linked `esp32-hal-rmt.c`, which is what gave this away).

## U3 register map (as proven live)

| Register | Meaning | Evidence |
|---|---|---|
| `0x00` (16-bit) | Key state. Bits 8–15 = buttons 1–8 in reading order (top row left→right, then bottom row), **active HIGH**. Press and release both update it; polling at 30 Hz is plenty and bounce-free. | Nine-press capture run, each press one clean bit, in press order. |
| `0x02` (16-bit) | Per-lamp white backlight control, **ACTIVE-LOW** (clear a bit to light a lamp). The byte order was never fully isolated, so the firmware clears the button's bit in both bytes — harmless and correct either way. | Setting the pressed button's bit lit every lamp *except* it (a perfect complement image ⇒ inverted logic). |
| `0x48` (16-bit) | Global brightness gate. `0x0000` = everything dark regardless of reg 2 (the stock idle state); `0xffff` = full. Write this nonzero or nothing ever lights. | Every single-register attempt lit nothing until a cumulative register hunt crossed this one and all lamps came on. |

The firmware's boot console keeps raw access for exploring further:
`w<reg><val16>` writes, `r<reg>` reads (hex).

## The sync button is a hardware reset

Button 9 never appears in U3's key register. It is wired into power/reset and
reboots the board on press. During mapping it caused a string of mystery
`rst:0x1 (POWERON)` resets until a press-order capture run landed its record
exactly on a reboot. Treat it as the pad's reset button; it cannot be a
software input.

## ⚠️ Pins that will hurt you

| Pin | Fact |
|---|---|
| **GPIO3** | In the power path. Driving it high killed the board mid-run. |
| **GPIO11** | In the power path. Driving it high power-cycles the board **instantly** — seven consecutive poweron resets, each at the exact drive step, fast enough to corrupt an in-flight NVS write. A 2 ms test pulse *survives*, which is how it first masqueraded as a ground. |
| **GPIO18** | In the power path; killed the board mid-drive once. Its resting level also changes with battery state — do not trust a single read. |
| GPIO10 | Tied low (and doubles as the LED data line in stock wiring). |
| GPIO2 | Real signal line; its ADC rests mid-scale (~721/4095) behind a divider — the leading candidate for battery sense, unconfirmed. |

If you write probe firmware: blacklist those pins in code AND persist a "died
while driving pin X" note to NVS **before** each drive, or a power-cut will
erase the lesson and you will loop.

## Stock firmware, for the record

Arduino ESP32 core 3.0.0 sketch ("Talli Device v2.0", v2.0.17, built
2024-07-09). A state machine (`WaitForPressState`, `TransmitState`, …) POSTs
each press to `https://api.talli.me/` with `deviceId`, `buttonIndex`,
`channelId`, `requestId`, `battery`, `devicePairingToken`, `firmwareVersion`,
`macAddress`, authenticated with an embedded `X-API-KEY` (in the flash dump in
the clear — one more reason not to publish your dump) plus an HMAC in
`X-TALLI-SIGNATURE`. Holding sync 8–10 s opens its own captive portal for
Wi-Fi setup, which is where its cloud pairing happens.

**If you reflash back to a stock dump, you lose the software download-mode
path** — the stock image has no `dl` console command, so every subsequent
flash needs the IO9-to-GND finger again.

## Dead ends, so you don't repeat them

1. Buttons as switches between module pins — no.
2. Buttons as a scanned GPIO matrix — no.
3. U3 as an I²C device on the three "low" lines — those aren't a bus (one is
   ground, one is *power*).
4. A "wake pin" for a sleeping U3 — no.
5. Buttons as an ADC resistor ladder — no.
6. The low lines as a shift register (load/clock/data) — no.

The actual answer (I²C on GPIO22/23) came from the stock firmware's own code,
which is the moral of the story: **disassemble first, probe second.**

## Power

Wi-Fi permanently associated draws enough that 4×AA cells last days, not
months. The stock firmware deep-sleeps between presses: it sets U3 to latch
the button port and unmasks its interrupt, then calls `esp_deep_sleep` with
a 30-minute timer. A press brings the chip back up and the firmware reads
the latched buttons on boot. Firmware v31 does the same.
