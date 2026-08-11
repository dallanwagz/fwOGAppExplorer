# FWOG probe — the committed CPU-prober image

A ~49 kB RP2040 UF2 whose only job is to answer **"which CPU am I running on?"**
It prints `main` or `display` over USB CDC, once per second, forever.

> **The source is not in this directory, and not in this repository.**
> It is **`apps/cpuprobe` in the [wiliOGBsp](https://github.com/freewili/wiliOGBsp)
> BSP**. Only the built artefact (`probe.uf2`) is committed here.
> This file is the provenance record and the safety evidence for that artefact.

Built from wiliOGBsp commit **`330ded5ecd2f83ea1a3e6d5cc1d16e3e58d38f34`**.

**Status: VERIFIED ON HARDWARE (MAIN CPU, Linux).** This image has been
statically verified in full (below), and has now also been written to a real
FreeWili 1-OG and run. Written twice by `fwog::copyToVolume()` on Linux to
MAIN's `RPI-RP2` volume, it was accepted by the bootrom, rebooted, enumerated as
`2e8a:000a "FWOG probe 002"` on MAIN's own hub port carrying MAIN's serial
(`E463A8574B251838`), and printed `main` on its CDC — read back through
`fwog::readSerialLine()` and accepted by `fwog::parseProbeLine()` as
`ProbeAnswer::Main`. It then honoured the 1200-baud touch back into BOOTSEL, and
MAIN was restored to `FreeWiliMainV92.uf2`.

Two things this does NOT cover, and neither should be read as verified: the
image has never run on the **DISPLAY** CPU (the destructive work was
deliberately confined to MAIN, which has a physical BOOTSEL button as a human
fallback), and the CC1101 probe was therefore only ever observed returning the
MAIN answer — a `display` answer from this image has not been seen on hardware.
See `docs/hardware-verification.md`.

## Why this exists

The FreeWili OG has two RP2040s: **MAIN** and **DISPLAY**. In BOOTSEL mode both
present an identical `RPI-RP2` mass-storage volume. When two are mounted at once
the desktop app cannot tell them apart and refuses to write — the only safe
answer, because a MAIN application image on the DISPLAY CPU can physically
damage the board.

That refusal used to be a dead end when *both* CPUs are erased: both enumerate
`RPI-RP2`, every path refuses — including the one that would fix it — and that
state is reachable using the app's own documented features. This image is what
closes it.

This image breaks the tie. The **CC1101 sub-GHz radio is wired to MAIN only**, so
we probe for it:

* CC1101 answers with VERSION `0x14` → **MAIN**
* anything else → **DISPLAY**

Flash it to one unknown volume, read the CDC port, and both drives are
identified — the second by elimination. Then 1200-baud touch it back into
BOOTSEL and flash the real image.

## Why it moved into the BSP

The previous prober lived here and was built standalone for `PICO_BOARD=pico`.
**It did not boot on this silicon.** Copied to a real board it accepted the UF2,
ran for about 5.6 s, never enumerated a CDC port, and returned to BOOTSEL on its
own. The image was well-formed; its *board configuration* was wrong.

`PICO_BOARD=pico` is not a neutral choice on this hardware:

| Setting | `pico` | `freewili_og` |
|---|---|---|
| `PICO_FLASH_SIZE_BYTES` | 2 MB | **16 MB** |
| `PICO_FLASH_SPI_CLKDIV` | **2** | **4** |
| `PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK` | unset | **1** |
| `clk_sys` | SDK default 125 MHz | **200 MHz** (`fwog_clocks_init`) |
| `PICO_BOOT_STAGE2_CHOOSE_W25Q080` | 1 | 1 |

The last row is why "same boot2" was a misleading reassurance. Both headers
select `boot2_w25q080` **by name**, but `PICO_FLASH_SPI_CLKDIV` is compiled
*into* boot2. Byte-comparing the 256-byte boot2 block of the old image against
the new one:

```
differing byte offsets in boot2: [28, 252, 253, 254, 255]
  off  28: old=0x02 new=0x04      <- the CLKDIV literal
  off 252..255                    <- the 4-byte boot2 checksum
```

One code byte. The failed image was clocking XIP at `clk_sys/2` where this board
wants `clk_sys/4`. Building against the board's own BSP is therefore a
**correctness requirement** for this image, not a tidiness preference.

## What it links, and what it deliberately does not

`cpuprobe` is the one binary in the BSP that is **neither a display app nor a
main app**, because it runs on a CPU nobody has identified yet.

* It links **`fwog_common`** (the CPU-agnostic library) and `hardware_spi`.
* It does **not** link `fwog_main_bsp` or `fwog_display_bsp`. Either one's
  `board_init()` brings up the inter-CPU link, the watchdog, the display, the
  FPGA or the LCD and drives that CPU's pins — on a processor that may be the
  other one. That is the exact accident this tool exists to prevent.
* It does **not** call `fwog_main_app()` / `fwog_display_app()`, and embeds no
  display image (`fwog_embed_display_image()` is not called).
* It does **not** call `fwog_usb_ids()` or `fwog_add_uf2_info()`: both admit only
  `display` or `main`, and there is no honest value for an image that is
  deliberately neither. Claiming one would be an authoritative-looking answer
  that is wrong half the time, about the single question this image answers.

The only thing it takes from `fwog_common` beyond the board header is
`fwog_clocks_init()` — VREG, PLLs and CLOCKS, **no GPIO at all**. Everything
else in the archive is unreferenced and never reaches the link.

## THE RULE: never touch GPIO 29

**On the DISPLAY CPU, GPIO 29 is `MIC_SIG`, the output of a PDM microphone.**
A MAIN application image drives GPIO 29 (it is `FPGA_RESET` on MAIN) and, running
on DISPLAY, fights the microphone's driver. That can damage the board.

GPIO 29 being untouched is the entire reason this image is safe to flash to an
unidentified CPU, which is the entire reason it exists. MAIN has a BOOTSEL
button; **DISPLAY has none and no recovery path.**

The complete set of GPIOs this firmware may touch:

| GPIO | MAIN name        | DISPLAY name (rev4+) | Role here                  |
|------|------------------|----------------------|----------------------------|
| 4    | `RADIO_SPI_MISO` | `I2S_SPK_DIN`        | spi0 RX                    |
| 6    | `RADIO_SPI_SCLK` | `I2S_SPK_BCLK`       | spi0 SCK                   |
| 7    | `RADIO_SPI_MOSI` | `LED_SERIAL`         | spi0 TX                    |
| 18   | `RADIO_SPI_CS1`  | (unassigned)         | SIO output, CS, idle HIGH  |

Plus the USB pins, which are not bank-0 GPIOs on RP2040.

**Do not add a status LED. Not on GPIO 25, not anywhere.** No backlight, no
display, no buzzer, no button read, no UART. On DISPLAY there is no BOOTSEL
button; the 1200-baud touch below is the only way back, and a damaged board has
no way back at all. Every extra pin is new risk on the CPU with no recovery path.

In particular, never define `PICO_STDIO_USB_RESET_BOOTSEL_ACTIVITY_LED` — it
makes the SDK's BOOTSEL path drive a GPIO.

`UART0 is the inter-CPU link on BOTH CPUs` and must never carry stdio. That is
enforced in three places: `PICO_STDIO_UART 0` forced in wiliOGBsp's top-level
`CMakeLists.txt`, `fwog_configure_stdio()` in the app, and no `PICO_DEFAULT_UART`
in the board header.

## Provenance of the probe sequence

The two-byte exchange is lifted verbatim from the shipping FreeWili firmware
(`fwsparta`), where the identical `CC1101_Comm_Check()` appears in both:

* `freewilimain/FreeWilliMain.cpp` — MAIN halts and returns to BOOTSEL if the
  radio does *not* answer.
* `freewilidisplay/FreeWilliDisplay.cpp` — DISPLAY halts and returns to BOOTSEL
  if the radio *does* answer, and runs this at every power-up from
  `powerup_verify()`.

So driving these pins on the DISPLAY CPU is already validated by shipping
firmware. This is not a new hardware interaction.

### One deliberate difference from DISPLAY's version

DISPLAY's shipping check uses `I2S_SPK_LRCLK` = **GPIO 5** as chip select
(`#define obRadio1SPICS obI2SLRCLK`). We use **GPIO 18** instead, matching
MAIN's version, because on MAIN:

* GPIO 18 (`RADIO_SPI_CS1`) selects **radio 1** — the one MAIN's own boot check
  requires to answer.
* GPIO 5 (`RADIO_SPI_CS0`) selects **radio 2**, which may not be populated.

A false `display` reading on a MAIN CPU is the dangerous direction of error, so
we use the CS that MAIN itself treats as authoritative. GPIO 18 on DISPLAY was
`IO_DIR5` on rev1 hardware (a level-shifter direction input, driven by DISPLAY
as an output) and is unassigned from rev4 onward, where IO direction moved to an
I2C expander. Driving it as an output creates no contention either way.

## Build

Built by the BSP's own task runner, which is the supported path. **Use
PowerShell**; `cmd.exe` invoked via bash no-ops on this machine.

```powershell
cd <your-wiliOGBsp-checkout>
python tools/fw.py build cpuprobe
# -> build/apps/cpuprobe/cpuprobe.uf2  (49,664 bytes)
Copy-Item build\apps\cpuprobe\cpuprobe.uf2 <this-repo>\probe\probe.uf2
```

Toolchain comes from the BSP's `CMakePresets.json` — **do not override it, and
never pass `-DPICO_BOARD` on the command line**; the BSP's top-level
`CMakeLists.txt` sets `PICO_BOARD freewili_og` as a `FORCE`d cache value and a
command-line `-DPICO_BOARD` silently reverts it to the wrong config.

| Component  | Version / path                                          |
|------------|---------------------------------------------------------|
| pico-sdk   | **2.3.0** (from the standard `.pico-sdk/sdk/2.3.0` install) |
| toolchain  | arm-none-eabi-gcc **15.2.Rel1**                          |
| Ninja      | **v1.13.2** (`.pico-sdk/ninja/v1.13.2`)                  |
| picotool   | from the same `.pico-sdk/picotool` install               |
| board      | `PICO_BOARD=freewili_og` (from the BSP; never overridden) |
| build type | `RelWithDebInfo` (the `target` preset)                   |

**Deterministic — and, since wiliOGBsp `330ded5`, deterministic *across days*.**

```
cpuprobe.bin  SHA-256 89471457357052713ec79312a407f0e6c2805cfc075799b21fcf1382e494e1a3
cpuprobe.uf2  SHA-256 d3f5eb07a5dc60e8cfa3854fa1c0736923cbcb5b98b38f74c09cef468583a6b6
```

The earlier claim of determinism here was true only *within a single day*, and
that is worth spelling out because it is the failure this section now exists to
prevent. The pico-sdk stamps the wall-clock build date into the binary-info
block — `pico_standard_binary_info/standard_binary_info.c` defaults
`PICO_PROGRAM_BUILD_DATE` to `__DATE__` — so the image changed by exactly one
byte, the day-of-month digit at offset 38433, the moment the clock rolled past
midnight. Every one of the three drift checks below then failed the next
morning with nothing whatsoever having changed.

A gate that cries wolf nightly is a gate people learn to route around, which is
precisely the behaviour it exists to prevent, so the cause was removed rather
than the symptom: `apps/cpuprobe/CMakeLists.txt` now **pins**
`PICO_PROGRAM_BUILD_DATE` to a fixed string. Pinned rather than dropped
(`PICO_NO_BI_PROGRAM_BUILD_DATE=1`), so `picotool info -a` still prints a build
date and the binary-info table keeps its shape; the value simply says for
itself that it is not a wall clock. That is the **only** use of `__DATE__`,
`__TIME__` or `__TIMESTAMP__` reachable from this image in either the SDK or
the BSP.

Evidence, in the order it was gathered:

| Check | Result |
|---|---|
| Same tree, two consecutive days, unpinned | `.uf2` differed at offset 38433 and nowhere else, `'8'` → `'9'` |
| Pin set to `"Jul 28 2026"`, built a day later | Reproduced the *previous* committed `probe.uf2` **byte for byte** (0 differing bytes) — so the clock was the only nondeterminism |
| Three fresh build trees, three different paths, final source | All three `.uf2` and `.bin` identical to the hashes above |
| Build with `-D__DATE__="Xxx 99 9999" -D__TIME__="99:99:99"` forced onto the target | **Same** `.uf2` — date-independence demonstrated, without touching the system clock |
| Negative control: same injection, pin removed | Hash changed, and `Xxx 99 9999` appears in `cpuprobe.bin` at offset 19196 — so the injection was live and the pin is what suppresses it |

The pin is `PRIVATE` to the `cpuprobe` target and reaches the SDK's
`standard_binary_info.c` only because `pico_standard_binary_info` is an
INTERFACE library whose sources compile straight into the target — the same
mechanism as `USBD_PRODUCT`. Verified in the generated `build.ninja`: 88
occurrences, **all** under `apps/cpuprobe/CMakeFiles`, out of 1516 `DEFINES`
lines in the tree. **No other BSP app is affected.**

Full build is **warning-clean** under `-Wall -Wextra`.

### The `.uf2` IS committed, and how the drift risk is answered

`probe/probe.uf2` is checked in — the only `.uf2` in this repository that is.
Two hard requirements collide and both have to hold:

* the desktop app must be **one self-contained executable**, so the prober has
  to be compiled into it;
* building the **desktop** app must not require an ARM cross-toolchain *or a
  firmware checkout*, so it cannot be built from source during that build.

49 kB in git is the price of both being true.

The objection to committing it was, and remains, the right one: a binary that
drifts out of sync with its source would be the worst possible failure mode for
a tool whose whole job is to be trustworthy on the CPU with no recovery path.
That is answered rather than waved away — the drift is **detected**:

1. **Configure time**, by `cmake/ProbeProvenance.cmake`, in two halves of
   deliberately different strength:
   * `probe.uf2` against `kProbeImageSha256` — **always**. This is the
     load-bearing check: it is the artefact that ships, the artefact written to
     a board, and the artefact the evidence below is about.
   * `apps/cpuprobe/main.c` and `apps/cpuprobe/CMakeLists.txt` against
     `kProbeSourceSha256` / `kProbeBuildScriptSha256` — **whenever a wiliOGBsp
     checkout is reachable**. `FWOG_BSP_DIR` defaults to a sibling directory;
     when it is absent the build reports STATUS and continues, because requiring
     a firmware checkout would break the second requirement above.

   The build script is hashed alongside the source because several safety
   properties are decided by it alone — the 1200-baud way back, UART stdio being
   off, the *absence* of `PICO_STDIO_USB_RESET_BOOTSEL_ACTIVITY_LED`, and which
   BSP libraries are linked.
2. **Embed time**, by `tools/embed_firmware.py`, which records the SHA-256 of
   the bytes it actually compressed into the executable.
3. **Run time**, in `identifyCpus()` (`src/flash/fwCpuProbe.cpp`): the loaded
   image is hashed and compared with `kProbeImageSha256` before a single byte
   of it reaches a board. This is the one image the app writes to a CPU it has
   *not* identified, and it does not get written on the strength of a build
   step nobody re-ran.

**To change the prober:** edit `wiliOGBsp/apps/cpuprobe`, rebuild it there, copy
the new `cpuprobe.uf2` over `probe/probe.uf2`, then build the desktop app and
paste the "actual" values the failure reports into `src/flash/fwCpuProbe.h`
(including `kProbeBspCommit`). And re-do the verification evidence below — the
hashes prove the binary matches the source, not that the new source is still
safe.

## Use

The desktop app drives all of this for you: **Recovery tab → "Two RPI-RP2 drives
are mounted" → Identify CPUs**. It refuses unless exactly two `RPI-RP2` volumes
are mounted and at most one FreeWili is connected, requires two agreeing
readings from the prober, and re-checks that the volumes have not changed
before it will still show you the answer. See `src/flash/fwCpuProbe.h`.

By hand, the same sequence is:

1. Put the unknown CPU in BOOTSEL so it mounts as `RPI-RP2`.
2. Copy `probe.uf2` onto that volume. It reboots and enumerates as a USB serial
   device named **`FWOG probe 002`** (manufacturer `FreeWili OG`, VID:PID
   `2E8A:000A`).
3. Open the port at any baud rate and read a line. It says `main` or `display`,
   once per second. That drive letter is now identified — and if two `RPI-RP2`
   volumes were mounted, the other one is the opposite CPU.
4. Return it to BOOTSEL by opening the port at **1200 baud** and closing it
   (the standard Pico touch — the same mechanism the desktop app uses
   everywhere else). `picotool reboot -u -f` works too.
5. Flash the real firmware.

The VID:PID are left at the SDK's stock `2E8A:000A` on purpose. `fwog_usb_ids()`
would stamp `093C:2054` (main) or `093C:2055` (display), and `fw.py` and
`freewili-finder` identify a CPU **by that PID first** — so either choice would
be an authoritative claim that is wrong half the time. The stock pair identifies
*nothing*, which is the truth here. For the same reason the product string is
`FWOG probe 002`, which matches neither of `fw.py`'s `FWOG main ` / `FWOG display `
prefixes.

The probe re-runs every second rather than caching one boot-time result: if the
SPI read is ever marginal, a repeated sample is a far better answer than a
single one taken microseconds after power-up, and a human can watch whether the
answer is stable. **The loop never exits and never resets itself** — the way
back is the 1200-baud touch, not a timer.

## Verification evidence

All statically verified against
`wiliOGBsp/build/apps/cpuprobe/cpuprobe.elf` / `.dis`, freshly derived for this
binary. The previous version's evidence does not carry over — linking BSP code
made it stale, so it was redone from scratch.

The static evidence below is what licensed the FIRST write. It has since been
flashed and run on MAIN (see Status, above), so the static analysis is no longer
the only thing standing behind this image on that CPU — but it is still the only
thing standing behind it on DISPLAY.

### The build-date pin (`330ded5`) changed no instruction

This is the one rebuild whose evidence *was* carried forward rather than
re-derived from nothing, so the licence to do that is set out here in full.
`objdump` output for the old and the new image differs in **exactly five
`.word` literal-pool constants** — two in `.text`, three in the RAM-resident
`.data` code — and every one of the five is a `.rodata` string address in
`0x100049b8`–`0x10004ea4`, moved because the pinned date string is a different
length from `Jul 28 2026`. **Zero mnemonic lines differ.** `.boot2` and
`.binary_info` are byte-identical; every section's size and VMA is unchanged.

Verified, not assumed: the sweeps below were re-run against the new
`cpuprobe.dis` and their output compared with the same sweeps against the old
one. It is identical, character for character —

* the complete literal-constant set in `0x40010000`–`0x4001ffff`;
* the count of GPIO-29-specific register addresses in the image (still **0**);
* the four `gpio_init` / `gpio_set_function` call sites;
* every SIO access, and `main`'s `lsls r4,r4,#24` / `lsls r5,r5,#11` (SIO base
  and the bit-18 mask `0x40000`);
