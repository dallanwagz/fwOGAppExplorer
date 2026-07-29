# Web (Emscripten) build — UNVERIFIED

**Nothing in this directory, and nothing in the `wasm-release` preset, has ever
been compiled or run.** There is no Emscripten SDK on the machine this was
written on and no intention to install one, so the entire web target was
written from the documentation and from what the desktop build proves about the
same source. Treat every claim below as a hypothesis until an `emsdk` owner has
actually run it.

The Windows/MSVC build **is** verified and was kept passing throughout: 279
cases / 931 assertions, `ctest` 1/1, zero warnings from this project's own
code at `/W4 /permissive-`, in both `win-msvc-debug` and `win-msvc-release`.

The Linux (`linux-gcc-release`) preset is **also unverified** — there is no
Linux toolchain on this machine and that preset has never been configured, let
alone built. It now passes `-static-libstdc++ -static-libgcc`, which is what
the design spec's build table promises and what the "no dependencies, one exe"
requirement needs; the flags are written from their documented behaviour and
have not been run. See the comment above them in `CMakeLists.txt`.

---

## What the web build is for

Browsing the catalog and reading the Recovery documentation. That is all it can
be. A browser cannot open a USB mass-storage volume or a serial port, so device
detection and flashing are impossible there — not "not implemented yet",
impossible.

The app says so rather than failing at click time:

- `fwog::kDeviceSupportAvailable` (`src/core/fwTypes.h`) is `false` under
  `__EMSCRIPTEN__`.
- `fwog::platformLimitationNotice()` (same header) is the single sentence shown
  everywhere flashing is unavailable, so the device bar's notice and every
  disabled Flash button's reason cannot drift apart.
- `flashDisabledReason()` checks it **first**, ahead of the no-device check, so
  the message names the platform instead of claiming "No FreeWili is
  connected." — which would be true and completely misleading.
- The Default Firmware cards still render with their plans and versions; the
  Recovery tab is untouched and fully functional.

Embedded firmware still compiles in. It cannot be flashed from a browser, but
the versions and plans stay visible, which is the point of that tab.

## Building it

```
cmake --preset wasm-release
cmake --build --preset wasm-release
```

Requires `EMSDK` to be set in the environment (the preset points
`CMAKE_TOOLCHAIN_FILE` at `$env{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`).
Output lands in `build/wasm-release/` as `fwOGAppExplorer.html` plus its
`.js` / `.wasm` / worker files.

## Serving it — read this before `python -m http.server`

**The brief's suggested command does not work, and it is worth understanding
why before reaching for a workaround.**

The build is compiled and linked with `-pthread`. That is not a style choice:
`fwHttp.cpp`'s Emscripten branch uses `EMSCRIPTEN_FETCH_SYNCHRONOUS`, which the
browser forbids on the main thread. `RemoteCatalog` (Task 14) already runs that
fetch on a `std::thread` worker, and under Emscripten a `std::thread` *is* a Web
Worker — which is exactly where a synchronous fetch is legal. So the existing
threading is what makes the web HTTP path work at all, and `-pthread` is what
makes `std::thread` exist.

`-pthread` implies `-sSHARED_MEMORY`, and browsers only hand out
`SharedArrayBuffer` to a **cross-origin isolated** page. That requires two
response headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

`python -m http.server` sends neither, so `fwOGAppExplorer.html` served that way
will refuse to start. `web/shell.html` detects this and shows a sentence saying
so instead of a blank canvas — but it can only report the problem, not fix it.

A server that does work:

```python
# save as coop-serve.py, run:  python coop-serve.py build/wasm-release 8080
import functools, http.server, socketserver, sys

class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

directory, port = sys.argv[1], int(sys.argv[2])
handler = functools.partial(Handler, directory=directory)
with socketserver.TCPServer(("", port), handler) as httpd:
    print(f"serving {directory} on http://localhost:{port}/")
    httpd.serve_forever()
```

Then open `http://localhost:8080/fwOGAppExplorer.html`.

The alternative — dropping `-pthread` — is not free: it would take
`RemoteCatalog`'s worker with it and leave the synchronous fetch illegal on the
main thread, so the remote catalog would need rewriting around asynchronous
`emscripten_fetch` callbacks first. That is a real design change, not a flag
flip.

## What an emsdk owner should try first

In this order, because each step's failure mode is distinct and the later steps
are worthless if an earlier one is broken:

1. **`cmake --preset wasm-release`.** Configure only. This is where the SDL3
   FetchContent build for Emscripten will fail if it is going to — SDL3 does
   support Emscripten, but this project has never exercised that path, and the
   `SDL_SHARED OFF` / `SDL_STATIC ON` combination is what the desktop build
   uses, not necessarily what SDL3's Emscripten path expects.
2. **Build `fwog_tests` alone** (`cmake --build --preset wasm-release --target
   fwog_tests`). It links no UI at all, so it is the cheapest way to find out
   whether *this project's own* code compiles for the web. It is deliberately
   built but **not** registered with `ctest` for this preset — the output is a
   `.js` that needs `node` with the right flags, not something the host can
   execute. Run it by hand with `node` if you want the assertions.
