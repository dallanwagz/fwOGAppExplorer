# Hardware verification — FreeWili OG App Explorer

Status at handoff: **detection verified on real hardware; no firmware write
has been performed.** The CPU prober has since been **rebuilt against the
board's own BSP after failing on hardware, and is itself awaiting hardware
verification** — see its section below.

This document records what was confirmed against a physical board, and gives
the remaining steps as a checklist ready to run. It is deliberately explicit
about what was *not* done and why.

---

## Why the flash steps were not run

The remaining steps write firmware to a physical board. Running them
unattended was the wrong call, for a reason that is about the images
available rather than about caution in general:

Every UF2 currently in `firmware/` that can legally target the MAIN CPU is
one that **replaces or erases whatever the board is running now**:

| Image | Target | Effect if flashed |
|---|---|---|
| `FreeWiliMainV92.uf2` | MAIN | Overwrites MAIN with the deprecated legacy firmware |
| `flash_nuke.uf2` | MAIN | Erases MAIN, leaving it blank (also erases DISPLAY as step 1 of `LegacyDirect` — see Step 5) |
| `bl_display.uf2` | DISPLAY | **Out of scope — DISPLAY writes are not authorised** |
| `FreeWiliDisplayV67.uf2` | DISPLAY | **Out of scope — DISPLAY writes are not authorised** |

There is no "known-good app UF2" on this machine that would leave the board
in the state it started in. Choosing which of the above to accept — and
accepting the loss of whatever MAIN currently holds — is the board owner's
call, not something to decide on their behalf. So the write stops here.

MAIN has a reachable BOOTSEL button, so every step below is recoverable by
hand. That is true of MAIN only.

---

## ✅ Step 1 — Identification (VERIFIED)

This was the spec's **stated assumption**, and the most important thing to
confirm, because the entire CPU-role assignment rests on it.

Observed on a connected FreeWili OG:

```
FREE-WILi [OG]  serial FW4788
MAIN:    COM24  (by hub position)
DISPLAY: COM59  (by hub position)
```

**Result: the assumption holds.** fwfinder reports usable hub port
locations, and the primary identification path — `USBHubPortLocation`
(`Main=1`, `Display=2`) — is what actually runs.

The USB-product-name fallback (`"FWOG main "` / `"FWOG display "` prefixes,
read from `DEVPKEY_Device_BusReportedDeviceDesc`) is also live and reachable
on Windows, but it is the fallback, not the primary. No revision to the
Recovery tab text is needed.

One real-world observation worth keeping: **this board has been seen
reporting its serial as the literal string `"Unknown"`**, which is what
fwfinder emits for a flashable OG board when it finds no FTDI child. The app
treats `"Unknown"` and empty as equally unidentified
(`serialIsUnidentified()`, `src/device/fwDeviceModel.cpp`) and refuses to
flash rather than trusting the topological `uniqueID`, which names a USB
socket rather than a board.

---

## ☐ Step 2 — `DisplayBootloader` on a board that already has one

**NOT AUTHORISED in this pass — this writes to the DISPLAY CPU.**

Listed here only for completeness of the original plan. Skip it unless you
have separately decided to accept DISPLAY-write risk. Note the owner has
confirmed that re-running a bootloader install on a board that already has
one is safe and idempotent.

---

## ☐ Step 3 — `OgApp` with a known-good app UF2  ← **start here**

This is the highest-value remaining test and the only one that exercises the
normal, everyday flash path.

1. Obtain a known-good OG app UF2 (an app built against `wiliOGBsp`).
   Place it in a `catalog/` directory beside the executable, or point the
   remote catalog URL at a store that serves one.
2. Launch the app. Confirm the device bar shows the board with both CPUs.
3. Select the app in **App Explorer** and press **Flash**.
4. Expect: the app performs a 1200-baud touch on MAIN, one `RPI-RP2` volume
   appears, the UF2 is copied, and the volume disappears as the board
   reboots.
5. Confirm the display image transfers over the inter-CPU link and the
   display comes up.

**Windows note:** the 1200-baud touch works by opening the CDC port at 1200
baud, which triggers `rom_reset_usb_boot_extra()`. On Windows the open then
*fails* with "A device attached to the system is not functioning." **That
failure is the success indicator**, not an error — the app is written to
expect it.

---

## ☐ Step 4 — Refusals (mostly non-destructive; run these)

These test that the app declines to act. Steps 4a and 4b write nothing.

- **4a. Unplug mid-plan.** Start a flash, pull the cable partway through.
  Expect a failure message that names exactly which steps completed. Confirm
  it does not claim success.
- **4b. Both CPUs in BOOTSEL by hand.** Two `RPI-RP2` volumes mount. Expect
  a **refusal**, not a copy — the two volumes are genuinely
  indistinguishable, so guessing is never acceptable. Confirm the message
  links to the Recovery tab.
- **4c. One CPU in BOOTSEL by hand.** Expect the typed-confirmation prompt
  to appear. Type a wrong answer and confirm it is rejected. (Typing the
  correct answer *will* proceed to a write — stop before that unless you
  intend the write.)

