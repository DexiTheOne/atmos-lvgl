# Project policy: develop the LVGL implementation

The native LVGL application in `desktop/` is the source of truth for the product UI. From now on, implement rendering, layout, animation, navigation, and sensor/history changes there by default. A request to change "the rendering", "preview", "animation", or "UI" means the LVGL simulator unless the user explicitly says otherwise.

The root `index.html`, `app.js`, `style.css`, and `server.mjs` are the original browser design reference. Consult them to recover design intent, but do not implement requested product changes only in those files or maintain a second browser implementation. Edit the browser reference only when the user explicitly requests it. A future browser preview should display actual LVGL output and forward controls to the native model.

## Code map

- `desktop/atmos.c`: platform-independent C UI, LVGL drawing, sensor simulation, history, scene navigation, and transition composition.
- `desktop/atmos.h`: UI/model interface, including `atmos_set_readings()` for real sensor data.
- `desktop/main_win32.c`: Windows window, input, frame presentation, snapshots, and native render/presentation verification.
- `desktop/lv_conf.h`: LVGL configuration.
- `desktop/CMakeLists.txt`: native application and CTest setup.
- `desktop/build.ps1`: dependency revision check, configure, compile, test, and optional launch.
- `desktop/render_review.py`: optional Pillow-based transition review sheets and GIFs.
- `desktop/README.md`: detailed runtime and hardware-port notes.

Keep Windows APIs in the platform layer. Keep UI/model code reusable for the later hardware port. The desktop build is not device firmware; do not imply that simulator frame rate or memory allocations have been validated on the physical panel.

## Build and verification

Run PowerShell commands from the project root unless stated otherwise:

```powershell
./desktop/build.ps1
```

Prerequisites: CMake, Git, MSYS2 UCRT64 GCC and `mingw32-make`. The script prepends `C:/msys64/ucrt64/bin` when present. LVGL 9.2.2 is pinned to commit `7f07a129e8d77f4984fff8e623fd5be18ff42e74`; the first build clones it into `desktop/vendor/lvgl`, requiring network access. Do not silently upgrade or modify vendored LVGL. Generated dependency and build directories are ignored.

The script builds with warnings treated as errors and runs `ctest --test-dir desktop/build --output-on-failure`. Complete these checks after C/configuration/build changes. For documentation-only edits, check the documented commands against the actual scripts; rebuilding is unnecessary.

CTest invokes `atmos --self-test`: model/history/navigation checks, transition endpoints, actual LVGL frame palette/nonempty checks, and Windows GDI presentation checks. Tests do not establish visual quality or hardware performance. Inspect rendered frames or animation captures when changing appearance or motion, including GOOD and CHECK AIR scenarios and forward/reverse navigation.

## Launch and operate

Build, verify, and launch with:

```powershell
./desktop/build.ps1 -Run
```

To launch an existing build with a predictable snapshot directory:

```powershell
$simulatorPath = Join-Path (Get-Location) 'desktop/build/atmos.exe'
$simulatorDir = Split-Path $simulatorPath
Start-Process -FilePath $simulatorPath -WorkingDirectory $simulatorDir -WindowStyle Hidden
```

The launcher hides the console; the application explicitly shows its native GUI. Preserve the startup visibility handling. In restricted execution environments, a background process may end with the sandbox job. Use the supported approval/escalation flow when needed to launch a persistent window. After launching, verify that the process is responding and has a nonzero `MainWindowHandle`; do not report that a window opened based only on `Start-Process` succeeding.

If the executable is locked during linking, close only this project's simulator instances, verified by their full executable path, then rebuild and relaunch. Do not terminate unrelated processes by name. A requested rebuild/run authorizes restarting this simulator as necessary.

| Input | Action |
|---|---|
| 1–5 | Overview, CO2, particles, gases, climate |
| Arrow keys, click, mouse wheel | Navigate |
| Space | Pause motion and history sampling |
| A | Toggle automatic cycling |
| F1–F4 | Quiet morning, occupied room, cooking, open windows |
| S | Save 400 × 300 `atmos-frame.bmp` in the process working directory |
| Escape | Close |

Minimizing freezes active time. For visual feedback, pause the desired scene and use a screenshot with annotations; the native window is not a browser annotation surface.

## Capture and review transitions

From the project root, after building:

```powershell
Push-Location desktop/build
try { ./atmos.exe --capture-transitions } finally { Pop-Location }
python desktop/render_review.py
```

The review script requires Pillow. Python with Pillow is currently available at `C:/msys64/ucrt64/bin/python.exe` if `python` on PATH differs. Captures are written to `desktop/build`; review sheets and GIFs go to `artifacts/`. GIFs sample every 200 ms and are review artifacts, not a measurement of native animation cadence.

## Preserve the display and interaction contract

- Native resolution: 400 × 300, landscape. Present at integer pixel scales with letterboxing.
- The target panel (Waveshare ESP32-S3-RLCD-4.2, ST7305) is 1-bit black/white. LVGL renders `LV_COLOR_FORMAT_I1`; bit 0 is ink, bit 1 is paper. There is no grey: chart fill is paper. Preserve hard opaque transitions; avoid dither, alpha fades, or intermediate tones.
- The simulator maps ink/paper to display tones only at presentation: defaults `#303030` / `#c6c6c6`, overridable with `--ink=RRGGBB --paper=RRGGBB` or `ATMOS_INK` / `ATMOS_PAPER`.
- Scene and transition buffers are 15 KB I1 frames. The transition compositor uses integer fixed-point maths only; other model/artwork maths is single-precision `float` (built with `-fsingle-precision-constant`), with active-time timestamps kept in `double`.
- Target animation cadence is 40 fps on desktop. Measure hardware separately.
- Present completed frames from an offscreen Windows bitmap in one copy. Do not clear the visible display to black between frames. Render the initial LVGL frame before showing the window.
- Keep scenes flowing through their artwork rather than reverting to generic slideshow transitions: overview → status-circle portal → CO2 → selected live-bubble portal → particles → airflow sweep → gases → horizontal ribbon sweep → climate → inverse status portal → overview.
- Keep both scenes animated during travel, preserve the selected bubble anchor, keep gas/climate history ribbons continuous, and support reverse navigation. Current scene holds are 6.5 seconds; portals take 1.4 seconds and sweeps 1.2 seconds. Timing may be deliberately tuned for a requested animation change.
- Navigation during movement queues the latest requested destination. Pause/minimize freezes motion and sampling time.
- History starts empty, samples every two active seconds, retains two minutes, and never invents historical readings. Scenario changes preserve the existing samples.
- Demo readings, derived PM values, and warning cutoffs are illustrative. Keep sensor units and the real-reading entry point intact.

These are the current product constraints, not an approval gate: explicit user changes take precedence. Update the relevant documentation when behavior or operation changes.