* the full disassembly of `tud_cdc_line_coding_cb`.

`main`'s literal pool — including both `.rodata` pointers it holds — is itself
unchanged. The only occurrence of `0x20000000` anywhere in the image is the
SRAM base in `runtime_init_install_ram_vector_table`, not a GPIO mask, and that
too is unchanged.

### `picotool info -a`, against the failed image

| Field | Failed `pico` build | This build | |
|---|---|---|---|
| `pico_board` | `pico` | **`freewili_og`** | ✅ the fix |
| `boot2_name` | `boot2_w25q080` | `boot2_w25q080` | same *name*, different bytes (see above) |
| `sdk version` | 2.1.1 | 2.3.0 | follows the BSP's pin |
| `features` | USB stdin / stdout | USB stdin / stdout | unchanged |
| `binary start` | `0x10000000` | `0x10000000` | unchanged — a UF2 copy runs from the base on either CPU |
| `binary end` | `0x10006d60` | `0x100060e0` | 3,200 bytes smaller |
| `Fixed Pin Information` | none | **none** | no pin is declared |
| `name` | `FWOG probe` | `FWOG cpuprobe` | |
| `version` | `1.0.0` | `002` | three digits, per the BSP's app contract |
| `web site` | `.../fwOGappexplorer` | `.../wiliOGBsp` | follows the source |
| `build attributes` | `Release` | `RelWithDebInfo` | the BSP's `target` preset |
| `build date` | wall clock | **`reproducible`** | pinned; the field is still present and still printed, it just is not a clock |
| `Metadata Blocks` | none | none | unchanged |

