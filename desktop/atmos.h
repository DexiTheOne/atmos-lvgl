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
#endif