---

## ☐ Step 5 — `LegacyDirect` (LAST, and destructive)

**Reordered after a real hardware failure — see below before running it.**

The plan is now three steps, and the order is enforced by
`buildFlashPlan()` rather than by the manifest's array position:

```
1. ERASE  DISPLAY   (flash_nuke.uf2)
2. FLASH  DISPLAY   (FreeWiliDisplayV67.uf2)
3. FLASH  MAIN      (FreeWiliMainV92.uf2)
```

**Why it changed.** The old plan was `[MAIN, DISPLAY]`. Run on the owner's
board, step 1 wrote MAIN successfully; step 2 then touched the DISPLAY CPU,
waited the full 30 s for an `RPI-RP2` volume, and timed out. The DISPLAY
image was never written. The cause is the same 10-second rule the Recovery
tab documents: the display bootloader's USB console only enumerates after
~10 s of MAIN-CPU silence, and MAIN running the freshly written legacy
firmware chatters continuously. **MAIN-first does not risk the DISPLAY
step — it makes it unreachable.**

So DISPLAY is dealt with while MAIN is still quiet, and MAIN — the CPU that
*has* a BOOTSEL button and is therefore always recoverable — goes last.
Erasing DISPLAY first is what makes this recoverable rather than a one-way
door: **an RP2040 with erased flash enumerates `RPI-RP2` by itself, with no
button**, so step 2 writes to a volume the erase brought back and never needs
the DISPLAY serial port at all. `flash_nuke.uf2` is safe on the DISPLAY CPU —
it only erases flash and reboots to BOOTSEL, and never drives GPIO 29, so the
PDM-microphone hazard that makes a main *application* image dangerous there
does not apply to it.

**Still destructive, and still writes to the DISPLAY CPU.** It destroys the
display bootloader from step 1 onward. If you run it, run it last, and
restore afterwards with Step 2.

**What to watch for on a real run:** step 2 should NOT prompt for a typed
`DISPLAY` confirmation. The engine credits the volume that appears after
step 1 to the CPU step 1 just erased (`VolumeState::ExpectedAfterErase`), so
a prompt there means either the erase did not take or something else was
mounted — stop and investigate rather than typing it.

---

## ☐ Erase MAIN

The `erase-main-cpu` action writes `flash_nuke.uf2` to MAIN, leaving it
blank. It is MAIN-only by two independent mechanisms (the `OgApp` scheme
permits only `Main`, and the display-retarget control is gated to
`Unlisted && schemeInferred` entries while this one is always `Embedded`).
Both are unchanged by the `LegacyDirect` reordering and by the addition of the
standalone DISPLAY erase below: the erase image reaching the DISPLAY CPU is
scoped by `eraseAllowsCpu()` to `LegacyDirect` plans plus the one
`erase-display-cpu` slug, neither of which the `erase-main-cpu` entry is. It is
still not retargetable — rewriting its asset's `cpu` to `Display` produces an
empty plan, which `test_fwFlashPlan.cpp` pins.

It requires typing **`ERASE MAIN`** — deliberately a *different* phrase from
the flash engine's `MAIN`, so the confirmation cannot be completed by
habit.

Recovery afterwards is by BOOTSEL button on MAIN plus flashing real
firmware, so only run this when you have a replacement image to hand.

---

## ☐ Erase DISPLAY — AWAITING HARDWARE VERIFICATION

**Status: shipped; never written to a board by this action.** The same
`flash_nuke.uf2` has been written to the DISPLAY CPU on real hardware as step 1
of the `LegacyDirect` plan, so the image and the CPU are not new to each other;
what is new is reaching it from a standalone entry.

The `erase-display-cpu` action writes `flash_nuke.uf2` to DISPLAY. It is
DISPLAY-only by the same two independent mechanisms, mirrored: the
`DisplayBootloader` scheme permits only `Display`, and the display-retarget
control is gated to `Unlisted && schemeInferred` entries while this one is
always `Embedded`. A third, narrower mechanism keeps the scheme itself from
being widened: `eraseAllowsCpu()` admits a DISPLAY erase for this **slug**
specifically, not for `DisplayBootloader` entries in general, so the bootloader
install — and any future display entry, local or remote — still cannot acquire
an erase step by listing the erase image beside its own.

It requires typing **`ERASE DISPLAY`**, again deliberately different from the
flash engine's `DISPLAY`.

**Why this is offerable at all on the CPU with no BOOTSEL button:** an RP2040
with blank flash re-enumerates `RPI-RP2` unprompted, with nothing pressed and
nothing running. That is the same property the `LegacyDirect` plan's leading
erase already depends on. Erasing DISPLAY is destructive and recoverable, in
that order — reinstall the bootloader from the same tab straight afterward.

**What to watch for on a real run:** an `RPI-RP2` volume should appear on its
own within a few seconds of the copy completing, with no button held. If it
does not, stop: that is the one outcome this action's safety argument rests on.

---

