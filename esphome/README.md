# ESPHome build and on-device frame-pacing benchmark

`components/atmos` is an ESPHome external component that runs the same UI code as the desktop simulator (`desktop/atmos.c`, copied into the build at code generation). `atmos-bench.yaml` runs it on a bare ESP32-S3 with ESPHome's LVGL and a local copy of the [ST7305 RLCD driver](https://github.com/kylehase/ESPHome-ST7305-RLCD) (Apache-2.0, `components/st7305_rlcd`; changes listed at the top of `st7305_rlcd.cpp`). Frames take the real path even with no panel attached:

1. **Render:** LVGL draws RGB565 into a full-screen buffer in PSRAM. During a transition, `atmos.c` also renders both scenes into 1-bit canvases and composites them.
2. **Convert:** ESPHome's LVGL flush hands the RGB565 area to the driver's bulk `draw_pixels_at()`, which thresholds each pixel by luminance (above half is white) and writes the panel's 2Ã—4-pixel block layout directly.
3. **Push:** the driver writes the 15 KB frame over SPI (10 MHz by default).

Run from the project root (see AGENTS.md for the Windows path workaround it applies):

```powershell
./esphome/bench.ps1 -Port COM12 -Seconds 120
```

## Keys

The panel has no touch. Its two keys are `binary_sensor: gpio` entries in `atmos-rlcd.yaml` (left GPIO18, right GPIO0, active low with pull-ups, 20 ms debounce) whose `on_press` runs the component's actions `atmos.previous` and `atmos.next`. Each press steps the underlined reading within the page (Gases VOC/NOx, Particles PM1.0â€“PM10, Climate rows) and moves to the neighbouring page past the last or first one (`atmos_advance()`), and restarts the page's hold. Presses during a page transition are applied, in order, to the page it arrives at. GPIO0 is a strapping pin: holding the right key at reset enters the bootloader. In the simulator, `,` and `.` do the same.

## Using the component

`components/` sits at the repository root so a device configuration can use `source: github://DexiTheOne/atmos-lvgl@main` with `components: [atmos, st7305_rlcd]` (see `../atmos-device.yaml`). ESPHome clones the whole repository, and the component copies the UI sources from `desktop/` at code generation. The YAML files here use the local checkout (`path: ../components`).

`frame_stats` (default `false`) turns on the debug log: every `report_interval` the component logs fps, frame intervals, late frames, render/convert/push times, CPU share, heap and the `PROFILE` lines, plus one line per transition. `bench.ps1` needs it, so `atmos-bench.yaml` and `atmos-rlcd.yaml` set it to `true`.

`key_interval` (default `250ms`, at most 5 s) rate-limits `atmos.next`/`atmos.previous`: a press within that time of the last accepted one is ignored, so contact bounce or rapid repeats cannot pile up page changes. Keep the binary sensors' `delayed_on`/`delayed_off` debounce filters as well.

## Home Assistant controls

`atmos-rlcd.yaml` joins Wi-Fi and the Home Assistant API (credentials in a gitignored `secrets.yaml` beside it: `wifi_ssid`, `wifi_password`, `api_encryption_key`, `ota_password`). Each `atmos:` entity below is optional:

- `reading_select` (select): CO2, VOC, NOx, Formaldehyde, PM1.0, PM2.5, PM4.0, PM10, Temperature, Humidity, Pressure. Choosing one moves to its page, underlines/charts it and restarts the hold (`atmos_select_reading()`). The select follows the display as keys and cycling move the selection; on the overview it keeps its last value.
- `auto_advance` (switch, restored after reboot): automatic cycling.
- `hold_time` (number, seconds, restored): hold per page before cycling moves on, split between the page's readings. The UI accepts 2–600 s; `min_value`/`max_value` narrow it (the YAML caps it at 120 s).
- `history_time` (number, minutes, restored): span the charts show, 1–1440 min, capped by `max_value` (240 min in the YAML). Samples start every 2 s and stretch with the history held (about 30 across it) up to span/60, so even a long span shows data within seconds; the 64-sample buffer thins evenly to cover the span. Shrinking it discards samples older than the new span at once; growing it keeps what is held and the chart fills until it reaches the new span (if the buffer fills first, the sample with the closest neighbours is dropped).
- `sensors`: the data source per reading, by the id of any ESPHome sensor (any platform, a `homeassistant` sensor, a template). Same keys as `ratings`. Each frame, readings that changed are passed to `atmos_set_readings()` together. Linking any sensor stops the demo data; unlinked readings and sensors that have not reported show `--` and leave the chart blank. Units: CO2 ppm, VOC/NOx index, formaldehyde ppb, PM ug/m3, temperature °C, humidity %, pressure hPa; convert with sensor filters.
- `ratings`: per reading (`co2`, `voc`, `nox`, `formaldehyde`, `pm1`, `pm2_5`, `pm4`, `pm10`, `temperature`, `humidity`, `pressure`), up to 8 bands `{min, max, text, level}`. The first band whose `[min, max)` holds the reading sets the badge text (up to 15 characters; long texts drop to the 12 px font); omit `min` or `max` for an open end. `level` (default: the band's position) orders severity: the overview shows the worst band's text and raises its warning from level 2. Readings not listed keep the built-in Good/Fair/Poor from `ui_theme.h`; a reading no band matches shows `--`.

The UI model is stepped every 25 ms (the 40 fps target). A frame is **late** when it arrives more than 37.5 ms (1.5 periods) after the previous one, meaning at least one 40 fps slot was dropped.

## Results: 2026-10-18, stock driver

Seeed XIAO ESP32-S3 (ESP32-S3R8: 240 MHz, 8 MB octal PSRAM), ESPHome 2026.9.1, ESP-IDF 5.5.5, LVGL 9.5.0, no Wi-Fi.

Build: static RAM 111.6 KB of 341.8 KB (32.7%); flash 670 KB (17% of the app partition). At runtime: about 324 KB internal heap free, about 7.4 MB PSRAM free.

| Frames | fps | Frame interval | Render | Convert (driver) | SPI push |
|---|---|---|---|---|---|
| Overview, still | 2.0 | 474â€“519 ms | 288â€“301 ms | 194 ms | 13 ms |
| CO2, still | 3.1 | 319â€“332 ms | 110â€“112 ms | 195 ms | 13 ms |
| Particles, still | 2.3 | 442â€“454 ms | 232â€“235 ms | 195 ms | 13 ms |
| Gases, still | 2.4â€“2.5 | 408â€“435 ms | 198â€“215 ms | 195 ms | 13 ms |
| Overview â†’ CO2 (portal) | 1.1 | 872â€“895 ms | 660â€“685 ms | 195 ms | 13 ms |
| CO2 â†’ Particles (bubble portal) | 0.8 | 1265â€“1290 ms | 1048â€“1079 ms | 195 ms | 13 ms |
| Particles â†’ Gases (airflow sweep) | 0.5 | 1956â€“2040 ms | 1747â€“1817 ms | 195 ms | 13 ms |

**Every frame was late. The device runs at 0.5â€“3 fps, not 40.** Transitions are 2â€“4 times slower than still scenes, so they would visibly stutter even if still scenes were fixed. The model keeps correct time by taking larger steps, so motion jumps rather than slowing down.

Where the time goes, per 25 ms budget:

- **Driver conversion, about 195 ms on every frame.** That is 8Ã— the whole frame budget, before anything is drawn. ESPHome's generic per-pixel `draw_pixels_at` path runs a 1-bit lookup for each of 120,000 pixels using tables in PSRAM.
- **LVGL rendering of a still scene, 110â€“300 ms.** Every frame is a full-screen redraw into a 16-bit buffer in PSRAM, plus the scene artwork. The overview's flower and the particle fan are the most expensive.
- **Transition rendering, 0.66â€“1.8 s.** Each frame redraws both scenes in full into 1-bit canvases (LVGL's 1-bit blending is per-pixel bit manipulation), then composites them.
- **SPI push, 13 ms.** Within budget, but more than half of it. A faster SPI clock would cut it.

Likely fixes, biggest first:

1. ~~Replace the per-pixel driver conversion with a bulk RGB565 â†’ 1-bit pass.~~ Done; see below.
2. Stop redrawing the full screen every frame: invalidate only animated regions in still scenes.
3. During transitions, rasterise each scene once (or at a reduced rate) instead of every frame, and optimise the costly artwork.
4. Raise the SPI clock (the panel's limit needs checking) and use DMA so the push overlaps rendering.

## Results: 2026-10-18, bulk flush

Same board and build settings. The driver now has a bulk RGB565 `draw_pixels_at()`, and its frame buffer is in internal RAM. Its layout was checked at start-up against the original lookup tables for all 120,000 pixels.

| Frames | fps | Frame interval | Render | Convert (driver) | SPI push |
|---|---|---|---|---|---|
| Overview, still | 2.8â€“3.0 | 334â€“366 ms | 291â€“303 ms | **39.7 ms** | 12.8 ms |
| CO2, still | 6.3 | 158â€“171 ms | 104â€“106 ms | 39.7 ms | 12.8 ms |
| Particles, still | 3.6 | 276â€“289 ms | 222â€“227 ms | 39.7 ms | 12.8 ms |
| Gases, still | 4.0â€“4.2 | 240â€“261 ms | 186â€“199 ms | 39.7 ms | 12.8 ms |
| Climate, still | 5.1 | 194â€“196 ms | 140 ms | 39.7 ms | 12.8 ms |
| Overview â†’ CO2 (portal) | 1.4 | 713â€“736 ms | 655â€“681 ms | 39.7 ms | 12.8 ms |
| CO2 â†’ Particles (bubble portal) | 0.9 | 1099â€“1122 ms | 1043â€“1066 ms | 39.7 ms | 12.8 ms |
| Particles â†’ Gases (airflow sweep) | 0.5 | 1827â€“1872 ms | 1770â€“1817 ms | 39.7 ms | 12.8 ms |
| Gases â†’ Climate (ribbon sweep) | 0.4 | 2274â€“2313 ms | 2220â€“2249 ms | 39.7 ms | 12.8 ms |

- **Conversion: 195 ms â†’ 39.7 ms (4.9Ã— faster).** It is still 1.6Ã— the whole 25 ms frame budget. Not yet profiled: likely causes are reading the 240 KB RGB565 frame back from PSRAM and the per-pixel read-modify-write, which could be done as one byte per 2Ã—4 block.
- **Still scenes: 2â€“3 fps â†’ 3â€“6 fps. Transitions: barely changed (0.4â€“1.4 fps)**, because LVGL rendering now dominates: 100â€“300 ms per still frame and 0.65â€“2.25 s per transition frame.
- **Every frame is still late.** The 40 fps target needs both render and convert cut by at least an order of magnitude.
- **Colour fix.** With the stock driver, LVGL's flush went through `Color::is_on()`, which treats any non-black pixel as ink, so white would have shown as black. The bulk path thresholds by luminance, matching the simulator. This hasn't been seen on a real panel yet.

## Results: 2026-10-18, partial redraw and transition snapshots

Same board, plus three changes:
- Still scenes invalidate only their animated box.
- Each transition rasterises its two scenes once, at its first frame.
- Per-pixel trig in the overview flower and the humidity drop is computed once per frame (bit-identical output; verified on 112 simulator captures).

| Frames | fps | Frame interval | Render | Convert | SPI push |
|---|---|---|---|---|---|
| Overview, still | 4.3â€“4.4 | 225â€“246 ms | 205â€“221 ms | 9â€“11 ms | 12.8 ms |
| CO2, still | 8.0â€“9.8 | 102â€“125 ms | 80â€“102 ms | 8â€“9 ms | 12.8 ms |
| Particles, still | 3.2 | 313â€“347 ms | 289â€“320 ms | 7â€“9 ms | 12.8 ms |
| Gases, still | 4.1â€“4.3 | 235â€“262 ms | 209â€“238 ms | 11 ms | 12.8 ms |
| Climate, still | 8.6 | 116â€“167 ms | 98â€“111 ms | 4 ms | 12.8 ms |
| Overview â†’ CO2 | 4.5â€“4.6 | first frame 487â€“497 ms, then about 198 ms | | 39.7 ms | 12.8 ms |
| CO2 â†’ Particles | 3.8 | first frame 1043 ms, then about 203 ms | | 39.7 ms | 12.8 ms |
| Particles â†’ Gases | 3.3 | first frame 1796 ms, then about 177 ms | | 39.7 ms | 12.8 ms |
| Gases â†’ Climate | 3.0 | first frame 2213 ms, then about 177 ms | | 39.7 ms | 12.8 ms |
| Climate â†’ Overview | 3.6 | first frame 1308 ms, then about 199 ms | | 39.7 ms | 12.8 ms |

- **Transitions: 0.4â€“1.4 fps â†’ 3.0â€“4.6 fps.** Frames after the first take about 177â€“203 ms (compositing, writing into the 16-bit buffer, conversion, push). **The first frame stalls for 0.5â€“2.2 s** while both scenes are rasterised into 1-bit canvases. That stall is now the worst pacing glitch.
- **Still scenes: 3â€“6 fps â†’ 3â€“10 fps.** Conversion fell to 4â€“11 ms because only the animated box is sent. Render time did not fall in proportion: each scene's draw function still runs in full (text measurement, charts, artwork) even when LVGL clips to a small box. Partial redraw alone briefly made the overview *slower* (about 620 ms) until its per-pixel trig was hoisted.
- **Every frame is still late.** Next candidates, biggest first:
  1. Skip drawing work outside the clip area, or cache each scene's static layer and redraw only the animated parts.
  2. Remove the first-frame stall: rasterise the incoming scene ahead of time (during the hold), or spread it over several frames.
  3. Make the compositor cheaper: process 8 pixels per byte, avoid 64-bit multiplies per pixel, and write the 16-bit output a word at a time.
  4. Particles scene: replace the per-pixel `atan2`/`hypot` fan with a precomputed or analytic blade test.

## Results: 2026-10-18, clip-aware artwork redraw and hold-time pre-render

Two more changes:
- When LVGL redraws only a still scene's animated box, only that scene's artwork function runs (no text, charts or header).
- In automatic cycling, the next transition's two snapshots are rasterised during the hold, one per step, after the last history sample before the transition. The transition then starts from them, with its clock aligned to the snapshot time.

| Frames | fps | Frame interval | Render | Convert | SPI push |
|---|---|---|---|---|---|
| Transitions using pre-rendered snapshots (6 of 7) | 5.0â€“5.7 | avg 176â€“200 ms, **max 181â€“214 ms** | 122â€“145 ms | 39.7 ms | 12.8 ms |
| Transition without them (Gases â†’ Climate, once) | 2.4 | first frame 3237 ms | | 39.7 ms | 12.8 ms |
| Overview, still | 4.2â€“4.5 | 223â€“237 ms | 202â€“205 ms | 9â€“10 ms | 12.8 ms |
| CO2 / Climate, still | 5.9â€“6.2 | 160â€“168 ms | 128â€“149 ms | 5â€“8 ms | 12.8 ms |
| Particles, still | 2.7â€“2.8 | 355â€“367 ms | 330â€“344 ms | 7â€“10 ms | 12.8 ms |
| Gases, still | 3.2â€“3.4 | 294â€“315 ms | 270â€“290 ms | 10 ms | 12.8 ms |
| Holds containing a pre-render step | | spikes of 495â€“1322 ms | | | |

- **Transition frame pacing is now even.** With pre-rendered snapshots there is no first-frame stall: the worst frame is within about 15 ms of the average. They still run at about 5 fps, not 40.
- **The stall moved into the hold.** Each pre-render step is one long frame (0.5â€“1.3 s) while the scene is otherwise still. That is less visible than a stalled transition, but it is still a dropped-frame burst.
- **The pre-render can miss.** If the last history sample before the transition lands in the final step or two, there is no time to pre-render and the transition falls back to the first-frame render (once in this run, 3.2 s). Manual navigation always falls back.
- **The clip-aware fast path barely helped.** The artwork functions themselves are the cost: the particle fan's per-pixel `atan2`/`hypot`, flower spans, particle and ribbon loops. Particles still frames came out slightly slower than the previous run (about 340 ms render vs about 300 ms); not yet explained.

Next candidates:
1. Make the artwork cheap: precompute the fan blade mask per angle step (or test blades with cross-products instead of `atan2`), and cache rotated flower spans.
2. Spread each pre-render over several frames (band by band), and start it earlier, re-rendering only the parts that a new sample changes.
3. Make the compositor and 16-bit write cheaper (about 120â€“145 ms per transition frame now).

## Results: 2026-10-18, own LVGL with a native 1-bit display

ESPHome's `lvgl:` component is gone. The atmos component owns LVGL with the shared `lv_conf.h` and an I1 display in direct mode (a static 15 KB frame in internal RAM). Its flush calls the driver's `load_rows_i1()` for the redrawn rows and `push_buffer()` for the SPI write. LVGL allocates from PSRAM. Transitions still freeze the artwork (snapshots), and the hold-time pre-render is unchanged. SPI is still 10 MHz.

| Frames | fps | Frame interval | Render | Convert | SPI push |
|---|---|---|---|---|---|
| Transitions (all 9 pre-rendered) | **9.4â€“12.0** | avg 83â€“106 ms, max 88â€“121 ms (one 1072 ms) | 68â€“90 ms | 2.5 ms | 12.6 ms |
| Still scenes (10 s windows) | **4.8â€“17.4** | avg 57â€“210 ms | 36â€“163 ms | 0.8â€“1.3 ms | 12.6 ms |
| Holds containing a pre-render step | | spikes of 350â€“1263 ms | | | |

Compared with the previous run (ESPHome LVGL, 16-bit):
- **Conversion: 39.7 ms â†’ 2.5 ms** for a full frame, about 1 ms for an animated box.
- **Transitions: 5.0â€“5.7 fps â†’ 9.4â€“12 fps**, with even pacing.
- **Still scenes: 2.7â€“6.2 fps â†’ 4.8â€“17.4 fps.**
- Free internal heap at start: about 294 KB (was 309 KB with ESPHome's LVGL; the 240 KB 16-bit buffer in PSRAM is gone).
- One transition had a single 1072 ms frame, not yet explained (possibly a pre-render step landing in the same loop iteration).

Still not 40 fps. What remains per frame:
1. **Transition compositor, about 65â€“90 ms.** Per-pixel 64-bit maths with per-bit reads and writes. A word-at-a-time compositor (32 pixels per operation, 32-bit maths, per-row precomputation) should be several times faster.
2. **Still-scene artwork, 36â€“160 ms.** Particle fan `atan2`/`hypot`, flower spans, particles and ribbons. Precomputed rotation frames or cheaper geometry would help.
3. **SPI push, 12.6 ms at 10 MHz.** Raising the clock (check the ST7305 limit) and using DMA would cut it or let it overlap the next frame.
4. **Pre-render spikes in the hold, 0.35â€“1.26 s.** Spread over several frames, or make scene rendering cheap enough not to matter.

## Results: 2026-10-18, direct fills, table compositor, 40 MHz SPI

Changes since the previous run:
- **Direct fills:** square opaque `rect()` calls on a 1-bit layer set bits directly instead of queuing LVGL draw tasks.
- **Compositor:** portal source positions come from per-frame tables and circle tests from per-row ranges; sweeps combine whole bytes through per-row masks. Bit-identical to the reference compositor (self-test, 9 progress points Ã— 20 pairs).
- **Screen background:** made transparent, since the scene surface covers every pixel and LVGL's 1-bit fill sets one bit at a time.
- **Bug fix:** a NaN at the ribbon tips (`pow` of a slightly negative `sin(Ï€)`) cast to `INT_MAX` on Xtensa and crashed the device through an overflowing bounds check. Fixed at the source, and `direct_fill` now rejects far-off coordinates and clamps to the layer buffer. The simulator was only lucky: x86 casts NaN to `INT_MIN`.
- **SPI clock:** 40 MHz, reported working on this panel by a [community build](https://www.elektroda.com/news/news4174232.html) (artefacts near 60 MHz). **Not yet verified on our panel.**

Device work per frame (ms, from the `PROFILE` line):

The component now logs three `PROFILE` lines (`still`, `trans`, `prerender`) with six columns; the sixth, Formaldehyde, has not been measured yet, so `--hw-pace` uses the CO2 costs for it.

| | Overview | CO2 | Particles | Gases | Climate |
|---|---|---|---|---|---|
| Still frame | 70.9 | 26.4 | 116.0 | 70.1 | 25.2 |
| Transition frame leaving this scene | 26.0 | 27.8 | 28.2 | 31.0 | 25.9 |
| Pre-render step drawing this scene | 71.8 | 62.2 | 151.8 | 96.9 | 62.0 |

- **Transitions: about 30 fps** (frame interval 33 ms on average, rarely late). Compositor about 20 ms, convert 2.5 ms, push 3.6 ms. At 10 MHz SPI they measured 27â€“28 fps.
- **Still scenes:** CO2 and Climate about 25 ms of work (within 30 fps); Overview and Gases about 70 ms; Particles about 116 ms.
- **Pre-render spikes:** 62â€“152 ms, down from 0.35â€“1.26 s.

Remaining costs for 30 fps everywhere:
1. Particles still frames: the flow lines (195 short LVGL line tasks with round caps), particle circles (LVGL arcs) and the per-pixel `atan2`/`hypot` fan.
2. Overview: the flower's per-pixel petal test (about 19,600 pixels Ã— 6 petals).
3. Gases: the centre line (160 LVGL line tasks) and particle circles.

## Results: 2026-10-30, first run on the real panel

Hardware: Waveshare ESP32-S3-RLCD-4.2 with its ST7305 panel, on native USB (COM3). It uses the same firmware as the previous run, built from `atmos-rlcd.yaml`: a copy of `atmos-bench.yaml` with the board's display pins, taken from the [Waveshare ESPHome tutorial](https://docs.waveshare.com/ESP32-ESPHome-Tutorials/Example-RLCD-Voice) and the [ESPHome device page](https://devices.esphome.io/devices/waveshare-esp32-s3-rlcd-42/):

| SPI SCK | SPI MOSI | CS | DC | RST |
|---|---|---|---|---|
| GPIO11 | GPIO12 | GPIO40 | GPIO5 | GPIO41 |

The community driver's README lists SCK GPIO39 and MOSI GPIO38 instead; the Waveshare pins work. Still no Wi-Fi, API or sensors.

```powershell
./esphome/bench.ps1 -Port COM3 -Seconds 120 -Config atmos-rlcd.yaml
```

**40 MHz SPI looks clean on this panel** (checked by eye). This is the first confirmation on our hardware.

| Frames (120 s run) | fps | Frame interval | Late | Render | Convert | SPI push |
|---|---|---|---|---|---|---|
| Transitions | **30.2** | avg 33 ms, max 90 ms | 11 of 518 | about 21 ms | 2.5 ms | 3.6 ms |
| Still scenes | **17.8** (10 s windows 10.6â€“25.3) | avg 56 ms, max 314 ms | 733 of 1657 | avg 45 ms, max 155 ms | about 1 ms | 3.6 ms |

The `PROFILE` line matches the previous section (still frames 70.9 / 25.9 / 115.0 / 69.3 / 24.8 ms for Overview / CO2 / Particles / Gases / Climate). The panel itself adds no cost.

- **Drawing is the limit, not SPI.** Convert plus push is about 6 ms. Going above 40 MHz (artefacts are reported near 60 MHz) would save about 1 ms per frame, so it isn't worth the risk.
- **The longest frames are hold-time pre-render steps** (60â€“150 ms each, plus the still frame). One reached 314 ms and triggered ESPHome's "took a long time" warning. Since 1,847 model steps were missed, motion runs slower than real time rather than skipping frames.
- **Memory:** about 290 KB internal heap free (largest block 237 KB), about 8 MB PSRAM free.

### Experiment: animating artwork during transitions (reverted)

Each transition frame redrew both scenes' animated boxes into the snapshot canvases at the live time, then composed as usual. The ends still met the live scenes exactly, and the simulator self-test passed. On the panel:

| | Still artwork (kept) | Live artwork (tried) |
|---|---|---|
| Transition fps | 30.2 | **9.7** |
| Frame interval | avg 33 ms, max 90 ms | avg 103 ms, max 256 ms |
| Late frames | 11 of 518 | **181 of 181** |
| Transition frame cost | 25â€“31 ms | 75â€“150 ms |

Each frame pays for two artwork redraws (about one still frame of each scene, since the artwork functions do their full per-frame work even when clipped) on top of the roughly 20 ms compositor. Transitions involving Particles were the worst, at about 150 ms. The change was reverted: transitions keep still snapshots. Live artwork is only affordable once each scene's artwork costs a few milliseconds, which is the same work that would bring still scenes toward 40 fps.

### Expected effect of Wi-Fi, Home Assistant and sensors (not yet measured)

Wi-Fi runs mostly on core 0 while the UI loop runs on core 1. API traffic and periodic IÂ²C sensor reads are light, so the average frame rate should change little. The risks are jitter from sensor drivers that block in the loop and brief PSRAM/flash bus contention. Conversely, our 150â€“300 ms frames delay network and sensor handling. Measure by adding `wifi:`, `api:` and the sensor to `atmos-rlcd.yaml` and repeating the 120 s run.

## Results: 2026-11-01, transitions chosen by the page being left

Firmware from commit `059bed1`: each page exits through its own effect (CO2 bubble portal, Particles dust bloom, Gases ribbons swell, Climate droplet fall, Formaldehyde atom portal). Real panel, `atmos-rlcd.yaml`, 120 s automatic cycling (the Overview is out of the cycle, so its column keeps the earlier figures).

| | CO2 | Particles | Gases | Climate | Formaldehyde |
|---|---|---|---|---|---|
| Effect when leaving | bubble portal | dust bloom | ribbons swell | droplet fall | atom portal |
| Transition frame (ms) | 27.7 | 36.5 | **63.6** | 19.4 | 21.9 |
| Transition fps | 30.4 | 24 | **15.5** | 30.7 | 30.5 |
| Still frame (ms) | 33.4 | 46.4 | 31.2 | 29.0 | 37.5 |
| Pre-render step (ms) | 38.3 | 57.0 | 55.4 | 33.5 | 41.9 |

- **Ribbons swell is the slow one:** about 57 ms of render per frame, so every frame is late (15.5 fps). Dust bloom misses 40 fps on about 40% of frames (24 fps). Bubble portal, droplet fall and atom portal hold about 30 fps with no late frames.
- Still scenes average about 26 fps across the run (late frames come mostly from Particles and pre-render steps; maximum frame 130 ms).
- Memory: about 284 KB internal heap free (largest block 229 KB), about 8 MB PSRAM free.
- `hw_*_ms` in `desktop/main_win32.c` now hold these figures.

## Results: 2026-11-01, frame-rate cap

The component no longer spins the main loop (`HighFrequencyLoopRequester` is gone). Each frame runs from a scheduler timeout set for the next frame period, so the loop task sleeps until the next frame is due and never draws faster than the cap (`frame_period`). The report line now shows the CPU share spent drawing ("UI busy"). Real panel, 120 s each:

| Cap | Still fps | Transition fps | Late frames | Max frame interval | Missed model steps | UI busy |
|---|---|---|---|---|---|---|
| none (25 ms steps, busy loop) | 26 | 26 | 370 of 1323 at 37.5 ms | 130 ms | 1067 | about 100% |
| 30 fps (33.3 ms) | 26 | 26 | 309 of 2890 at 50 ms | 133 ms | 262 | 93â€“98% |
| 20 fps (50 ms) | 19.8 | 19.1 | 42 of 2170 at 75 ms | 138 ms | 36 | 67â€“78% |
| **17 fps (59 ms), default** | 16.9 | 16.8 | **32 of 1862** at 88.5 ms | 147 ms | **9** | **58â€“67%** |

- At 30 fps most frames cost close to the period, so the cap frees almost no CPU.
- At 20 fps pacing is even: only ribbons swell (about 64 ms) and pre-render steps run over, and about a quarter of the CPU is left for Wi-Fi, the API and sensors.
- At 17 fps (the default) only pre-render steps run over, and about a third to two-fifths of the CPU is left.
- `--hw-pace` in the simulator now holds every frame for at least 59 ms.
