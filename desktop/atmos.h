#ifndef ATMOS_H
#define ATMOS_H
#include "lvgl.h"
/* All PM fields are independent cumulative mass concentrations in ug/m3.
   pm retains its existing name for PM2.5. Supply NAN for missing PM channels. */
typedef struct {
    float co2, pm, voc, nox, temp, rh;
    float pm1, pm4, pm10;
} atmos_readings;
void atmos_init(void);
void atmos_step(double seconds);
void atmos_navigate(int direction);
void atmos_set_scene(int scene);
void atmos_set_scenario(int scenario);
void atmos_toggle_pause(void);
void atmos_toggle_auto(void);
void atmos_set_hidden(bool hidden);
/* Hardware integration: pass measured readings and disable the demo generator. */
void atmos_set_readings(atmos_readings readings);
void atmos_select_pm(int size);
void atmos_cycle_pm(int direction);
bool atmos_select_pm_at(int x,int y);
int atmos_self_test(void);
/* Test hook: the 1-bit frame (50-byte MSB-first rows, 1 = paper) composed for
   the last transition draw, or NULL when no transition was drawn. */
const uint8_t *atmos_transition_frame(void);
/* True while a scene transition is in progress (for frame-pacing measurement). */
bool atmos_is_moving(void);
/* Transitions that started from snapshots pre-rendered during the hold. */
unsigned atmos_prerendered_transitions(void);
/* Current scene (0-4) and, while moving, the transition's destination. */
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
