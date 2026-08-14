# DECK — Declarative Escalating Color Kit
**The UI system for FreeWili 1-OG. One grammar, five colors, screens as data.**

Spine: the DECK proposal (first-ranked by 2 of 3 judges). Grafts from all four other proposals as mandated. Every judge fatalFlaw is excluded; conflicts are resolved explicitly in §11.

---

## 1. The System

DECK is a bespoke declarative UI library (`ui/fwog_ui.h`) in wiliOGbsp. Apps declare screens as `const` structs in flash; the framework owns navigation, chrome, exit, feedback, and the entire input grammar. Exactly **one** button is app-definable. The one-sentence manual for the whole device: **"Red always gets you out; the bottom of the screen tells you the rest."**

---

## 2. Definitive Input Grammar

FLAGGED ASSUMPTION (ship blocker, see §10): buttons are a single row under the screen in hardware-bitfield order GRAY | YELLOW | GREEN | BLUE | RED. Hint chips align positionally to buttons.

| Button | PRESS | LONG (600 ms) | HOLD / REPEAT | Scope |
|---|---|---|---|---|
| **GRAY** | PREV (row up / page back / decrement) | — (none; hold = repeat) | Auto-repeat: 400 ms delay → 8 Hz → 20 Hz after 2 s | GLOBAL, fixed |
| **YELLOW** | NEXT (row down / page fwd / increment) | — (none; hold = repeat) | Same accelerating repeat | GLOBAL, fixed |
| **GREEN** | SELECT / YES / commit field / dismiss toast | Screen's declared long action; must be labeled or it's inert (blip) | — | Press GLOBAL; long is declared-contextual |
| **BLUE** | **The app verb** — the screen's one declared action | Declared overflow menu, else inert | — | CONTEXTUAL — the only app-definable button; label mandatory ("the chip is the license to bind") |
| **RED** | OUT: pop one screen; at root → exit dialog | **FORBIDDEN to apps, forever** | 1.5 s = System Card (any depth, non-suppressible, countdown ring from 500 ms); ~3 s = power-off countdown banner; ~6 s = hardware power-off (untouched) | GLOBAL, fixed. Never repeats, never a nav or gameplay verb |

Rules that complete the grammar:
- **RED means "out of the current context," universally.** In a confirm dialog, RED dismisses (out of the dialog = cancel the act). In the framework's exit dialog / System Card, RED **confirms exit** (out of the app) — the panic masher escapes, never loops. This is safe because exit is never destructive: `fwog_ui_on_exit()` saves state first.
- **Escalation ladder:** holding RED only ever moves toward "more out": back < System Card < power-off. Scroll never lives on RED (fatal flaw of Compass), so routine use never touches the power ladder.
- **No silent bindings:** the framework refuses any BLUE/GREEN-long binding without a hint label. Unbound buttons render a dim chip band — silence is labeled.
- **Long-press pedagogy:** at the 600 ms threshold the chip fills solid in the button's color, shows the long label, and plays a rise chime — users learn long meanings safely by accident.
- **Destructive actions:** any verb declared `destructive` is force-routed through the Dialog component. No single-press deletes/resets, ever (excludes the wiliui factory-reset flaw).
- Repeat exists only on GRAY/YELLOW. No chords, no double-press. Games needing more axes use IMU tilt (`fwog_ui_tilt()`), proven by wilidoro — grammar exemptions are never granted.

---

## 3. Screen Anatomy (320×240 landscape, no framebuffer)

Three fixed bands. Apps touch only the content band.

**STATUS BAR — y 0–19 (320×20), framework-drawn every frame.**
App name left, scale-2 text (12×16 glyphs), max 10 chars (120 px). Screen title center, max 12 chars, ellipsized. Right 60 px: breadcrumb depth dots (one 4 px dot per nav level, max 6) + battery glyph. The permanent "where am I."

**CONTENT — y 20–215 (320×196), owned by the screen kind.**
- LIST/FORM: 8 rows × 24 px (scale-2, 25 cols); rightmost 6 px = scroll ticks; cursor row inverse-video.
- DETAIL: 8 wrapped scale-2 lines; "2/5" page indicator rendered into status-bar right.
- PROGRESS: title line + scale-4 digits (24×32) + 280×16 bar, centered.
- CANVAS: raw 320×196. `FWOG_UI_FULLSCREEN` grants 320×240, but the hint bar flashes for 1.5 s on entry and the RED countdown ring always draws over app pixels.

