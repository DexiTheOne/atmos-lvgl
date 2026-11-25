/* Settings page: for now a hardware test of the settings key. The sheet drops in
   from the top over the frozen page and slides back up to close. */
#include "atmos.h"
#include "settings_internal.h"
#include <math.h>

#define SCREEN_W 400
#define SCREEN_H 300
#define SLIDE_TIME .45f   /* seconds, each way */
#define EDGE 2            /* ink edge under the moving sheet */

static bool open_, queued;   /* queued: a press waiting for a page transition to end */
static float pos;            /* 0 hidden, 1 fully down */

static void page(void){
    ui_fill(0,0,SCREEN_W,SCREEN_H,false);
    ui_wifi_header("SETTINGS");
}
bool settings_draw(void){
    if(pos<=0)return false;
    /* Smoothstep: the sheet eases off the top and settles at the bottom. */
    float p=pos*pos*(3-2*pos);
    int top=(int)lroundf((p-1)*SCREEN_H);
    if(top>-SCREEN_H && top<0)ui_draw_page_behind();
    ui_set_offset(top);
    page();
    if(ui_dark())ui_invert(0,0,SCREEN_W,SCREEN_H);
    if(top<0)ui_fill(0,SCREEN_H,SCREEN_W,EDGE,true);
    ui_set_offset(0);
    return true;
}
bool settings_step(double dt){
    if(!open_ && pos<=0)return false;
    float target=open_?1:0;
    if(pos!=target && dt>0){
        float d=(float)fmin(dt,.1)/SLIDE_TIME;
        pos=open_?fminf(1,pos+d):fmaxf(0,pos-d);
        ui_invalidate();
        if(pos<=0){ui_redraw_page();return false;}
    }else ui_flush_dirty();
    return true;
}
void settings_transition_done(void){if(queued)atmos_set_settings(!open_);}

/* A press during a page transition waits for it to end (the sheet slides over a still page). */
void atmos_set_settings(bool open){
    if(ui_moving()){queued=open!=open_;return;}
    queued=false;
    if(open==open_)return;
    open_=open;ui_invalidate();
}
void atmos_toggle_settings(void){atmos_set_settings(ui_moving()?!(open_^queued):!open_);}
bool atmos_settings_open(void){return open_;}
