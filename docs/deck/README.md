# DECK — Declarative Escalating Color Kit

The UI design system for the FreeWili 1-OG's on-device interface: five color
buttons + the 320x240 LCD, one fixed grammar, screens as data. The goal it
serves: crank out features, not UI.

- **`deck-ui-spec.md`** — the specification: input grammar, screen anatomy,
  component inventory, C API sketch, exit guarantees, feedback rules, the
  LVGL-vs-bespoke decision, migration plan for the existing apps, and the
  open questions that need hardware in hand.
- **`deliberation-record.json`** — how it was decided: five competing design
  proposals (soft-key rail, spatial navigation, fixed grammar, declarative
  screen-graph, resource pragmatist) and three judges' verdicts (first-contact,
  feature-velocity, embedded-reality lenses), whose mandated grafts and fatal
  flaws the spec honors.

This documents lives here for safekeeping; DECK's implementation home is the
board support package (wiliOGbsp, as `ui/fwog_ui.h` on the display CPU), where
the spec's §9 migration plan applies.

The one-sentence manual for the whole device:
**"Red always gets you out; the bottom of the screen tells you the rest."**
