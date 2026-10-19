# Native visual consistency audit

This audit applies to the 400 x 300 landscape LVGL product surface. The 300 x
400 wording in the supplied brief conflicts with the native display contract;
the implementation and validation therefore use the actual 400 x 300 raster.

## Findings and corrections

The pre-audit screens used four history layouts: panel tops at y=163 and y=210,
plot left edges at x=48, 64, 74 and 76, different tick/caption baselines, a light
CO2 graph, solid gray on CO2/particles, and patterned pseudo-gray on gases and
climate. PM cards were 65 px tall while other selection/status components used
unrelated geometry. CO2 used a 40 px title while other page titles used 28 px.
Bubbles were clipped at a rectangular boundary instead of completing a designed
exit. Equivalent headers were drawn separately.

The correction centralizes these choices in `desktop/ui_theme.h`, reuses one
detail-header renderer, one history-panel renderer, and one full-size history
chart geometry on every detail page. Multi-measurement pages select which real
series occupies that chart instead of changing the graph layout.

## Final specification

- Raster: 400 x 300 landscape; integer coordinates; 18 px outer margin.
- Palette: ink `#303030`, paper `#c6c6c6`, support fill `#999999` only.
- Type: DejaVu Sans Bold one-bit LVGL fonts. Page title 28 px; badge/status 16
  px; every detail-page primary readout uses the same 40 px role, with a named
  32 px overflow fallback. Supporting labels use 16/12 px roles. Width tests
  choose only named size variants.
- Detail header: x=18, y=6, 28 px. Every top-right badge occupies the exact
  rectangle x=290..381, y=6..34 (92 x 29), with centered 16 px text.
- History frame: full-width solid ink panel, y=163 through 299. Plot x=74
  through 382, overall y=181 through 252. X ticks start y=260 and `SEC AGO`
  starts y=282. Every page uses the full 308 x 71 px plot. Gases and climate
  retain both live values; clicking either value selects its history series.
- Graph treatment: paper axes, labels and 2 px series line; solid `#999999`
  area to the baseline. No alpha, dither, stipple, gradient or point markers.
- Particles: four equal 88 px cards at x=18/110/202/294, y=98, height 59,
  gap 4. Selection inversion is internal and does not change outer geometry.
- Particle artwork: fan center (204,64), radius 26; three directional flow
  lanes are centered at y=43/65/87 so neither fan nor streams meet the cards.
- Motion: 40 fps desktop target; sensors sample every two active seconds.
  Animation uses active time, not sample arrivals. PM density and gas width use
  bounded smoothed mappings. CO2 bubbles shrink to fit their top/bottom exit
  clearance and are never cut by the rectangular animation bounds.

## Page results

- Overview: retained the status flower and inverted two-column footer; numeric
  anchors and tabular advances remain stable across supplied value ranges.
- CO2: uses the canonical header/badge and dark history frame. Bubble recycling
  now has geometric shrink exits. The graph uses the shared frame and fill.
- Particles: the primary readout and cards were compacted above y=163 so its
  history frame aligns exactly with every other detail page. Fan, directional
  streams and data-bounded particle density are retained.
- Gases: switched from patterned fill to solid support gray; VOC/NOx selection
  now feeds the same full-size graph used elsewhere; retained data-linked ribbons.
- Climate: switched from patterned fill to solid support gray; temperature/RH
  selection now feeds the same full-size graph; retained thermometer/drop motion.

## Validation

`./desktop/build.ps1` compiles with warnings as errors and runs CTest. The native
self-test covers history/model behavior, all 20 directed transition endpoints,
all scenes and scenarios, exact three-color output, and GDI presentation at
integer scales plus letterboxed sizes. `desktop/render_review.py` rejects any
capture that is not 400 x 300 or contains a color outside the palette; it also
asserts every detail history panel begins at y=163, every plot has boundaries
(74,181)-(382,252), and every badge has boundaries (290,6)-(381,34). Compile-time
assertions also bind all primary font roles and graph coordinates. It emits current native and 2x page comparisons, transition sheets,
GIFs, and representative bubble, airflow, gas and climate motion sheets.

Software rendering, layout, palette, navigation and captured motion were
verified. Physical reflective-panel ghosting, partial-refresh behavior, cadence,
power, memory pressure and grayscale response remain unverified on hardware.
