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
/* Left/right hardware key: next/previous reading on the page, then the next/previous page. */
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
void atmos_select_pm(int size);
void atmos_cycle_pm(int direction);
bool atmos_select_pm_at(int x,int y);
/* Number of scenes. */
#define ATMOS_SCENES 6
/* Readings, in page order: what a reading selector lists. */
enum { ATMOS_CO2, ATMOS_VOC, ATMOS_NOX, ATMOS_HCHO, ATMOS_PM1, ATMOS_PM25, ATMOS_PM4, ATMOS_PM10,
       ATMOS_TEMPERATURE, ATMOS_HUMIDITY, ATMOS_PRESSURE, ATMOS_READINGS };
/* Move to the page showing a reading and underline/chart it. Restarts the hold. */
void atmos_select_reading(int reading);
/* The reading currently underlined/charted (the destination's while moving). */
int atmos_reading(void);
void atmos_set_auto(bool on);
bool atmos_auto(void);
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
   the overview (worst wins) and its warning (level >= 2). Readings with no
   matching band show "--". May be called before atmos_init(). */
#define ATMOS_BANDS_MAX 8
#define ATMOS_BAND_TEXT 16
typedef struct { float min, max; int level; char text[ATMOS_BAND_TEXT]; } atmos_band;
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
#endif
