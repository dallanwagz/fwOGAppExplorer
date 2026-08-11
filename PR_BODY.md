# Linux support

`fwOGAppExplorer` builds, runs, and flashes a real FreeWili 1-OG on Linux.

Before this branch the `linux-gcc-release` preset had, per the README, "never
been configured or built", and every POSIX branch in `src/platform/` carried an
`UNVERIFIED` comment. Those comments are gone because the claims are now true,
not because they were deleted.

**The headline: the display bootloader install — the thing the README tells you
to do first — works.** It was run end to end through the app's own
`FlashController`, and then the board's owner drove it through the UI on a board
that had ended up with *both* CPUs blank. It recovered and completed. DISPLAY
now enumerates as `093c:2055 FWOG display bl 001`, which only the bootloader
publishes, and MAIN sits blank in its bootrom — the documented end state, the
board "wanting an app, which is the next thing you flash".

## What was actually broken

The build failed at 113/559 on a missing `#include <functional>` in the fetched
`freewili-finder` — one line, fixed through the `PATCH_COMMAND` precedent this
repo already had. That was the easy part.

**Found by review, against real hardware:**

- **`copyToVolume()` reported success with 7% of a 4.9 MB image still in page
  cache** — 8,931 of 9,605 sectors on the device at the instant of return. The
  49 kB prober image hides it entirely, so testing with `probe.uf2` alone would
  have concluded nothing was wrong.
- **A directory named `RPI-RP2` was accepted as a board**, written into, and
  reported as a completed flash. Matching a path component is a weaker question
  than the Windows branch asks. The device is now asked instead: fstype, then
  `Board-ID` from the volume's own `INFO_UF2.TXT`.
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

**Found by using it.** These four came out of one real flash going wrong, after
every review pass had finished, and they are the ones worth reading closely —
none of them was reachable from the test suite or from any amount of staring at
the code:

- **The erase-reboot budget was too short by more than half, on a shipping
  destructive path.** `freewili-original-deprecated` plans ERASE DISPLAY →
  WRITE DISPLAY → WRITE MAIN, and step 2 waited on `kVolumeWaitMs`, whose 30 s
  is justified in its own comment by "a touch works in about a second". An erase
  is not a touch: `flash_nuke` erases the whole flash chip before resetting.
  Measured six times on the real board — **61.6 s, 6.3 s, 62.0 s, 62.0 s,
  61.9 s, 61.8 s**. So the install erased the DISPLAY CPU, waited half as long
  as the hardware needed, and reported that the CPU never came back. It had; the
  app stopped looking about thirty seconds early. On the one CPU with no BOOTSEL
  button.

  This is the worst failure shape the program has — not a wrong write, but a
  true statement arriving after the damage, telling the user nothing they can
  act on. `kEraseRebootWaitMs` is now 150 s, and the budget is carried on the
  wait rather than read from a global, so the countdown always belongs to the
  question actually being asked.

  **Not Linux-specific.** `kVolumeWaitMs` is shared code; Windows raced the same
  budget. Linux got there first only because someone finally ran it.

- **A failed run kept claiming progress.** The step bar stayed at "Step 2 of 3 —
  48%" with the time estimate still counting "taking longer than expected". The
  reported symptom was "looks like it might be stalled", on a dialog that had
  finished and printed exactly what went wrong — a half-filled bar with a
  ticking clock argues louder than a paragraph.

- **The wrong-CPU refusal recommended a remedy that could not work.** It ended
  "Flash the *other* CPU first, or unmount that volume". Flashing the other CPU
  is a different plan; unmounting leaves the step with no drive *and* no port,
  refusing again one branch over. The user got unstuck by doing the one thing
  the message never mentioned — putting the step's own CPU into BOOTSEL.

- **The MAIN recovery procedure gave no observable signal.** It said to
  disconnect the battery, which is true and needs the case open, and explained
  that until the board is really off the button is not read — without saying how
  to know. That gap is exactly how a replug that merely *looks* like a power
  cycle gets mistaken for one. It now says: hold the red button until the
  internal LED stops blinking, keep holding, plug back in.

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
`readelf` understates it — SDL3 is statically linked but `dlopen`s its
backends. The README now states both truths.

`docs/hardware-verification.md` records what was and was not confirmed against
the board. Two entries in it were actively dangerous and are fixed: a Step 4b
that told a tester a state "writes nothing" when it now writes immediately, and
a claim that no shipping plan erases and then rewrites the same CPU — one does,
and it is the one whose timeout was wrong.

## Verification

- **553 test cases / 2177 assertions**, up from 494/1771. Green with network,
  without it (`unshare -rn`), and with libcurl unloadable.
- Warning-clean at `-Wall -Wextra` across a forced recompile of all 67 TUs.
- The GUI was never allowed onto a real display during development; everything
  ran on a private `Xvfb` with a per-run assertion that the compositor socket
  was never opened.

Every component was built by one agent and then verified by a separate one with
fresh context that inspected real output — binaries, screenshots, `readelf`,
sysfs, and the board itself. Six of the ten failed their first review.

That process was worth having and it was not sufficient. The four defects in
the "found by using it" section above all survived it, and the most serious one
— the erase-reboot timeout — had actually been *predicted* in
`docs/hardware-verification.md` as an unmeasured risk, flagged, written down,
and left unmeasured because measuring it required erasing a CPU and watching a
clock. It took a real install failing on a real board to close that loop.

## Not verified

The physical display panel (nobody looked at the screen), Wayland end-to-end,
any second machine, and two real boards in BOOTSEL simultaneously.
