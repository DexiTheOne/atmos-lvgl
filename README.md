# Atmos — LVGL desktop simulator and browser reference

The native implementation is now in [`desktop/`](desktop/README.md). It compiles real **LVGL 9.5.0 C code** (the version released ESPHome uses) into a Windows application; LVGL draws the entire 400 × 300 interface. Windows only presents the framebuffer and forwards controls. Run `./desktop/build.ps1 -Run` to build, verify, and launch it. The browser version below remains a design reference.

The desktop migration includes all five scenes and the shared sensor/history model. The native journey zooms through the overview status circle and a CO2 bubble, sweeps from particles into gases, and carries continuous ribbons horizontally into climate before returning through the status portal. Typography, ribbon geometry, and some animation details still differ from the browser reference. Hardware integration and hardware performance validation remain future work.

A local animated air-quality display preview. The surrounding webpage only supplies preview controls.

## Run

```powershell
node server.mjs
```

Open http://localhost:4173. No dependencies or network connection are needed. You can also open `index.html` directly.

## Display fidelity

- Native framebuffer: **400 × 300 pixels**, landscape.
- 1-bit output (ink/paper). The simulator shows them as **#303030** and **#c6c6c6** by default; set `--ink=` / `--paper=` to change.
- Every output pixel is one of those two colors. Shapes use a hard monochrome threshold, and text uses a hard pixel mask. There is no dithering, fine stipple, intermediate pixel shading, or temporal gray flashing.
- Magnification is restricted to **1×, 2×, or 3×**. Each logical pixel becomes a square block. There is no fractional responsive scaling, canvas border radius, or CSS filter.
- Narrow viewports scroll the preview instead of shrinking the pixels. Use 100% browser zoom for the cleanest pixel view; physical monitor density affects apparent size.
- Animation targets **40 fps**. The live browser counter reports measured rendering cadence. This is not a guarantee of hardware performance or a measurement of panel scanout.
- The two simulated gray levels approximate the panel's appearance; real reflectance and contrast depend on ambient light and viewing angle.

## Controls and scenes

The journey starts directly on the overview and its animated status symbol. A 1.4-second zoom through the active checkmark or warning triangle reveals the actual detail screen. CO₂ → particles → gases → climate move as adjoining pages in 1.1-second vertical scrolls. Climate returns through the inverse portal. Each settled scene holds for 6.5 seconds. There are no title cards or intermediate animation screens. Navigation during movement queues only the latest requested destination.

All detail screens reserve the lower 120 pixels for animated history, with older samples on the left and NOW on the right. The shared window holds two minutes of samples collected every two active seconds. It starts empty, reports the available duration while filling, and never invents past measurements. Scenario changes continue the same history. Pause and hidden tabs freeze both motion and history time.

- CO₂ history is rising carbonation: bubble density encodes concentration. Larger live bubbles beside the reading use the same current measurement.
- PM2.5 history is an airflow chamber: historical particle density encodes concentration, while a constant-speed fan and flow paths illustrate air movement. The fan does not imply a measured fan-speed signal. PM1/PM4/PM10 remain visible above the history and are derived demo values.
- VOC and NOx have separate sculpted ribbon lanes. Historical values control band width; projected folds, depth ordering, and reflective seams create the animated surface.
- Temperature and humidity form separate woven bands. Temperature controls fold height; humidity controls band thickness. Transverse seams give the bands a fabric appearance.

History samples are immutable. Only arrival is eased over 0.6 seconds; motion phase continues across sensor updates. Historical ribbons interpolate between recorded values, and their recent edge holds the latest sample between updates. Density is quantized for legibility. Motion is illustrative; the large numbers remain the precise readings.

Fixed artwork scales (not health thresholds): CO₂ 400–2,000 ppm; PM2.5 0–75 µg/m³; VOC 0–500 index; NOx 0–50 index; temperature 15–35 °C; humidity 0–100%. Values outside these ranges saturate the artwork without clamping the displayed numbers.

Quiet morning and open-window demo scenarios settle to an animated check and GOOD. Occupied-room and cooking scenarios settle to CHECK AIR with a large pulsing triangle. The illustrative cue follows the simulated readings (demo cutoffs: CO₂ over 1,000 ppm or PM2.5 over 20 µg/m³), so it stays consistent while scenarios interpolate. These are design-demo cutoffs, not a regulatory AQI or a firmware alarm recommendation. No third color or alpha fading is used: status motion uses drawing, clipping, scale, and stroke changes.

Pause with Space, scroll over or click the display, navigate with arrow keys or scene buttons, select a room scenario, or disable auto-cycle. Readings update every two active seconds; geometry animates continuously around those measurements. Save frame creates a native-resolution 400 × 300 PNG through the browser's download handler.

All readings are simulated; history records those simulated readings during the current session. Particle trajectories and gas traces are illustrative artwork, not individual measured particles or sampled gas waveforms. Descriptions are scenario names, not a regulatory AQI.

SCD30 is the primary CO₂ source. SEN66 provides PM1/PM2.5/PM4/PM10, VOC and NOx indices, temperature and humidity. Its additional CO₂ signal is not shown in this display concept. Calibration and actual sensor agreement are not modeled. VOC and NOx are relative indices, not ppb concentrations.

## Firmware boundary

The original browser version is a design reference. The `desktop/` implementation now uses real LVGL, but neither version is ESPHome or device firmware. A hardware implementation needs display driver integration and checks of panel refresh mode, SPI throughput, CPU load, memory, and power. The native two-color output could be packed into a 15,000-byte monochrome framebuffer; desktop memory allocations must be revised for hardware.

References: [Waveshare panel specifications](https://www.waveshare.com/product/esp32-s3-rlcd-4.2.htm), [Waveshare ESPHome example](https://docs.waveshare.com/ESP32-ESPHome-Tutorials/Example-RLCD-Voice), [Sensirion SEN66 outputs](https://sensirion.com/products/catalog/SEN66).

## Renderer verification

`tests/render-check.cjs` exercises the actual canvas renderer using an externally installed `@napi-rs/canvas` (set `ATMOS_CANVAS_MODULE` to its module path). The application itself remains dependency-free. Run `node tests/render-check.cjs` from this directory. Checks cover empty history, shared values, immutable samples, two-minute pruning, visualization bounds, queued navigation, pause/hidden time freezing, forward/reverse portal and scroll endpoints, and two-tone frame output. It writes `artifacts/journey-review.png`. Set `ATMOS_CAPTURE=1` to export a full 40-second cycle as native-size PNG frames for animation review. Canvas render timings are not browser FPS or hardware performance measurements.