3. **Build `fwOGAppExplorer`.** Expect the interesting failures here to be in
   SDL3 or in the ImGui SDL3/SDL_Renderer3 backends, not in `src/`.
4. **Serve it with COOP/COEP** (above) and load it.

Expected on a successful load: the app renders with the Wili theme; the device
bar shows the desktop-only notice; every Flash button is disabled with that same
sentence; the Recovery tab is fully readable; the Default Firmware cards show
their plans and the embedded bootloader version.

## Known gaps and specific risks

Ordered roughly by how likely each is to actually bite.

- **The remote catalog needs a URL typed in on every page load.** There *is* an
  in-app control for it now — the App Explorer tab's "Remote catalog" field and
  Update button, added after this document was first written (commit `81dec97`)
  and **deliberately not gated on `kDeviceSupportAvailable`**: fetching a
  catalog is the one thing a browser is unambiguously good at, and it is the
  only feature the web build has left. Typing an address and pressing Update
  (or Enter) calls `RemoteCatalog::start()` immediately, so it should work in
  the same session on the web exactly as it does on the desktop.

  What does not carry over is persistence. `settings.ini` lives in MEMFS, which
  the page discards on unload, so `settings.remoteCatalogUrl` is empty again on
  the next load and the startup fetch in `App::run()` never fires. A web user
  has to re-enter the URL every time. Reading a URL from a query-string
  parameter on the page would fix that and is the natural next step; it is
  deliberately not invented here because it could not be tested.

  Note the URL must be `https://` — `normalizeRemoteCatalogUrl()` refuses plain
  `http://` for the catalog (a catalog entry is authoritative over the target
  CPU; see its header comment in `src/core/fwSettingsIo.h`). That is not an
  Emscripten-specific constraint, but a page served over `http://` whose
  operator reaches for an `http://` catalog beside it will meet it there first.
- **`-sALLOW_MEMORY_GROWTH=1` together with `-pthread`** makes the heap a
  growable `SharedArrayBuffer`. Modern browsers support this, but emcc may warn
  about it and the growth path is slower than the non-threaded one. If the page
  is unusably slow, this pairing is the first thing to look at.
- **The shutdown path never runs.** `emscripten_set_main_loop_arg(..., /*fps=*/0,
  /*simulate_infinite_loop=*/1)` does not return; Emscripten unwinds to the
  browser event loop without running destructors. Everything after the main loop
  in `App::run()` — settings persistence, `ImGui::DestroyContext()`,
  `SDL_DestroyWindow()`, the `fwFinderManager` shutdown guard — is dead code on
  the web. This is correct rather than merely tolerated: a browser tab has no
  orderly close event to hook, `fwFinderManager` is a no-op stub there, no flash
  can ever start so there is no worker to join, and MEMFS is discarded on unload
  regardless. It does mean the `frame` lambda captures `App::run()`'s locals by
  reference and relies on Emscripten leaving that stack frame intact — which is
  the documented behaviour of `simulate_infinite_loop = 1`, and is the whole
  reason the flag is set to 1 rather than 0.
- **`-sSTACK_SIZE=4194304`** is a guess, not a measurement. emcc's 64 KB default
  is a well-known footgun for ImGui applications; 4 MB matches what a desktop
  thread gets. If the page aborts with a stack overflow, raise it.
- **`-sPTHREAD_POOL_SIZE=4`** likewise. It must be large enough that
  `RemoteCatalog`'s worker can start without blocking the main thread to spawn
  one. Four leaves headroom for `FlashController`'s worker, which can never
  actually start on the web but whose thread object still exists.
  `-sPROXY_TO_PTHREAD` was deliberately **not** used: the synchronous fetch
  already has its own thread, so proxying the whole application would be solving
  a problem this code does not have.
- **`web/shell.html` has never been parsed by a browser.** The
  `crossOriginIsolated` guard, the `Module` wiring and the canvas focus handling
  are all written from the Emscripten docs.
- **Font atlas size.** `Fonts::initialize()` bakes FiraCode plus the Material
  Icons private-use block. That is fine at desktop memory budgets; it is
  untested against a wasm heap.
- **`userDataDir()` returns `/data` in MEMFS**, so `settings.ini` is written and
  then thrown away on every page unload. Theme, last tab and window geometry do
  not persist on the web, and nothing depends on them persisting. The remote
  catalog URL is the one setting where losing it is actually felt — see the
  first gap above. `RemoteCatalog::loadCache()`'s `apps-cache.json` is discarded
  the same way, so the web build also starts each load with no cached catalog,
  unlike the desktop.
- **`copyToVolume()` refuses outright on Emscripten** rather than falling
  through to `std::filesystem`. It is unreachable — `findRpiRp2Volumes()`
  returns nothing and every Flash button is disabled before that — but MEMFS
  would otherwise happily "succeed" at writing a UF2 into the browser sandbox
  and report a flash that never happened, which is the single worst thing that
  function can do.
