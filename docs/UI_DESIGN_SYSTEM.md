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
| CO2 detail | Fixed 96 px value in a 248 px slot; 24 px PPM; combined visible block vertically centered between header and graph |
| PM hero | Fixed 72 px value at y=48, unit y=119 (dense card-layout exception) |
| Gas/climate | Fixed 40 px values; label/value/unit y=84/109/152 |
| Footer | y=205; equal columns; 40 px values, common baseline 267; units y=275 |
| PM cards | y=141, 91 x 58, gap 4; 24 px values, 12 px labels/units |
| History panel | x=0, y=205, 400 x 95 |
| Plot | x=60..382, y=217..267 |
| Axes | 12 px; three Y ticks, five X ticks; X labels y=275, caption y=286 |
| Chart fill/trace | Solid #999999 fill, 2 px #c6c6c6 trace on ink |

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
the dataset and scale in place. The selected name appears in the header; units
appear beside the Y axis, avoiding duplicate vertical metric labels.

History remains empty at startup, samples every two active seconds and retains
two minutes. X labels show seconds before the newest stored sample, covering
the available history. No reference-image data is generated. Missing samples
break the trace. CO2/VOC/NOx/PM are nonnegative for chart purposes; temperature
supports negative values. Y ranges ignore invalid samples and use three evenly
spaced tick positions. Fractional ticks are printed to one decimal place.

Status petals, rising bubbles, fan streams, gas ribbons and climate icons remain
animated from active time. Gas artwork uses two broad tapered ribbons and a thin central streamline,
all following one coherent S-shaped curve. Its 17 px bend changes gently with
active time; smoothed VOC/NOx intensity controls the broad ribbons' thickness.
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

Climate artwork has a fixed 80 px height centered on the visible value/unit block. The thermometer uses a 20 px stem and
36 px bulb with a three-pixel outline; the droplet has 42 px-wide shoulders.
The thermometer has no animated surrounding dots. Humidity retains its gentle
surface motion. Gas readouts, labels and units are centered in equal 90 px slots
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
