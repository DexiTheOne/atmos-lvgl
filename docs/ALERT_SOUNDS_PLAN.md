# Alert sounds and alert hysteresis: plan

Status: implemented 2026-11-16 (model, self-test, simulator, component, `atmos-rlcd.yaml`); alert sound only, no clear sound yet. Differences from the plan: boot and simulator scenario changes are not special-cased (the 60 s enter delay covers sensor start-up); the alert sound is the user's 2.6 s bell.

## Goal

Play a short sound when the display enters its alert-focus cycle, and a pleasant
sound when it leaves it, in step with the transition on screen. Sounds must
never nag: alert focus pages and sounds share one time-based hysteresis, and
sounds only exist while alert focus is on.

## Behaviour

### Two toggles

| `alert_focus` | `alert_sound` | Result |
|---|---|---|
| off | either | Normal cycle, silent |
| on  | off | Alert-focus cycle, silent |
| on  | on  | Alert-focus cycle; sound on entering and leaving it |

`alert_sound` is subordinate to `alert_focus`: it does nothing while focus is off.

### Hysteresis (global, time-based)

Each reading keeps a debounced alert flag next to its raw band alert:

- **Joins** the alert set after its band has been alerting continuously for
  `alert_enter` (default 60 s).
- **Leaves** the alert set after its band has been non-alerting continuously
  for `alert_leave` (default 180 s).
- Timers use active time (pause/minimize freezes them).
- One pair of times for all readings for now (per-reading later if needed).
- A reading with no valid sample yet (`startup_delay`) is never alerting.

What uses which state:

| Consumer | State |
|---|---|
| Badge rating, overview warning | raw (always the true current reading) |
| Focus cycle (`auto_next()`, `hold_items`) | debounced |
| `alert_active` binary sensor / `atmos_alert_active()` | debounced |

### Sound events, synced to transitions

Events are emitted at the first frame of the transition, only when
`alert_focus` and `alert_sound` are both on:

- **ALERT_START**: the transition into the first focus page — because the
  debounced set became non-empty, or because focus was turned on while
  alerts were already set.
- **ALERT_CLEAR**: the transition back to the normal cycle after the last
  reading left the debounced set.

Silent cases:

- Turning focus off mid-alert (nothing cleared).
- Turning sounds on mid-alert (waits for the next change).
- Another reading joining an alert already in progress.
- Manual navigation, and the automatic return to the focus cycle after one hold.
- Boot, startup delay, scenario changes in the simulator.

Cooldown: after an ALERT_CLEAR, ALERT_START sounds are suppressed for
`alert_sound_cooldown` (default 20 min). The display still enters the focus
cycle; only the sound is held back.

### Sound design

- Alert: under 1 s, soft two-note descending chime. No beep trains or sirens.
- Clear: under 1 s, gentle ascending interval.
- Played once, never repeated. Default volume about 40 %.
- Format: PCM WAV, 16-bit, mono, 16 000 Hz (the speaker pipeline's rate, so
  no resampling). Peak about -3 dBFS, 5–10 ms fade in/out, no leading silence.
- Files live in the repo (`esphome/sounds/alert.wav`, `clear.wav`) and are
  embedded at build time via the media player's `files:` (raw GitHub URL at the
  pinned commit, since the dashboard builds remotely). To change a sound:
  replace the file under the same name, commit, push, re-pin, OTA. The
  simulator embeds the same two files.

## Hardware (Waveshare ESP32-S3-RLCD-4.2)

Verify all pins against Waveshare's schematic before writing YAML.

- **ES8311 codec**: I2C (0x18) for configuration only — a few register writes
  at boot and on volume change; audio goes over I2S (MCLK, BCLK, LRCLK, DOUT)
  by DMA.
- **Speaker amplifier** (NS4150B class): enable GPIO; keep it off when idle to
  avoid hiss and pops.
- **ES7210 mic ADC** (0x40), if fitted: unused.
- **I2C**: shared bus at 100 kHz (the SEN68's maximum, address 0x6B). No address
  conflicts expected; codec traffic is negligible.
- **Pins to check**: I2S and amp-enable must not use GPIO 0 (right key / boot),
  1 (LEDs), 5, 11, 12, 40, 41 (display), 18 (left key).
- **Internal RAM**: I2S DMA buffers need internal RAM, which already holds four
  15 KB frame buffers. Keep the speaker buffer small (~100 ms) and check free
  internal heap after adding audio.
- **CPU**: playback runs in its own task; WAV is cheap. Re-run the `PROFILE`
  benchmark while a sound plays and look for late frames.
- **LVGL**: no interaction; nothing changes in `lv_conf.h`.

ESPHome pieces: `i2c`, `i2s_audio`, `audio_dac: platform: es8311`,
`speaker: platform: i2s_audio`, `media_player: platform: speaker` with `files:`
and `media_player.speaker.play_on_device_media_file`.

## Implementation steps

1. **Model (`desktop/atmos.c`, `atmos.h`)** — no audio code here.
   - Per-reading debounced alert flag with enter/leave timers.
   - `auto_next()`, `hold_items` and `atmos_alert_active()` use the debounced set.
   - New API: `atmos_set_alert_sound(bool)`,
     `atmos_set_alert_times(float enter_s, float leave_s)`,
     `atmos_set_alert_sound_cooldown(float s)`,
     `atmos_poll_event()` returning `ATMOS_EVT_NONE` / `ATMOS_EVT_ALERT_START` /
     `ATMOS_EVT_ALERT_CLEAR`, emitted at the transition's first frame.
2. **Self-test (`--self-test`)**
   - A reading oscillating around a threshold neither flaps the focus cycle nor
     emits events.
   - Events coincide with a transition starting into / out of the focus cycle.
   - No events while focus or sound is off; the cooldown holds; none at boot or
     during startup delay.
3. **Simulator (`desktop/main_win32.c`)**
   - Play two embedded WAVs with `PlaySound(..., SND_ASYNC | SND_MEMORY)` and
     log events.
   - Key `M` toggles alert sounds; `--alert-enter=s`, `--alert-leave=s` for
     quick testing.
4. **Component (`components/atmos`)**
   - YAML options: `alert_enter` (default 60s), `alert_leave` (180s),
     `alert_sound_cooldown` (20min).
   - `alert_sound` switch next to `alert_focus`.
   - Automation triggers `on_alert_start:` and `on_alert_clear:`, fired from the
     polled events; the YAML decides what to play.
5. **Device YAML (`esphome/atmos-rlcd.yaml`, then `atmos.yaml` on the dashboard)**
   - `i2c` at 100 kHz, `i2s_audio`, ES8311 DAC, speaker, media player with the
     two WAVs, amp enable handling, volume number.
   - Wire the triggers to `play_on_device_media_file`.
6. **Measure and document**
   - Bench with a sound playing (heap, late frames, UI busy); listen on the
     real panel for pops and loudness.
   - Update `AGENTS.md`, `desktop/README.md`, `esphome/README.md`.

Steps 1–3 need no audio hardware and are done and verified in the simulator first.

## Open questions

- Exact I2S / amp-enable pins on this board revision.
- Whether the onboard speaker is loud enough at 40 %, or needs a volume number
  default change.
- Per-reading hysteresis times (deferred).
