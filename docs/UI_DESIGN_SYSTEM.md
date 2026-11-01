# Native UI design system

The product renderer is `desktop/atmos.c`; reusable geometry and type roles live
in `desktop/ui_theme.h`. The browser files remain the original design reference.

## Review and design decisions

The before sheet (`artifacts/redesign-before.png`) exposed missing detail header
dividers, undersized PM values, a third chart color, cramped main regions and
excessive chart side margins. The redesign uses one header and history renderer,
with fixed font roles, a larger main region, and wider, shorter instrument charts.
The two overview states are one scene with identical geometry.

## Shared geometry (400 x 300)

| Element | Specification |
|---|---|
| Palette | Ink #303030, paper/trace #c6c6c6, chart fill #999999 |
| Typeface | DejaVu Sans Bold, one-bit glyphs and tabular digits |
| Margin | 12 px |
| Header | 24 px title; badge x=260, y=10, 128 x 29; all badges 16 px |
| Divider | y=48, x=12..387, 1 px |
| Overview | Label y=60, fixed 80 px value at y=80, unit y=175 |
| CO2 detail | 72 px value and 24 px PPM, both centred on the CARBON DIOXIDE title ink (x=12..247); bubbles x=260..387 (the badge width) |
| PM hero | 60 px value and 16 px unit centred on the PM1/PM2.5 card gap (x=104.5); widest value "999.9" spans x=13..196 |
| Gases | Fixed 40 px values; label/value/unit y=84/109/152 |
| Climate | Three inline rows (temperature, humidity, pressure): 16 px label, 48 px value, 16 px unit on one baseline; 41 px icons centred on each value |
| Footer | y=205; equal columns; 40 px values, common baseline 267; units y=275 |
| PM cards | 91 x 59, gap 4, 4 px corner radius (row spans, `pill()`); 24 px values, 12 px labels/units |
| History panel | x=0, y=205, 400 x 95 |
| Plot | x=51..384, y=214..277 (every page) |
| Axes | 12 px; three Y ticks, five X ticks; Y label ink from x=12; X label ink ends y=293; no unit or SEC AGO captions |
| Chart fill/trace | Paper fill and 2 px paper trace on the ink panel, trace drawn as 1 px column spans |
| Time pill | "LAST n SEC/MIN/HRS", x=299..384, y=214..230; ink fill, 1 px paper outline, 5 px corner radius, drawn over the chart |

The header centers visible glyph bounds; the same title size fits even CARBON
DIOXIDE. Temperature uses the same badge font and geometry as every other badge.
Text coordinates otherwise specify font line boxes. Labels do not change size
as values update. Wide readings first omit fractional digits, then use a `k`
suffix for thousands, or `OVR` if the fixed slot cannot represent them. Missing
readings display `--`; negative PM values also display `--`. Historical data
retains full precision regardless of display formatting.

## Components and behavior

`detail_header`, `measurement_text`, `detail_value`, `footer_measurement`, and
`history_graph` share the visual roles. All detail charts use `history_graph` for
axes, sample interpolation, gaps, fill and trace. Dataset range calculation is
separate from geometry. The PM wrapper supplies its existing animated channel
weights and fixed snapshot during selection. Gas and climate selections change
the dataset and scale in place. The selected name appears in the header and the unit in the readout; the
chart itself has no unit or time captions (see the 2026-10-31 section).

History remains empty at startup, samples every two active seconds and retains
two minutes. X labels show seconds before the newest stored sample, covering
the available history. No reference-image data is generated. Missing samples
break the trace. CO2/VOC/NOx/PM are nonnegative for chart purposes; temperature
supports negative values. Y ranges ignore invalid samples and use three evenly
spaced tick positions. Fractional ticks are printed to one decimal place.

