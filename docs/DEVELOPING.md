# Making your own changes to TallyPad

This is how the firmware was actually developed: the loop that finally
worked, the tools, and the traps. If you change the firmware, follow the
same loop and you won't repeat our mistakes.

## The loop

1. **See what the stock firmware does** before inventing anything. It
   already solved most problems (button reading, lamps, deep sleep). The
   disassembly shows how. See [Reading the stock firmware](#reading-the-stock-firmware).
2. **Measure on the pad with a diagnostic build** before writing the
   fix. A diagnostic build is normal firmware plus a few lines at boot that
   try register values and print what the chip reports. It needs no
   button presses, so you can run it with nobody at the pad. See
   [Diagnostic builds](#diagnostic-builds).
3. **Write the change**, bump the banner, commit it, and tag it (`vNN`).
4. **Bench-test it** with presses going to `tools/bench-sink.py`, not to
   your real listener. Press single buttons, press once after the pad has
   gone to sleep, and press several buttons quickly. Pass means the sink
   shows exactly what you pressed and nothing else.
5. **Only then** build it with your real listener address and flash it for
   daily use. Keep the previous tag handy to go back.

The one time we skipped steps 1, 2 and 4 (v31 and v32), each build went
straight onto the pad in daily use. Both turned one press into presses on
every button. Guessing the cause from the code was wrong twice. One
diagnostic build and one bench test found and proved the fix (v34) within
the hour.

## Setup

- ESP-IDF 5.1 with target `esp32c6` (README Step 1).
- Put your settings in `firmware/main/local_config.h`: copy
  `local_config.h.example`. It's git-ignored, so the tracked source stays
  the same for everyone and a pull never conflicts with your address.
- Keep the stack sizes in `firmware/sdkconfig.defaults` (main task 8192,
  event task 4608). With the IDF defaults, early builds crashed.
  If you delete `sdkconfig`, it's rebuilt from these defaults.

```sh
cd firmware
idf.py set-target esp32c6   # first time only
idf.py build
```

## Versions

Every build that goes onto a pad gets a commit and a tag, even
diagnostic builds and even ones that turned out broken. That way going
back is always `git checkout vNN`, then build and flash. Say plainly in the
commit message whether it has been tested on a pad.

| Tag | What | Status |
|---|---|---|
| v29 | Always-on firmware, per-button white lamps | Works; empties AA cells in days |
| v30 | Light sleep + modem sleep | Built, never run on a pad |
| v31 | Stock-style deep sleep | **Broken**: one press became presses on all buttons |
| v32 | v31 + input latch cleared on wake | **Broken**, same way (that wasn't the cause) |
| v29d | Diagnostic: key reads with lamps dark vs lit | Found the cause |
| v29e | Diagnostic: how fast keys change, stock-style lamps | Proved the fix needs no delay |
| v33 | Fix with a 10 ms dark gap | Superseded by v34 before use |
| **v34** | Deep sleep; keys read only with the lamp gate off | **Recommended.** Bench-tested and in daily use; battery life not yet measured |

To flash an older version: `git checkout v29`, then `idf.py build`, then
flash, then `git checkout main`. Tags older than the `local_config.h`
change (v29 to v32 and the diagnostics) have the address written directly
in `main.c` (`INGEST_URL`). Set it there for that build, and
`git checkout -- firmware/main/main.c` afterwards.

## Flashing

- **First time:** hold IO9 to GND while powering up (README Step 7).
- **After that:** run `tools/flash.sh`. It resets the pad, types `dl` into the
  6-second boot console window, and flashes. No hands needed.
- **Only one program may have the serial port open.** Stop any serial
  monitor or logger first. Two readers split the bytes between them, so
  esptool fails to connect ("multiple access on port") and you'll miss the
  boot banner.
- **Check what's running by reading the boot banner.** Reset the pad
  (pulse RTS, or `tools/flash.sh` does it) and read the first lines. Every
  build prints `=== TallyPad vNN (...) ===`.
- **Never flash the stock backup back** unless you mean to stay on stock.
  The stock firmware has no `dl` command, so every later flash would need
  the IO9 wire again.

## Reading the stock firmware

You need your own `stock-backup.bin` (README Step 8; keep it private, it
contains your pad's cloud key) and the ESP-IDF RISC-V toolchain
(`riscv32-esp-elf-objdump`, installed with ESP-IDF).

1. Find the app image and its segments:
   ```sh
   esptool.py --chip esp32c6 image-info --version 2 stock-backup.bin   # or cut out the app partition at 0x10000 first
   ```
   The flash code segment maps to `0x42000020`, and IRAM to `0x40800000`.
2. Cut each segment out with `dd`, using the file offsets image-info
   prints, and disassemble it at its load address:
   ```sh
   riscv32-esp-elf-objdump -D -b binary -m riscv:rv32 \
     --adjust-vma=0x42000020 irom.bin > dis-irom.txt
   ```
3. Find the U3 (I²C expander) helpers. They're small functions called
   with a register number in `a1` and a value in `a2`, for example
   `li a1,72` just before a `jal`. In our image the 16-bit write helpers
   are at `0x420011f0` and `0x4200122c`, and the 16-bit read is at
   `0x420010ac`. Yours will match if your pad runs the same stock build
   (v2.0.17).
4. List every call site with its arguments. A 10-line script that
   looks back a few instructions from each `jal` to those addresses gives
   you the complete register sequence. `docs/hardware.md` has the result.

This is how we found SDA/SCL and the U3 address, the stock deep-sleep
sequence, and that the stock firmware lights lamps without ever raising
`0x48`.

## Diagnostic builds

Add a block right after `u3_init()` in `app_main` that sets registers,
reads the keys, and prints the result:

```c
{   /* do the keys read as pressed while the lamps are lit? */
    uint16_t v;
    u3_wr16(0x48, 0x0000); v = 0; u3_rd16(0, &v); printf("DIAG dark keys=%02x\n", (v >> 8) & 0xff);
    u3_wr16(0x48, 0xffff); v = 0; u3_rd16(0, &v); printf("DIAG lit  keys=%02x\n", (v >> 8) & 0xff);
    u3_wr16(0x48, 0x0000);
}
```

Flash it, reset the pad, and read the serial output. Repeat each
measurement a few times in a loop, so one odd read doesn't fool you. Give
the build its own banner and tag (`v29d`, `v29e` are the real examples in
this repo).

## Bench testing

```sh
python3 tools/bench-sink.py          # on your computer, listens on :4189
```

Build the test firmware with `INGEST_URL "http://<your-computer>:4189/press"`
in `local_config.h`, flash it, and press:

- one button once;
- the same button again after the pad has gone to sleep (20 s after a
  press, or 2 min after a reset), which tests the wake path;
- three buttons quickly;
- a few buttons at once right after it wakes, which tests the wake batch.

The sink prints each press with a timestamp. Anything you didn't press
means the build fails. Then put your real address back in
`local_config.h`, rebuild and flash.

## Traps

- **`0x48` makes every key read as pressed.** Never read reg `0x00` while
  it's on. Measured, not guessed: the effect is instant in both directions.
- **GPIO3, GPIO11, GPIO18 are in the power path.** Driving them high
  power-cycles the board.
- **The 9th ("sync") button is a hardware reset**, not an input.
- **A console that prints nothing isn't proof that nothing happened.**
  After a flash, confirm the version from the banner on a fresh reset.
