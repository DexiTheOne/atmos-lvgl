#include "atmos.h"
#if LVGL_VERSION_MAJOR!=9
#error "atmos requires LVGL 9.x"
#endif
/* Scenes are rasterised into 1-bit canvases. ESPHome enables canvas; the device YAML
   must add build flag -DLV_DRAW_SW_SUPPORT_I1=1 (see esphome/atmos-bench.yaml). */
#if !LV_USE_CANVAS || !LV_DRAW_SW_SUPPORT_I1
#error "atmos needs LV_USE_CANVAS=1 and LV_DRAW_SW_SUPPORT_I1=1 (see AGENTS.md, ESPHome integration)"
#endif
#include "ui_theme.h"
#include "ui_fonts.h"
#include <tgmath.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.283185307179586f
_Static_assert(UI_PM_VALUE_SIZE==UI_GAS_VALUE_SIZE &&
    UI_GAS_VALUE_SIZE==UI_CLIMATE_VALUE_SIZE,"detail primary values must share one font role");
_Static_assert(UI_CHART_LEFT==UI_PM_CHART_LEFT && UI_CHART_RIGHT==UI_PM_CHART_RIGHT &&
    UI_CHART_TOP==UI_PM_CHART_TOP && UI_CHART_BOTTOM==UI_PM_CHART_BOTTOM,
    "single history graphs must share exact geometry");
typedef struct { double at; atmos_readings value; } sample;
static const atmos_readings profiles[] = {
    {624,3.2f,78,1,22.4f,46,2.1f,4.0f,5.3f}, {1380,7.8f,126,4,24.1f,58,4.6f,9.5f,13.1f},
    {925,38.6f,218,27,25.6f,63,21.2f,46.9f,61.3f}, {438,1.4f,47,1,20.6f,42,.8f,1.9f,2.8f}
};
static atmos_readings sensor, origin, target;
static sample history[62];
static int count, scene, destination, queued=-1, scenario;
static int gas_selected,climate_selected;
static double t, changed, next_sample, age, movement;
/* Time the scene artwork is drawn at: t for live frames; a transition snapshots the
   outgoing scene at its start time and the incoming scene at its end time. */
static double anim_t;
/* Redraw the whole screen next frame (readings, history, selection or scene changed).
   Otherwise only the current scene's animated artwork is redrawn. */
static bool full_dirty=true;
/* Transition scenes are rasterised once per transition, not every frame. */
static bool snapshots_ready;
/* Bumped whenever displayed data changes (sample, readings, selection), so a
   pre-rendered snapshot made before the change is not used. */
static unsigned data_version;
/* Automatic cycling: the next transition's time and destination are known, so its
   two snapshots are rasterised during the hold, one per step, after the last
   history sample before it. pre_stage: 0 none, 1 outgoing done, 2 both done. */
static int pre_from=-1,pre_to=-1,pre_stage;
static double pre_start;
static unsigned pre_version,prerendered_transitions;
static int prerendered_last_step=-1; /* scene rasterised by the latest atmos_step(), or -1 */
static bool paused, automatic=true, hidden, external_data, moving;
static int pm_selected=1,pm_snapshot_count;
static float pm_from_weights[4]={0,1,0,0},pm_to_weights[4]={0,1,0,0};
static const char *pm_labels[]={"PM1.0","PM2.5","PM4.0","PM10"};
static sample pm_snapshot[62];
static float pm_blend_age=.4f,pm_density;
static float gas_intensity[2];
static float pm_from_low,pm_from_high,pm_to_low,pm_to_high;
static lv_obj_t *surface;
/* lv_obj_add_flag/remove_flag are deprecated from LVGL 9.6 onwards. */
#if LVGL_VERSION_MINOR>=6
#define hide_obj(o) lv_obj_set_hidden(o,true)
#define not_scrollable(o) lv_obj_set_scrollable(o,false)
#else
#define hide_obj(o) lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN)
#define not_scrollable(o) lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE)
#endif
/* Small LVGL text raster, transposed without interpolation for vertical labels. */
/* All raster buffers are LVGL I1: an 8-byte palette, then MSB-first rows.
   Bit 1 is paper (white), bit 0 is ink (black), matching the 1-bit panel. */
#define I1_PALETTE 8
#define ROW_BYTES (UI_WIDTH/8)
#define FRAME_BYTES (ROW_BYTES*UI_HEIGHT)
#define AXIS_W 128
#define AXIS_H 16
static lv_obj_t *axis_label_canvas;
static uint8_t axis_label_buf[I1_PALETTE+AXIS_W/8*AXIS_H];
/* Transition workspace: three 15 KB frames, small enough for internal SRAM. */
static uint8_t scene_buf[2][I1_PALETTE+FRAME_BYTES];
static uint8_t transition_bits[FRAME_BYTES];
static bool transition_drawn;
#define SCENE_BITS(i) (scene_buf[i]+I1_PALETTE)
static lv_layer_t *layer;
static float offset;
static float duration=1.4f;
/* Portal anchor in Q8 pixels. */
static int32_t portal_x=UI_FLOWER_X<<8, portal_y=UI_FLOWER_Y<<8, portal_radius=25<<8;
static int portal_bubble=-1;
static float clamp(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}
static float ease(float v){v=clamp(v,0,1);return v*v*(3-2*v);}
static float mix(float a,float b,float p){return a+(b-a)*p;}
/* Per-measurement rating shown in each page's badge. Limits live in ui_theme.h
   and are placeholders until tuned. Missing readings have no rating. */
typedef enum { RATING_NONE, RATING_GOOD, RATING_FAIR, RATING_POOR } rating;
static const char *rating_text(rating r){
    static const char *text[]={"--","Good","Fair","Poor"};
    return text[r];
}
static rating rate_upper(float v,float fair,float poor){
    if(!isfinite(v) && !(isinf(v) && v>0))return RATING_NONE;
    return v>=poor?RATING_POOR:v>=fair?RATING_FAIR:RATING_GOOD;
}
static rating rate_band(float v,float good_lo,float good_hi,float fair_lo,float fair_hi){
    if(!isfinite(v) && !isinf(v))return RATING_NONE;
    if(v>=good_lo && v<=good_hi)return RATING_GOOD;
    return v>=fair_lo && v<=fair_hi?RATING_FAIR:RATING_POOR;
}
static rating rate_co2(void){return rate_upper(sensor.co2,UI_CO2_FAIR,UI_CO2_POOR);}
static rating rate_pm(int size){
    static const float fair[]={UI_PM1_FAIR,UI_PM25_FAIR,UI_PM4_FAIR,UI_PM10_FAIR};
    static const float poor[]={UI_PM1_POOR,UI_PM25_POOR,UI_PM4_POOR,UI_PM10_POOR};
    float v=size==0?sensor.pm1:size==1?sensor.pm:size==2?sensor.pm4:sensor.pm10;
    return rate_upper(v,fair[size],poor[size]);
}
static rating rate_gas(int lane){
    return lane?rate_upper(sensor.nox,UI_NOX_FAIR,UI_NOX_POOR):rate_upper(sensor.voc,UI_VOC_FAIR,UI_VOC_POOR);
}
static rating rate_climate(int key){
    return key?rate_band(sensor.rh,UI_RH_GOOD_LOW,UI_RH_GOOD_HIGH,UI_RH_FAIR_LOW,UI_RH_FAIR_HIGH):
        rate_band(sensor.temp,UI_TEMP_GOOD_LOW,UI_TEMP_GOOD_HIGH,UI_TEMP_FAIR_LOW,UI_TEMP_FAIR_HIGH);
}
/* Overview: the worst rating among the measurements on the detail pages. */
static rating rate_overall(void){
    rating r[]={rate_co2(),rate_pm(0),rate_pm(1),rate_pm(2),rate_pm(3),
        rate_gas(0),rate_gas(1),rate_climate(0),rate_climate(1)};
    rating worst=RATING_NONE;
    for(unsigned i=0;i<sizeof r/sizeof *r;i++)if(r[i]>worst)worst=r[i];
    return worst;
}
static bool warning(void){return rate_overall()==RATING_POOR;}
static lv_color_t color(bool reverse){return lv_color_hex(reverse?UI_COLOR_PAPER:UI_COLOR_INK);}

/* Direct fills whose clip area extended past the layer buffer (diagnostic). */
static unsigned clip_overruns;
static char overrun_detail[160];
/* Optional microsecond clock from the platform, for on-device profiling. */
static uint32_t (*clock_us)(void);
static uint32_t compose_us_total,compose_count;
/* Run any LVGL draw tasks still queued for a layer, so direct writes land on top
   of everything drawn before them. */
static void sync_layer(lv_layer_t *target){
    while(target->draw_task_head){
        lv_draw_dispatch_wait_for_request();
        if(!lv_draw_dispatch_layer(lv_obj_get_display(surface),target))lv_draw_dispatch_request();
    }
}
/* Square opaque fills on a 1-bit layer set bits directly instead of queuing an LVGL
   draw task: the artwork is made of hundreds of tiny spans, and per-task overhead
   dominated device frame time. Identical pixels to LVGL's own fill. */
