#ifndef ATMOS_H
#define ATMOS_H
#include "lvgl.h"
/* All PM fields are independent cumulative mass concentrations in ug/m3.
   pm retains its existing name for PM2.5. Supply NAN for missing PM channels.
   pressure is barometric pressure in hPa (NAN without a sensor).
   hcho is formaldehyde in ppb (SEN68; NAN without a sensor). */
typedef struct {
    float co2, pm, voc, nox, temp, rh;
    float pm1, pm4, pm10;
    float pressure;
    float hcho;
} atmos_readings;
void atmos_init(void);
/* Boot splash: the device name over a "Made for ESPHome" badge, drawn on its own
   screen object. Call before atmos_init(); atmos_init() then opens the first page
   from it with the overview's portal, zooming into the badge's logo. */
void atmos_show_splash(const char *name);
/* Line under the name, default "Visualizing Air Quality" (NULL or empty for none). Call before atmos_show_splash(). */
void atmos_set_splash_subtitle(const char *text);
void atmos_step(double seconds);
void atmos_navigate(int direction);
/* Left/right hardware key: next/previous reading on the page. Right after a page's last
   reading opens the overview (once per run of presses: a hold without a
   press re-arms it), and from there the page after it; left steps back
   through the readings, skipping the overview. */
void atmos_advance(int direction);
void atmos_set_scene(int scene);
/* Scene number at a page position (0 = overview, then CO2, gases, formaldehyde,
   particles, climate): the order of navigation, keys 1-6 and a page selector. */
int atmos_page(int position);
void atmos_set_scenario(int scenario);
void atmos_toggle_pause(void);
void atmos_toggle_auto(void);
void atmos_set_hidden(bool hidden);
/* Hardware integration: pass measured readings and disable the demo generator. */
void atmos_set_readings(atmos_readings readings);
/* Wi-Fi strength shown in every page's badge: 0-3 lit arcs, or -1 (the default) for
   disconnected, drawn as an X. atmos_wifi_bars() maps an RSSI in dBm (NAN when
   disconnected) to bars: -60, -70 and -80 dBm are the 3/2/1 thresholds. */
void atmos_set_wifi(int bars);
int atmos_wifi(void);
int atmos_wifi_bars(float dbm);
/* Display units of the climate page, the overview's temperature and their history
   charts. Only what is drawn converts: readings passed in, rating bands, alerts and
   history stay in C and hPa (what Home Assistant receives). Bad values are ignored. */
enum { ATMOS_TEMP_C, ATMOS_TEMP_F };
enum { ATMOS_PRESSURE_HPA, ATMOS_PRESSURE_KPA, ATMOS_PRESSURE_MMHG, ATMOS_PRESSURE_INHG };
void atmos_set_temperature_unit(int unit);
int atmos_temperature_unit(void);
void atmos_set_pressure_unit(int unit);
int atmos_pressure_unit(void);
void atmos_select_pm(int size);
void atmos_cycle_pm(int direction);
bool atmos_select_pm_at(int x,int y);
/* Number of scenes. */
#define ATMOS_SCENES 6
/* Readings, in page order: what a reading selector lists. */
enum { ATMOS_CO2, ATMOS_VOC, ATMOS_NOX, ATMOS_HCHO, ATMOS_PM1, ATMOS_PM25, ATMOS_PM4, ATMOS_PM10,
       ATMOS_TEMPERATURE, ATMOS_HUMIDITY, ATMOS_PRESSURE, ATMOS_READINGS };
/* Start-up delay: a reading whose sensor reports nothing for a while after boot (the
   SEN6x, about a minute). Its history and chart span then start at its first valid
   reading, and the chart waits empty at "LAST 0 SEC" until then; its page's badge
   reads "Starting" instead of "--". Off by default:
   the chart's span runs from the first sample of any reading. */
void atmos_set_startup_delay(int reading, bool on);
/* Move to the page showing a reading and underline/chart it. Restarts the hold. */
void atmos_select_reading(int reading);
/* The reading currently underlined/charted (the destination's while moving). */
int atmos_reading(void);
/* Show the overview (a detour from the cycle; see atmos_advance()). Restarts the hold;
   with cycling on, the hold ends on the page after the last page shown. */
void atmos_select_overview(void);
/* Low light, from the device's ambient sensor. Mutes alert sounds (a cycle change is
   announced once it is lit again). With atmos_set_overview_dark() on it also parks on
   the overview: at the next hold end while cycling, at once otherwise, and again one
   hold after a manual visit; alerts do not override it. Lit again, the display leaves
   one hold later, for the page after the last page shown (cycling) or that page itself. */
void atmos_set_low_light(bool dark);
bool atmos_low_light(void);
void atmos_set_overview_dark(bool on);
bool atmos_overview_dark(void);
void atmos_set_auto(bool on);
bool atmos_auto(void);
/* Alert focus: while any reading's band has alert set, automatic cycling visits only
   alerting readings (in page order, stepping the underline within a page), holding
   each page for the hold time. With none alerting it cycles normally. Manual
   navigation is unaffected; cycling returns to the alerts after one hold. */
