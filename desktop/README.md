# Native LVGL desktop simulator

This is a C application linked against LVGL **9.5.0** (the version in released ESPHome), pinned to commit `85aa60d18b3d5e5588d7b247abf90198f07c8a63`. There is no HTML, JavaScript, browser, or webview in its render path. `atmos.c` issues LVGL draw commands during an LVGL object's draw event. LVGL rasterizes those commands into its framebuffer. The display renders RGB565, as under ESPHome; scene canvases and the transition frame are 1-bit `LV_COLOR_FORMAT_I1`. `main_win32.c` thresholds each pixel to 1 bit, as the ST7305 driver must, and maps the bits to simulator tones (default #303030 ink, #c6c6c6 paper; override with `--ink=RRGGBB --paper=RRGGBB` or `ATMOS_INK`/`ATMOS_PAPER`) and presents them with Windows GDI. Transition composition is integer fixed-point and writes the finished 1-bit frame straight into LVGL's output buffer in the display's colour format. See AGENTS.md, ESPHome integration, for the build flags the device YAML needs.

## Build and run

On Windows, install CMake, Git, and the MSYS2 UCRT64 GCC toolchain (including `mingw32-make`). The script detects the standard `C:/msys64/ucrt64/bin` location; other installations must be on PATH.

```powershell
./desktop/build.ps1 -Run
```

The first build downloads the pinned LVGL source into `desktop/vendor/lvgl`. It requires network access. Later builds are local. The script checks the dependency revision, compiles the application with warnings treated as errors, runs CTest, and optionally launches `desktop/build/atmos.exe`.

The window opens at 2Ãƒâ€” native resolution. Resizing uses integer pixel scales and letterboxing. The render target is always 400 Ãƒâ€” 300. Animation targets 40 fps; this is not measured hardware throughput.

Windows presentation composes the frame and letterboxing in an offscreen bitmap before one visible copy, avoiding a visible black clear between frames. The first LVGL frame is rendered before the window is shown. Verification also checks the actual GDI presentation pixels at 1Ãƒâ€”, 2Ãƒâ€”, 3Ãƒâ€”, and a letterboxed size.

## Controls

| Input | Action |
|---|---|
| 1Ã¢â‚¬â€œ6 | Overview, CO2, gases, formaldehyde, particles, climate |
| Arrow keys, click, mouse wheel | Navigate |
| `,` and `.` | The device's left/right keys: previous/next underlined reading, then previous/next page (`atmos_advance()`) |
| Space | Pause motion and history sampling |
| A | Toggle automatic cycling |
| F1Ã¢â‚¬â€œF5 | Quiet morning, occupied room, cooking, open windows, sensor full scale |
| S | Save native-resolution `atmos-frame.bmp` in the working directory |
| Escape | Close |

Minimizing freezes active time. Scenes hold for 6.5 seconds by default (`atmos_set_hold()`). Transitions take 1.3 to 1.6 seconds, depending on the page being left. Navigation during movement keeps the latest requested destination. History starts empty and retains two minutes by default (`atmos_set_history_window()`), sampled every span/60 seconds (at least two). All default readings are simulated. The demo status cutoffs remain illustrative. All four PM channels have independent simulated readings and independent fields for real sensor input.

Transitions are chosen by the page being left, so any page can be reached from any other: the overview zooms into its status circle, CO2 zooms into one of its visible bubbles, particles leave by dust bloom, gases by ribbons swell, climate by a droplet falling onto its humidity icon and splashing open, and formaldehyde by an oxygen atom whose ink swells over the frame and opens like an iris. Anchors (the bubble, dust specks, ribbon line, droplet icon and oxygen atom) are taken from the outgoing snapshot. Transitions use nearest-pixel sampling and opaque masks, with no alpha crossfade or tones other than ink and paper.

## Migration status

All six scenes, 1-bit output, animated status/particle field/bubbles/ribbons, scenario interpolation, shared immutable history, queued navigation, pause, automatic cycling, frame export, and external sensor input are implemented in C.

This native port is not pixel-identical to the reference. It uses shared native-size DejaVu Sans Bold fonts, a 3D particle field in place of the reference fan, simplified status/ribbon geometry, and immediate numeric changes. The browser's checkmark reveal and rolling digits/footer transitions remain fidelity work. Browser preview controls remain in the original files.

During transitions, two hidden LVGL canvases rasterize the live source/destination scenes. A platform-independent C compositor zooms and reveals those pixels, and LVGL presents the composed image. The gas ribbon artwork uses opaque LVGL spans. The browser renderer is not involved.

## Verification

`build.ps1` runs `atmos --self-test` through CTest. Model checks cover empty history, sample timing, immutable history, scenario warning behavior, pruning, queued navigation, pause/minimize time freezing, and supplied sensor readings. Transition endpoints are compared to live source/destination rasters for all 20 directed scene pairs. Render checks exercise all 20 scene/scenario combinations, forward/reverse navigation, and intermediate transitions using the actual linked LVGL renderer, checking every pixel for the exact palette and rejecting empty frames. BMPs are written in `desktop/build/`. These are smoke/invariant checks, not a pixel comparison against the browser reference or a benchmark of the physical panel.