static lv_obj_t *scene_canvas[2];
static const lv_draw_buf_t *scene_canvas_buf(int i){return scene_canvas[i]?lv_canvas_get_draw_buf(scene_canvas[i]):NULL;}
static bool direct_fill(int x,int y,int w,int h,bool paper){
    lv_draw_buf_t *buf=layer->draw_buf;
    if(!buf || buf->header.cf!=LV_COLOR_FORMAT_I1)return false;
    /* Reject far-off coordinates (e.g. from a NaN cast) before any arithmetic can overflow. */
    if(x<-4096 || x>4096 || y<-4096 || y>4096 || w<=0 || h<=0 || w>4096 || h>4096)return true;
    const lv_area_t *clip=&layer->_clip_area;
    lv_area_t a={LV_MAX(x,clip->x1),LV_MAX(y,clip->y1),LV_MIN(x+w-1,clip->x2),LV_MIN(y+h-1,clip->y2)};
    if(w<=0 || h<=0 || a.x1>a.x2 || a.y1>a.y2)return true;
    sync_layer(layer);
    lv_area_t b=layer->buf_area;
    /* Never write outside the layer's buffer, even if the clip area says otherwise. */
    lv_area_t inside={LV_MAX(a.x1,b.x1),LV_MAX(a.y1,b.y1),
        LV_MIN(a.x2,b.x1+(int)buf->header.w-1),LV_MIN(a.y2,b.y1+(int)buf->header.h-1)};
    if(inside.x1!=a.x1 || inside.y1!=a.y1 || inside.x2!=a.x2 || inside.y2!=a.y2){
        if(!clip_overruns++)snprintf(overrun_detail,sizeof overrun_detail,
            "fill %d,%d %dx%d clip %d,%d-%d,%d buf_area %d,%d-%d,%d buf %dx%d scene %d moving %d layer %s",
            x,y,w,h,(int)clip->x1,(int)clip->y1,(int)clip->x2,(int)clip->y2,(int)b.x1,(int)b.y1,(int)b.x2,(int)b.y2,
            (int)buf->header.w,(int)buf->header.h,scene,moving,
            buf==scene_canvas_buf(0)?"canvas0":buf==scene_canvas_buf(1)?"canvas1":"other");
    }
    a=inside;
    if(a.x1>a.x2 || a.y1>a.y2)return true;
    int x1=a.x1-b.x1,x2=a.x2-b.x1;
    for(int yy=a.y1;yy<=a.y2;yy++){
        uint8_t *row=lv_draw_buf_goto_xy(buf,0,(uint32_t)(yy-b.y1));
        int first=x1>>3,last=x2>>3;
        uint8_t head=(uint8_t)(0xff>>(x1&7)),tail=(uint8_t)(0xff<<(7-(x2&7)));
        if(first==last){
            uint8_t m=head&tail;row[first]=paper?(uint8_t)(row[first]|m):(uint8_t)(row[first]&~m);
            continue;
        }
        row[first]=paper?(uint8_t)(row[first]|head):(uint8_t)(row[first]&~head);
        if(last>first+1)memset(row+first+1,paper?0xff:0x00,(size_t)(last-first-1));
        row[last]=paper?(uint8_t)(row[last]|tail):(uint8_t)(row[last]&~tail);
    }
    return true;
}
static void rect(int x,int y,int w,int h,bool reverse,int radius){
    if(radius==0 && direct_fill(x,y+(int)offset,w,h,reverse))return;
    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);
    d.bg_color=color(reverse);d.bg_opa=255;d.radius=radius;
    lv_area_t a={x,y+(int)offset,x+w-1,y+(int)offset+h-1};lv_draw_rect(layer,&d,&a);
}
static void line(float x,float y,float xx,float yy,int width,bool reverse){
    lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);
    d.p1.x=(int)round(x);d.p1.y=(int)round(y+offset);
    d.p2.x=(int)round(xx);d.p2.y=(int)round(yy+offset);
    d.width=width;d.color=color(reverse);d.round_start=1;d.round_end=1;
    lv_draw_line(layer,&d);
}
static void circle(float x,float y,int r,int width,bool reverse){
    lv_draw_arc_dsc_t d;lv_draw_arc_dsc_init(&d);
    d.center.x=(int)round(x);d.center.y=(int)round(y+offset);
    d.radius=r;d.width=width;d.start_angle=0;d.end_angle=360;d.color=color(reverse);
    lv_draw_arc(layer,&d);
}
static float norm(float value,float low,float high){return clamp((value-low)/(high-low),0,1);}
static float field(atmos_readings v,int key){
    switch(key){case 0:return v.co2;case 1:return v.pm;case 2:return v.voc;
    case 3:return v.nox;case 4:return v.temp;default:return v.rh;}
}
static const lv_font_t *dashboard_font(int size){
    switch(size){case 12:return &ui_bold_12;case 112:return &ui_bold_112;case 96:return &ui_bold_96;case 80:return &ui_bold_80;case 72:return &ui_bold_72;
    case 40:return &ui_bold_40;case 32:return &ui_bold_32;
    case 28:return &ui_bold_28;case 24:return &ui_bold_24;default:return &ui_bold_16;}
}
static void dashboard_text(const char *s,int x,int y,int size,bool reverse){
    lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);
    d.font=dashboard_font(size);d.text=s;d.text_local=1;d.color=color(reverse);
    lv_area_t a={x,y+(int)offset,UI_WIDTH-1,y+(int)offset+100};lv_draw_label(layer,&d,&a);
}
/* Measure labels using the same font metrics LVGL uses to draw them. */
static int text_width(const char *s,const lv_font_t *font){
    lv_point_t size;lv_text_get_size(&size,s,font,0,0,1000,LV_TEXT_FLAG_NONE);return size.x;
}
/* Header strings are ASCII. Center their visible ink, not their line boxes. */
static lv_area_t header_ink_bounds(const char *text,const lv_font_t *font){
    lv_area_t bounds={1000,1000,-1000,-1000};
    int pen=0;
    for(const char *c=text;*c;c++){
        lv_font_glyph_dsc_t g;
        if(!lv_font_get_glyph_dsc(font,&g,(unsigned char)*c,(unsigned char)c[1]))continue;
        if(g.box_h && g.box_w){
            int y=font->line_height-font->base_line-g.box_h-g.ofs_y;
            bounds.x1=LV_MIN(bounds.x1,pen+g.ofs_x);
            bounds.x2=LV_MAX(bounds.x2,pen+g.ofs_x+g.box_w-1);
            bounds.y1=LV_MIN(bounds.y1,y);
            bounds.y2=LV_MAX(bounds.y2,y+g.box_h-1);
        }
        pen+=g.adv_w;
    }
    return bounds;
}
static void detail_header(const char *title,const char *badge){
    lv_area_t ink=header_ink_bounds(title,dashboard_font(UI_FONT_TITLE));
    int title_y=UI_PM_HEADER_Y+(UI_BADGE_HEIGHT-(ink.y2-ink.y1+1))/2-ink.y1;
    dashboard_text(title,UI_MARGIN-ink.x1,title_y,UI_FONT_TITLE,false);
    rect(UI_MARGIN,UI_HEADER_BOTTOM,UI_WIDTH-2*UI_MARGIN,UI_DIVIDER,false,0);
    if(!badge)return;
    int badge_size=16;
    ink=header_ink_bounds(badge,dashboard_font(badge_size));
    int text_x=UI_BADGE_X+(UI_BADGE_WIDTH-(ink.x2-ink.x1+1))/2-ink.x1;
    int text_y=UI_PM_HEADER_Y+(UI_BADGE_HEIGHT-(ink.y2-ink.y1+1))/2-ink.y1;
    rect(UI_BADGE_X,UI_PM_HEADER_Y,UI_BADGE_WIDTH,UI_BADGE_HEIGHT,false,0);
    dashboard_text(badge,text_x,text_y,badge_size,true);
}
static void dashboard_header(void){
    detail_header("AIR QUALITY",rating_text(rate_overall()));
}
static void status_flower(void){
    const int cx=UI_FLOWER_X,cy=UI_FLOWER_Y;
    circle(cx,cy,UI_FLOWER_RADIUS,UI_FLOWER_RING,false);
    /* Scan-convert the transformed petal geometry as opaque spans. This avoids
       shared-edge cracks from separately rasterized triangles. The exclusion
       disk is part of the geometry, so no petal can cover the fixed symbol. */
    float phase=fmod(anim_t*UI_FLOWER_SPEED,TAU);
    /* Petal rotations depend only on the frame, not the pixel: compute them once. */
    float petal_cos[UI_FLOWER_PETALS],petal_sin[UI_FLOWER_PETALS];
    for(int i=0;i<UI_FLOWER_PETALS;i++){
        float q=phase+i*TAU/UI_FLOWER_PETALS;petal_cos[i]=cos(q);petal_sin[i]=sin(q);
    }
    for(int y=-UI_FLOWER_RADIUS;y<=UI_FLOWER_RADIUS;y++){
        int start=-999;
        for(int x=-UI_FLOWER_RADIUS;x<=UI_FLOWER_RADIUS+1;x++){
            bool ink=false;
            if(x<=UI_FLOWER_RADIUS && x*x+y*y>=UI_SYMBOL_CLEARANCE*UI_SYMBOL_CLEARANCE){
                for(int i=0;i<UI_FLOWER_PETALS;i++){
                    float radial=(x*petal_cos[i]+y*petal_sin[i]-UI_PETAL_CENTER)/UI_PETAL_LENGTH;
                    float tangent=(-x*petal_sin[i]+y*petal_cos[i])/UI_PETAL_HALF_WIDTH;
                    if(radial*radial+tangent*tangent<=1){ink=true;break;}
                }
            }
            if(ink && start==-999)start=x;
            if(!ink && start!=-999){rect(cx+start,cy+y,x-start,1,false,0);start=-999;}
        }
    }
    if(warning()){
        line(cx,cy-18,cx+21,cy+18,3,false);
        line(cx+21,cy+18,cx-21,cy+18,3,false);
        line(cx-21,cy+18,cx,cy-18,3,false);
        line(cx,cy-5,cx,cy+6,4,false);circle(cx,cy+13,2,2,false);
    }else{
        /* Symmetric visible bounds around the flower center. */
        line(cx-18,cy+2,cx-7,cy+14,6,false);
        line(cx-7,cy+14,cx+18,cy-14,6,false);
    }
}
/* Shared centered text for measurement groups. Advance widths preserve the
   tabular numeric grid and support the UTF-8 unit glyphs in our font subset. */
static void centered_text(const char *text,int center,int y,int size,bool reverse){
    dashboard_text(text,center-text_width(text,dashboard_font(size))/2,y,size,reverse);
}
/* Fixed font roles: over-range numbers use a compact textual notation, never
   change font size as readings change. Missing values retain the same anchors. */
