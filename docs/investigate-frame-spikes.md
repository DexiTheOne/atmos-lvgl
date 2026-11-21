# Investigate: remaining frame spikes at 34 Hz

For later. Read `AGENTS.md` first. State on 2026-11-20, firmware `b110027` (pipelined TE frames,
`frame_period: 29ms`) on atmos.

## What is left

The device shows a frame on almost every panel refresh (29.2 ms): still pages 34.1â€“34.2 fps,
transitions 32.8â€“34.2 fps, 0 late pushes. What remains is roughly **one skipped refresh (a 58 ms
interval) per 10 s on still pages, and 0â€“2 per transition**. These frames took 28â€“54 ms of work
(`render max` in the `still`/`transition` lines; one transition frame reached 113 ms in an earlier
run). They are rare and hard to see on the slow liquid crystal, so this is optional polish.

## Suspected causes (unverified)

1. **History samples.** A sample redraws the chart panel and the reading text (header and body
   bands), so a still frame that also takes a sample does more work than usual.
2. **Overview redraws.** The overview has no bands: any reading change redraws it whole (about
   30 ms, `PROFILE still` first value 29.5 ms). Sensor updates arrive independently, so it may
   redraw several times a minute.
3. **The first frame of a transition** (or of a reading selection, which redraws the whole frame)
   when its snapshots were not pre-rendered (manual navigation, or a sample that invalidated them).
4. **Wi-Fi, API and sensor work** in the loop task between frames: the frame's wake can be late
   (`late frame: woke N ms after its pulse`), so the frame has less than its budget.
5. **Pre-render steps** during the hold (`PROFILE prerender`, up to 12.3 ms for Particles) on top
   of a still frame.

## How to investigate

- Keep `frame_stats: true`. Each late frame logs `late frame: woke ... work ... (pre-render N,
  convert ...) scene S moving M | tick T target G at present P, pushed at X for Y`. Collect a
  few minutes (`.venv-esphome/Scripts/python.exe esphome/dashboard.py logs atmos.yaml 300`) and
  group late frames by cause: `pre-render` >= 0 (cause 5), `woke` well above 0 (cause 4),
  `moving 1` at a transition's start (cause 3), scene 0 (cause 2).
- For cause 1, add the frame's dirty bands (`atmos_take_band_cost()` already reports per band
  mask) or a "sample taken" flag to the late-frame line.
- `BANDS scene N header body panel: ... ms avg` shows what a full band redraw costs per page.

## Possible fixes, by likely payoff

- Skip a pre-render step on a frame that already has band redraws, or split the heavier steps.
- Spread a sample's work: redraw the chart on one frame and the reading text on the next.
- Overview: redraw only the changed tiles (it would need bands per tile).
- Pre-render the first transition frame for manual navigation is not possible (the
  destination is unknown), but the outgoing snapshot could be kept current during the hold.

Measure with the `still` / `transition` lines (`late(>43.8ms)`, `render max`) and the `TE:` line
before and after, and record results in `esphome/README.md`.
