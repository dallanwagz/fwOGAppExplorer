# FreeWili OG App Explorer

**A single-executable desktop app for loading OG apps onto the FreeWili 1-OG.**

An OG app ships as one `<name>_main.uf2`. You pick it, you press Flash, and both
CPUs end up running it — the app finds the board, works out which RP2040 it is
talking to, and writes to the right one. No drag-and-drop into `RPI-RP2`, no
guessing which of the two identical mass-storage volumes is MAIN and which is
DISPLAY.

![FreeWili OG App Explorer](assets/OGexplore.png)

## ⚠️ First: the board needs the OG display bootloader

**OG apps will not work without it.** An OG app's display half is *embedded
inside the main UF2* and travels to the DISPLAY CPU over the inter-CPU link —
and the OG display bootloader is the thing on the other end that receives it.
On a board that does not have it, flashing an OG app leaves you with a MAIN CPU
running the app and **a display that never comes up**.

It is a one-time, per-board install, and this app does it for you:

> **Default Firmware tab → "FreeWili 1-OG Display Bootloader"**

Do that once. After that, every OG app is a single file and a single click.

Two things worth knowing:

- Installing the bootloader **erases the MAIN CPU first**. That is deliberate, not
  collateral: a running MAIN app talks continuously on the inter-CPU link, and
  the DISPLAY bootloader's console only enumerates after ~10 seconds of MAIN
  silence. It also leaves the board wanting an app, which is the next thing you
  flash.
- Installing the **original (deprecated) FreeWili 1 firmware removes the display
  bootloader again.** Coming back to OG apps means reinstalling it from the same
  tab.

The app warns you about this at flash time rather than assuming — it cannot
prove a bootloader's absence without interrogating the board, and a false
refusal would be worse than a false warning.

## Download