Flash geometry is not a `picotool info` field. It is established instead by the
SDK's generated linker region for this configure, which is built from
`PICO_FLASH_SIZE_BYTES`:

```
build/pico-sdk/src/rp2_common/pico_standard_link/pico_flash_region.ld
    FLASH(rx) : ORIGIN = 0x10000000, LENGTH = (16 * 1024 * 1024)
```

`cpuprobe` then caps *itself* at `ORIGIN = 0x10000000, LENGTH = 128k` via
`fwog_flash_region()`. That is a **link-time ceiling, not a load offset** — the
image still links and loads at the flash base. Its purpose is to turn "the
prober grew past 128 KB" into a link error rather than an image whose UF2 blocks
reach the display CPU's metadata sector at `0x10020000`. Today the binary is
24,800 bytes, so the ceiling is ample headroom.

### GPIO 29 is never configured or driven

1. **Only two functions in the image touch bank-0 GPIO configuration
   registers**: `gpio_set_function` (`0x100003b8`) and `gpio_init`
   (`0x100003e8`). Established by enumerating **every literal-pool constant in
   `cpuprobe.dis`** in the `0x40014000`–`0x4001ffff` range (IO_BANK0 and
   PADS_BANK0 *including all three atomic SET/CLR/XOR aliases*). This grep is
   exhaustive because those bases are **not shift-constructible on Cortex-M0+**,
   so they cannot reach a register without appearing in a literal pool. The
   complete result, for the whole binary:

   | Constant | Alias | Found in |
   |---|---|---|
   | `0x40014000` | IO_BANK0 direct | `gpio_init`, `gpio_set_function` |
   | `0x4001c004` | PADS_BANK0 direct | `gpio_init`, `gpio_set_function` |
   | `0x4001d004` | PADS_BANK0 XOR | `gpio_init`, `gpio_set_function` |
   | `0x4001f000` | PADS_BANK0 CLR | `runtime_init_rp2040_gpio_ie_disable` (point 4) |

   Both functions index `PADS_BANK0`/`IO_BANK0` purely by their `gpio` argument.
   The GPIO-29-specific addresses `0x400140e8` (GPIO29_STATUS), `0x400140ec`
   (GPIO29_CTRL) and `0x4001c078` (PADS_BANK0 GPIO29) are **absent from the
   binary entirely**.