static void format_reading(char *out,size_t capacity,float value,int decimals,int size,int width){
    if(!isfinite(value)){snprintf(out,capacity,"--");return;}
    snprintf(out,capacity,"%.*f",decimals,value);
    if(text_width(out,dashboard_font(size))>width && decimals)snprintf(out,capacity,"%.0f",value);
    if(text_width(out,dashboard_font(size))>width && fabs(value)>=1000)snprintf(out,capacity,"%.1fk",value/1000);
    if(text_width(out,dashboard_font(size))>width && fabs(value)>=1000)snprintf(out,capacity,"%.0fk",value/1000);
    if(text_width(out,dashboard_font(size))>width)snprintf(out,capacity,"OVR");
}
static void primary_measurement(void){
    char value[32];format_reading(value,sizeof value,sensor.co2,0,80,UI_PRIMARY_WIDTH);
    int center=UI_MARGIN+UI_PRIMARY_WIDTH/2;
    centered_text("CO2",center,UI_MAIN_LABEL_Y,UI_FONT_LABEL,false);
    int size=80;
    centered_text(value,center,UI_MAIN_VALUE_Y,size,false);
    centered_text("PPM",center,UI_MAIN_UNIT_Y,UI_FONT_LABEL,false);
}
static void footer_measurement(int x,const char *label,float value,const char *unit){
    char s[32];format_reading(s,sizeof s,value,1,40,UI_COLUMN_WIDTH-2*UI_MARGIN);
    int center=x+(UI_COLUMN_WIDTH-2*UI_MARGIN)/2;
    centered_text(label,center,UI_FOOTER_LABEL_Y,UI_FONT_LABEL,true);
    centered_text(s,center,UI_FOOTER_BASELINE-ui_bold_40.line_height+ui_bold_40.base_line,40,true);
    centered_text(unit,center,UI_FOOTER_UNIT_Y,16,true);
}
static void draw_dashboard(void){
    dashboard_header();primary_measurement();status_flower();
    rect(0,UI_FOOTER_TOP,UI_WIDTH,UI_HEIGHT-UI_FOOTER_TOP,false,0);
    rect((UI_WIDTH-UI_FOOTER_DIVIDER)/2,UI_FOOTER_TOP+UI_FOOTER_PADDING,UI_FOOTER_DIVIDER,
        UI_HEIGHT-UI_FOOTER_TOP-2*UI_FOOTER_PADDING,true,0);
    footer_measurement(UI_MARGIN,"PM2.5",sensor.pm,"\xc2\xb5" "g/m\xc2\xb3");
    footer_measurement(UI_COLUMN_WIDTH+UI_MARGIN,"Temperature",sensor.temp,"\xc2\xb0" "C");
}
static int co2_bubble_radius(int j){
    static const int radii[UI_BUBBLE_COUNT]={6,11,16,23,8,13,5,9};return radii[j];
}
static void co2_bubble_position(int j,float *x,float *y){
    int r=co2_bubble_radius(j);
    float height=UI_BUBBLE_BOTTOM-UI_BUBBLE_TOP+2*UI_BUBBLE_MAX_RADIUS;
    float phase=fmod(anim_t*UI_BUBBLE_SPEED/height+(float)j/UI_BUBBLE_COUNT,1);
    /* Reserve the full sway envelope so the outer ring never crosses the clip. */
    float inset=r+UI_BUBBLE_SWAY+1;
    *x=mix(UI_BUBBLE_LEFT+inset,UI_BUBBLE_RIGHT-inset,fmod(j*.618034f,.999f))+sin(anim_t*.7f+j)*UI_BUBBLE_SWAY;
    *y=UI_BUBBLE_BOTTOM+UI_BUBBLE_MAX_RADIUS-phase*height;
    if(moving && j==portal_bubble){*x=portal_x;*y=portal_y;}
}
static void co2_live_bubbles(void){
    lv_area_t clip=layer->_clip_area;
    layer->_clip_area=(lv_area_t){LV_MAX(clip.x1,UI_BUBBLE_LEFT),LV_MAX(clip.y1,UI_BUBBLE_TOP),
        LV_MIN(clip.x2,UI_BUBBLE_RIGHT),LV_MIN(clip.y2,UI_BUBBLE_BOTTOM)};
    for(int j=0;j<UI_BUBBLE_COUNT;j++){
        float x,y;co2_bubble_position(j,&x,&y);int base=co2_bubble_radius(j);
        int r=(int)floor(fmin(base,fmin(UI_BUBBLE_BOTTOM-y,y-UI_BUBBLE_TOP)));
        if(r<2)continue;
        circle(x,y,r,UI_BUBBLE_STROKE,false);
        if(r>=11){
            lv_draw_arc_dsc_t d;lv_draw_arc_dsc_init(&d);
            d.center=(lv_point_t){(int)round(x),(int)round(y)};d.radius=r-5;
            d.width=2;d.start_angle=205;d.end_angle=255;d.color=color(false);d.rounded=1;
            lv_draw_arc(layer,&d);
        }
    }
    layer->_clip_area=clip;
}
static float chart_nice_step(float value){
    float power=pow(10,floor(log10(value))),n=value/power;
    return (n<=1?1:n<=2?2:n<=2.5f?2.5f:n<=5?5:10)*power;
}
/* User-selected solid medium-gray fill; paper trace stays visibly separate. */
static void chart_fill_span(int x,int top,int bottom){
    if(top>=bottom)return;
    _Static_assert(UI_COLOR_CHART_FILL==UI_COLOR_PAPER,"direct chart fill assumes paper");
    if(direct_fill(x,top,1,bottom-top,true))return;
    lv_draw_rect_dsc_t fill;lv_draw_rect_dsc_init(&fill);
    fill.bg_color=lv_color_hex(UI_COLOR_CHART_FILL);fill.bg_opa=255;
    lv_area_t area={x,top,x,bottom-1};lv_draw_rect(layer,&fill,&area);
}
static void history_panel(void){
    rect(0,UI_HISTORY_PANEL_Y,UI_WIDTH,UI_HEIGHT-UI_HISTORY_PANEL_Y,false,0);
}
/* One axis renderer for every history, including the animated PM scale. */
static void chart_tick(float value,float y){
    char label[32];
    if(fabs(value)>=10000)snprintf(label,sizeof label,"%.0fk",value/1000);
    else snprintf(label,sizeof label,fabs(value-round(value))>.01f?"%.1f":"%.0f",value);
    lv_area_t ink=header_ink_bounds(label,&ui_bold_12);
    dashboard_text(label,UI_CHART_LEFT-6-ink.x2,
        (int)round(y)-(ink.y2-ink.y1+1)/2-ink.y1,UI_FONT_AXIS,true);
    line(UI_CHART_LEFT-3,y,UI_CHART_LEFT,y,1,true);
}
static bool axis_paper(int x,int y){
    return axis_label_buf[I1_PALETTE+y*(AXIS_W/8)+(x>>3)]>>(7-(x&7))&1;
}
static void vertical_axis_label(const char *text,int center_x){
    lv_layer_t text_layer;lv_canvas_init_layer(axis_label_canvas,&text_layer);
    lv_draw_rect_dsc_t bg;lv_draw_rect_dsc_init(&bg);bg.bg_color=color(false);bg.bg_opa=255;
    lv_area_t full={0,0,AXIS_W-1,AXIS_H-1};lv_draw_rect(&text_layer,&bg,&full);
    lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);
    d.font=&ui_bold_12;d.text=text;d.text_local=1;d.color=color(true);
    lv_area_t area={0,0,AXIS_W-1,AXIS_H-1};lv_draw_label(&text_layer,&d,&area);
    lv_canvas_finish_layer(axis_label_canvas,&text_layer);
    int left=AXIS_W,right=-1,top=AXIS_H,bottom=-1;
    for(int y=0;y<AXIS_H;y++)for(int x=0;x<AXIS_W;x++)
        if(axis_paper(x,y)){
            left=LV_MIN(left,x);right=LV_MAX(right,x);
            top=LV_MIN(top,y);bottom=LV_MAX(bottom,y);
        }
    /* Exact quarter-turn: retain native glyph pixels and the output palette. */
    int center_y=(UI_CHART_TOP+UI_CHART_BOTTOM)/2;
    for(int x=left;x<=right;x++){
        int start=-1;
        for(int y=top;y<=bottom+1;y++){
            bool ink=y<=bottom && axis_paper(x,y);
            if(ink && start<0)start=y;
            if(!ink && start>=0){
                rect(center_x-(bottom-top+1)/2+start-top,
                    center_y+(right-left+1)/2-(x-left),y-start,1,true,0);
                start=-1;
            }
        }
    }
}
static void chart_axes(const char *name,const char *unit,float span){
    vertical_axis_label(unit,UI_CHART_UNIT_X);
    (void)name; /* Selected channel is already named by the unified header. */
    line(UI_CHART_LEFT,UI_CHART_TOP,UI_CHART_LEFT,UI_CHART_BOTTOM,1,true);
    line(UI_CHART_LEFT,UI_CHART_BOTTOM,UI_CHART_RIGHT,UI_CHART_BOTTOM,1,true);
    for(int i=0;i<=UI_CHART_X_INTERVALS;i++){
        int x=UI_CHART_LEFT+i*(UI_CHART_RIGHT-UI_CHART_LEFT)/UI_CHART_X_INTERVALS;
        char label[24];snprintf(label,sizeof label,"%.0f",span*(UI_CHART_X_INTERVALS-i)/UI_CHART_X_INTERVALS);
        lv_area_t ink=header_ink_bounds(label,&ui_bold_12);
        if(span>0 || i==UI_CHART_X_INTERVALS)
            dashboard_text(label,x-(ink.x2-ink.x1+1)/2-ink.x1,UI_CHART_TICK_Y-ink.y1,UI_FONT_AXIS,true);
        line(x,UI_CHART_BOTTOM,x,UI_CHART_BOTTOM+3,1,true);
    }
    dashboard_text("SEC AGO",(UI_CHART_LEFT+UI_CHART_RIGHT-text_width("SEC AGO",&ui_bold_12))/2,
        UI_CHART_CAPTION_Y,UI_FONT_AXIS,true);
}
static void detail_value(const char *value,int x,int y,int width){
    centered_text(value,x+width/2,y,UI_FONT_VALUE,false);
}
/* One chart component consumes real samples, a channel selector (or PM blend),
   and scale policy. Geometry never depends on the selected measurement. */