**HINT BAR — y 216–239 (320×24), framework-drawn.**
Five 64×24 chips in hardware order, each above its physical button. Chip = 4 px band in the button's actual color across the top + scale-1 (6×8) label, max 10 chars, centered. Auto-derived from the screen declaration — developers never write hints. Dim band = inert. RED chip always reads "Back" (or "Exit" at root).

**OVERLAYS (paint over content band; chrome never damaged).**
Dialog 280×120 centered, 2 px border, GREEN/RED chips relabel. System Card 280×180. Toast 300×28 bottom-anchored above the hint bar, 2 s, queued never stacked.

Latency contract (ships in the docs as numbers, not folklore): full-screen push ≈ 20 ms, transitions only; dirty list row ≈ 2.6 ms; value repaint ≈ 1 ms — comfortably ahead of the 8–20 Hz repeat.

---

## 4. Component Inventory (12 — frozen; wanting a 13th is the signal an app should pay the LVGL tax)

1. **StatusBar** — name/title/depth-dots/battery; apps can't touch it.
2. **HintBar** — the five auto-derived chips; the live manual.
3. **List** — scrolling menu; rocker moves (accelerating repeat), GREEN selects; scroll ticks.
4. **Detail** — paged read-only text (the orca use case); rocker pages; auto page indicator.
5. **Form** — INT/BOOL/ENUM fields pointer-bound to app variables; rocker adjusts, GREEN advances/commits; `on_commit`. The velocity multiplier — settings screens are near-zero code.
6. **Progress** — polled `get_pct`, big numeral + bar, auto-mirrored to the 7-LED strip; BLUE slot for pause/skip.
7. **Dialog** — modal confirm, GREEN=yes / RED=out; **async, keeps pumping `on_tick`** (audio never stutters); destructive verbs route here automatically.
8. **Toast** — 2 s banner + paired chime; GREEN dismisses early.
9. **Canvas** — `draw(dirty)` + `on_btn` escape hatch (GREEN, BLUE±long, rocker only — never RED); tilt available.
10. **SystemCard** — RED-hold 1.5 s overlay: full breadcrumb path, live legend of every button's current meaning, app-registered menu items, Resume, "Exit to App Explorer." RED inside = exit. The universal "where am I and how do I leave," on one non-suppressible gesture.
11. **FeedbackMap** — semantic chime + LED events (see §7), inherited not reimplemented.
12. **Watchdog** — framework tick runs below app code; if the app busy-loops >2 s, paints "App unresponsive — hold RED to power off." RED handling and chrome survive a hung app.

---

## 5. C API Sketch

```c
/* ui/fwog_ui.h — display-CPU library in wiliOGbsp. C11. */
typedef struct { const char *hint; void (*fn)(void); bool destructive; } fwog_ui_verb_t; /* BLUE */
typedef struct fwog_ui_screen fwog_ui_screen_t;  /* tagged union: LIST/DETAIL/FORM/PROGRESS/CANVAS,
                                                    built via designated-init macros */
void fwog_ui_run(const char *app_name, const fwog_ui_screen_t *screens,
                 unsigned count, unsigned root_id);          /* main loop; never returns */
void fwog_ui_push(unsigned screen_id);
void fwog_ui_pop(void);                                      /* what RED-press calls */
void fwog_ui_dirty(void);                                    /* data changed, repaint dirty rows */
void fwog_ui_toast(const char *msg);
void fwog_ui_dialog(const char *q, void (*on_yes)(void));    /* async; on_tick keeps pumping */
void fwog_ui_on_tick(void (*fn)(uint32_t now_ms));           /* app background work, 20 Hz */
void fwog_ui_on_exit(void (*save_state)(void));              /* runs before ANY exit path */
void fwog_ui_menu_add(const char *label, void (*fn)(void));  /* extra row in the System Card */
void fwog_ui_led(uint8_t idx, uint32_t rgb);                 /* override the strip */
int8_t fwog_ui_tilt_x(void); int8_t fwog_ui_tilt_y(void);    /* IMU escape valve for games */

/* ---- complete 3-screen feature ---- */
enum { SCR_MENU, SCR_BREW, SCR_SET };
static struct { int32_t cups; bool strong; } cfg = { 4, false };
static void start_brew(void)      { brew_start(cfg.cups, cfg.strong); fwog_ui_push(SCR_BREW); }
static void menu_pick(unsigned i) { i == 0 ? start_brew() : fwog_ui_push(SCR_SET); }
static void brew_done(void)       { fwog_ui_toast("Enjoy!"); fwog_ui_pop(); }
static const char *rows[] = { "Brew", "Settings" };
static const fwog_ui_field_t flds[] = {
    FWOG_FIELD_INT("Cups", &cfg.cups, 1, 12),
    FWOG_FIELD_BOOL("Strong", &cfg.strong),
};
static const fwog_ui_screen_t scr[] = {
    [SCR_MENU] = FWOG_UI_LIST("Coffee", rows, 2, menu_pick),
    [SCR_BREW] = FWOG_UI_PROGRESS("Brewing", brew_pct, brew_done,
                                  .blue = { "Cancel", brew_cancel, .destructive = true }),
    [SCR_SET]  = FWOG_UI_FORM("Settings", flds, 2, cfg_save),
};
int main(void) { fwog_ui_on_exit(cfg_save); fwog_ui_run("Coffee", scr, 3, SCR_MENU); }
/* Nav, chrome, hint chips, exit ladder, System Card, LED mirror, chimes: all inherited. */
```