2. **Those two functions are called from exactly four sites**, all in `main`,
   all with compile-time-constant pin numbers:

   ```
   100002a2:  movs r0, #18   ;  100002a4: bl gpio_init
   100002cc:  movs r0, #4    ;  100002d6: bl gpio_set_function   (r1 = 1, GPIO_FUNC_SPI)
   100002dc:  movs r0, #6    ;  100002de: bl gpio_set_function
   100002e4:  movs r0, #7    ;  100002e6: bl gpio_set_function
   ```

   No other call sites exist anywhere in the image, and neither function's
   address is ever taken as data — so there is no indirect path to them either.
   `gpio_set_dir` and `gpio_put` are inlined and appear only as the SIO writes
   in point 3.

3. **Every SIO GPIO write in the image uses the mask `0x40000`, i.e. bit 18
   only.** In `main`, `r4 = 0xd0 << 24 = 0xD0000000` (SIO base) and
   `r5 = 0x80 << 11 = 0x40000`. The complete set of SIO GPIO accesses:

   | Instruction | Register | Value |
   |---|---|---|
   | `str r5,[r4,#20]` | `GPIO_OUT_SET` | `0x40000` |
   | `str r5,[r4,#36]` | `GPIO_OE_SET` | `0x40000` |
   | `str r5,[r4,#24]` | `GPIO_OUT_CLR` | `0x40000` |
   | `ldr r3,[r4,#4]` + `tst r3,r5` | `GPIO_IN` | read only |

   Bit 29 (`0x20000000`) never appears as a mask. Inside `gpio_init` the mask is
   `1 << r0` with `r0` the constant 18 from its single call site.