static float pm_weighted(atmos_readings value,const float *weights);
static float chart_sample(atmos_readings value,int key,const float *weights){
    return weights?pm_weighted(value,weights):field(value,key);
}
static float chart_interpolate(const sample *samples,int n,double at,int key,const float *weights){
    for(int i=1;i<n;i++)if(at<=samples[i].at){
        float a=chart_sample(samples[i-1].value,key,weights),b=chart_sample(samples[i].value,key,weights);
        if(!isfinite(a) || !isfinite(b) || (key!=4 && (a<0 || b<0)))return NAN;
        return mix(a,b,clamp((at-samples[i-1].at)/(samples[i].at-samples[i-1].at),0,1));
    }
    return n?chart_sample(samples[n-1].value,key,weights):NAN;
}
static void history_graph(const sample *samples,int n,int key,const float *weights,
                          const char *name,const char *unit,float low,float high){
    history_panel();
    for(int i=0;i<=UI_CHART_Y_INTERVALS;i++){
        float fraction=(float)i/UI_CHART_Y_INTERVALS;
        chart_tick(mix(low,high,fraction),mix(UI_CHART_BOTTOM,UI_CHART_TOP,fraction));
    }
    double first=n?samples[0].at:0,last=n?samples[n-1].at:0,span=last-first;
    if(!n){chart_axes(name,unit,span);return;}
    float previous=NAN,trace[UI_CHART_RIGHT-UI_CHART_LEFT+1];
    int begin=span>0?UI_CHART_LEFT:UI_CHART_RIGHT;
    for(int x=begin;x<=UI_CHART_RIGHT;x++){
        double at=span>0?first+(last-first)*(x-UI_CHART_LEFT)/(double)(UI_CHART_RIGHT-UI_CHART_LEFT):last;
        float value=chart_interpolate(samples,n,at,key,weights);
        if(!isfinite(value) || (key!=4 && value<0)){trace[x-UI_CHART_LEFT]=NAN;continue;}
        float y=clamp(UI_CHART_BOTTOM-(value-low)/(high-low)*(UI_CHART_BOTTOM-UI_CHART_TOP),UI_CHART_TOP,UI_CHART_BOTTOM);
        chart_fill_span(x,(int)ceil(y),UI_CHART_BOTTOM);
        trace[x-UI_CHART_LEFT]=y;
    }
    for(int x=begin;x<=UI_CHART_RIGHT;x++){
        float y=trace[x-UI_CHART_LEFT];
        if(!isfinite(y)){previous=NAN;continue;}
        if(isfinite(previous))line(x-1,previous,x,y,UI_CHART_LINE_WIDTH,true);
        else line(x,y,x,y,UI_CHART_LINE_WIDTH,true);
        previous=y;
    }
    chart_axes(name,unit,span);
}
static void history_chart(int key,const char *name,const char *unit,float minimum_range,float padding_ratio){
    float minimum=INFINITY,maximum=-INFINITY;
    for(int i=0;i<count;i++){
        float value=field(history[i].value,key);
        if(isfinite(value) && (key==4 || value>=0)){minimum=fmin(minimum,value);maximum=fmax(maximum,value);}
    }
    if(!isfinite(minimum)){minimum=0;maximum=minimum_range;}
    float range=fmax(minimum_range,maximum-minimum),padding=range*padding_ratio;
    float step=chart_nice_step((range+2*padding)/UI_CHART_Y_INTERVALS);
    float low=floor((minimum-padding)/step)*step;
    if(key!=4)low=fmax(0,low);
    float high=fmax(low+step,ceil((maximum+padding)/step)*step);
    history_graph(history,count,key,NULL,name,unit,low,high);
}
static void draw_co2_detail(void){
    char value[32];format_reading(value,sizeof value,sensor.co2,0,UI_CO2_VALUE_SIZE,UI_CO2_VALUE_WIDTH);
    detail_header("CARBON DIOXIDE",rating_text(rate_co2()));
    /* Center the visible value/unit block, not the fonts' padded line boxes.
       Canonical digit bounds keep its position stable as readings change. */
    lv_area_t digits=header_ink_bounds("0123456789",dashboard_font(UI_CO2_VALUE_SIZE));
    lv_area_t unit=header_ink_bounds("PPM",dashboard_font(UI_CO2_UNIT_SIZE));
    int content_top=UI_HEADER_BOTTOM+UI_DIVIDER;
    int unit_offset=digits.y2+1+UI_CO2_UNIT_GAP-unit.y1;
    int block_height=unit_offset+unit.y2-digits.y1+1;
    int value_y=content_top+(UI_HISTORY_PANEL_Y-content_top-block_height)/2-digits.y1;
    int center=UI_MARGIN+UI_CO2_VALUE_WIDTH/2;
    centered_text(value,center,value_y,UI_CO2_VALUE_SIZE,false);
    dashboard_text("PPM",center-(unit.x2-unit.x1+1)/2-unit.x1,value_y+unit_offset,UI_CO2_UNIT_SIZE,false);
    co2_live_bubbles();
    history_panel();
    history_chart(0,"CO2","PPM",UI_CHART_MIN_RANGE,UI_CHART_PADDING);
}
static float pm_value(atmos_readings value,int size){
    switch(size){case 0:return value.pm1;case 1:return value.pm;case 2:return value.pm4;default:return value.pm10;}
}
static float pm_weighted(atmos_readings value,const float *weights){
    float total=0;
    for(int i=0;i<4;i++)if(weights[i]>0)total+=pm_value(value,i)*weights[i];
    return total;
}
static void pm_chart_range(const sample *samples,int n,const float *weights,float *low,float *high){
    float lo=INFINITY,hi=-INFINITY;
    for(int i=0;i<n;i++){
        float value=pm_weighted(samples[i].value,weights);
        if(isfinite(value) && value>=0){lo=fmin(lo,value);hi=fmax(hi,value);}
    }
    if(!isfinite(lo)){lo=0;hi=UI_PM_CHART_MIN_RANGE;}
    float range=fmax(UI_PM_CHART_MIN_RANGE,hi-lo),step=chart_nice_step((range*(1+2*UI_PM_CHART_PADDING))/UI_PM_CHART_Y_INTERVALS);
    *low=fmax(0,floor((lo-range*UI_PM_CHART_PADDING)/step)*step);
    *high=fmax(*low+step,ceil((hi+range*UI_PM_CHART_PADDING)/step)*step);
}
void atmos_select_pm(int size){
    size=(size%4+4)%4;if(size==pm_selected)return;
    float q=ease(pm_blend_age/UI_PM_TRANSITION_TIME);
    for(int i=0;i<4;i++){pm_from_weights[i]=mix(pm_from_weights[i],pm_to_weights[i],q);pm_to_weights[i]=i==size?1:0;}
    if(pm_blend_age<UI_PM_TRANSITION_TIME){
        pm_from_low=mix(pm_from_low,pm_to_low,q);pm_from_high=mix(pm_from_high,pm_to_high,q);
    }else pm_chart_range(history,count,pm_from_weights,&pm_from_low,&pm_from_high);
    if(pm_blend_age>=UI_PM_TRANSITION_TIME){memcpy(pm_snapshot,history,sizeof history);pm_snapshot_count=count;}
    pm_chart_range(pm_snapshot,pm_snapshot_count,pm_to_weights,&pm_to_low,&pm_to_high);
    pm_selected=size;pm_blend_age=0;data_version++;lv_obj_invalidate(surface);
}
void atmos_cycle_pm(int direction){if(scene==2 && !moving)atmos_select_pm(pm_selected+direction);}
bool atmos_select_pm_at(int x,int y){
    if(moving)return false;
    if(scene==2 && y>=UI_PM_CARD_Y && y<UI_PM_CARD_Y+UI_PM_CARD_HEIGHT){
        for(int i=0;i<4;i++){
            int left=UI_MARGIN+i*(UI_PM_CARD_WIDTH+UI_PM_CARD_GAP);
            if(x>=left && x<left+UI_PM_CARD_WIDTH){atmos_select_pm(i);return true;}
        }
    }
    if(scene==3 && y>=UI_GAS_LABEL_Y && y<UI_HISTORY_PANEL_Y && x<UI_GAS_ART_DIVIDER_X){
        gas_selected=x>=UI_GAS_DIVIDER_X;data_version++;lv_obj_invalidate(surface);return true;
    }
    if(scene==4 && y>=UI_CLIMATE_LABEL_Y && y<UI_HISTORY_PANEL_Y){
        climate_selected=x>=UI_COLUMN_WIDTH;data_version++;lv_obj_invalidate(surface);return true;
    }
    return false;
}
static float pm_flow_y(float x,int lane){
    return UI_PM_FLOW_Y+lane*UI_PM_FLOW_GAP+sin((x-UI_PM_FLOW_LEFT)*UI_PM_FLOW_CURVATURE+lane*UI_PM_FLOW_LANE_PHASE-anim_t*1.8f)*UI_PM_FLOW_AMPLITUDE;
}
static void pm_fan_flow(void){
    const int cx=UI_PM_FAN_X,cy=UI_PM_FAN_Y;
    circle(cx,cy,UI_PM_FAN_RADIUS,UI_PM_FAN_STROKE,false);
    /* Four identical opaque radial blades, with one shared active-time phase. */
    float phase=fmod(anim_t*UI_PM_FAN_SPEED,TAU);
    for(int y=-UI_PM_FAN_RADIUS+UI_PM_FAN_INSET;y<UI_PM_FAN_RADIUS-3;y++){
        int start=-999;
        for(int x=-UI_PM_FAN_RADIUS;x<=UI_PM_FAN_RADIUS;x++){
            float r=hypot(x,y),q=remainder(atan2(y,x)-phase,TAU/4);
            bool ink=r<UI_PM_FAN_HUB || (r<UI_PM_FAN_RADIUS-UI_PM_FAN_INSET && fabs(q)<UI_PM_BLADE_HALF_ANGLE);
            if(ink && start==-999)start=x;
            if(!ink && start!=-999){rect(cx+start,cy+y,x-start,1,false,0);start=-999;}
        }
    }
    for(int lane=0;lane<UI_PM_FLOW_LANES;lane++){
        for(int x=UI_PM_FLOW_LEFT;x<UI_PM_FLOW_RIGHT;x+=2)
            line(x,pm_flow_y(x,lane),x+2,pm_flow_y(x+2,lane),2,false);
        float yy=pm_flow_y(UI_PM_FLOW_RIGHT,lane);
        float slope=UI_PM_FLOW_CURVATURE*UI_PM_FLOW_AMPLITUDE*
            cos((UI_PM_FLOW_RIGHT-UI_PM_FLOW_LEFT)*UI_PM_FLOW_CURVATURE+lane*UI_PM_FLOW_LANE_PHASE-anim_t*1.8f);
        float tx=1/sqrt(1+slope*slope),ty=slope*tx;
        for(int side=-1;side<=1;side+=2)
            line(UI_PM_FLOW_RIGHT-5*tx-side*3*ty,yy-5*ty+side*3*tx,UI_PM_FLOW_RIGHT,yy,2,false);
    }
    float cumulative[4],mass=0;
    for(int i=0;i<4;i++){
        float bin=fmax(0,pm_value(sensor,i)-(i?pm_value(sensor,i-1):0));
        mass+=bin;cumulative[i]=mass;
    }
    for(int j=0;j<UI_PM_PARTICLE_MAX;j++){
        /* Deterministic admission thresholds prevent random per-frame flicker. */
        if(j+.5f>pm_density)continue;
        float travel=UI_PM_FLOW_RIGHT-UI_PM_FLOW_LEFT-2*UI_PM_PARTICLE_PADDING;
        float x=UI_PM_FLOW_LEFT+UI_PM_PARTICLE_PADDING+fmod(anim_t*UI_PM_FLOW_SPEED*(1+(j%3)*UI_PM_PARTICLE_SPEED_VARIATION)+j*37.1f,travel);
        int lane=j%UI_PM_FLOW_LANES;
        float y=pm_flow_y(x,lane)+((j%2)?-7:7);
        float quantile=fmod(j*.618034f+.2f,1)*mass;int category=0;
        while(category<3 && quantile>cumulative[category])category++;
        int radius=category+1;
        if(radius==1)rect((int)round(x),(int)round(y),2,2,false,0);
        else circle(x,y,radius,1,false);
    }
}
static void pm_history_chart(void){
    bool blending=pm_blend_age<UI_PM_TRANSITION_TIME;
    const sample *samples=blending?pm_snapshot:history;int n=blending?pm_snapshot_count:count;
    float q=ease(pm_blend_age/UI_PM_TRANSITION_TIME),low,high;
    float weights[4];for(int i=0;i<4;i++)weights[i]=mix(pm_from_weights[i],pm_to_weights[i],q);
    if(blending){low=mix(pm_from_low,pm_to_low,q);high=mix(pm_from_high,pm_to_high,q);}
    else pm_chart_range(samples,n,weights,&low,&high);
    history_graph(samples,n,1,weights,pm_labels[pm_selected],"\xc2\xb5" "g/m\xc2\xb3",low,high);
}
static void pm_format(char *buffer,size_t size,float value){
    if(!isfinite(value) || value<0)snprintf(buffer,size,"--");
    else snprintf(buffer,size,"%.1f",value);
}
static void draw_pm_detail(void){
    const char *unit="\xc2\xb5" "g/m\xc2\xb3";
    detail_header("PARTICLES",rating_text(rate_pm(pm_selected)));
    int x;
    char value[32];pm_format(value,sizeof value,pm_value(sensor,pm_selected));
    format_reading(value,sizeof value,pm_value(sensor,pm_selected)<0?NAN:pm_value(sensor,pm_selected),1,72,UI_PM_VALUE_WIDTH);
    centered_text(value,UI_MARGIN+UI_PM_VALUE_WIDTH/2,UI_PM_VALUE_Y,72,false);
    centered_text(unit,UI_MARGIN+UI_PM_VALUE_WIDTH/2,UI_PM_UNIT_Y,UI_FONT_UNIT,false);pm_fan_flow();
    int common_size=UI_PM_CARD_VALUE_SIZE;
    for(int i=0;i<4;i++){
        x=UI_MARGIN+i*(UI_PM_CARD_WIDTH+UI_PM_CARD_GAP);bool selected=i==pm_selected;
        rect(x,UI_PM_CARD_Y,UI_PM_CARD_WIDTH,UI_PM_CARD_HEIGHT,false,2);
        if(!selected)rect(x+1,UI_PM_CARD_Y+1,UI_PM_CARD_WIDTH-2,UI_PM_CARD_HEIGHT-2,true,1);
        centered_text(pm_labels[i],x+UI_PM_CARD_WIDTH/2,UI_PM_CARD_Y+2,UI_PM_LABEL_SIZE,selected);
        format_reading(value,sizeof value,pm_value(sensor,i)<0?NAN:pm_value(sensor,i),1,24,UI_PM_CARD_WIDTH-2*UI_PM_CARD_PADDING);
        int card_size=common_size;
        centered_text(value,x+UI_PM_CARD_WIDTH/2,UI_PM_CARD_Y+15,card_size,selected);
        centered_text(unit,x+UI_PM_CARD_WIDTH/2,UI_PM_CARD_Y+40,UI_PM_CARD_UNIT_SIZE,selected);
    }
    pm_history_chart();
}
static void climate_chart(int key,int top){
    (void)top;
    history_chart(key,key==4?"Temperature":"Humidity",key==4?"\xC2\xB0" "C":"%",
        key==4?UI_CLIMATE_MIN_TEMP_RANGE:UI_CLIMATE_MIN_RH_RANGE,UI_CLIMATE_PADDING);
}
/* Artwork aligns with the value+unit block; labels occupy their own row.
   Canonical glyph bounds keep this axis stable across live readings. */