---

## 6. Universal Exit / Home Story

Four concentric guarantees, all enforced in the framework's input layer **below app code** (CHROMABAR's interception, grafted) — so raw-canvas games and even misbehaving apps yield:

1. **Where am I:** status bar (name + title + depth dots) on every screen; fullscreen canvases still get the entry hint-flash and the System Card.
2. **Mash RED:** every press pops one level; at root it raises the exit dialog, where another RED press **also confirms** — a panicking masher always lands in App Explorer, with state saved by `on_exit`. No dialog trap (DECK's RED=No flaw excluded).
3. **Hold RED 1.5 s from any depth:** System Card — breadcrumb, full button legend, app menu, labeled "Exit to App Explorer." Non-suppressible, countdown ring visible from 500 ms.
4. **Keep holding:** from ~3 s a "release to cancel — power off in 3..2..1" banner; at ~6 s the hardware cutoff fires regardless of firmware state. The Watchdog banner covers the hung-app case.

The orca incident becomes structurally impossible: exit lives in library code above the app, and the way out is printed over the red button on every frame.

---

## 7. Multi-Channel Feedback Rules

- **Accepted press:** LED flash in the pressed button's color + tick chime. **Inert press:** low blip, no LED. Eyes-free confirmation, one-handed use.
- **Long threshold:** chip fill + rise chime (the pedagogy moment).
- **Semantic chimes (FeedbackMap, overridable per theme):** select tick, page-turn soft tick, error buzz, exit chirp, power-countdown beeps at 3/2/1.
- **LED strip:** Progress screens auto-mirror as a 7-step ring; List mirrors cursor position; apps override via `fwog_ui_led`.
- **Accessibility rule (launch requirement, not polish):** color is never the sole carrier — position + text on chips, glyphs in dialogs; LEDs mirror on-screen info, never carry exclusive meaning.

---

## 8. Resource Architecture — DECIDED

**Bespoke over `lcd_text` + ST7789 windowed blits. LVGL is opt-in only, never default.** All three judges converged; LVGL-as-default is a fatal flaw (143 KB of 264 KB RAM on a CPU that also runs audio, LEDs, IR, and sensors — the "display CPU's only job is UI" premise is false per the BSP tree).

**Budget (a contract, tested in CI):** framework ≤ 48 KB flash (font + chime index included), ≤ 10 KB static RAM (nav stack depth 8, hint strings, dialog/card state, LED mirror, ~1 KB glyph scratch). No framebuffer — screens are `const` flash data costing app RAM nothing. A DECK app lands near the ~32 KB bare-app class, leaving >200 KB RAM for features.

LVGL (~390 KB / 143 KB) stays in the BSP for showcase apps, with its keypad indev **patched to the DECK grammar in the same release**: gray=PREV, yellow=NEXT, green=ENTER, blue=custom, red=LV_KEY_ESC. `fw new-app` scaffolds DECK; choosing LVGL is an explicit CMake line with the RAM price in a comment. If LVGL is used, widget handles stay opaque behind the fwui layer (Compass's raw `lv_obj_t` leak excluded).

---

## 9. Migration Notes — 4 Apps, One Atomic Release

The framework, the LVGL keymap patch, and all four app migrations ship together — a fleet speaking two dialects is worse than either alone (unanimous fatal flaw).

1. **orca field notes** — migrate first (it caused the incident). Story index → List; story pages → Detail. Deletes its bespoke input code; exit comes free. ~Half a day.
2. **wilidoro** — menu → List; duration/theme → Form; running timer → Progress (LED ring now auto-mirrored); BLUE = "Pause", tilt-to-pause kept via `fwog_ui_tilt`. Themes recolor content-band only; chrome stays system-styled.
3. **ogvegas** — stays on LVGL (opt-in, pays its own RAM), rebuilt against the patched keymap so red exits. The RED-hold System Card works over it anyway via the input-layer intercept.
4. **lvgl demo** — retired. Its D-pad mapping teaches the dead dialect. Replaced by a `fw new-app`-scaffolded DECK demo exercising all five screen kinds — which doubles as the template's living documentation.

Plus one BSP suite addition (blocking, per fatal flaw): a **shared-ownership test for the RED-hold path** proving the framework only overlays countdowns and never eats, re-arms, or double-handles the 6 s power-off.

---

## 10. Open Questions — Hardware in Hand

1. **Physical button layout (SHIP BLOCKER):** confirm single row, GRAY..RED left-to-right, under the screen. Chip geometry and the rocker's spatial logic freeze only after this; a split/diamond layout forces re-derivation, not a lookup table.
2. **RED ladder hallway test:** naive users, real device — does 1.5 s System Card vs 3 s countdown vs 6 s cutoff feel legible? Do panicking users mash (safe) or hold (needs the ring to save them)?
3. **SPI throughput:** verify the 20 ms full-push / 2.6 ms row numbers at the real SPI1 clock; check flicker during 20 Hz repeat scrolling.
4. **Audio during chrome repaints:** confirm I2S DMA doesn't glitch while dialogs/toasts paint (shared bus/DMA contention on the display CPU).
5. **Chip legibility:** 6×8 scale-1 labels at arm's length under the real backlight; red/green colorblind check of chips and dialog glyphs.
6. **LED strip position:** is it physically near the buttons (per-button flash mapping meaningful) or elsewhere (fall back to whole-strip flash)?
7. **IMU orientation:** tilt axes vs case orientation for the game escape valve.
8. **Battery readability:** can the display CPU actually read battery state for the status glyph, or does it drop in v1?
9. **Fullscreen hint-flash duration:** is 1.5 s on canvas entry enough to teach, short enough not to annoy?
10. **Long-press vs debounce margins:** confirm 500 ms ring start / 600 ms long threshold sit cleanly above the debouncer's phase timings.

---

## 11. Conflict Resolutions (rule chosen, and why)

- **Winner:** DECK (judges 2 and 3 rank it first; judge 1's winner contributes grafts). Majority rules the spine.
- **Back on GRAY (CHROMABAR) vs RED (DECK/One Verb/wiliui):** RED. Three proposals and two winning verdicts converge; red=stop matches first-contact instinct (judge 1 scored red-back 10/10); GRAY is freed to make the nav rocker adjacent.
- **Legend overlay's home:** judges mandated it, but its proposed key (GRAY-long) collides with GRAY's hold-to-repeat in the DECK spine. Rule: it merges into the RED-hold System Card — one non-suppressible gesture answers "where am I," "what do the buttons do," and "how do I leave" simultaneously, and keeps long-press off the repeat keys.
- **RED in the exit dialog:** confirms exit (judge 1 fatal-flaw mandate), reconciled by the universal rule "RED = out of the current context" plus the `on_exit` save hook making exit never destructive.
- **`owns_back` canvases:** excluded entirely (judge 1: must not ship without tighter limits; the tightest limit is zero). RED is never claimable; games get GREEN, BLUE±long, rocker, tilt.
- **Contextual budget:** exactly one button (BLUE), force-labeled (judge 2 fatal flaw kills CHROMABAR's four rebindables).
- **Blocking confirms:** excluded (judges 2 and 3); Dialog is async and pumps `on_tick`.
- **Compass exclusions honored in full:** no RED=DOWN, no repeat on RED, no LONG-YELLOW-as-only-back, no LVGL-as-default, no raw LVGL handles.