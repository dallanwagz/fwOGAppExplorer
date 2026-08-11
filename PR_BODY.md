# Linux support

`fwOGAppExplorer` builds, runs, and flashes a real FreeWili 1-OG on Linux.

Before this branch the `linux-gcc-release` preset had, per the README, "never
been configured or built", and every POSIX branch in `src/platform/` carried an
`UNVERIFIED` comment. Those comments are gone because the claims are now true,
not because they were deleted.

**The headline: the display bootloader install — the thing the README tells you
to do first — was run end to end on Linux through the app's own
`FlashController`.** Erase MAIN with `flash_nuke`, then write `bl_display` to
DISPLAY; 2/2 in 7.7 s. The board now enumerates `093c:2055` with product
`FWOG display bl 001`, which only the bootloader publishes.

## What was actually broken

The build failed at 113/559 on a missing `#include <functional>` in the fetched
`freewili-finder` — one line, fixed through the `PATCH_COMMAND` precedent this
repo already had. That was the easy part. What the hardware found:

- **`copyToVolume()` reported success with 7% of a 4.9 MB image still in page
  cache** — 8,931 of 9,605 sectors on the device at the instant of return. The
  49 kB prober image hides it entirely, so testing with `probe.uf2` alone would
  have concluded nothing was wrong.
- **A directory named `RPI-RP2` was accepted as a board**, and written into,
  and reported as a completed flash. Matching a path component is a weaker
  question than the Windows branch asks. The device is now asked instead:
  fstype, then `Board-ID` from the volume's own `INFO_UF2.TXT`.
- **A board that HAD the OG bootloader was reported as `Missing`** — the false
  negative that sends a user to erase and reflash a CPU that was fine.
  `fwfinder`'s device name is the raw USB product string only on Windows; on
  Linux it prepends the manufacturer, and every prefix test is anchored at
  position 0.
- **libcurl's write callback let `std::bad_alloc` unwind through C frames** —
  SIGABRT, exit 134, core dumped. And `CURLOPT_TIMEOUT` was a *total* 30 s cap
  on a path that downloads a 16.4 MB firmware image, so Linux aborted downloads
  Windows completes.
- **Redirects were unbounded and could walk an https catalog URL down to plain
  http**, reopening from the server side exactly what `normalizeRemoteCatalogUrl`
  refuses to let a user configure.
- **`readSerialLine` never asserted DTR**, which the header calls load-bearing.
- **Relative `$XDG_DATA_HOME`/`$HOME`/`TMPDIR` were taken at face value**, so
  settings and staged firmware followed the working directory instead of
  persisting — every write succeeding.

## What was already right

The 1200-baud BOOTSEL touch needed no change, on either CPU — including
DISPLAY, which has no BOOTSEL button and for which this is the only way back.
The reset is triggered by the baud write alone: holding the fd open still
resets with DTR high and `close()` eight seconds away, and dropping DTR at
B9600 does nothing. It works even from a port left in a hostile termios state.

The UI needed no rendering changes at all. It runs at 60 fps, 198 MB RSS,
window mapped 91–95 ms after exec, clean exit 15 of 15 runs.

## Honesty, not just function

The stated requirement is "no dependencies, one exe". On Linux that was never
true. `libudev.so.1` is a hard dependency from `freewili-finder`, and
`readelf` understates it — SDL3 is static but `dlopen`s its backends. The
README now states both truths. `docs/hardware-verification.md` records what was
and was not confirmed against the board, including a Step 4b that used to tell
testers a state "writes nothing" when it now writes immediately.

## Verification

- **551 test cases / 2170 assertions**, up from 494/1771. Green with network,
  without it (`unshare -rn`), and with libcurl unloadable.
- Warning-clean at `-Wall -Wextra` across a forced recompile of all 67 TUs.
- The GUI was never allowed onto a real display; everything ran on a private
  `Xvfb` with a per-run assertion that the compositor socket was never opened.

Every component was built by one agent and then verified by a separate one with
fresh context that inspected real output — binaries, screenshots, `readelf`,
sysfs, and the board itself. Six of the ten failed their first review.

## Not verified

The physical display panel (nobody looked at the screen), Wayland end-to-end,
any second machine, and two real boards in BOOTSEL simultaneously.
`freewili-original-deprecated` erases and then writes DISPLAY against a 30 s
budget with an erase reboot measured at "well over 20 s" from the wrong
starting point — that margin is unknown and is called out in the docs.