static int detail_art_center(void){
    lv_area_t value=header_ink_bounds("0123456789",dashboard_font(UI_FONT_VALUE));
    lv_area_t unit=header_ink_bounds("Index",dashboard_font(UI_FONT_UNIT));
    return (UI_DETAIL_VALUE_Y+value.y1+UI_DETAIL_UNIT_Y+unit.y2+1)/2;
}
static void climate_icons(void){
    int center=detail_art_center();
    int x=UI_CLIMATE_ICON_X,top=center-UI_CLIMATE_ICON_HEIGHT/2,
        bottom=center+UI_CLIMATE_ICON_HEIGHT/2,r=UI_CLIMATE_ICON_RADIUS;
    /* Shorter, wider silhouette: consistent three-pixel outline and round bulb. */
    rect(x-10,top,20,bottom-top-r,false,10);circle(x,bottom-r,r,r,false);
    rect(x-7,top+3,14,bottom-top-r-1,true,7);circle(x,bottom-r,r-3,r-3,true);
    float n=isfinite(sensor.temp)?norm(sensor.temp,UI_CLIMATE_TEMP_MIN,UI_CLIMATE_TEMP_MAX):0;
    int fill=(int)round((bottom-top-r-8)*n);
    rect(x-3,bottom-r-fill,6,fill,false,3);circle(x,bottom-r,r-6,r-6,false);
    x+=UI_COLUMN_WIDTH;
    /* Rasterize a pointed drop as opaque spans; wave is clipped by its silhouette. */
    int height=bottom-top;
    r=21; /* The droplet needs broader shoulders than the thermometer bulb. */
    /* The water surface depends only on the column: compute it once per column. */
    float level=bottom-(isfinite(sensor.rh)?norm(sensor.rh,0,100):0)*height;
    float surface_at[2*21+1];
    for(int xx=-r;xx<=r;xx++)
        surface_at[xx+r]=level+UI_CLIMATE_WAVE_AMPLITUDE*sin(xx*UI_CLIMATE_WAVE_LENGTH+anim_t*UI_CLIMATE_WAVE_SPEED);
    for(int yy=0;yy<=height;yy++){
        float q=(float)yy/height;
        float half=q<UI_CLIMATE_DROP_SHOULDER?r*q/UI_CLIMATE_DROP_SHOULDER:r*sqrt(fmax(0,1-pow((q-UI_CLIMATE_DROP_SHOULDER)/(1-UI_CLIMATE_DROP_SHOULDER),2)));
        int w=(int)round(half);
        if(w<1)continue;
        rect(x-w,top+yy,2*w+1,1,false,0);
        for(int xx=-w+UI_CLIMATE_STROKE;xx<=w-UI_CLIMATE_STROKE;xx++){
            if(top+yy<surface_at[xx+r] && yy<height-UI_CLIMATE_STROKE)rect(x+xx,top+yy,1,1,true,0);
        }
    }
}
static void draw_climate_detail(void){
    detail_header("CLIMATE",rating_text(rate_climate(climate_selected)));
    line(UI_COLUMN_WIDTH,UI_DETAIL_DIVIDER_TOP,UI_COLUMN_WIDTH,UI_DETAIL_DIVIDER_BOTTOM,UI_DIVIDER,false);
    for(int col=0;col<2;col++){
        int x=UI_MARGIN+col*UI_COLUMN_WIDTH;char value[32];float v=col?sensor.rh:sensor.temp;
        format_reading(value,sizeof value,v,col?0:1,40,UI_CLIMATE_VALUE_WIDTH);
        centered_text(col?"Humidity":"Temperature",x+UI_CLIMATE_VALUE_WIDTH/2,UI_CLIMATE_LABEL_Y,UI_CLIMATE_LABEL_SIZE,false);

        detail_value(value,x,UI_CLIMATE_VALUE_Y,UI_CLIMATE_VALUE_WIDTH);
        centered_text(col?"%":"\xC2\xB0" "C",x+UI_CLIMATE_VALUE_WIDTH/2,UI_CLIMATE_UNIT_Y,UI_CLIMATE_UNIT_SIZE,false);
    }
    climate_icons();history_panel();
    climate_chart(climate_selected?5:4,UI_CHART_TOP);
}
/* Opaque integer rasterization avoids antialiased edges on the two-tone panel. */
static void gas_line(float ax,float ay,float bx,float by,int width,bool reverse){
    int x=(int)round(ax),y=(int)round(ay),end_x=(int)round(bx),end_y=(int)round(by);
    int dx=abs(end_x-x),dy=-abs(end_y-y),sx=x<end_x?1:-1,sy=y<end_y?1:-1,error=dx+dy;
    for(;;){
        rect(x-width/2,y-width/2,width,width,reverse,0);
        if(x==end_x && y==end_y)break;
        int twice=2*error;
        if(twice>=dy){error+=dy;x+=sx;}
        if(twice<=dx){error+=dx;y+=sy;}
    }
}
/* The reference uses a single coherent S-shaped flow: two broad, pointed
   ribbons with a fine streamline between them. Motion bends that silhouette
   gently instead of translating a sine wave until its direction reverses. */
static float gas_wave(float u){
    float bend=.12f*sin(anim_t*UI_GAS_WAVE_SPEED);
    float phase=u+bend*sin(TAU*u/2);
    return UI_GAS_WAVE_AMPLITUDE*sin(TAU*phase)+2*sin(anim_t*.43f)*sin(TAU*u/2);
}
static void gas_ribbons(void){
    const int left=UI_GAS_RIBBON_LEFT,right=UI_GAS_RIBBON_RIGHT;
    float ribbon_top=detail_art_center()-UI_GAS_RIBBON_GAP;
    for(int lane=0;lane<3;lane++){
        float center=ribbon_top+lane*UI_GAS_RIBBON_GAP;
        float half=lane==1?.8f:UI_GAS_HALF_WIDTH+UI_GAS_WIDTH_GAIN*gas_intensity[lane/2];
        for(int x=left;x<=right;x++){
            float u=(float)(x-left)/(right-left);
            float y=center+gas_wave(u);
            if(lane==1){
                if(x>left)line(x-1,center+gas_wave((float)(x-1-left)/(right-left)),x,y,1,false);
                continue;
            }
            /* Both edges meet at the tips; broad shoulders carry visual weight. */
            /* sin(pi) rounds slightly below zero; pow() of a negative is NaN. */
            float taper=pow(fmax(0,sin(TAU*u/2)),.8f);
            float w=half*taper;
            int top=(int)ceil(y-w),bottom=(int)floor(y+w);
            if(bottom>=top)rect(x,top,1,bottom-top+1,false,0);
        }
    }
    /* Sparse suspended motes follow the same flow, clear of its silhouettes.
       Geometry shrinks at the ends, avoiding a visible wrap/reset. */
    for(int i=0;i<UI_GAS_PARTICLES;i++){
        float u=fmod(anim_t*UI_GAS_PARTICLE_SPEED/(right-left)+i*.618034f,1);
        float x=left+u*(right-left);
        float y=ribbon_top+gas_wave(u)+(i%2?2*UI_GAS_RIBBON_GAP+18:-18);
        float r=(i%3==0?2:1)*fmin(1,8*fmin(u,1-u));
        if(r<.65f)continue;
        circle(x,y,(int)round(r),1,false);
    }
}
static void gas_chart(int key,int top){
    (void)top;
    history_chart(key,key==2?"VOC":"NOx","Index",
        key==2?UI_GAS_CHART_MIN_VOC:UI_GAS_CHART_MIN_NOX,UI_GAS_CHART_PADDING);
}
static void draw_gas_detail(void){
    detail_header("GASES",rating_text(rate_gas(gas_selected)));
    gas_line(UI_GAS_DIVIDER_X,UI_DETAIL_DIVIDER_TOP,UI_GAS_DIVIDER_X,UI_DETAIL_DIVIDER_BOTTOM,UI_DIVIDER,false);

    char values[2][32];
    for(int col=0;col<2;col++){
        float v=col?sensor.nox:sensor.voc;
        format_reading(values[col],sizeof values[col],v,0,40,UI_GAS_VALUE_WIDTH);
    }
    for(int col=0;col<2;col++){
        int x=UI_MARGIN+col*UI_GAS_COLUMN_STEP,center=x+UI_GAS_VALUE_WIDTH/2;
        centered_text(col?"NOx":"VOC",center,UI_GAS_LABEL_Y,UI_GAS_LABEL_SIZE,false);

        centered_text(values[col],center,UI_GAS_VALUE_Y,40,false);
        centered_text("Index",center,UI_CLIMATE_UNIT_Y,UI_CLIMATE_UNIT_SIZE,false);
    }
    gas_ribbons();history_panel();
    gas_chart(gas_selected?3:2,UI_CHART_TOP);
}
static void draw_scene(int which){
    rect(0,0,UI_WIDTH,UI_HEIGHT,true,0);
    switch(which){case 0:draw_dashboard();break;case 1:draw_co2_detail();break;
        case 2:draw_pm_detail();break;case 3:draw_gas_detail();break;default:draw_climate_detail();break;}
}
/* Fixed-point transition maths: Q16 progress/scale, Q8 screen positions,
   angles as Q16 fractions of a turn, sines in Q14. No floating point per pixel. */
#define FX 65536
static const int16_t sine_table[257]={
0,402,804,1205,1606,2006,2404,2801,3196,3590,3981,4370,4756,5139,5520,5897,
6270,6639,7005,7366,7723,8076,8423,8765,9102,9434,9760,10080,10394,10702,11003,11297,
11585,11866,12140,12406,12665,12916,13160,13395,13623,13842,14053,14256,14449,14635,14811,14978,
15137,15286,15426,15557,15679,15791,15893,15986,16069,16143,16207,16261,16305,16340,16364,16379,
16384,16379,16364,16340,16305,16261,16207,16143,16069,15986,15893,15791,15679,15557,15426,15286,
15137,14978,14811,14635,14449,14256,14053,13842,13623,13395,13160,12916,12665,12406,12140,11866,
11585,11297,11003,10702,10394,10080,9760,9434,9102,8765,8423,8076,7723,7366,7005,6639,
6270,5897,5520,5139,4756,4370,3981,3590,3196,2801,2404,2006,1606,1205,804,402,
0,-402,-804,-1205,-1606,-2006,-2404,-2801,-3196,-3590,-3981,-4370,-4756,-5139,-5520,-5897,
-6270,-6639,-7005,-7366,-7723,-8076,-8423,-8765,-9102,-9434,-9760,-10080,-10394,-10702,-11003,-11297,
-11585,-11866,-12140,-12406,-12665,-12916,-13160,-13395,-13623,-13842,-14053,-14256,-14449,-14635,-14811,-14978,
-15137,-15286,-15426,-15557,-15679,-15791,-15893,-15986,-16069,-16143,-16207,-16261,-16305,-16340,-16364,-16379,
-16384,-16379,-16364,-16340,-16305,-16261,-16207,-16143,-16069,-15986,-15893,-15791,-15679,-15557,-15426,-15286,
-15137,-14978,-14811,-14635,-14449,-14256,-14053,-13842,-13623,-13395,-13160,-12916,-12665,-12406,-12140,-11866,
-11585,-11297,-11003,-10702,-10394,-10080,-9760,-9434,-9102,-8765,-8423,-8076,-7723,-7366,-7005,-6639,
-6270,-5897,-5520,-5139,-4756,-4370,-3981,-3590,-3196,-2801,-2404,-2006,-1606,-1205,-804,-402,
0};
static int32_t fx_sin(int32_t turn){
    uint32_t u=(uint32_t)turn;int i=(u>>8)&255,f=u&255;
    return sine_table[i]+(((sine_table[i+1]-sine_table[i])*f)>>8);
}
static int32_t fx_ease(int32_t p){
    if(p<=0)return 0;
    if(p>=FX)return FX;
    return (int32_t)((((int64_t)p*p)>>16)*(3*FX-2*p)>>16);
}
static int fx_round8(int64_t v){return (int)((v+128)>>8);}
static bool bit_at(const uint8_t *bits,int x,int y){
    if(x<0 || x>=UI_WIDTH || y<0 || y>=UI_HEIGHT)return true;
    return bits[y*ROW_BYTES+(x>>3)]>>(7-(x&7))&1;
}
static void put_bit(uint8_t *bits,int x,int y,bool paper){
    uint8_t mask=(uint8_t)(0x80>>(x&7));uint8_t *b=&bits[y*ROW_BYTES+(x>>3)];
    *b=paper?(uint8_t)(*b|mask):(uint8_t)(*b&~mask);
}
static bool is_portal(int from,int to){return from==0 || to==0 || (from==1 && to==2) || (from==2 && to==1);}
static void select_portal(int from,int to){
    portal_x=UI_FLOWER_X<<8;portal_y=UI_FLOWER_Y<<8;portal_radius=25<<8;
    if(from==0 || to==0)return;
    if((from==1 && to==2) || (from==2 && to==1)){
        /* Select an actual visible bubble; keep that one anchored during travel. */
        int best=-1;
        for(int j=0;j<UI_BUBBLE_COUNT;j++){
            float x,y;co2_bubble_position(j,&x,&y);int r=co2_bubble_radius(j);
            if(y-r>=UI_BUBBLE_TOP && y+r<=UI_BUBBLE_BOTTOM && r>best){
                best=r;portal_x=(int32_t)lround(x*256);portal_y=(int32_t)lround(y*256);portal_radius=r<<8;
            }
        }
    }
}
/* Reference compositors (the original per-pixel versions). The self-test requires
   the fast versions below to match them bit for bit. */