## ☐ CPU prober (`probe/probe.uf2`) — REBUILT, AWAITING HARDWARE VERIFICATION

**Status: rebuilt against the board's own BSP; never written to a board.**

### What happened the first time

The original prober was built standalone in this repository for
`PICO_BOARD=pico`. It **failed on real hardware**. Copied directly to `E:\`
(bypassing this app), with drives, USB PIDs and COM ports polled every 300 ms:

```
22:55:27.9  drives=[E:,G:]  pids=[VID_2E8A&PID_0003, &MI_00, &MI_01]  ports=[COM70]
22:55:30.9  drives=[G:]     pids=[VID_2E8A&PID_0003, ...]             ports=[COM70]
22:55:36.5  drives=[E:,G:]  pids=[VID_2E8A&PID_0003, ...]             ports=[COM70]
```

`E:` accepted the UF2, rebooted, ran ~5.6 s, then returned to BOOTSEL on its
own. **No CDC ever enumerated** — `PID_0003` is the RP2040 bootloader PID and
was the only PID seen throughout. (COM70 was the board's unrelated FTDI.) The
UF2 was well-formed; its *board configuration* was wrong.

### What changed

The prober is now `apps/cpuprobe` in **wiliOGBsp** and is built there, so it
inherits the FreeWili OG board header. The sharpest difference is one that the
`boot2_name` field hides: both board headers select `boot2_w25q080` *by name*,
but `PICO_FLASH_SPI_CLKDIV` is compiled **into** boot2 — `pico.h` sets 2,
`freewili_og.h` sets 4. The two 256-byte boot2 blocks differ at exactly one code
byte (offset 28, `0x02` → `0x04`) plus the 4-byte checksum. The failed image was
clocking XIP at `clk_sys/2`.

It links `fwog_common` only — never `fwog_main_bsp` or `fwog_display_bsp`, whose
`board_init()` would drive the wrong CPU's pins. Full provenance, the
`picotool` field-by-field diff, and the freshly re-derived GPIO-29 and
1200-baud evidence are in `probe/README.md`.

### What is established, and what is not

Statically verified (this is the entire verification budget — no hardware):

- compiles and links clean, warning-free, and produces a UF2;
- `picotool info -a` reports `pico_board: freewili_og`, `boot2_w25q080`, USB
  stdin/stdout, and `Fixed Pin Information: none`; the SDK's generated linker
  region for this configure is `LENGTH = (16 * 1024 * 1024)`;
- **GPIO 29 is never configured or driven** — re-derived from scratch against
  the new `.dis`, because linking BSP code made the previous proof stale. The
  exhaustive literal-pool sweep over IO_BANK0/PADS_BANK0 and all their atomic
  aliases finds only `gpio_init`/`gpio_set_function`, called from exactly four
  sites with constant pins 18/4/6/7, plus the SDK's known IE-disable on pads
  26–29. Every SIO GPIO write is masked to bit 18 alone;
- the 1200-baud BOOTSEL path is present in the instruction stream, and passes
  an activity-LED mask of 0, so it drives no pin either;
- reproducible: three independent build trees give byte-identical `.bin`/`.uf2`.

**NOT established — this is the checklist item that remains open:**

- [ ] The image **boots on real silicon**. This is the specific thing the
      previous version failed at, and no static check can substitute for it.
      The root cause above is a well-supported inference from the board headers
      and the boot2 byte diff, **not a measurement**.
- [ ] It **enumerates a USB CDC port** and holds it (the previous version never
      did).
- [ ] It prints `main` or `display`, once per second, and keeps doing so.
- [ ] The answer is **correct** on a known CPU — flash it to a CPU whose
      identity is already known and confirm the word matches.
- [ ] The **1200-baud touch** actually returns it to BOOTSEL.

Suggested first run, on **MAIN only**, because MAIN has a reachable BOOTSEL
button and DISPLAY has none: put MAIN in BOOTSEL by hand, copy `probe.uf2` to
the single `RPI-RP2` volume, and watch for a new COM port saying `main`. Do not
exercise this on DISPLAY, and do not use the two-volumes-mounted flow, until
the single-CPU case is confirmed.

---

## Not verified anywhere

- **Linux** — compiled and unit-tested, **never exercised against hardware.**
  The `linux-gcc-release` preset builds warning-clean and `ctest` is green (515
  cases / 2058 assertions), and the POSIX branches of `fwPaths.cpp` and
  `fwSerialPorts.cpp` have been compiled and run on that machine — each states
  in its own file comment exactly what was observed and what was not. Nothing
  in the checklist above has been repeated there: no `RPI-RP2` volume has been
  discovered, no 1200-baud touch performed, no UF2 written, and the app itself
  has never been launched on Linux. The remote catalog `dlopen`s libcurl rather
  than link-depending on it, and neither the libcurl-present nor the
  libcurl-absent path has been run; the absent one needs a container without it
  to test honestly.
- **Emscripten / web** — never compiled, by explicit decision. See
  `web/README.md` for what a person with emsdk should try first, including
  the COOP/COEP headers `-pthread` requires.