For visual review, run `./atmos.exe --capture-transitions` from `desktop/build`, then run `python desktop/render_review.py` from the project root (requires Pillow). This creates frame sheets and GIFs in `artifacts/` for quiet and warning scenarios. Those GIFs sample the animation every 200 ms for inspection; the native application targets 40 fps.

## Later hardware port

`atmos.c` and `atmos.h` contain no Windows APIs. On hardware, initialize LVGL and a 400 Ãƒâ€” 300 display, register the panel flush callback, call `atmos_init()`, advance `atmos_step()` with elapsed active seconds, and service LVGL's tick/timer handler. Pass real readings through `atmos_set_readings()`; this disables generated demo readings until another demo scenario is selected. Values are CO2 ppm, PM2.5 ug/m3, VOC/NOx indices, temperature Celsius, relative humidity percent, and barometric pressure hPa (`pressure`, NAN without a sensor).

The current desktop setup uses a 480 KB draw buffer, a separate 480 KB presentation buffer, three 480 KB transition buffers, an 8 KB axis-label raster, and a 4 MB LVGL heap, plus Windows presentation bitmaps. Those are desktop choices, not an ESP32 memory budget. A hardware port must select an appropriate color format, memory placement and buffering strategy, verify panel support for the requested third gray and pack pixels for the panel, integrate SCD30/SEN66 drivers, and measure refresh throughput, CPU, memory, and power. [LVGL display driver documentation](https://docs.lvgl.io/9.2/porting/display.html) describes the flush boundary. No firmware build or physical panel support is claimed here.

LVGL remains under its upstream MIT license in `vendor/lvgl/LICENCE.txt`.

The redesigned overview and reusable native design tokens are documented in [UI design system](../docs/UI_DESIGN_SYSTEM.md). The overview footer now keeps PM2.5 and temperature visible; VOC and humidity remain available in their detail scenes.

CO2 detail captures: run `./atmos.exe --capture-co2` from `desktop/build` for empty, short, full and wide-range histories. Chart labels use seconds before the newest stored sample. History charts on every page have no unit or SEC AGO captions and grow into their space; a "LAST n SEC/MIN/HRS" pill (ink fill, paper outline and text) sits over the top-right corner, right-aligned to the time axis. The trace is drawn as 1-bit column spans, so steep slopes have no holes. The reading and PPM unit are centred on the CARBON DIOXIDE title's ink width, and the reading uses the largest font whose widest value (40.0k) fits that width. Bubble count follows CO2 on a log scale (2 at 400 ppm, all 12 from 3000 ppm); a bubble only appears or leaves while below the box. Chart fill is paper on the ink history panel; the panel has no grey. See [UI design system](../docs/UI_DESIGN_SYSTEM.md).


PM detail: press 5, click a measurement card, or use Tab / ] / [ to cycle the selected
PM size. All four cards stay live. atmos_readings fields pm1, pm (PM2.5), pm4 and
pm10 are independent cumulative mass concentrations; integrations must supply each
field, or NAN for unavailable channels. Use designated initializers. No PM channel
is derived from another. All detail charts use paper fill and shared axes.

Run ./atmos.exe --capture-pm from desktop/build for independent-channel history,
all four selections, graph blends, rapid reselection, large readings and missing
channels. The full geometry, motion and new sensor contract are documented in
[UI design system](../docs/UI_DESIGN_SYSTEM.md).


### Gases detail review

GASES uses shared bold typography, a selected-channel badge, equal-size VOC/NOx
readouts and two animated ribbons. One fixed-size history chart shows the
selected channel, whose label is underlined; click a readout to switch. Automatic
cycling arrives on VOC and switches to NOx halfway through the hold; it
likewise steps Particles through PM1.0 to PM10 left to right and Climate (whose
selected label is underlined too) from top to bottom, splitting the hold evenly. The graph uses the same paper
fill and axes as the other detail pages. The top ribbon follows VOC and the bottom
ribbon NOx: thickness (6 px clean, 14 px at Fair, 22 px at Poor) and mote count
settle toward the live index with a 1.5-second time constant. Current geometry and typography are
specified in `../docs/UI_DESIGN_SYSTEM.md`.

After building, from `desktop/build` run `./atmos.exe --capture-gases` for empty,
short/full history, animation, live changes, missing values and both navigation
directions under GOOD/CHECK AIR readings. Captures are desktop verification only.

Gases opens onto the next page by ribbons swell: its own ribbons, continued across the
frame at the ribbon gap, thicken from the middle lane outward until they merge.
Both scenes are still snapshots during the transition.

The full visual standardization is documented in [UI design system](../docs/UI_DESIGN_SYSTEM.md). All headers use the same title, badge and divider geometry; all charts share one renderer. The native six-state comparison is `../artifacts/redesign-after.png`.

Header badges show a rating of the page's selected measurement: Good, Fair or Poor (`--` when the reading is missing). The overview badge shows the worst rating across all measurements, and its warning symbol appears when that is Poor. The limits in `ui_theme.h` (`UI_CO2_FAIR`, `UI_PM25_POOR`, ...) are placeholder defaults; `atmos_set_bands()` replaces a reading's bands with any ranges, texts and severity levels (the device's `ratings:` YAML).