static void compose_portal_ref(int from,int to,int32_t progress){
    bool reverse=to==0 || (from==2 && to==1);
    int32_t q=fx_ease(reverse?FX-progress:progress);
    const uint8_t *outer=SCENE_BITS(reverse?1:0),*inner=SCENE_BITS(reverse?0:1);
    int32_t cx=portal_x+(int32_t)(((int64_t)((200<<8)-portal_x)*q)>>16);
    int32_t cy=portal_y+(int32_t)(((int64_t)((150<<8)-portal_y)*q)>>16);
    int32_t q2=(int32_t)(((int64_t)q*q)>>16);
    int64_t zoom=FX+18LL*q2,arrival=FX+18LL*(FX-q)/100;
    int64_t inv_zoom=((int64_t)FX<<16)/zoom,inv_arrival=((int64_t)FX<<16)/arrival;
    /* Reveal begins inside the selected symbol, then opens to all four corners. */
    int32_t opening=fx_ease((int32_t)((int64_t)(q-FX*8/100)*100/92));
    int64_t radius=((int64_t)opening*430)>>8,radius2=radius*radius;
    bool ring_on=from!=0 && to!=0 && q>0 && q<FX;
    int64_t ring=((int64_t)portal_radius*zoom)>>16,ring_in=ring-((2*zoom)>>8);
    int64_t ring2=ring*ring,ring_in2=ring_in*ring_in;
    for(int y=0;y<UI_HEIGHT;y++){
        int64_t dy=((int64_t)y<<8)-cy,dy2=dy*dy;
        int outer_y=fx_round8(portal_y+((dy*inv_zoom)>>16));
        int inner_y=fx_round8((150<<8)+((((int64_t)(y-150)*256)*inv_arrival)>>16));
        for(int x=0;x<UI_WIDTH;x++){
            int64_t dx=((int64_t)x<<8)-cx,rr=dx*dx+dy2;
            bool inside=opening>0 && rr<=radius2,p;
            if(inside)p=bit_at(inner,fx_round8((200<<8)+((((int64_t)(x-200)*256)*inv_arrival)>>16)),inner_y);
            else p=bit_at(outer,fx_round8(portal_x+((dx*inv_zoom)>>16)),outer_y);
            /* Carry the bubble's own outline outwards instead of introducing a cut. */
            if(ring_on && !inside && rr<ring2 && rr>ring_in2)p=false;
            put_bit(transition_bits,x,y,p);
        }
    }
}
static void compose_sweep_ref(int from,int to,int32_t progress){
    /* The horizontal gas ribbons travel vertically: enter downward, exit upward. */
    bool vertical=from==3 || to==3;
    bool reverse=vertical?from==3:to<from;
    bool airflow=!vertical && (from==2 || to==2);
    int32_t q=fx_ease(progress),envelope=fx_sin(q/2);
    int along=vertical?UI_HEIGHT:UI_WIDTH,cross=vertical?UI_WIDTH:UI_HEIGHT;
    int32_t spatial=airflow?261:177; /* .025 / .017 rad per pixel, in Q16 turns */
    int32_t phase=(int32_t)((int64_t)q*65/100),amplitude=airflow?32:72;
    int32_t edge_base=-100*256+(int32_t)(((int64_t)(along+200)*q)>>8);
    int32_t band=((airflow?4:14)*envelope)>>6;
    int sign=reverse?-1:1;
    int drift_in=(int)((28LL*(FX-q)+FX/2)>>16)*sign,drift_out=-(int)((28LL*q+FX/2)>>16)*sign;
    for(int c=0;c<cross;c++){
        int32_t wave=fx_sin((c-226)*spatial-phase);
        int32_t edge=edge_base+(int32_t)(((int64_t)((wave*amplitude)>>6)*envelope)>>14);
        for(int a=0;a<along;a++){
            int x=vertical?c:a,y=vertical?a:c;
            int32_t travel=(reverse?along-1-a:a)<<8;
            bool incoming=travel<edge;
            int drift=incoming?drift_in:drift_out;
            bool p=bit_at(SCENE_BITS(incoming?1:0),x+(vertical?0:drift),y+(vertical?drift:0));
            int32_t distance=travel>edge?travel-edge:edge-travel;
            /* One continuous seam, without decorative cross stitches. */
            if(distance<band)p=distance<358;
            put_bit(transition_bits,x,y,p);
        }
    }
}
/* Read one bit of a 1-bit frame; outside the frame is paper. */
static inline unsigned frame_bit(const uint8_t *bits,int x,int y){
    if((unsigned)x>=UI_WIDTH || (unsigned)y>=UI_HEIGHT)return 1;
    return bits[y*ROW_BYTES+(x>>3)]>>(7-(x&7))&1;
}
/* Fast compositors: identical maths to the references, but each output byte is built
   from 8 pixels in a register, per-row values are computed once, and the per-pixel
   64-bit multiplies become additions. */
