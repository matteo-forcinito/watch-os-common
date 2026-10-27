# Agent Deck artwork

Native LVGL critter.c, critter.h and hats.c are reused from
https://github.com/brushknight/agents-deck/tree/e52251be3cb50f64dfc8f59a186d6d74c8da09e1/firmware/devices/t4s3-panel/src

Original file headers are retained. Local adaptation replaces the two fixed fonts
with Watch OS configured fonts. Integration and mapping to monitor states are local.
Reuse was explicitly authorized by the project owner in this session.

## Full panel renderer and web sprites

`upstream/` vendors the transitive pure C++ headers for SquarePanel, BigPanel
and SpringPanel from the same upstream revision, `firmware/panel-core/src`.
`PROVENANCE.json` records original SHA-256 hashes and local adaptations.
The Roboto Mono OFL notice is retained. `panel.cpp` is the Watch OS LVGL
adapter: no Arduino drivers or Deck Link transport are imported.

Local changes free lazy Spring caches when the app closes and render missing
usage metrics as unavailable. Known context percentages do not invent a token
window. Round/square canvases are scaled to actual board dimensions; this
requires device-specific visual and touch validation. Scope remains View.

The web dashboard imports the original SVG sprite/hat/animation section from
`backend/internal/web/static/app.js` and its corresponding CSS sprite section.
It maps local status values and assigns hats deterministically from project.
The source markers are retained in the generated pages and Aetherion assets.

Round first-open watchdog correction (27 October 2026): short arcs now restrict
rasterization to conservative sector bounds while retaining upstream angle and
coverage tests. A saved baseline rasterizer is used only in host regression tests;
48 cases cover clipping, wrap-around, large/full arcs and thin notice dashes.
The empty Connecting screen is exercised explicitly. No watchdog extension.

Landscape adaptation (27 October 2026): Watch OS uses a separate landscape.h
specification with the upstream SquarePanel renderer, fonts, sprites and touch
handling. Wide rectangular panels reflow tiles into three columns at >=420px.
No upstream renderer file is modified for this adaptation. Host previews cover
list, card, touch/back and logical-to-native quarter-turn geometry.