4. **One honest exception, disclosed — unchanged from the previous version and
   re-confirmed on this binary:** the SDK's
   `runtime_init_rp2040_gpio_ie_disable` (`0x100016c8`) executes at pre-init:

   ```
   movs r2, #64                 ; 0x40 = the IE bit
   ldr  r3, =0x4001f000         ; PADS_BANK0 CLEAR alias
   str  r2, [r3, #120]          ; pad 29
   str  r2, [r3, #116]          ; pad 28
   str  r2, [r3, #112]          ; pad 27
   str  r2, [r3, #108]          ; pad 26
   ```

   It **clears the input-enable bit** on pads `io[26..29]`, GPIO 29 included.
   This is the only GPIO-29-related register access in the whole image. It is
   retained deliberately:
   * It only *clears* `IE`. It never writes `OD`, and never writes
     `IO_BANK0 GPIO29_CTRL` (that address is absent from the binary, point 1),
     so `FUNCSEL` stays at its reset value `NULL` (`0x1f`) and **the output
     driver for GPIO 29 remains disabled**. The pin is not driven, cannot be
     driven, and no contention is possible.
   * Clearing `IE` makes the pin *more* isolated, not less — it disconnects the
     digital input buffer. The SDK does this precisely because GPIO 26–29 can sit
     at mid-rail and burn current in the input buffer.
   * Every pico-sdk image ever flashed to this board does the same, including
     the shipping FreeWili firmware. Suppressing it
     (`PICO_RUNTIME_SKIP_INIT_RP2040_GPIO_IE_DISABLE=1`) would make the grep
     cleaner at a small real cost in hardware behaviour — optimising the
     evidence rather than the safety. We did not do that.