**[Download FwOGExplorerV1.zip →](https://github.com/freewili/fwOGAppExplorer/releases/latest)**

Windows x64. Unzip and run `fwOGExp.exe` — one statically linked executable with
the firmware images baked in, so there is nothing to install and no
redistributable to chase. The `catalog/` folder beside it holds a few sample app
UF2s; the App Explorer tab picks up anything dropped in there.

## What it does

| Tab | Purpose |
|---|---|
| **App Explorer** | **The main event: load OG apps.** Browse the catalog — embedded, a local `catalog/` folder, or a remote `apps.json` URL — and flash any app in one click. Requires the display bootloader (above). |
| **Default Firmware** | **Install the OG display bootloader here first.** Also restores the original (deprecated) FreeWili 1 firmware, or erases either CPU. |
| **Recovery** | Documentation for getting a board back when it will not enumerate. |
| **Settings** | Theme, remote catalog URL, window state. |

The parts that make it more than a file copier:

- **The two CPUs are told apart.** In BOOTSEL both MAIN and DISPLAY present an
  identical `RPI-RP2` volume, and writing an app to the wrong one is not a
  recoverable mistake on every board. The app resolves the ambiguity from serial
  enumeration where it can, and where it cannot it flashes a tiny probe image
  that reports which CPU it woke up on. See [`probe/README.md`](probe/README.md).
- **Flash plans, not single writes.** Installing the display bootloader means
  erasing MAIN first (a running MAIN keeps DISPLAY's bootloader console from ever
  enumerating), then writing DISPLAY. That ordering is data in
  `firmware/manifest.json`, not scattered through the UI.
- **Catalog entries declare their target CPU**, and the flash scheme is what
  decides which CPU an entry can reach — a `DisplayBootloader` entry cannot
  address MAIN even if it asks to.
- **UF2s describe themselves.** A FwOGapp image carries its own name, version,
  description and build identity, so dropping an unknown `.uf2` into `catalog/`
  shows you what it is and what it will do to both CPUs — with nothing
  downloaded and no catalog entry written. See below.

## The FwOGapp Contract

Firmware for the FreeWili OG follows the **FwOGapp Contract**, a set of seven
build-enforced rules defined in the BSP
([`AGENTS.md` in wiliOGBsp](https://github.com/freewili/wiliOGBsp/blob/main/AGENTS.md)).
The point of it is that an OG image is identifiable, versioned, self-describing
and recoverable without anyone opening the case. In short:

| # | Rule | Enforced by |
|---|---|---|
| 1 | Every app declares a three-digit `VERSION` and a `DESCRIPTION` | configure error |
| 2 | DISPLAY apps declare a power policy — `FWOG_POWER_DEFAULT()` or `FWOG_POWER_CUSTOM()`; the default handles the 6-second red-button shutdown hold | link error |
| 3 | 1200-baud USB BOOTSEL must not be built away — the DISPLAY CPU has no BOOTSEL button, so this is its only way back | configure error |
| 4 | The FPGA bitstream SHA-256 is pinned | automated test |
| 5 | USB identity is declared: VID/PID `093C:2054` (MAIN) / `093C:2055` (DISPLAY), product string `FWOG <cpu> <name> <version>` | build-time check |
| 6 | Every image embeds an `fwog_uf2_info_t` metadata record — magic, name, description, version, CRC | build-time check |
| 7 | MAIN apps declare a watchdog policy; the default kicks an 8.3-second hardware watchdog each loop | link error |

**Rules 5 and 6 are what this app reads.** Rule 5's `FWOG main ` / `FWOG display `
product-string prefix is one of the signals that identifies a connected board
down to which CPU it is, before anything is written — second in line behind USB
hub port location, which is authoritative, and ahead of the probe image as a
last resort. Rule 6's record is how an image explains itself: a MAIN UF2 carries
*two* records — its own, plus one describing the DISPLAY image embedded inside
it — which is what lets a single `<name>_main.uf2` be shown, and flashed, as the
one file that provisions both CPUs.

That second record is also the clearest statement of why the display bootloader
is a prerequisite rather than a nicety: the DISPLAY image genuinely is inside the
MAIN UF2, and the bootloader is what unpacks it across the link. It decides
whether to transfer by comparing CRC32, which is why a display record carries no
build identity of its own — there is nothing for it to disagree with. With no
bootloader on the board, nothing ever reads that record and the DISPLAY CPU
keeps whatever it had.

One caveat worth stating plainly: the record layout this app parses was
**derived from real images rather than from a published header** (see
[`src/catalog/fwOgAppInfo.h`](src/catalog/fwOgAppInfo.h), which documents the
offsets and the evidence). Every field is bounds-checked and a record that does
not fit is rejected rather than read past.

## Firmware for the board itself

The RP2040-side firmware — the display bootloader, the CPU prober this app
embeds, and the OG board support package — lives in the BSP repo:

**[github.com/freewili/wiliOGBsp](https://github.com/freewili/wiliOGBsp)**

## Building from source

Requires CMake 3.28+, Ninja, and a C++23 compiler. SDL3, Dear ImGui,
nlohmann/json, doctest and `freewili-finder` are fetched automatically by CMake;
nothing needs to be installed by hand.

```sh
cmake --preset win-msvc-release
cmake --build --preset win-msvc-release
```

Output lands in `build/win-msvc-release/`. On Windows, run this from a shell
with the MSVC environment loaded (the "x64 Native Tools Command Prompt") — the
preset fails loudly rather than silently falling back to a GCC on `PATH`.

Presets: `win-msvc-debug`, `win-msvc-release`, `linux-gcc-release`,
`wasm-release`.

The two MSVC presets are the verified ones, and they are the only ones that
have been run against a board. `linux-gcc-release` **configures, builds and
passes its tests** — warning-clean at `-Wall -Wextra` for this project's own
sources, `ctest --preset linux-gcc-release` green. That is a build claim and
nothing more: no Linux run has touched a device, and the app has not been
launched there. `wasm-release` has still never been configured or built — see
[`web/README.md`](web/README.md), which is explicit about what that means.

### Tests

```sh
cmake --preset win-msvc-debug
cmake --build --preset win-msvc-debug
ctest --preset win-msvc-debug
```

### Release builds need the firmware images

The `.uf2` payloads in `firmware/` are **not** committed — 20+ MB of binaries do
not belong in git history. A build without them still succeeds, with an empty
firmware manifest and a warning; only a release build needs the real files.
[`firmware/README.md`](firmware/README.md) lists each image and where it comes
from.

The one committed binary is `probe/probe.uf2` (49 kB), and its provenance is
checked at configure time and re-verified at run time before it is ever written
to a board. `probe/README.md` explains why that trade was made.

## Hardware verification status

[`docs/hardware-verification.md`](docs/hardware-verification.md) records exactly
what has been confirmed against a physical board and what has not. Read it before
trusting a flash path you have not exercised yourself.

## License

MIT — see [LICENSE](LICENSE).

Bundled third-party code: [miniz](third_party/miniz) (public domain) and Brad
Conte's [SHA-256](third_party/sha256) (public domain). Dependencies fetched at
build time keep their own licenses: SDL3 (Zlib), Dear ImGui (MIT), nlohmann/json
(MIT), doctest (MIT).