/* Exact integer square root of a non-negative 64-bit value. */
static int64_t isqrt64(int64_t v){
    int64_t r=(int64_t)sqrt((double)v);
    while(r*r>v)r--;
    while((r+1)*(r+1)<=v)r++;
    return r;
}
/* Columns x whose dx=(x<<8)-cx satisfies dx*dx <= limit, as [*lo,*hi] (empty if lo>hi). */
static void circle_span(int64_t limit,int32_t cx,int *lo,int *hi){
    if(limit<0){*lo=1;*hi=0;return;}
    int64_t r=isqrt64(limit);
    int64_t a=cx-r,b=cx+r; /* x<<8 in [a,b] */
    int64_t first=a>=0?(a+255)>>8:-((-a)>>8),last=b>=0?b>>8:-((-b+255)>>8);
    *lo=(int)LV_MAX(first,-1);*hi=(int)LV_MIN(last,UI_WIDTH);
}
static void compose_portal(int from,int to,int32_t progress){
    bool reverse=to==0 || (from==2 && to==1);
    int32_t q=fx_ease(reverse?FX-progress:progress);
    const uint8_t *outer=SCENE_BITS(reverse?1:0),*inner=SCENE_BITS(reverse?0:1);
    int32_t cx=portal_x+(int32_t)(((int64_t)((200<<8)-portal_x)*q)>>16);
    int32_t cy=portal_y+(int32_t)(((int64_t)((150<<8)-portal_y)*q)>>16);
    int32_t q2=(int32_t)(((int64_t)q*q)>>16);
    int64_t zoom=FX+18LL*q2,arrival=FX+18LL*(FX-q)/100;
    int64_t inv_zoom=((int64_t)FX<<16)/zoom,inv_arrival=((int64_t)FX<<16)/arrival;
    int32_t opening=fx_ease((int32_t)((int64_t)(q-FX*8/100)*100/92));
    int64_t radius=((int64_t)opening*430)>>8,radius2=opening>0?radius*radius:-1;
    bool ring_on=from!=0 && to!=0 && q>0 && q<FX;
    int64_t ring=((int64_t)portal_radius*zoom)>>16,ring_in=ring-((2*zoom)>>8);
    int64_t ring2=ring*ring,ring_in2=ring_in*ring_in;
    /* The horizontal source position depends only on x: one table per frame.
       Out-of-frame samples read a paper byte. */
    static uint8_t ob[UI_WIDTH],om[UI_WIDTH],ib[UI_WIDTH],im[UI_WIDTH],of[UI_WIDTH],if_[UI_WIDTH];
    for(int x=0;x<UI_WIDTH;x++){
        int64_t dx=((int64_t)x<<8)-cx;
        int so=fx_round8(portal_x+((dx*inv_zoom)>>16));
        int si=fx_round8((200<<8)+((((int64_t)(x-200)*256)*inv_arrival)>>16));
        of[x]=(unsigned)so>=UI_WIDTH;ob[x]=of[x]?0:(uint8_t)(so>>3);om[x]=of[x]?0:(uint8_t)(0x80>>(so&7));
        if_[x]=(unsigned)si>=UI_WIDTH;ib[x]=if_[x]?0:(uint8_t)(si>>3);im[x]=if_[x]?0:(uint8_t)(0x80>>(si&7));
    }
    static const uint8_t paper_row[ROW_BYTES]={[0 ... ROW_BYTES-1]=0xff};
    for(int y=0;y<UI_HEIGHT;y++){
        int64_t dy=((int64_t)y<<8)-cy,dy2=dy*dy;
        int outer_y=fx_round8(portal_y+((dy*inv_zoom)>>16));
        int inner_y=fx_round8((150<<8)+((((int64_t)(y-150)*256)*inv_arrival)>>16));
        const uint8_t *orow=(unsigned)outer_y<UI_HEIGHT?outer+outer_y*ROW_BYTES:paper_row;
        const uint8_t *irow=(unsigned)inner_y<UI_HEIGHT?inner+inner_y*ROW_BYTES:paper_row;
        /* inside: dx^2 <= radius2-dy2. ring: ring_in2-dy2 < dx^2 < ring2-dy2. */
        int in_lo,in_hi,ring_lo=1,ring_hi=0,hole_lo=1,hole_hi=0;
        circle_span(radius2-dy2,cx,&in_lo,&in_hi);
        if(radius2<0){in_lo=1;in_hi=0;}
        if(ring_on){
            circle_span(ring2-dy2-1,cx,&ring_lo,&ring_hi);
            circle_span(ring_in2-dy2,cx,&hole_lo,&hole_hi);
        }
        uint8_t *out=transition_bits+y*ROW_BYTES;
        for(int xb=0,x=0;xb<ROW_BYTES;xb++){
            unsigned byte=0;
            for(int k=0;k<8;k++,x++){
                unsigned p;
                if(x>=in_lo && x<=in_hi)p=if_[x] | ((irow[ib[x]]&im[x])!=0);
                else{
                    p=of[x] | ((orow[ob[x]]&om[x])!=0);
                    /* Carry the bubble's own outline outwards instead of introducing a cut. */
                    if(x>=ring_lo && x<=ring_hi && !(x>=hole_lo && x<=hole_hi))p=0;
                }
                byte=byte<<1|p;
            }
            out[xb]=(uint8_t)byte;
        }
    }
}
/* 8 pixels of a 1-bit row starting at column x (any x); outside the row is paper. */
static inline uint8_t row_byte_at(const uint8_t *row,int x){
    if(!row)return 0xff;
    if(x>=0 && x+7<UI_WIDTH && (x&7)==0)return row[x>>3];
    unsigned v=0;
    for(int k=0;k<8;k++){
        int xx=x+k;
        v=v<<1|((unsigned)xx>=UI_WIDTH?1u:(unsigned)(row[xx>>3]>>(7-(xx&7))&1));
    }
    return (uint8_t)v;
}
static inline uint8_t row_byte_shifted(const uint8_t *row,int x){
    if(!row)return 0xff;
    if(x<0 || x+7>=UI_WIDTH)return row_byte_at(row,x);
    int b=x>>3,s=x&7;
    if(!s)return row[b];
    return (uint8_t)((row[b]<<s)|(row[b+1]>>(8-s)));
}
static void compose_sweep(int from,int to,int32_t progress){
    /* The horizontal gas ribbons travel vertically: enter downward, exit upward. */
    bool vertical=from==3 || to==3;
    bool reverse=vertical?from==3:to<from;
    bool airflow=!vertical && (from==2 || to==2);
    int32_t q=fx_ease(progress),envelope=fx_sin(q/2);
    int along=vertical?UI_HEIGHT:UI_WIDTH,cross=vertical?UI_WIDTH:UI_HEIGHT;
    int32_t spatial=airflow?261:177; /* .025 / .017 rad per pixel, in Q16 turns */
    int32_t phase=(int32_t)((int64_t)q*65/100),amplitude=airflow?32:72;
    int32_t edge_base=-100*256+(int32_t)(((int64_t)(along+200)*q)>>8);
    int32_t band=((airflow?4:14)*envelope)>>6;
    int sign=reverse?-1:1;
    int drift_in=(int)((28LL*(FX-q)+FX/2)>>16)*sign,drift_out=-(int)((28LL*q+FX/2)>>16)*sign;
    static int32_t edge_at[UI_WIDTH>UI_HEIGHT?UI_WIDTH:UI_HEIGHT];
    for(int c=0;c<cross;c++){
        int32_t wave=fx_sin((c-226)*spatial-phase);
        edge_at[c]=edge_base+(int32_t)(((int64_t)((wave*amplitude)>>6)*envelope)>>14);
    }
    for(int y=0;y<UI_HEIGHT;y++){
        /* Source rows: vertical sweeps drift rows, horizontal sweeps drift columns. */
        int yi=vertical?y+drift_in:y,yo=vertical?y+drift_out:y;
        const uint8_t *in_row=(unsigned)yi<UI_HEIGHT?SCENE_BITS(1)+yi*ROW_BYTES:NULL;
        const uint8_t *out_row=(unsigned)yo<UI_HEIGHT?SCENE_BITS(0)+yo*ROW_BYTES:NULL;
        int dxi=vertical?0:drift_in,dxo=vertical?0:drift_out;
        uint8_t *out=transition_bits+y*ROW_BYTES;
        for(int xb=0;xb<ROW_BYTES;xb++){
            unsigned incoming=0,in_band=0,seam=0;
            for(int k=0;k<8;k++){
                int x=xb*8+k,a=vertical?y:x;
                int32_t edge=edge_at[vertical?x:y],travel=(reverse?along-1-a:a)<<8;
                int32_t distance=travel>edge?travel-edge:edge-travel;
                incoming=incoming<<1|(travel<edge);
                in_band=in_band<<1|(distance<band);
                seam=seam<<1|(distance<358);
            }
            unsigned b=(row_byte_shifted(in_row,xb*8+dxi)&incoming)|(row_byte_shifted(out_row,xb*8+dxo)&~incoming);
            /* One continuous seam, without decorative cross stitches. */
            out[xb]=(uint8_t)((b&~in_band)|(seam&in_band));
        }
    }
}
static void compose_transition(int from,int to,int32_t progress){
    /* Exact endpoints avoid a one-frame jump when returning to direct rendering. */
    if(progress<=0){memcpy(transition_bits,SCENE_BITS(0),FRAME_BYTES);return;}
    if(progress>=FX){memcpy(transition_bits,SCENE_BITS(1),FRAME_BYTES);return;}
    if(is_portal(from,to))compose_portal(from,to,progress);
    else compose_sweep(from,to,progress);
}
static void render_scene_buffer(int index,int which){
    lv_layer_t offscreen;lv_canvas_init_layer(scene_canvas[index],&offscreen);
    layer=&offscreen;offset=0;draw_scene(which);
    lv_canvas_finish_layer(scene_canvas[index],&offscreen);
}
/* Finish LVGL's pending work on the output layer, then write the composed
   1-bit frame straight into it in whatever format the display uses (I1 on the
   bare panel, RGB565 under ESPHome). Ink and paper are pure black and white,
   so every format is written as all-zero or all-one bytes (alpha stays opaque). */
static void blit_transition(lv_layer_t *output){
    sync_layer(output);
    lv_draw_buf_t *buf=output->draw_buf;lv_area_t a=output->buf_area;
    lv_color_format_t cf=buf->header.cf;
    int bpp=lv_color_format_get_bpp(cf);
    bool alpha=cf==LV_COLOR_FORMAT_ARGB8888;
    int x1=LV_MAX(a.x1,0),x2=LV_MIN(a.x2,UI_WIDTH-1);
    for(int y=LV_MAX(a.y1,0);y<=LV_MIN(a.y2,UI_HEIGHT-1);y++){
        uint8_t *row=lv_draw_buf_goto_xy(buf,(uint32_t)(x1-a.x1),(uint32_t)(y-a.y1));
        const uint8_t *bits=transition_bits+y*ROW_BYTES;
        if(bpp==1){memcpy(row,bits+x1/8,(size_t)(x2-x1+1)/8);continue;}
        int bytes=bpp/8;
        for(int x=x1;x<=x2;x++,row+=bytes){
            memset(row,bits[x>>3]>>(7-(x&7))&1?0xff:0x00,(size_t)bytes);
            if(alpha)row[3]=0xff;
        }
    }
}
static lv_area_t animated_area(int which);
static bool area_within(const lv_area_t *in,const lv_area_t *out){
    return in->x1>=out->x1 && in->y1>=out->y1 && in->x2<=out->x2 && in->y2<=out->y2;
}
/* Redraw of only the animated box: run just that scene's artwork. Static content
   inside the box must be drawn here too (the self-test compares with full redraws). */
static void draw_animated(int which){
    rect(0,0,UI_WIDTH,UI_HEIGHT,true,0);
    switch(which){case 0:status_flower();break;case 1:co2_live_bubbles();break;
        case 2:pm_fan_flow();break;case 3:gas_ribbons();break;default:climate_icons();break;}
}
static void draw(lv_event_t *event){
    lv_layer_t *output=lv_event_get_layer(event);layer=output;offset=0;transition_drawn=false;
    if(!moving){
        anim_t=t;
        lv_area_t box=animated_area(scene);
        if(area_within(&output->_clip_area,&box))draw_animated(scene);else draw_scene(scene);
        return;
    }
    if(!snapshots_ready){
        double start=t-movement;
        anim_t=start;render_scene_buffer(0,scene);
        anim_t=start+duration;render_scene_buffer(1,destination);
        anim_t=t;snapshots_ready=true;
    }
    uint32_t compose_start=clock_us?clock_us():0;
    compose_transition(scene,destination,(int32_t)(clamp(movement/duration,0,1)*FX));
    if(clock_us){compose_us_total+=clock_us()-compose_start;compose_count++;}
    layer=output;blit_transition(output);transition_drawn=true;
}
const uint8_t *atmos_transition_frame(void){return transition_drawn?transition_bits:NULL;}
/* Screen box containing everything that moves in a still scene. Anything that
   changes only with readings, history or selection triggers a full redraw instead.
   The simulator self-test checks every partial redraw against a full one. */