5. **Linking `fwog_common` introduced no new pin activity.** This was the open
   question for this rebuild, and the answer is in point 1: the literal sweep
   over the whole binary finds nothing outside the two SDK GPIO helpers and the
   SDK's IE-disable. The one BSP function reached, `fwog_clocks_init()`
   (`bsp/common/clocks.c`), touches VREG, the PLLs and CLOCKS only. The BSP's
   per-CPU libraries are not linked, so `board_init()`, the inter-CPU link, the
   watchdog, the LCD, the FPGA and the WS2812 driver are all absent from the
   symbol table.

6. `picotool info -a` reports **`Fixed Pin Information: none`**.

### The 1200-baud BOOTSEL touch is genuinely compiled in

* **Compile flags** for this target include
  `-DPICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1` and
  `-DPICO_STDIO_USB_RESET_MAGIC_BAUD_RATE=1200`, and contain **no**
  `PICO_STDIO_USB_RESET_BOOTSEL_ACTIVITY_LED`. `fwog_configure_stdio()`
  additionally fails the build outright if `PICO_ENABLE_USB_RESET_VIA_BAUD_RATE`
  is ever turned off globally.
* **Symbol table**: `tud_cdc_line_coding_cb` at `0x10002380`,
  `rom_reset_usb_boot_extra` at `0x100015dc`, plus `usb_reset_interface_*` and
  `tud_vendor_control_xfer_cb` (the picotool vendor-reset path).
* **Disassembly** of `tud_cdc_line_coding_cb` shows the magic value materialised
  in the instruction stream, not merely configured:

  ```
  10002396:  movs r2, #150
  10002398:  lsls r2, r2, #3      ; 150 << 3 = 1200
  1000239a:  cmp  r3, r2          ; against the requested bit rate
  1000239c:  beq  100023a0
  100023a0:  movs r2, #0
  100023a2:  movs r0, #1
  100023a6:  negs r0, r0          ; r0 = -1
  100023a8:  bl   rom_reset_usb_boot_extra
  ```

  The leading `-1` is the activity-LED GPIO. `rom_reset_usb_boot_extra` branches
  on `r4 < 0` and leaves `r0 = 0`, so the activity-pin **mask is 0** — entering
  BOOTSEL drives no pin either. This matters here: it is what keeps the recovery
  path itself inside the four-GPIO budget.

### Build cleanliness

Full build of the whole BSP tree produces **zero warnings and zero errors**, and
the BSP's host test suite passes (40/40 CTest, 167/167 Python).
