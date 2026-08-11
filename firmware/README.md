# Embedded firmware images

The `.uf2` files here are compiled into the executable, deflated, by
`tools/embed_firmware.py` at build time. They are **not** committed -- over
20 MB of binaries do not belong in git history. Obtain them before building a
release:

| id | file | source |
|---|---|---|
| `bl_display` | `bl_display.uf2` | `fw build bl_display` in wiliOGBsp, then `build/apps/bl_display/bl_display.uf2` |
| `FreeWiliMain` | `FreeWiliMainV92.uf2` | freewili-firmware, `freewilimain/build-ramcheck/FreeWiliMain.uf2` |
| `FreeWiliDisplay` | `FreeWiliDisplayV67.uf2` | freewili-firmware, `freewiliclassicdisplay/build-recovery/FreeWiliDisplay.uf2` |
| `flash_nuke` | `flash_nuke.uf2` | Raspberry Pi's stock `flash_nuke` example (`pico-examples`, target `flash_nuke`, or the prebuilt `.uf2` from the Pico SDK docs). A RAM-resident program that erases the whole flash chip and resets back to BOOTSEL; identical for any RP2040 board, not FreeWili-specific and not built by this project. See Task 22: it backs the Default Firmware tab's "Erase MAIN CPU" action, which exists only because a running MAIN CPU keeps the DISPLAY bootloader's console from ever enumerating (it needs ~10s of MAIN silence). |

`bl_display`'s recorded version, `aab2ff3`, is the git description baked into
that binary by its build. It is shown verbatim rather than prettified, because
that is what is actually running on the board once flashed.

It used to read `826de9a-dirty`, and that value was a dead end: `826de9a`
exists in no public wiliOGBsp history -- the repo has a single squashed
`Initial public release` commit, `aab2ff3` -- and `-dirty` says the tree it was
built from was never committed anywhere. Nobody could reproduce those bytes,
and nothing in this project would have noticed, because the version shown in
the UI comes from this manifest rather than from the image. The current image
is built from `aab2ff3` with pico-sdk 2.3.0 (`cmake -DPICO_SDK_PATH=... ` on
`apps/bl`, target `bl_display`; wiliOGBsp's own CMakePresets.json is
Windows-only and has to be bypassed on Linux). One caveat that will not show up
in any test: it was compiled with arm-none-eabi-gcc 16.1.0, where
`probe/README.md` pins 15.2.Rel1 for the probe image.

The general point this is a specific case of: unlike `probe.uf2`, which
`cmake/ProbeProvenance.cmake` hash-pins and re-verifies at run time,
`bl_display` has no provenance gate at all. `test_fwCatalogEmbedded` proves the
embedded blob matches its own recorded hash -- not that the version string
beside it is true. Any bytes would pass. Treat the version field here as a
claim someone has to keep honest by hand.

**`flash_nuke.uf2` is no longer published as an RP2040-only image, and the one
you will find does not load here as downloaded.** The prebuilt at
`https://datasheets.raspberrypi.com/soft/flash_nuke.uf2` is a universal image:
224 blocks, 112 under family `0xE48BFF56` (RP2040) and 112 under `0xE48BFF57`,
each half declaring `numBlocks = 112`. The two halves carry **byte-identical
payloads** -- it is one payload advertised under two family IDs, not two images.
`file(1)` reads only the first block header and calls it a clean RP2040 image,
so it looks fine. `parseUf2` rejects it, correctly, with `BlockCountMismatch`:
112 declared against 224 actual. That is the error you get and not
`WrongFamily`, because the count check runs on block 0 while the family check
that would catch the second half only runs from block 1 onward.

`0xE48BFF57` is **not** an RP2350 family ID, and mislabelling it is how people
talk themselves into "the second half is the RP2350 build." It is
`ABSOLUTE_FAMILY_ID` -- pico-sdk `boot/uf2.h`, and `picotool` 2.3.0 prints it as
`absolute`. It marks blocks whose addresses are absolute rather than belonging
to one silicon's memory map, which is what a universal RAM image needs. The
RP2350's own families are `0xE48BFF59` (`rp2350-arm-s`), `0xE48BFF5A`
(`rp2350-riscv`) and `0xE48BFF5B` (`rp2350-arm-ns`); `0xE48BFF58` is `data`.
`tests/test_fwFlashEngine.cpp` uses `0xE48BFF59` as its wrong-family fixture for
this reason. Neither half of the download is silicon-specific: they are the same
bytes, and the silicon choice happens inside the payload, described below.

The image in this directory is the 112 RP2040-family blocks selected out of that
download and sorted by `blockNo`, with no byte modified -- each block already
carries a self-consistent `blockNo 0..111 / numBlocks = 112`. 57,344 bytes,
SHA-256 `92e25930dd1e40f7ba76fd183fcbe62cc3ddc69eae7e354c9df9d4902be1ea23`.
`picotool` can combine these images but not split them, so the selection was
done by hand.

