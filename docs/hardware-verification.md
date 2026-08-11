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

> **Qualified by the Linux pass — see "Identification on Linux" below.** Two
> things about that paragraph turned out to need narrowing. The prefix test was
> anchored at position 0 against a string that is only the USB product string on
> Windows, so it could never match on Linux or macOS; and on a FreeWili 1-OG the
> fallback is not merely "not the primary", it is **unreachable for this
> hardware on every platform**, because fwfinder always resolves these CPUs'
> USB identities to `SerialMain`/`SerialDisplay` and `identifyCpus()` skips any
> record carrying a structural signal. Both are measured, below.

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

## ✅ Identification on Linux, and the display-bootloader install (VERIFIED)

Run against the attached board (`FW4852`, MAIN serial `E463A8574B251838` on hub
port `3-4.1.1`, DISPLAY serial `E463A8574B531838` on `3-4.1.2`), driving the
real `FlashController`, `CpuProbeController` and `identifyCpus()` through a
scratch program linked against the built libraries.

**Signal 1 — hub port location. Works, and is always the answer.** Every
identification observed in this pass, in every board state — both CPUs running
firmware, both in BOOTSEL, one of each — resolved with
`IdentitySource::HubLocation`. `ProductString` was never the source of any
answer.

**Signal 2 — the rule-5 product string. Was broken on Linux; the prefix test is
now fed the right string, but it remains unreachable on this hardware.** Two
separate facts, and they were being conflated:

- fwfinder's `USBDevice::name` is *not* the USB product string except on
  Windows. Measured: the board reports `product=MainCPU v92` /
  `manufacturer=FreeWili` in sysfs and fwfinder hands out
  `"FreeWili MainCPU v92"`. With the display bootloader installed — whose
  descriptors really do say `manufacturer="FreeWili OG"`,
  `product="FWOG display bl 001"`, `093C:2055` — fwfinder hands out
  `"FreeWili OG FWOG display bl 001"`, which begins with neither `"FWOG display "`
  nor `"FWOG "`. `productStringOf()` (`src/device/fwDeviceRecords.cpp`) now reads
  the descriptor from sysfs instead, and the field holds what its own comment
  always said it held.
- Even so, **`identifyCpus()`' pass 2 cannot fire for a FreeWili 1-OG on any
  platform.** `getUSBDeviceTypeFrom()` maps `093C:2054`/`093C:2055` straight to
  `SerialMain`/`SerialDisplay`, and refines the RP2040 CDC PID by hub port to the
  same two — so every CPU record carries a structural flag, and pass 2 skips
  those by design. The product string's remaining live consumer is
  `ogBootloaderState()`, which is not gated on pass 2.

**What the broken string actually cost, measured on the board with the
bootloader installed:**

```
fwfinder name   = "FreeWili OG FWOG display bl 001" -> ogBootloaderState Missing
kernel iProduct = "FWOG display bl 001"             -> ogBootloaderState Present
```

`Missing` is what drives the device bar's "no OG bootloader" banner, so before
this fix Linux told the owner of a correctly provisioned board to go and install
the bootloader it already had — the false negative `ogBootloaderState()`'s own
comment singles out as the expensive direction. Windows was never affected.

**Both CPUs in BOOTSEL at once — the two-volume case.** Confirmed with two real
CPUs rather than the loopback filesystems `fwVolume.cpp` was previously measured
against. Both bootrom devices published the *identical* USB serial
`E0C9125B0D9B`, udisks2 mounted them at `/run/media/drebbe/RPI-RP2` and
`/run/media/drebbe/RPI-RP21`, `findRpiRp2Volumes()` returned **both**,
`countBootselDevices()` returned 2, and `identifyCpus()` mapped
`RPI-RP21`→MAIN and `RPI-RP2`→DISPLAY. Independently checked against
`/sys/block/sd*` → `3-4.1.1`/`3-4.1.2`: correct.

**The CPU prober's first DISPLAY answer.** With both CPUs in BOOTSEL, the real
`CpuProbeController` wrote `probe.uf2` to the DISPLAY volume; that CPU
re-enumerated as `2E8A:000A` on hub port 2 carrying DISPLAY's own serial and
product `FWOG probe 002`, and answered `display` over its CDC. Whole flow took
3.1 s. `proberCpu=DISPLAY`, `remainingCpu=MAIN`, `outcome=Success`. The 1200-baud
touch then returned the prober's CPU to BOOTSEL, so the flow is a round trip.

**`DisplayBootloader` install, end to end (the headline feature).** Run through
`FlashController` from a normally-running board. Plan, timings and enumeration:

| t | Step | Board |
|---|---|---|
| 0.1 s | ERASE MAIN — touch `/dev/ttyACM0` at 1200 baud | MAIN `093C:2054` → absent → `2E8A:0003 RP2 Boot`, auto-mounts |
| 3.1 s | copy `flash_nuke.uf2` | MAIN volume releases |
| 4.1 s | WRITE DISPLAY — touch `/dev/ttyACM1` at 1200 baud | DISPLAY `093C:2055` → absent → `2E8A:0003 RP2 Boot`, auto-mounts |
| 6.8 s | copy `bl_display.uf2` | |
| 7.7 s | `Success`, 2/2 steps | DISPLAY re-enumerates as `093C:2055 "FWOG display bl 001"` |

Both steps went through `GuardAction::TouchThenWait`, and both copies landed on
the mount point spelled `/run/media/drebbe/RPI-RP2` — the *same* path, because
MAIN's volume released and DISPLAY's then took the name. The release wait
between steps is the only thing that makes that safe, and this run is a live
instance of the case its comment describes.

**`LegacyDirect` also verified on Linux**, as the restore between phases:
`dropRedundantErases()` correctly dropped the DISPLAY erase (that CPU was
already in its bootloader), the 8.2 MB display image took 128 s and the 2.5 MB
main image 41 s, `Success` 2/2, and the board came back to `MainCPU v92` /
`DisplayCPU v67` with unchanged serials.

**The state the board was left in, and how it got there**, because the sequence
above does not by itself produce it and a reader checking the board against this
document should not have to guess. The bootloader plan's first step erases MAIN,
so immediately after it MAIN held nothing. A further `WRITE MAIN` from
`FreeWiliMainV92.uf2` was run to restore it. Final state, read from live sysfs:

    3-4.1.1  093c:2054  product=MainCPU v92          manufacturer=FreeWili
    3-4.1.2  093c:2055  product=FWOG display bl 001  manufacturer=FreeWili OG
    3-4.1.3  0403:6014  product=FreeWili             serial=FW4852

Serials unchanged throughout on all three. That DISPLAY line is the evidence
that the bootloader install worked: `FWOG display bl 001` is the bootloader's
own USB identity, and only the bootloader publishes it.

**An observation worth checking before trusting a 30 s budget.** After
`flash_nuke.uf2`, the erased MAIN CPU took a long time to come back as
`2E8A:0003`. What was actually timed: it was still absent when checked
immediately after the plan reported success, and was still absent through a
further 18 s of polling, appearing on the next 2 s sample. The interval from the
write itself was **not** timed, so the honest bound is "well over 20 s", not a
figure.

Nothing in this pass depended on it — the bootloader plan's erase is the last
thing that touches MAIN, and the DISPLAY step that follows waits on a different
CPU. `GuardAction::WaitForEraseReboot` allows `kVolumeWaitMs` (30 s) for exactly
this event.

**A plan of that shape does ship, and this is the one thing in this document
that should worry someone.** An earlier draft of this section said no such plan
exists; that was wrong. `freewili-original-deprecated` — a `defaultFirmware`
entry, offered on the Default Firmware tab — is `LegacyDirect` with three
assets, and `buildFlashPlan` orders it **ERASE DISPLAY → WRITE DISPLAY → WRITE
MAIN**. The project's own test at `tests/test_fwFlashPlan.cpp:244` pins that
sequence against the real compiled-in catalog. So the erase-then-write-the-same-
CPU race is not hypothetical: it is a shipping path, and it runs on DISPLAY,
the CPU with no BOOTSEL button.

What happens there: `fwFlashEngine.cpp:388` sets `expectedFromPriorErase`,
`classifyVolumes()` returns `ExpectedAfterErase`, and step 2 waits under
`WaitForEraseReboot` — 30 s — for a `flash_nuke` reboot measured here at "well
over 20 s". If it loses that race the outcome is `FlashOutcome::Timeout`, on a
CPU whose firmware the app has just destroyed.

The run recorded above dodged it only by accident: `dropRedundantErases()`
removed the erase because DISPLAY was already sitting in its bootrom. On the
ordinary running board this entry exists for, the erase is kept and the race is
real.

**Still not timed properly**, and that is the gap: the interval was measured
from the wrong starting point, so the margin against 30 s is unknown — it could
be 8 s or it could be negative. Anyone touching this path should time the erase
reboot from the write itself before trusting `kVolumeWaitMs`.

**Not established here:** what the physical display panel shows in any of these
states (nobody looked at the screen), and whether the DISPLAY bootloader console
behaves differently under an OG main image than under `FreeWiliMainV92` — with
v92 running, the bootloader console enumerated within 10 s and stayed up for at
least 60 s, but v92 is not an OG image and may not speak the inter-CPU protocol
the "~10 s of MAIN silence" rule is about.

---

## Not verified anywhere

- **Linux** — the identification, probe and flash paths are now verified against
  hardware; see the section above. Still unexercised there: the App Explorer
  `OgApp` flash path (no known-good OG app UF2 on this machine), and the remote
  catalog's `dlopen` of libcurl in the libcurl-absent configuration.
- **Linux, before this pass** — compiled and unit-tested, **never exercised
  against hardware.**
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