Status petals, rising bubbles, fan streams, gas ribbons and climate icons remain
animated from active time. Gas artwork uses two broad tapered ribbons and a thin central streamline,
all following one coherent S-shaped curve. Its 17 px bend changes gently with
active time; smoothed VOC (top) and NOx (bottom) levels, scaled to the rating cutoffs
(half at Fair, full at Poor), set each broad ribbon's thickness and mote count.
Sparse motes shrink at the flow endpoints rather than abruptly wrapping. Portal anchors, forward/reverse navigation, latest-destination
queueing, pause/minimize behavior, sensor interfaces and history sampling are
preserved. There is no opacity blending or dithering. At the user's explicit request,
charts now use a third solid gray (#999999), with the light paper color reserved
for the trace and axes. Windows preserves this fill and quantizes remaining
pixels to ink/paper; the verifier rejects colors outside this three-color palette.

## Reproduce visual verification

Run `./desktop/build.ps1` from the project root. Then run the executable from
`desktop/build` with each of `--capture-dashboard`, `--capture-co2`,
`--capture-pm`, `--capture-gases`, `--capture-climate`, `--capture-stress`, and `--capture-transitions`.
Run `C:/msys64/ucrt64/bin/python.exe desktop/render_review.py` from the root.
The script validates palette, dimensions, canonical plot boundaries and header
badge geometry, then writes native six-state `artifacts/redesign-after.png`,
selection sheets, artwork motion sheets and transition GIFs.

CTest verifies model/history invariants, all directed transition endpoints,
rendered palettes and GDI presentation. Captures cover live values, missing
channels, wide readings, rapid PM reselection and both navigation directions.
Desktop checks do not establish physical-panel performance or memory suitability.

## Icon and alignment refinement

Climate is three rows (temperature, humidity, pressure) split by full-width
rules, with equal space above and below each value's ink between the header
rule, the row rules and the history panel. Each row reads label, value, unit,
divider, icon; label and unit sit on the value's baseline so the value can use
the 48 px font, the largest whose four-digit pressure fits beside the
"Temperature" label (`climate_layout()`). Labels start at the left margin; each
value is centred on the display (x=200) with its unit 6 px after it. Right of
the widest value and unit, the space is split into equal gaps around the
divider and each icon is centred between the divider and the right margin. Icons are drawn from spans with a 2 px outline: a 41 px
thermometer (11 px stem, 21 px bulb) filled to the temperature, a 23 x 41 px
teardrop (round base, straight sides tangent to it) whose surface waves at the humidity, and a 41 px 270-degree gauge
(960–1060 hPa). Ticks and the tapered needle fill pixels whose centres lie within
the segment (`segment()`), so they stay smooth at any angle; the needle trembles
slightly. The row rules cross the icon column, so `climate_icons()`
draws them for partial redraws. Clicking a row selects its history chart.
Pressure has no warning band: its badge reads Good whenever a reading exists. Gas readouts, labels and units are centered in equal 90 px slots
at x=12 and x=128; the separator is exactly midway between their centers, x=115.
CO2 detail relies on its header badge and no longer repeats a CO2 label above
the number. Graph X labels center their visible glyph bounds on ticks; Y labels
align their visible right edge and vertical center, independent of digit count.
Fills render before traces and axes so the gray area cannot cover their edges.


The CO2 detail readout uses a dedicated 96 px font to occupy the space freed by
removing its repeated label. The bubbles start at x=270, reserving the wider
number slot while retaining their motion and portal anchoring.

CO2 value and PPM form one vertically centered block between y=49 and y=204.
Layout uses canonical visible digit bounds and the PPM glyph bounds, with a
10 px visible gap; changing readings cannot shift the block vertically.

CO2 detail retains its vertically centered value/unit block and 10 px visible
gap, with both elements centered on x=136. Overview CO2 and particle readouts
are centered in their allocated numeric regions. All four PM cards center their
labels, values and units. Gas, climate and overview footer groups remain centered.

The shared `centered_text` helper centers tabular advance widths for stable
numeric alignment. Gas/climate label/value/unit anchors remain y=84/109/152,
with separators spanning y=78..176. Climate icons and gas ribbons share the vertical center of the visible value/unit
block, calculated from canonical font ink bounds; labels occupy a separate row. Header geometry, charts, history and selection remain intact.

Artwork alignment uses `detail_art_center()`, derived from the 40 px numeric
glyph top and 16 px unit glyph bottom. This aligns thermometer, droplet and
gas ribbon centers to the readouts rather than to the label-inclusive group.
The axis stays fixed when the sensor digits or selected measurement change.

## 2026-10-31: CO2 page finish and shared history graph

Earlier sections record the design as it evolved; where they disagree, this
section and the geometry table are current.

**CO2 readout.** The reading and PPM are centred on the ink width of the
CARBON DIOXIDE title (`co2_title_width()`), so neither moves as the reading
changes. The value font is the largest of 80/72/64/40 px whose widest reading
("40.0k", or four digits) fits that width (`co2_value_size()`); with DejaVu
Sans Bold that is 72 px.

**Bubbles.** The bubble box spans the rating badge (`UI_BUBBLE_LEFT` is
`UI_BADGE_X`, x=260..387). Bubble count follows CO2 on a log scale: 2
(`UI_BUBBLE_MIN`) at 400 ppm (`UI_CO2_BUBBLE_BASE`) up to all 12 slots at
3000 ppm (`UI_CO2_BUBBLE_FULL`). Slots join in a fixed order that keeps the
column evenly spaced, and a slot's visibility is decided once per rise while
it is below the box, so a changing reading never pops a bubble in or out
mid-flight. The portal anchor is chosen only among visible bubbles.

**History graph (every page).** The rotated unit and SEC AGO captions are
gone; the plot grows left until the widest (four-digit) Y label starts at the
12 px margin, and down into the old caption row (`chart_geometry()`). A
"LAST n UNIT" pill sits over the plot's top-right corner, right-aligned to the
end of the time axis. It shows seconds below 90 s, minutes below 90 min, then
hours. The pill is solid, so it covers the chart rather than inverting. Its
size comes from the text's rendered ink (`rendered_ink_x()`), because the
font metrics are a pixel off from the drawn glyphs; the ink-to-outline gap is
5 px on both sides and 3 px above and below the capitals. Rounded shapes are
built from row spans (`pill()`): LVGL's anti-aliased rounded rectangles lose
their curve when thresholded to 1 bit.

**Trace.** The trace is drawn as 1 px column spans from the previous point to
the current one, 2 px thick, instead of LVGL lines. Thresholded anti-aliased
lines left ink holes where steep slopes met the fill. The spans are also
direct 1-bit fills, which avoids one LVGL draw task per column on the device.

**Particles readout.** The reading and its unit are centred on the gap between
the PM1 and PM2.5 cards (`pm_value_center()`). The value font is the largest
whose widest reading ("999.9") stays inside the 12 px margin, the header
rule's left edge (`pm_value_size()`). Only 186 px fit there; 64 px needs 197,
so a 60 px font (185 px) was added to `generate_ui_fonts.py`. The DejaVu Sans
Bold source bundled with MSYS2's matplotlib
(`C:/msys64/ucrt64/lib/python3.12/site-packages/matplotlib/mpl-data/fonts/ttf/DejaVuSans-Bold.ttf`)
reproduces the checked-in fonts byte for byte.
The particle field spans the PM4 and PM10 cards (`UI_PM_FIELD_LEFT`, x=202,
to the right margin, x=387), clear of the readout; the floor and dust scale
with it. The field is 186 px wide, so its centre is x=294.5: floor dots round
toward the centre, keeping every row's inset from both card edges equal
(the bottom row is 1 px in on each side).
Dust sizes are always a mix: each size class is weighted by its own channel's
reading (PM1, PM2.5, PM4, PM10), and the overall level sets only how many
dots show. Weighting by band differences (PM2.5-PM1, ...) put every dot in
the smallest class when the channels read alike, as at full scale.