**What that selection does and does not achieve, because the distinction
matters when someone audits this file:** it makes the *container* a well-formed
RP2040 UF2 that `parseUf2` accepts. It does not make the *contents* RP2040-only,
and it does not recover some older single-target build. The 28,672-byte payload
loads at `0x20000000` and holds three sub-images end to end, each with its own
binary-info structure: the RP2040 build at `0x20000000` (`pico_board: pico`, at
payload offset `0x17d0`), the RP2350 ARM-S build at `0x20002000` (`pico2`, at
`0x35e0`), and the RP2350 RISC-V build at `0x20004000` (`pico2`, at `0x5b60`).

**There is no dispatcher.** Payload offset 0 is pico-sdk `crt0.S`'s
`_entry_point` for a no-flash RAM image, and it is unconditional:

    20000000: 4818       ldr   r0, [pc, #96]    @ -> 0x20000100
    20000002: 4919       ldr   r1, [pc, #100]   @ -> 0xe000ed08 (SCB->VTOR)
    20000004: 6008       str   r0, [r1, #0]     @ WRITES VTOR
    20000006: c806       ldmia r0!, {r1, r2}
    20000008: f381 8808  msr   MSP, r1
    2000000c: 4710       bx    r2

It *writes* `SCB->VTOR` with the address of the image's own vector table at
`0x20000100`, then loads SP and PC out of that table and jumps. It never reads
VTOR and it tests nothing about the silicon; the only conditional near it is a
core-number check against SIO `CPUID` (`0xd0000000`), which sends everything
that is not core 0 back into the bootrom. This is worth getting right rather
than plausible: if you believe offset 0 selects, you also believe an RP2350
handed this file would sort itself out, and the selection you are relying on
does not exist.

Selection is the **RP2350 bootrom's** job and happens before any of this code
runs. It walks the IMAGE_DEF block loop -- `picotool info -a` prints all eight
blocks -- takes the IMAGE_DEF matching its architecture, applies that block's
load map (for ARM-S: `Copy 0x20002000->0x20003bb8 to 0x20000000`), and only then
enters through the vector table that block names. The RP2040 bootrom has no such
machinery: it loads the blocks to `0x20000000` and branches to the start of what
it loaded, which is precisely why `crt0.S` must set VTOR and MSP itself. That
entry point is the RP2040 sub-image, so on an RP2040 the RP2040 build runs --
the same conclusion as before, by the mechanism that is actually there.

`picotool info -a firmware/flash_nuke.uf2` therefore reports `target chip:
RP2350`, `image type: ARM Secure`, `pico_board: pico2`. That output is expected
and is not evidence of corruption. It is also not "the last binary-info block in
the file": picotool picks an IMAGE_DEF (the ARM-S one) and reads binary-info
through that block's load map, so what it surfaces is the structure belonging to
the ARM-S sub-image -- the MIDDLE of the three, at payload offset `0x35e0`.
Patching each `PICO_BOARD` string in a copy of the file and re-running `picotool
info -a` shows it directly: only the middle one changes the reported
`pico_board`; the first (`0x17d0`, the RP2040 build) and the third (`0x5b60`,
the RISC-V build) never surface at all.

`flash_nuke`'s recorded version, `pico-flash-nuke`, is not a git description
-- there is no wiliOGBsp or freewili-firmware build behind it. It is Raspberry
Pi's own image, versioned honestly as "this is the standard Pico flash-erase
tool," not as if it came from a project build here.

Update `version` in `manifest.json` whenever an image is replaced -- the
Default Firmware tab shows it, and nothing else knows the version.

**Replacing a `.uf2` in place (same filename) used to be a staleness trap; it
is not one now.** The embed step depends on the CONTENT of its inputs, not on
their timestamps: `fwog_generate_firmware` runs on every build,
`tools/embed_firmware.py` SHA-256s `manifest.json`, every image the manifest
names (including `../probe/probe.uf2`, which lives outside this directory) and
itself, and regenerates when any of that differs from the stamp it wrote last
time. So an image restored from a backup or copied with `cp -p`, arriving with
an mtime *older* than the already-generated blob, is still picked up by a plain
`cmake --build` with no reconfigure -- the case every mtime-based `DEPENDS`
list, including the one this used to be, silently gets wrong. Adding a *new*
filename needs no reconfigure either: the script reads `manifest.json`, so a new
image counts as soon as the manifest names it. The cost is one always-run
command, about 0.05 s; `cmake/EmbedFirmware.cmake` explains why that is the
right trade. Bump `version` in `manifest.json` when you replace an image
regardless -- nothing else records which build the embedded bytes came from,
and the test suite cannot catch a stale-but-differently-named version string:
it only proves the generated blob matches its own recorded hash, not that
either one is current.

`checksums.txt`, if present, is a scratch file from wherever the images were
obtained and is not read by the build. The embedder computes its own SHA-256
of the actual bytes at embed time and records that instead.

A build with images missing still succeeds: `embed_firmware.py` emits an empty
manifest and prints a warning. Only release builds need the real files.
