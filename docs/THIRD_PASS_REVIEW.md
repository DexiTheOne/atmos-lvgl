# Third-pass visual review

## Audit findings and corrections

The baseline contact sheet is saved as `artifacts/before-third-pass.png`.
All five pages were compared before editing. The initial code already shared
some fonts, but retained independent size/fallback and axis drawing paths.

| Finding | Correction |
|---|---|
| History starts at x=74; labels consume a disproportionate gutter | Shared plot starts at x=52, gaining 22 px of data width |
| Chart units crowd the top Y tick | Units moved above the plot at x=52 |
| PM tick spacing/vertical alignment differ by a pixel from other charts | One `chart_tick` and `chart_axes` implementation |
| CO2 artwork occupies only 98 px at far right | Bubble region widened to 172 px; 16 px carbon-dioxide label balances the readout |
| Gas columns force three-digit values to shrink | 90 px readout regions accommodate 40 px digits |
| Compact values jump to another baseline | Shared readout helper anchors the baseline |
| Overview labels are 24 px while detail labels are 16 px | Common 16 px label/unit role |
| Particle unit overlaps the primary-number region | Unit moved to y=83, cards to y=104 |
| CHECK AIR reaches the badge edges | All badges widened to 116 px, with centered 16 px text |
| Design documentation describes obsolete fonts and patterned graphs | Replaced with current shared specification |

## Software verification

- Warning-as-error native build and CTest passed after the final badge change.
- Model/history checks, all 20 directed transition endpoints, approved three-color
  output, and integer-scale/letterboxed GDI presentation checks passed.
- Fresh captures generated for all scenarios, detail selections, overflow and
  missing readings, forward travel and reverse gas/climate travel.
- Final native pages and sampled motion sheets visually inspected. No readout
  overlaps observed in these cases. Bubble arcs remain inside the wider region;
  the existing shrink-before-recycle behavior remains in place.
- Render review checks exact native dimensions, shared plot corners, palette and
  badge boundaries. Native and nearest-neighbor 2x sheets are exported.

## Deliverables

See `artifacts/final-page-0.png` through `final-page-4.png` for individual native
screens. `final-pages-native.png` and `final-pages-2x.png` compare all pages.
`co2-bubble-frames.png`, `particle-airflow-frames.png`, `gas-ribbon-frames.png`,
`climate-animation-frames.png`, and `lvgl-transitions-0.png` /
`lvgl-transitions-2.png` show representative motion and GOOD/CHECK AIR travel.
`secondary-history-selections.png` compares the gas and climate selections.

## Scope and unverified criteria

This cleanup keeps the existing 400 x 300 landscape canvas, dark #303030 ink,
and artwork-driven transitions. It does not implement the attachment's portrait
orientation, pure-black background, or a stationary graph during portal travel.
The overview hero and compact particle header are documented content variants.
PM's data blending remains separate from the generic history interpolation;
axes, typography, dimensions, background and solid fill are shared.

Physical-panel grayscale support, refresh cadence, power, memory and readability
were not tested. Sampled motion frames do not prove flicker-free hardware output.
No exhaustive guarantee is made for arbitrary out-of-range sensor values.