static lv_area_t animated_area(int which){
    int c=detail_art_center();
    switch(which){
    case 0: return (lv_area_t){UI_FLOWER_X-UI_FLOWER_RADIUS-2,UI_FLOWER_Y-UI_FLOWER_RADIUS-2,
                               UI_FLOWER_X+UI_FLOWER_RADIUS+2,UI_FLOWER_Y+UI_FLOWER_RADIUS+2};
    case 1: return (lv_area_t){UI_BUBBLE_LEFT-2,UI_BUBBLE_TOP-2,UI_BUBBLE_RIGHT+2,UI_BUBBLE_BOTTOM+2};
    case 2: return (lv_area_t){UI_PM_FAN_X-UI_PM_FAN_RADIUS-4,58,UI_PM_FLOW_RIGHT+4,132};
    case 3: return (lv_area_t){UI_GAS_RIBBON_LEFT-4,c-66,UI_GAS_RIBBON_RIGHT+4,LV_MIN(c+66,UI_HISTORY_PANEL_Y-1)};
    default: return (lv_area_t){UI_CLIMATE_ICON_X+UI_COLUMN_WIDTH-24,c-UI_CLIMATE_ICON_HEIGHT/2-2,
                                UI_CLIMATE_ICON_X+UI_COLUMN_WIDTH+24,c+UI_CLIMATE_ICON_HEIGHT/2+2};
    }
}
static void invalidate_frame(void){
    if(moving || full_dirty || pm_blend_age<UI_PM_TRANSITION_TIME){
        lv_obj_invalidate(surface);full_dirty=false;
    }else{
        lv_area_t a=animated_area(scene);lv_obj_invalidate_area(surface,&a);
    }
}
static void take_sample(void){
    if(!external_data){
        float p=ease((t-changed)/3);
        sensor=(atmos_readings){mix(origin.co2,target.co2,p)+sin(t*.16f)*7,
            mix(origin.pm,target.pm,p)+sin(t*.16f+1)*.22f,
            mix(origin.voc,target.voc,p)+sin(t*.16f+2)*2,
            mix(origin.nox,target.nox,p),mix(origin.temp,target.temp,p)+sin(t*.16f+4)*.1f,
            mix(origin.rh,target.rh,p)+sin(t*.16f+5)*.5f,
            mix(origin.pm1,target.pm1,p)+sin(t*.19f+.4f)*.12f,
            mix(origin.pm4,target.pm4,p)+sin(t*.13f+1.8f)*.28f,
            mix(origin.pm10,target.pm10,p)+sin(t*.11f+2.4f)*.38f};
    }
    if(count==62){memmove(history,history+1,61*sizeof *history);count--;}
    history[count++]=(sample){t,sensor};full_dirty=true;data_version++;
}
void atmos_init(void){
    axis_label_canvas=lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(axis_label_canvas,axis_label_buf,AXIS_W,AXIS_H,LV_COLOR_FORMAT_I1);
    hide_obj(axis_label_canvas);
    sensor=origin=target=profiles[0];count=scene=destination=scenario=gas_selected=climate_selected=0;queued=-1;
    pm_selected=1;pm_blend_age=UI_PM_TRANSITION_TIME;for(int i=0;i<4;i++)pm_from_weights[i]=pm_to_weights[i]=i==1?1:0;pm_density=6;
    gas_intensity[0]=norm(sensor.voc,0,UI_GAS_VOC_SCALE);gas_intensity[1]=norm(sensor.nox,0,UI_GAS_NOX_SCALE);
    t=age=movement=anim_t=0;changed=-10;next_sample=2;paused=hidden=external_data=moving=false;automatic=true;
    full_dirty=true;snapshots_ready=false;pre_stage=0;pre_from=pre_to=-1;data_version=0;prerendered_transitions=0;
    surface=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(surface);
    lv_obj_set_size(surface,400,300);not_scrollable(surface);
    lv_obj_add_event_cb(surface,draw,LV_EVENT_DRAW_MAIN,NULL);
    lv_obj_set_style_bg_color(lv_screen_active(),color(true),0);
    /* The scene surface covers every pixel, so skip LVGL's screen fill (on a 1-bit
       display it sets one bit at a time: a large part of device frame time). */
    lv_obj_set_style_bg_opa(lv_screen_active(),LV_OPA_TRANSP,0);
    for(int i=0;i<2;i++){
        scene_canvas[i]=lv_canvas_create(lv_screen_active());
        lv_canvas_set_buffer(scene_canvas[i],scene_buf[i],UI_WIDTH,UI_HEIGHT,LV_COLOR_FORMAT_I1);
        hide_obj(scene_canvas[i]);
    }
}
static float transition_duration(int from,int to){return is_portal(from,to)?1.4f:1.2f;}
static void prerender_step(void){
    if(!automatic || pm_blend_age<UI_PM_TRANSITION_TIME){pre_stage=0;return;}
    double start=t+(6.5f-age);
    int to=(scene+1)%5;
    if(pre_from!=scene || pre_to!=to || pre_version!=data_version || fabs(pre_start-start)>1e-4){
        pre_stage=0;pre_from=scene;pre_to=to;pre_version=data_version;pre_start=start;
    }
    /* Wait for the last sample before the transition so the snapshot shows its data. */
    if(pre_stage==2 || next_sample<=start+1e-4)return;
    if(pre_stage==0){anim_t=start;render_scene_buffer(0,scene);prerendered_last_step=scene;}
    else{anim_t=start+transition_duration(scene,to);render_scene_buffer(1,to);prerendered_last_step=to;}
    anim_t=t;pre_stage++;
}
void atmos_step(double dt){
    if(paused || hidden || dt<=0)return;
    /* Limit catch-up after suspension; normal operation advances at active time. */
    dt=fmin(dt,.1f);t+=dt;prerendered_last_step=-1;
    pm_blend_age=fmin(UI_PM_TRANSITION_TIME,pm_blend_age+dt);
    float concentration=isfinite(sensor.pm)?clamp(sensor.pm,0,UI_PM_DENSITY_LIMIT):0;
    float density_target=UI_PM_DENSITY_MIN+(UI_PM_PARTICLE_MAX-UI_PM_DENSITY_MIN)*
        sqrt(concentration/(concentration+UI_PM_DENSITY_HALF));
    pm_density+=(density_target-pm_density)*(1-exp(-dt/UI_PM_DENSITY_TIME));
    for(int lane=0;lane<2;lane++){
        float value=lane?sensor.nox:sensor.voc;
        float target_intensity=isfinite(value)?norm(value,0,lane?UI_GAS_NOX_SCALE:UI_GAS_VOC_SCALE):0;
        gas_intensity[lane]+=(target_intensity-gas_intensity[lane])*(1-exp(-dt/UI_GAS_INTENSITY_TIME));
    }
    if(t+1e-7f>=next_sample){take_sample();next_sample+=2;}
    while(count && t-history[0].at>120){memmove(history,history+1,(--count)*sizeof *history);}
    if(moving){
        movement+=dt;
        if(movement>=duration){scene=destination;moving=false;age=0;full_dirty=true;
            if(queued>=0){int requested=queued;queued=-1;atmos_set_scene(requested);}}
    }else{age+=dt;prerender_step();if(automatic && age>=6.5f)atmos_navigate(1);}
    invalidate_frame();
}
void atmos_set_scene(int requested){
    requested=(requested%5+5)%5;
    if(moving){queued=requested;return;}
    if(requested==scene)return;
    duration=transition_duration(scene,requested);
    /* Use the hold-time snapshots when they match; the transition clock then starts
       at their time so the end still meets the live scene exactly. */
    bool ready=pre_stage==2 && pre_from==scene && pre_to==requested && pre_version==data_version && pre_start<=t;
    double start=ready?pre_start:t;
    /* The anchored bubble must match the CO2 snapshot: start time when leaving CO2,
       end time when arriving at it. */
    anim_t=scene==1?start:start+duration;select_portal(scene,requested);anim_t=t;
    destination=requested;movement=t-start;moving=true;snapshots_ready=ready;pre_stage=0;
    if(ready)prerendered_transitions++;
}
void atmos_navigate(int direction){atmos_set_scene((queued>=0?queued:moving?destination:scene)+direction);}
void atmos_set_scenario(int value){
    if(value<0 || value>3)return;
    scenario=value;origin=sensor;target=profiles[value];changed=t;external_data=false;
}
void atmos_set_readings(atmos_readings value){sensor=value;external_data=true;full_dirty=true;data_version++;}
void atmos_toggle_pause(void){paused=!paused;}
void atmos_toggle_auto(void){automatic=!automatic;}
void atmos_set_hidden(bool value){hidden=value;}
bool atmos_is_moving(void){return moving;}
unsigned atmos_prerendered_transitions(void){return prerendered_transitions;}
int atmos_scene(void){return scene;}
unsigned atmos_clip_overruns(void){return clip_overruns;}
const char *atmos_overrun_detail(void){return overrun_detail;}
void atmos_set_clock(uint32_t (*us)(void)){clock_us=us;}
float atmos_compose_ms(void){return compose_count?compose_us_total/1000.0f/compose_count:0;}
int atmos_destination(void){return destination;}
int atmos_step_prerendered(void){return prerendered_last_step;}
int atmos_self_test(void){
    if(count!=0)return 1;
    automatic=false;
    for(int i=0;i<80;i++)atmos_step(.025f);
    if(count!=1 || fabs(history[0].at-2)>.001f)return 2;
    sample first=history[0];atmos_set_scenario(2);
    for(int i=0;i<160;i++)atmos_step(.025f);
    if(memcmp(&first,history,sizeof first)!=0 || !warning())return 3;
    double before=t;int samples=count;atmos_toggle_pause();atmos_step(.1f);atmos_toggle_pause();
    atmos_set_hidden(true);atmos_step(.1f);atmos_set_hidden(false);
    if(t!=before || count!=samples)return 4;
    atmos_set_scene(1);atmos_set_scene(2);atmos_set_scene(4);
    for(int i=0;i<128;i++)atmos_step(.025f);
    if(scene!=4 || moving || queued!=-1)return 5;
    for(int i=0;i<5000;i++)atmos_step(.025f);
    if(count<59 || count>61 || t-history[0].at>120)return 6;
    atmos_set_readings((atmos_readings){800,2,40,1,23,50,1.1f,3.4f,5.8f});
    for(int i=0;i<80;i++)atmos_step(.025f);
    if(sensor.co2!=800 || history[count-1].value.co2!=800 ||
        history[count-1].value.pm1!=1.1f || history[count-1].value.pm4!=3.4f || history[count-1].value.pm10!=5.8f)return 7;
    atmos_select_pm(0);double fixed_time=pm_snapshot[0].at;
    atmos_step(.1f);atmos_select_pm(3);
    if(pm_snapshot[0].at!=fixed_time || pm_selected!=3)return 17;
    for(int i=0;i<4;i++)atmos_step(.1f);
    float selected_weights[4]={0,0,0,1};
    if(pm_weighted(sensor,selected_weights)!=5.8f)return 18;
    atmos_select_pm(1);for(int i=0;i<4;i++)atmos_step(.1f);
    /* Every directed pair must meet the real, live scenes at both endpoints. */
    for(int from=0;from<5;from++)for(int to=0;to<5;to++)if(from!=to){
        select_portal(from,to);render_scene_buffer(0,from);render_scene_buffer(1,to);
        compose_transition(from,to,0);
        if(memcmp(transition_bits,SCENE_BITS(0),FRAME_BYTES))return 8;
        compose_transition(from,to,FX);
        if(memcmp(transition_bits,SCENE_BITS(1),FRAME_BYTES))return 9;
        static uint8_t reference[FRAME_BYTES];
        const int32_t probes[]={1,FX/9,FX/4,FX/3,FX/2,FX*2/3,FX*3/4,FX*9/10,FX-1};
        for(unsigned k=0;k<sizeof probes/sizeof *probes;k++){
            if(is_portal(from,to))compose_portal_ref(from,to,probes[k]);else compose_sweep_ref(from,to,probes[k]);
            memcpy(reference,transition_bits,FRAME_BYTES);
            compose_transition(from,to,probes[k]);
            if(memcmp(reference,transition_bits,FRAME_BYTES))return 26;
        }
    }
    sample gap_samples[3]={
        {0,{.pm4=1}}, {2,{.pm4=NAN}}, {4,{.pm4=3}}};
    float gap_weights[4]={0,0,1,0},gap_low,gap_high;
    if(isfinite(chart_interpolate(gap_samples,3,1,1,gap_weights)) ||
        isfinite(chart_interpolate(gap_samples,3,3,1,gap_weights)))return 21;
    sample known[2]={{0,{.co2=400,.temp=-10,.pm4=2}},{2,{.co2=800,.temp=10,.pm4=6}}};
    if(chart_interpolate(known,2,1,0,NULL)!=600 ||
       chart_interpolate(known,2,1,4,NULL)!=0 ||
       chart_interpolate(known,2,1,1,gap_weights)!=4)return 27;
    pm_chart_range(gap_samples,3,gap_weights,&gap_low,&gap_high);
    if(!isfinite(gap_low) || !isfinite(gap_high) || gap_high<=gap_low)return 22;
    {
        /* Automatic cycling pre-renders the next transition during the hold. Its
           snapshots must equal fresh renders at the snapshot times. */
        automatic=true;
        for(int i=0;i<2000 && pre_stage<2;i++)atmos_step(.025f);
        if(pre_stage!=2)return 23;
        static uint8_t saved[2][I1_PALETTE+FRAME_BYTES];
        memcpy(saved,scene_buf,sizeof saved);
        anim_t=pre_start;render_scene_buffer(0,pre_from);
        anim_t=pre_start+transition_duration(pre_from,pre_to);render_scene_buffer(1,pre_to);anim_t=t;
        if(memcmp(saved,scene_buf,sizeof saved))return 24;
        unsigned before=prerendered_transitions;
        for(int i=0;i<400 && !moving;i++)atmos_step(.025f);
        if(!moving || prerendered_transitions!=before+1 || !snapshots_ready)return 25;
        while(moving)atmos_step(.025f);
        automatic=false;
    }
    puts("Prerender: hold-time transition snapshots match fresh renders and are used.");
    puts("PM: independent history channels, rapid selection, fixed transition window and missing-sample gaps passed.");
    puts("Model: sampling, history, pruning, navigation, pause, visibility and external data passed.");
    puts("Transition endpoints: all 20 directed scene pairs match their live LVGL frames exactly.");
    puts("Compositor: fast portal and sweep match the reference compositors at 9 progress points for all 20 pairs.");
    return 0;
}

