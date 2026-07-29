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

`bl_display`'s recorded version, `826de9a-dirty`, is the git description baked
into that binary by its build (an uncommitted tree at build time). It is
shown verbatim rather than prettified, because that is what is actually
running on the board once flashed.

`flash_nuke`'s recorded version, `pico-flash-nuke`, is not a git description
-- there is no wiliOGBsp or freewili-firmware build behind it. It is Raspberry
Pi's own image, versioned honestly as "this is the standard Pico flash-erase
tool," not as if it came from a project build here.

Update `version` in `manifest.json` whenever an image is replaced -- the
Default Firmware tab shows it, and nothing else knows the version.

**Replacing a `.uf2` in place (same filename) is a staleness trap.** The
build re-embeds automatically because CMake depends on every `*.uf2` in this
directory, but that dependency is a configure-time glob: it only sees files
that already existed at the last `cmake` configure. A file replaced in place
after that is picked up by an incremental build (its mtime changed, so the
custom command reruns); a *newly added* filename is not seen until the next
`cmake` configure. Either way, also bump `version` in `manifest.json` when
you replace an image -- nothing else records which build the embedded bytes
came from, and the test suite cannot catch a stale-but-differently-named
version string: it only proves the generated blob matches its own recorded
hash, not that either one is current.

`checksums.txt`, if present, is a scratch file from wherever the images were
obtained and is not read by the build. The embedder computes its own SHA-256
of the actual bytes at embed time and records that instead.

A build with images missing still succeeds: `embed_firmware.py` emits an empty
manifest and prints a warning. Only release builds need the real files.