void atmos_set_alert_focus(bool on);
bool atmos_alert_focus(void);
/* True while the debounced alert set (see atmos_set_alert_times()) is non-empty. */
bool atmos_alert_active(void);
/* True while any reading's band has alert set right now (no hysteresis). */
bool atmos_alert_raw(void);
/* Alert hysteresis, in active seconds, for every reading: a reading joins the alert
   set after alerting continuously for `enter` and leaves it after `leave` clear
   (defaults 60 and 180; NAN keeps a value). The focus cycle, atmos_alert_active()
   and the sound events use this set; badges and the overview warning do not. */
#define ATMOS_ALERT_TIME_MAX 86400.0f
void atmos_set_alert_times(float enter, float leave);
/* Alert sounds (needs alert focus): events raised where automatic cycling enters the
   alert-focus cycle (ALERT_START, at that transition's start) or returns to the
   normal cycle (ALERT_CLEAR). ALERT_START is skipped for the cooldown (default
   1200 s) after an ALERT_CLEAR. Poll after atmos_step(); ATMOS_EVT_NONE when empty. */
enum { ATMOS_EVT_NONE, ATMOS_EVT_ALERT_START, ATMOS_EVT_ALERT_CLEAR };
void atmos_set_alert_sound(bool on);
bool atmos_alert_sound(void);
void atmos_set_alert_sound_cooldown(float seconds);
int atmos_poll_event(void);
/* Seconds each page (split between its readings) is held before automatic cycling moves on. */
#define ATMOS_HOLD_MIN 2.0f
#define ATMOS_HOLD_MAX 600.0f
void atmos_set_hold(float seconds);
float atmos_hold(void);
/* Time span the history charts show. Shrinking it discards older samples; growing it
   lets the chart fill up to the new span. Samples start every 2 s and stretch up to span/60 as history accumulates. */
#define ATMOS_HISTORY_MIN 60.0f
#define ATMOS_HISTORY_MAX 86400.0f
void atmos_set_history_window(float seconds);
float atmos_history_window(void);
/* Rating bands: a reading inside [min, max] shows `text` in its page's badge. NAN
   leaves that end open; the first matching band wins. level orders severity for
   the overview (worst wins). alert marks bands
   that raise the overview's warning and, with alert focus on, the alert cycle.
   Readings with no matching band show "--". May be called before atmos_init(). */
#define ATMOS_BANDS_MAX 8
#define ATMOS_BAND_TEXT 16
/* has_color/color: the status LED colour (0xRRGGBB) for the band; without it the
   device picks one by level. mark: the overview tile's marker (ATMOS_MARK_LEVEL: by
   level, check / ringed ! / warning triangle). Temperature, humidity and pressure are
   comfort readings: they never alert, whatever `alert` says, and stay out of the
   overview's rating and LED colour. */
enum { ATMOS_MARK_LEVEL, ATMOS_MARK_CHECK, ATMOS_MARK_SNOW, ATMOS_MARK_COOL, ATMOS_MARK_SUN,
       ATMOS_MARK_FLAME, ATMOS_MARK_DRY, ATMOS_MARK_DROP, ATMOS_MARK_DROPS, ATMOS_MARKS };
typedef struct { float min, max; int level; char text[ATMOS_BAND_TEXT]; bool alert; bool has_color; uint32_t color; int mark; } atmos_band;
/* The LED colour set on the band currently holding a reading; false when none is set. */
bool atmos_band_color(int reading, uint32_t *rgb);
/* Returns false (and changes nothing) for a bad reading or more than ATMOS_BANDS_MAX bands. */
bool atmos_set_bands(int reading, const atmos_band *bands, int n);
int atmos_self_test(void);
/* Test hook: the 1-bit frame (50-byte MSB-first rows, 1 = paper) composed for
   the last transition draw, or NULL when no transition was drawn. */
const uint8_t *atmos_transition_frame(void);
/* True while a scene transition is in progress (for frame-pacing measurement). */
bool atmos_is_moving(void);
/* Transitions that started from snapshots pre-rendered during the hold. */
unsigned atmos_prerendered_transitions(void);
/* Current scene (0-5: overview, CO2, particles, gases, climate, formaldehyde) and, while moving, the transition's destination. */
int atmos_scene(void);
/* Diagnostic: direct fills that were clamped to the layer buffer. Should stay 0. */
unsigned atmos_clip_overruns(void);
const char *atmos_overrun_detail(void);
/* Optional profiling: a microsecond clock, and the average transition compose time. */
void atmos_set_clock(uint32_t (*us)(void));
float atmos_compose_ms(void);
int atmos_destination(void);
/* Scene rasterised by a hold-time pre-render during the latest atmos_step(), or -1. */
int atmos_step_prerendered(void);
/* Status LEDs. Transition progress 0..1 while moving, else -1. The reading whose
   rating a scene shows (its underlined reading; the overview's worst-rated one), or -1.
   The index of the band holding a reading's value in its atmos_set_bands() list and
   that band's level, or -1 when it has no rating. */
float atmos_transition_progress(void);
int atmos_led_reading(int scene);
int atmos_band_index(int reading);
int atmos_band_level(int reading);
#endif
