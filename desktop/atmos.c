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
#include "splash_badge.h"
#include <tgmath.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#define TAU 6.283185307179586f
_Static_assert(UI_CHART_LEFT==UI_PM_CHART_LEFT && UI_CHART_RIGHT==UI_PM_CHART_RIGHT &&
    UI_CHART_TOP==UI_PM_CHART_TOP && UI_CHART_BOTTOM==UI_PM_CHART_BOTTOM,
    "single history graphs must share exact geometry");
typedef struct { double at; atmos_readings value; } sample;
static const atmos_readings profiles[] = {
    {624,3.2f,78,1,22.4f,46,2.1f,4.0f,5.3f,1013.2f,9}, {1380,7.8f,126,4,24.1f,58,4.6f,9.5f,13.1f,1008.6f,24},
    {925,38.6f,218,27,25.6f,63,21.2f,46.9f,61.3f,1004.9f,46}, {438,1.4f,47,1,20.6f,42,.8f,1.9f,2.8f,1019.4f,4},
    /* Sensor full scale: SCD4x CO2, SEN6x PM, VOC/NOx index, temperature, humidity, pressure and SEN68 HCHO. */
    {40000,1000,500,500,60,100,1000,1000,1000,1100,1000}
};
static atmos_readings sensor, origin, target;
#define HISTORY_CAP 64
static sample history[HISTORY_CAP];
/* Seconds held per page, and the history span. */
static float hold=6.5f,history_window=120;
static int count, scene, destination, queued=-1, scenario;
static double t;
/* Sampling starts every 2 s and stretches with the history held (about 30 samples
   across it) up to span/60, so a long span fills in from the first seconds instead
   of waiting a whole interval. The buffer thins evenly when it overflows. */
static double sample_every(void){
    double held=count?t-history[0].at:0;
    return fmax(2,fmin(held/30,history_window/60.0));
}
static int gas_selected,climate_selected;
static void select_gas(int lane);
static void select_climate(int row);
static int pending_advance;
static int gas_divider_x=UI_GAS_DIVIDER_X; /* Set by draw_gas_detail() for hit-testing. */
static double changed, next_sample, age, movement;
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
/* The first transition after a splash opens from it onto the first page. */
static bool intro;
static int pm_selected=1,pm_snapshot_count;
static float pm_from_weights[4]={0,1,0,0},pm_to_weights[4]={0,1,0,0};
static const char *pm_labels[]={"PM1.0","PM2.5","PM4.0","PM10"};
static sample pm_snapshot[64];
static float pm_blend_age=.4f,pm_density;
static int64_t pm_last_tick=-1; /* particle drift tick last redrawn */
static float gas_intensity[2];
static float hcho_grow[UI_HCHO_MOLECULES]; /* 0 absent .. 1 full size */
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
/* Page order: scene numbers by position, grouped from "open a window?" to comfort:
   overview, CO2, gases, formaldehyde, particles, climate. The overview (position 0)
   is left out of the cycle for now (CYCLE_FIRST); its code stays, and key 1 still
   selects it directly. Scene numbers themselves are fixed identifiers. */
static const int page_order[ATMOS_SCENES]={0,1,3,5,2,4};
#define CYCLE_FIRST 1
#define SCENE_BITS(i) (scene_buf[i]+I1_PALETTE)
static lv_layer_t *layer;
static float offset;
static float duration=1.4f;
/* Portal anchor in Q8 pixels. */
static int32_t portal_x=UI_FLOWER_X<<8, portal_y=UI_FLOWER_Y<<8, portal_radius=25<<8;
static int portal_bubble=-1;
/* Artwork recorded by the latest draw of each scene. Transitions take their anchors
   from it right after rasterising the incoming snapshot. */
static int16_t pm_dust[UI_PM_PARTICLE_MAX][3]; /* x, y, radius */
static int pm_dust_count;
static float hcho_atoms[UI_HCHO_MOLECULES][3]; /* oxygen x, y, radius */
static int hcho_atom_count;
static float gas_ribbon_top;
static float clamp(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}
static float ease(float v){v=clamp(v,0,1);return v*v*(3-2*v);}
static float mix(float a,float b,float p){return a+(b-a)*p;}
/* Per-measurement rating shown in each page's badge: the first band whose range
   holds the reading (min inclusive, max exclusive, NAN open). Defaults come from
   ui_theme.h and are placeholders; the device may replace them (atmos_set_bands()).
   Missing readings have no rating. */
#define OPEN NAN
#define BAND(lo,hi,level,text) {lo,hi,level,text}
#define UPPER(fair,poor) {BAND(OPEN,fair,0,"Good"),BAND(fair,poor,1,"Fair"),BAND(poor,OPEN,2,"Poor")},3
#define RANGE(glo,ghi,flo,fhi) {BAND(glo,ghi,0,"Good"),BAND(flo,fhi,1,"Fair"),BAND(OPEN,OPEN,2,"Poor")},3
static struct { atmos_band band[ATMOS_BANDS_MAX]; int n; } bands[ATMOS_READINGS]={
    [ATMOS_CO2]={UPPER(UI_CO2_FAIR,UI_CO2_POOR)},[ATMOS_VOC]={UPPER(UI_VOC_FAIR,UI_VOC_POOR)},
    [ATMOS_NOX]={UPPER(UI_NOX_FAIR,UI_NOX_POOR)},[ATMOS_HCHO]={UPPER(UI_HCHO_FAIR,UI_HCHO_POOR)},
    [ATMOS_PM1]={UPPER(UI_PM1_FAIR,UI_PM1_POOR)},[ATMOS_PM25]={UPPER(UI_PM25_FAIR,UI_PM25_POOR)},
    [ATMOS_PM4]={UPPER(UI_PM4_FAIR,UI_PM4_POOR)},[ATMOS_PM10]={UPPER(UI_PM10_FAIR,UI_PM10_POOR)},
    [ATMOS_TEMPERATURE]={RANGE(UI_TEMP_GOOD_LOW,UI_TEMP_GOOD_HIGH,UI_TEMP_FAIR_LOW,UI_TEMP_FAIR_HIGH)},
    [ATMOS_HUMIDITY]={RANGE(UI_RH_GOOD_LOW,UI_RH_GOOD_HIGH,UI_RH_FAIR_LOW,UI_RH_FAIR_HIGH)},
    /* Pressure is weather, not air quality: it has no warning band. */
    [ATMOS_PRESSURE]={{BAND(OPEN,OPEN,0,"Good")},1},
};
static float reading_value(int r){
    switch(r){
    case ATMOS_CO2:return sensor.co2;case ATMOS_VOC:return sensor.voc;case ATMOS_NOX:return sensor.nox;
    case ATMOS_HCHO:return sensor.hcho;case ATMOS_PM1:return sensor.pm1;case ATMOS_PM25:return sensor.pm;
    case ATMOS_PM4:return sensor.pm4;case ATMOS_PM10:return sensor.pm10;case ATMOS_TEMPERATURE:return sensor.temp;
    case ATMOS_HUMIDITY:return sensor.rh;default:return sensor.pressure;}
}
typedef const atmos_band *rating;
static rating rate(int r){
    float v=reading_value(r);
    if(isnan(v))return NULL;
    for(int i=0;i<bands[r].n;i++){
        const atmos_band *b=&bands[r].band[i];
        if((isnan(b->min) || v>=b->min) && (isnan(b->max) || v<b->max))return b;
    }
    return NULL;
}
static const char *rating_text(rating r){return r?r->text:"--";}
/* Lowest reading rated at `level` or worse, for artwork that tracks the badge. */
static float band_start(int r,int level,float fallback){
    float lo=INFINITY;
    for(int i=0;i<bands[r].n;i++)if(bands[r].band[i].level>=level && isfinite(bands[r].band[i].min))lo=fminf(lo,bands[r].band[i].min);
    return isfinite(lo)?lo:fallback;
}
bool atmos_set_bands(int reading,const atmos_band *band,int n){
    if(reading<0 || reading>=ATMOS_READINGS || n<0 || n>ATMOS_BANDS_MAX || (n && !band))return false;
    for(int i=0;i<n;i++){bands[reading].band[i]=band[i];bands[reading].band[i].text[ATMOS_BAND_TEXT-1]=0;}
    bands[reading].n=n;full_dirty=true;data_version++;
    return true;
}
static rating rate_co2(void){return rate(ATMOS_CO2);}
static rating rate_hcho(void){return rate(ATMOS_HCHO);}
static rating rate_pm(int size){return rate(ATMOS_PM1+size);}
static rating rate_gas(int lane){return rate(ATMOS_VOC+lane);}
static rating rate_climate(int key){return rate(ATMOS_TEMPERATURE+key);}
/* Overview: the worst-level rating among all readings. */
static rating rate_overall(void){
    rating worst=NULL;
    for(int i=0;i<ATMOS_READINGS;i++){rating r=rate(i);if(r && (!worst || r->level>worst->level))worst=r;}
    return worst;
}
static bool warning(void){rating r=rate_overall();return r && r->level>=2;}
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
        if(!lv_draw_dispatch_layer(lv_display_get_default(),target))lv_draw_dispatch_request();
    }
}
/* Square opaque fills on a 1-bit layer set bits directly instead of queuing an LVGL
   draw task: the artwork is made of hundreds of tiny spans, and per-task overhead
   dominated device frame time. Identical pixels to LVGL's own fill. */
static lv_obj_t *scene_canvas[2];
static const lv_draw_buf_t *scene_canvas_buf(int i){return scene_canvas[i]?lv_canvas_get_draw_buf(scene_canvas[i]):NULL;}
enum{FILL_INK,FILL_PAPER,FILL_INVERT};
static bool direct_fill(int x,int y,int w,int h,int mode){
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
        if(first==last){head&=tail;tail=0;}
        if(mode==FILL_INVERT){
            row[first]^=head;for(int i=first+1;i<last;i++)row[i]=(uint8_t)~row[i];
            if(last>first)row[last]^=tail;
            continue;
        }
        bool paper=mode==FILL_PAPER;
        row[first]=paper?(uint8_t)(row[first]|head):(uint8_t)(row[first]&~head);
        if(last>first+1)memset(row+first+1,paper?0xff:0x00,(size_t)(last-first-1));
        if(last>first)row[last]=paper?(uint8_t)(row[last]|tail):(uint8_t)(row[last]&~tail);
    }
    return true;
}
static void rect(int x,int y,int w,int h,bool reverse,int radius){
    if(radius==0 && direct_fill(x,y+(int)offset,w,h,reverse?FILL_PAPER:FILL_INK))return;
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
    case 3:return v.nox;case 4:return v.temp;case 5:return v.rh;case 6:return v.pressure;default:return v.hcho;}
}
static const lv_font_t *dashboard_font(int size){
    switch(size){case 12:return &ui_bold_12;case 112:return &ui_bold_112;case 96:return &ui_bold_96;case 80:return &ui_bold_80;case 72:return &ui_bold_72;case 64:return &ui_bold_64;
    case 60:return &ui_bold_60;case 48:return &ui_bold_48;case 40:return &ui_bold_40;case 32:return &ui_bold_32;
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
            /* Our 1-bit glyph boxes come from antialiased outlines, so their edge
               rows/columns can be blank after thresholding: scan the set bits. */
            const lv_font_fmt_txt_dsc_t *dsc=font->dsc;
            const uint8_t *bits=dsc->glyph_bitmap+dsc->glyph_dsc[g.gid.index].bitmap_index;
            int x1=g.box_w,x2=-1,y1=g.box_h,y2=-1;
            for(int row=0;row<g.box_h;row++)for(int col=0;col<g.box_w;col++){
                int i=row*g.box_w+col;
                if(bits[i>>3]&(0x80>>(i&7))){x1=LV_MIN(x1,col);x2=LV_MAX(x2,col);y1=LV_MIN(y1,row);y2=LV_MAX(y2,row);}
            }
            if(x2<0){pen+=g.adv_w;continue;}
            int y=font->line_height-font->base_line-g.box_h-g.ofs_y;
            bounds.x1=LV_MIN(bounds.x1,pen+g.ofs_x+x1);
            bounds.x2=LV_MAX(bounds.x2,pen+g.ofs_x+x2);
            bounds.y1=LV_MIN(bounds.y1,y+y1);
            bounds.y2=LV_MAX(bounds.y2,y+y2);
        }
        pen+=g.adv_w;
    }
    return bounds;
}
/* Wi-Fi strength in the badge, in paper: a dot and one arc per bar, so a weak signal
   is a small symbol. Disconnected shows the arcs as 1 px outlines and a bold X. */
static int wifi_bars=-1;
/* Hand-drawn so the arcs end cleanly at 45 degrees and the dot is round:
   d = dot, 1-3 = the arc lit from that many bars. */
static const char wifi_glyph[UI_WIFI_H][UI_WIFI_W+1]={
    ".......333333333.......",
    "....333333333333333....",
    "..33333.........33333..",
    ".3333.............3333.",
    "333.....2222222.....333",
    ".....2222222222222.....",
    "....2222.......2222....",
    "...222...........222...",
    ".........11111.........",
    ".......111111111.......",
    "......111.....111......",
    ".......................",
    "..........ddd..........",
    ".........ddddd.........",
    ".........ddddd.........",
    "..........ddd.........."
};
/* The disconnected X, symmetric both ways. */
static const char wifi_x[UI_WIFI_X_SIZE][UI_WIFI_X_SIZE+1]={
    "##....##","###..###",".######.","..####..","..####..",".######.","###..###","##....##"};
static void wifi_icon(void){
    int x0=UI_WIFI_X,y0=UI_PM_HEADER_Y+(UI_BADGE_HEIGHT-UI_WIFI_H)/2;
    for(int y=0;y<UI_WIFI_H;y++){
        int run=-1;
        for(int x=0;x<=UI_WIFI_W;x++){
            bool on=false;
            if(x<UI_WIFI_W){
                char c=wifi_glyph[y][x];
                /* Disconnected: every arc as a 1 px outline (its top edge), with an X beside
                   the stem in the empty bottom-right corner, a pixel clear of the arcs. */
                int xs=x-(UI_WIFI_W-UI_WIFI_X_SIZE),ys=y-(UI_WIFI_H-UI_WIFI_X_SIZE);
                if(wifi_bars<0 && xs>=-1 && ys>=-1)
                    on=xs>=0 && ys>=0 && wifi_x[ys][xs]=='#';
                else if(c=='d')on=true;
                else if(c!='.')on=wifi_bars<0?(y==0 || wifi_glyph[y-1][x]!=c):c-'0'<=wifi_bars;
            }
            if(on && run<0)run=x;
            if(!on && run>=0){rect(x0+run,y0+y,x-run,1,true,0);run=-1;}
        }
    }
}
static int ink_width(const char *text,int size);
static void detail_header(const char *title,const char *badge){
    /* Titles are capitals: centre the cap height, so a tail (the Q in
       AIR QUALITY) neither raises the title nor differs between pages. */
    lv_area_t ink=header_ink_bounds(title,dashboard_font(UI_FONT_TITLE));
    lv_area_t caps=header_ink_bounds("H",dashboard_font(UI_FONT_TITLE));
    int title_y=UI_PM_HEADER_Y+(UI_BADGE_HEIGHT-(caps.y2-caps.y1+1))/2-caps.y1;
    dashboard_text(title,UI_MARGIN-ink.x1,title_y,UI_FONT_TITLE,false);
    rect(UI_MARGIN,UI_HEADER_BOTTOM,UI_WIDTH-2*UI_MARGIN,UI_DIVIDER,false,0);
    if(!badge)return;
    /* The badge splits into the rating's box and the Wi-Fi icon's box, each centred. */
    int text_w=UI_RATING_W;
    /* Long configured texts drop to the small font. */
    int badge_size=ink_width(badge,16)<=text_w-6?16:12;
    ink=header_ink_bounds(badge,dashboard_font(badge_size));
    int text_x=UI_BADGE_X+(text_w-(ink.x2-ink.x1+1))/2-ink.x1;
    int text_y=UI_PM_HEADER_Y+(UI_BADGE_HEIGHT-(ink.y2-ink.y1+1))/2-ink.y1;
    rect(UI_BADGE_X,UI_PM_HEADER_Y,UI_RATING_W,UI_BADGE_HEIGHT,false,0);
    rect(UI_WIFI_BOX_X,UI_PM_HEADER_Y,UI_WIFI_BOX_W,UI_BADGE_HEIGHT,false,0);
    wifi_icon();
    dashboard_text(badge,text_x,text_y,badge_size,true);
}
static void dashboard_header(void){
    detail_header("AIR QUALITY",rating_text(rate_overall()));
}
/* Layout rules shared by every page: content ink spans UI_MARGIN..UI_WIDTH-1-UI_MARGIN,
   like the header rule; groups are centred on the band between the header rule
   and the history panel; stacked text rows have equal gaps between their ink. */
static int content_center(void){
    int top=UI_HEADER_BOTTOM+UI_DIVIDER;
    return top+(UI_HISTORY_PANEL_Y-top)/2;
}
static int ink_width(const char *text,int size){
    lv_area_t ink=header_ink_bounds(text,dashboard_font(size));
    return ink.x2-ink.x1+1;
}
/* Stacks n rows with `gap` between their ink, centred on `center`. Rows are
   measured with canonical strings so they hold still as readings change.
   y[i] receives each row's draw y; returns the block's ink top. */
static int stack_rows(int n,const char *const *canon,const int *size,int gap,int center,int *y,int *height){
    lv_area_t ink[4];int h=gap*(n-1);
    for(int i=0;i<n;i++){ink[i]=header_ink_bounds(canon[i],dashboard_font(size[i]));h+=ink[i].y2-ink[i].y1+1;}
    int top=center-h/2,at=top;
    for(int i=0;i<n;i++){y[i]=at-ink[i].y1;at+=ink[i].y2-ink[i].y1+1+gap;}
    if(height)*height=h;
    return top;
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
/* Centre text on its visible ink, not its advance box. Our UTF-8 unit glyphs
   (degree, micro, cube) are code points 0xB0..0xB5, so header_ink_bounds finds
   them from their second byte and skips the 0xC2 lead byte. */
static void centered_text(const char *text,int center,int y,int size,bool reverse){
    lv_area_t ink=header_ink_bounds(text,dashboard_font(size));
    dashboard_text(text,center-(ink.x1+ink.x2+1)/2,y,size,reverse);
}
/* Readings never change font size. Only the CO2 page abbreviates, from 10000
   ("10.0k"); elsewhere values print in full. Decimals drop once the rounded
   value reaches 1000, so "999.9" is followed by "1000". */
static void format_reading(char *out,size_t capacity,float value,int decimals,bool abbreviate){
    if(!isfinite(value)){snprintf(out,capacity,"--");return;}
    if(abbreviate && fabsf(value)>=10000.0f){snprintf(out,capacity,"%.1fk",value/1000.0f);return;}
    if(decimals && fabsf(value)>=999.95f)decimals=0;
    snprintf(out,capacity,"%.*f",decimals,value);
}
/* Ink width of the widest number with `digits` digits (and a decimal point
   when `point`), so fixed layouts fit any reading up to the sensor maximum. */
static int widest_number(int digits,bool point,int size){
    char widest='0';int best=-1;
    for(char d='0';d<='9';d++){char s[2]={d,0};int w=text_width(s,dashboard_font(size));if(w>best){best=w;widest=d;}}
    char s[16];int n=0;
    for(int i=0;i<digits;i++){if(point && i==digits-1)s[n++]='.';s[n++]=widest;}
    s[n]=0;return ink_width(s,size);
}
/* The column starts at the left margin, sized to its widest ink; the flower
   ends at the right margin. */
static void primary_measurement(void){
    char value[32];format_reading(value,sizeof value,sensor.co2,0,false);
    static const char *const canon[]={"CO2","0123456789","PPM"};
    static const int size[]={UI_FONT_LABEL,UI_MAIN_VALUE_SIZE,UI_FONT_LABEL};
    int y[3];stack_rows(3,canon,size,UI_MAIN_GAP,content_center(),y,NULL);
    int width=LV_MAX(LV_MAX(ink_width("CO2",UI_FONT_LABEL),widest_number(5,false,UI_MAIN_VALUE_SIZE)),ink_width("PPM",UI_FONT_LABEL));
    int center=UI_MARGIN+width/2;
    centered_text("CO2",center,y[0],UI_FONT_LABEL,false);
    centered_text(value,center,y[1],UI_MAIN_VALUE_SIZE,false);
    centered_text("PPM",center,y[2],UI_FONT_LABEL,false);
}
/* Footer rows share one canonical stack, centred in the panel. */
static void footer_measurement(int center,const char *label,float value,const char *unit){
    char s[32];format_reading(s,sizeof s,value,1,false);
    static const char *const canon[]={"PM2.5Temperature","0123456789","\xc2\xb5" "g/m\xc2\xb3" "\xc2\xb0" "C"};
    static const int size[]={UI_FONT_LABEL,UI_FONT_VALUE,UI_FONT_UNIT};
    int y[3];stack_rows(3,canon,size,UI_FOOTER_GAP,(UI_FOOTER_TOP+UI_HEIGHT)/2,y,NULL);
    centered_text(label,center,y[0],UI_FONT_LABEL,true);
    centered_text(s,center,y[1],UI_FONT_VALUE,true);
    centered_text(unit,center,y[2],UI_FONT_UNIT,true);
}
static void draw_dashboard(void){
    dashboard_header();primary_measurement();status_flower();
    rect(0,UI_FOOTER_TOP,UI_WIDTH,UI_HEIGHT-UI_FOOTER_TOP,false,0);
    rect((UI_WIDTH-UI_FOOTER_DIVIDER)/2,UI_FOOTER_TOP+UI_FOOTER_PADDING,UI_FOOTER_DIVIDER,
        UI_HEIGHT-UI_FOOTER_TOP-2*UI_FOOTER_PADDING,true,0);
    footer_measurement(UI_WIDTH/4,"PM2.5",sensor.pm,"\xc2\xb5" "g/m\xc2\xb3");
    footer_measurement(3*UI_WIDTH/4,"Temperature",sensor.temp,"\xc2\xb0" "C");
}
static int co2_bubble_radius(int j){
    static const int radii[UI_BUBBLE_COUNT]={6,11,16,23,8,13,5,9,19,7,12,10};return radii[j];
}
/* Bubble density follows the reading: from UI_BUBBLE_MIN at outdoor CO2 to every
   slot at UI_CO2_BUBBLE_FULL, on a log scale. Slots join in an order that keeps
   the column evenly spaced, and only switch while below the box, so a reading
   change never pops a bubble in or out mid-flight. */
static int co2_bubble_target(void){
    float c=isfinite(sensor.co2)?sensor.co2:UI_CO2_BUBBLE_BASE;
    float f=clamp(log(fmaxf(c,UI_CO2_BUBBLE_BASE)/UI_CO2_BUBBLE_BASE)/log(UI_CO2_BUBBLE_FULL/UI_CO2_BUBBLE_BASE),0,1);
    return UI_BUBBLE_MIN+(int)lroundf(f*(UI_BUBBLE_COUNT-UI_BUBBLE_MIN));
}
static float co2_bubble_phase(int j){
    float height=UI_BUBBLE_BOTTOM-UI_BUBBLE_TOP+2*UI_BUBBLE_MAX_RADIUS;
    return (float)fmod(anim_t*UI_BUBBLE_SPEED/height+(double)j/UI_BUBBLE_COUNT,1);
}
static bool co2_bubble_visible(int j){
    static const int rank[UI_BUBBLE_COUNT]={0,6,3,9,1,7,4,10,2,8,5,11};
    static long cycle[UI_BUBBLE_COUNT];static bool on[UI_BUBBLE_COUNT],ready;
    float height=UI_BUBBLE_BOTTOM-UI_BUBBLE_TOP+2*UI_BUBBLE_MAX_RADIUS;
    long now=(long)floor(anim_t*UI_BUBBLE_SPEED/height+(double)j/UI_BUBBLE_COUNT);
    if(!ready || now!=cycle[j]){
        if(!ready)for(int i=0;i<UI_BUBBLE_COUNT;i++)cycle[i]=LONG_MIN;
        ready=true;cycle[j]=now;on[j]=rank[j]<co2_bubble_target();
    }
    return on[j];
}
static void co2_bubble_position(int j,float *x,float *y){
    int r=co2_bubble_radius(j);
    float height=UI_BUBBLE_BOTTOM-UI_BUBBLE_TOP+2*UI_BUBBLE_MAX_RADIUS;
    float phase=co2_bubble_phase(j);
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
        if(!co2_bubble_visible(j) && !(moving && j==portal_bubble))continue;
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
    if(direct_fill(x,top,1,bottom-top,FILL_PAPER))return;
    lv_draw_rect_dsc_t fill;lv_draw_rect_dsc_init(&fill);
    fill.bg_color=lv_color_hex(UI_COLOR_CHART_FILL);fill.bg_opa=255;
    lv_area_t area={x,top,x,bottom-1};lv_draw_rect(layer,&fill,&area);
}
/* History chart geometry, shared by every page. With no unit or "SEC AGO"
   captions, tick-label ink starts at the margin (the widest label is four
   digits) and the plot takes the old caption row; x-tick labels keep their gap
   below the axis. Set by chart_geometry() from rendered font metrics. */
static int chart_left,chart_bottom,chart_tick_y;
static void chart_geometry(void){
    static bool ready;if(ready)return;ready=true;
    lv_area_t digits=header_ink_bounds("0123456789",&ui_bold_12),caption=header_ink_bounds("SEC AGO",&ui_bold_12);
    int grow=digits.y2-digits.y1+1+UI_CHART_LABEL_GAP-(digits.y1-caption.y1);
    chart_left=UI_MARGIN+widest_number(4,false,UI_FONT_AXIS)+UI_CHART_TICK_LENGTH+UI_CHART_LABEL_GAP+1;
    chart_bottom=UI_CHART_BOTTOM+grow;chart_tick_y=UI_CHART_TICK_Y+grow;
}
static void history_panel(void){
    rect(0,UI_HISTORY_PANEL_Y,UI_WIDTH,UI_HEIGHT-UI_HISTORY_PANEL_Y,false,0);
}
/* One axis renderer for every history, including the animated PM scale. */
static void chart_tick(float value,float y){
    char label[32];
    /* Four glyphs at most, so labels clear the rotated unit caption. */
    if(fabs(value)>=10000)snprintf(label,sizeof label,"%.0fk",value/1000);
    else snprintf(label,sizeof label,fabs(value-round(value))>.01f && fabs(value)<100?"%.1f":"%.0f",value);
    lv_area_t ink=header_ink_bounds(label,&ui_bold_12);
    dashboard_text(label,chart_left-UI_CHART_TICK_LENGTH-UI_CHART_LABEL_GAP-1-ink.x2,
        (int)round(y)-(ink.y2-ink.y1+1)/2-ink.y1,UI_FONT_AXIS,true);
    line(chart_left-UI_CHART_TICK_LENGTH,y,chart_left,y,1,true);
}
static bool axis_paper(int x,int y){
    return axis_label_buf[I1_PALETTE+y*(AXIS_W/8)+(x>>3)]>>(7-(x&7))&1;
}
/* Solid rounded rectangle built from row spans: an LVGL rounded rectangle
   thresholded to 1 bit loses its curve. */
static void pill(int x,int y,int w,int h,int radius,bool paper){
    for(int i=0;i<h;i++){
        float dy=fmaxf(0,fmaxf(radius-(i+.5f),(i+.5f)-(h-radius)));
        int inset=dy>0?(int)lroundf(radius-sqrtf(fmaxf(0,radius*radius-dy*dy))):0;
        rect(x+inset,y+i,w-2*inset,1,paper,0);
    }
}
/* Horizontal ink extent of text as LVGL actually renders it at x 0. Font
   metrics can be a pixel off from the drawn glyphs. */
static void rendered_ink_x(const char *text,int *left,int *right){
    lv_layer_t text_layer;lv_canvas_init_layer(axis_label_canvas,&text_layer);
    lv_draw_rect_dsc_t bg;lv_draw_rect_dsc_init(&bg);bg.bg_color=color(false);bg.bg_opa=255;
    lv_area_t full={0,0,AXIS_W-1,AXIS_H-1};lv_draw_rect(&text_layer,&bg,&full);
    lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);
    d.font=&ui_bold_12;d.text=text;d.text_local=1;d.color=color(true);
    lv_draw_label(&text_layer,&d,&full);
    lv_canvas_finish_layer(axis_label_canvas,&text_layer);
    *left=AXIS_W;*right=-1;
    for(int y=0;y<AXIS_H;y++)for(int x=0;x<AXIS_W;x++)
        if(axis_paper(x,y)){*left=LV_MIN(*left,x);*right=LV_MAX(*right,x);}
}
/* "LAST n UNIT" in a pill at the chart's top-right corner, right-aligned to
   the time axis and drawn over the chart. Equal ink-to-outline gaps all round
   the text horizontally, and above and below the capitals. */
static void history_caption(double span){
    char text[32];
    if(span<90)snprintf(text,sizeof text,"LAST %d SEC",(int)lround(span));
    else if(span<90*60)snprintf(text,sizeof text,"LAST %d MIN",(int)lround(span/60));
    else snprintf(text,sizeof text,"LAST %d HRS",(int)lround(fmin(span/3600,9999)));
    int left,right;rendered_ink_x(text,&left,&right);
    lv_area_t caps=header_ink_bounds("H",&ui_bold_12);
    int pad_y=UI_HISTORY_PILL_PADDING,pad_x=UI_HISTORY_PILL_PADDING+UI_HISTORY_PILL_PADDING_X;
    int w=right-left+1+2*pad_x+2,h=caps.y2-caps.y1+1+2*pad_y+2,x=UI_CHART_RIGHT+1-w,y=UI_CHART_TOP;
    pill(x,y,w,h,UI_HISTORY_PILL_RADIUS,true);
    pill(x+1,y+1,w-2,h-2,UI_HISTORY_PILL_RADIUS-1,false);
    dashboard_text(text,x+1+pad_x-left,y+1+pad_y-caps.y1,UI_FONT_AXIS,true);
}
static void chart_axes(const char *name,const char *unit,float span){
    (void)name;(void)unit; /* The page header names the channel and its unit. */
    line(chart_left,UI_CHART_TOP,chart_left,chart_bottom,1,true);
    line(chart_left,chart_bottom,UI_CHART_RIGHT,chart_bottom,1,true);
    for(int i=0;i<=UI_CHART_X_INTERVALS;i++){
        int x=chart_left+i*(UI_CHART_RIGHT-chart_left)/UI_CHART_X_INTERVALS;
        char label[24];snprintf(label,sizeof label,"%.0f",span*(UI_CHART_X_INTERVALS-i)/UI_CHART_X_INTERVALS);
        lv_area_t ink=header_ink_bounds(label,&ui_bold_12);
        if(span>0 || i==UI_CHART_X_INTERVALS)
            dashboard_text(label,x-(ink.x2-ink.x1+1)/2-ink.x1,chart_tick_y-ink.y1,UI_FONT_AXIS,true);
        line(x,chart_bottom,x,chart_bottom+UI_CHART_TICK_LENGTH,1,true);
    }
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
/* Gases and Climate selection changes ease the trace in screen space from the
   previous reading's curve (on its own scale) to the new one, so it rises or
   falls rather than jumping. The previous scale is frozen at the change. */
static int chart_from_key=-1,chart_to_key=-1;
static float chart_from_low,chart_from_high,chart_blend_age=UI_CHART_BLEND_TIME;
/* The first sample's trace rises from the chart floor with the same easing. */
static float chart_rise_age=UI_CHART_BLEND_TIME;
static float chart_y(float value,float low,float high){
    return clamp(chart_bottom-(value-low)/(high-low)*(chart_bottom-UI_CHART_TOP),UI_CHART_TOP,chart_bottom);
}
static void history_graph(const sample *samples,int n,int key,const float *weights,
                          const char *name,const char *unit,float low,float high){
    bool blend=!weights && chart_blend_age<UI_CHART_BLEND_TIME && key==chart_to_key && chart_from_key>=0;
    float q=ease(chart_blend_age/UI_CHART_BLEND_TIME),rise=ease(chart_rise_age/UI_CHART_BLEND_TIME);
    history_panel();
    chart_geometry();
    for(int i=0;i<=UI_CHART_Y_INTERVALS;i++){
        float fraction=(float)i/UI_CHART_Y_INTERVALS;
        chart_tick(mix(low,high,fraction),mix(chart_bottom,UI_CHART_TOP,fraction));
    }
    double first=n?samples[0].at:0,last=n?samples[n-1].at:0;
    /* Once the history span is held, the chart shows exactly that span up to the
       newest sample, interpolating into the one sample kept from before it. */
    if(last-first>history_window)first=last-history_window;
    double span=last-first;
    if(!n){chart_axes(name,unit,span);history_caption(span);return;}
    /* A single sample spans the full width as a flat trace, so the chart does
       not start as one column and then jump to full width at the second. */
    float previous=NAN,trace[UI_CHART_RIGHT+1];
    int begin=chart_left;
    for(int x=begin;x<=UI_CHART_RIGHT;x++){
        double at=span>0?first+(last-first)*(x-chart_left)/(double)(UI_CHART_RIGHT-chart_left):last;
        float value=chart_interpolate(samples,n,at,key,weights);
        bool valid=isfinite(value) && (key==4 || value>=0);
        float y=valid?chart_y(value,low,high):NAN;
        if(blend){
            float from=chart_interpolate(samples,n,at,chart_from_key,NULL);
            if(isfinite(from) && (chart_from_key==4 || from>=0)){
                float y0=chart_y(from,chart_from_low,chart_from_high);
                y=valid?mix(y0,y,q):y0;valid=true;
            }
        }
        if(!valid){trace[x-chart_left]=NAN;continue;}
        y=mix(chart_bottom,y,rise);
        chart_fill_span(x,(int)ceil(y),chart_bottom);
        trace[x-chart_left]=y;
    }
    for(int x=begin;x<=UI_CHART_RIGHT;x++){
        float y=trace[x-chart_left];
        if(!isfinite(y)){previous=NAN;continue;}
        /* Each column spans from the previous point to this one: anti-aliased
           LVGL lines thresholded to 1 bit left ink holes on steep slopes. */
        float lo=isfinite(previous)?fminf(previous,y):y,hi=isfinite(previous)?fmaxf(previous,y):y;
        int top=(int)lroundf(lo)-UI_CHART_LINE_WIDTH/2,bottom=(int)lroundf(hi)+(UI_CHART_LINE_WIDTH-1)/2;
        rect(x,top,1,bottom-top+1,true,0);
        previous=y;
    }
    chart_axes(name,unit,span);
    history_caption(span);
}
/* Highest reading each sensor can report; chart scales never pass it. */
static float field_limit(int key){
    static const float limit[]={UI_SENSOR_CO2_MAX,UI_SENSOR_PM_MAX,UI_SENSOR_INDEX_MAX,UI_SENSOR_INDEX_MAX,UI_SENSOR_TEMP_MAX,UI_SENSOR_RH_MAX,UI_SENSOR_PRESSURE_MAX,UI_SENSOR_HCHO_MAX};
    return limit[key];
}
static void chart_range(int key,float minimum_range,float padding_ratio,float *low_out,float *high_out){
    float minimum=INFINITY,maximum=-INFINITY;
    for(int i=0;i<count;i++){
        float value=field(history[i].value,key);
        if(isfinite(value) && (key==4 || value>=0)){minimum=fmin(minimum,value);maximum=fmax(maximum,value);}
    }
    if(!isfinite(minimum)){minimum=0;maximum=minimum_range;}
    /* Large readings get a wider scale, so ticks stay short round numbers
       (whole thousands for CO2 from UI_CHART_THOUSANDS). */
    minimum_range=fmax(minimum_range,fabs(maximum)*UI_CHART_MIN_RANGE_RATIO);
    if(key==0 && maximum>=UI_CHART_THOUSANDS)minimum_range=fmax(minimum_range,2000);
    float range=fmax(minimum_range,maximum-minimum),padding=range*padding_ratio;
    float step=chart_nice_step((range+2*padding)/UI_CHART_Y_INTERVALS);
    float low=floor((minimum-padding)/step)*step;
    if(key!=4)low=fmax(0,low);
    float high=fmin(fmax(low+step,ceil((maximum+padding)/step)*step),field_limit(key));
    *low_out=low;*high_out=high;
}
static void history_chart(int key,const char *name,const char *unit,float minimum_range,float padding_ratio){
    float low,high;chart_range(key,minimum_range,padding_ratio,&low,&high);
    history_graph(history,count,key,NULL,name,unit,low,high);
}
/* Scale policy of the Gases (keys 2, 3) and Climate (4-6) charts. */
static void selection_chart_range(int key,float *low,float *high){
    static const float minimum[]={UI_GAS_CHART_MIN_VOC,UI_GAS_CHART_MIN_NOX,
        UI_CLIMATE_MIN_TEMP_RANGE,UI_CLIMATE_MIN_RH_RANGE,UI_CLIMATE_MIN_PRESSURE_RANGE};
    chart_range(key,minimum[key-2],key<4?UI_GAS_CHART_PADDING:UI_CLIMATE_PADDING,low,high);
}
/* Start easing the visible chart from key `from` to key `to`. Off-screen
   selection changes (automatic cycling resetting the next page) switch at once. */
static void start_chart_blend(int s,int from,int to){
    if(s!=scene || moving){chart_blend_age=UI_CHART_BLEND_TIME;return;}
    if(chart_blend_age<UI_CHART_BLEND_TIME)from=chart_to_key; /* retarget from the newest */
    selection_chart_range(from,&chart_from_low,&chart_from_high);
    chart_from_key=from;chart_to_key=to;chart_blend_age=0;
}
/* Changing the underlined reading first wipes the old bar away left to right, then
   draws the new one left to right, each over half of UI_UNDERLINE_TIME. Like the
   chart blend, off-screen changes switch at once. */
static float underline_age=UI_UNDERLINE_TIME,underline_from_end=1;static int underline_from=-1;
static void start_underline(int s,int from){
    if(s!=scene || moving){underline_age=UI_UNDERLINE_TIME;return;}
    /* Rapid changes never restart a bar: a bar already wiping away keeps going (the
       newest selection draws after it), and a bar still drawing wipes away from the
       length it has reached. */
    float half=UI_UNDERLINE_TIME/2;
    if(underline_age<half)return;
    underline_from_end=underline_age<UI_UNDERLINE_TIME?ease((underline_age-half)/half):1;
    underline_from=from;underline_age=0;
}
/* Visible pixel span [*x0,*x1) of a w-pixel underline under `item`, given the
   selected one; both edges travel left to right. Returns false when nothing shows. */
static bool underline_span(int item,int selected,int w,int *x0,int *x1){
    float a=0,b=item==selected?1:0,half=UI_UNDERLINE_TIME/2;
    if(underline_age<half){a=ease(underline_age/half);b=item==underline_from?underline_from_end:0;}
    else if(underline_age<UI_UNDERLINE_TIME)b=item==selected?ease((underline_age-half)/half):0;
    *x0=(int)lroundf(w*a);*x1=(int)lroundf(w*b);
    return *x1>*x0;
}
/* The reading and unit share the header title's column: the largest value font
   whose widest reading ("40.0k", or four digits) fits the title's ink width. */
static int co2_title_width(void){return ink_width("CARBON DIOXIDE",UI_FONT_TITLE);}
static int co2_value_size(void){
    static const int sizes[]={UI_CO2_VALUE_SIZE,72,64,40};
    for(int i=0;i<(int)(sizeof sizes/sizeof *sizes);i++)
        if(LV_MAX(ink_width("40.0k",sizes[i]),widest_number(4,false,sizes[i]))<=co2_title_width())return sizes[i];
    return 40;
}
static void draw_co2_detail(void){
    int size=co2_value_size();
    char value[32];format_reading(value,sizeof value,sensor.co2,0,true);
    detail_header("CARBON DIOXIDE",rating_text(rate_co2()));
    /* Center the visible value/unit block, not the fonts' padded line boxes.
       Canonical digit bounds keep its position stable as readings change. */
    lv_area_t digits=header_ink_bounds("0123456789.k",dashboard_font(size));
    lv_area_t unit=header_ink_bounds("PPM",dashboard_font(UI_CO2_UNIT_SIZE));
    int content_top=UI_HEADER_BOTTOM+UI_DIVIDER;
    int unit_offset=digits.y2+1+UI_CO2_UNIT_GAP-unit.y1;
    int block_height=unit_offset+unit.y2-digits.y1+1;
    int value_y=content_top+(UI_HISTORY_PANEL_Y-content_top-block_height)/2-digits.y1;
    int center=UI_MARGIN+co2_title_width()/2;
    centered_text(value,center,value_y,size,false);
    centered_text("PPM",center,value_y+unit_offset,UI_CO2_UNIT_SIZE,false);
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
    float range=fmax(fmax(UI_PM_CHART_MIN_RANGE,hi*UI_CHART_MIN_RANGE_RATIO),hi-lo),step=chart_nice_step((range*(1+2*UI_PM_CHART_PADDING))/UI_PM_CHART_Y_INTERVALS);
    *low=fmax(0,floor((lo-range*UI_PM_CHART_PADDING)/step)*step);
    *high=fmin(fmax(*low+step,ceil((hi+range*UI_PM_CHART_PADDING)/step)*step),UI_SENSOR_PM_MAX);
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
    start_underline(2,pm_selected);pm_selected=size;pm_blend_age=0;data_version++;lv_obj_invalidate(surface);
}
void atmos_cycle_pm(int direction){if(scene==2 && !moving)atmos_select_pm(pm_selected+direction);}
static int climate_row_at(int y);
static void pm_layout(int *y,int *value_y);
bool atmos_select_pm_at(int x,int y){
    if(moving)return false;
    int pm_y[3],pm_value_y[2];pm_layout(pm_y,pm_value_y);
    if(scene==2 && y>=pm_y[2] && y<pm_y[2]+UI_PM_CARD_HEIGHT){
        for(int i=0;i<4;i++){
            int left=UI_MARGIN+i*(UI_PM_CARD_WIDTH+UI_PM_CARD_GAP);
            if(x>=left && x<left+UI_PM_CARD_WIDTH){atmos_select_pm(i);return true;}
        }
    }
    if(scene==3 && y>UI_HEADER_BOTTOM && y<UI_HISTORY_PANEL_Y && x<UI_GAS_ART_DIVIDER_X){
        select_gas(x>=gas_divider_x);return true;
    }
    if(scene==4 && y>UI_HEADER_BOTTOM && y<UI_HISTORY_PANEL_Y){
        select_climate(climate_row_at(y));return true;
    }
    return false;
}
/* Particle field: a dotted perspective floor with dust drifting above it. The camera
   sits UI_PM_CAMERA_HEIGHT above the floor; a point (x, height, depth) projects to
   (UI_PM_FIELD_CX+f*x/depth, UI_PM_HORIZON_Y+f*(camera-height)/depth). */
#define PM_FOCAL ((float)(UI_PM_FIELD_BOTTOM-UI_PM_HORIZON_Y)/UI_PM_CAMERA_HEIGHT)
#define PM_DRIFT_TICKS (UI_PM_FIELD_FPS*600) /* every drift period divides ten minutes */
static int16_t pm_grid_dots[UI_PM_GRID_DOT_MAX][2];
static int pm_grid_count=-1;
/* The field is an even number of pixels wide, so its centre falls between two
   columns: round x toward the centre, keeping the floor's inset from both
   field edges equal. */
#define PM_GRID_CX ((UI_PM_FIELD_LEFT+UI_PM_FIELD_RIGHT)*.5f)
static void pm_grid_add(float x,float y){
    int xx=(int)(x<PM_GRID_CX?ceilf(x-.001f):floorf(x+.001f)),yy=(int)lroundf(y);
    if(pm_grid_count>=UI_PM_GRID_DOT_MAX || xx<UI_PM_FIELD_LEFT || xx>UI_PM_FIELD_RIGHT ||
       yy<UI_PM_FIELD_TOP || yy>UI_PM_FIELD_BOTTOM)return;
    pm_grid_dots[pm_grid_count][0]=(int16_t)xx;pm_grid_dots[pm_grid_count++][1]=(int16_t)yy;
}
/* The floor never moves, so its dots are laid out once. */
static void pm_grid_build(void){
    /* A few rows, widening toward the viewer, and columns converging on one vanishing
       point: a plain floor plane. The outer columns bound every row. */
    static const float depths[]={UI_PM_GRID_NEAR,1.5f,2.3f,UI_PM_GRID_FAR};
    const int rows=sizeof depths/sizeof *depths;
    float f=PM_FOCAL,cx=PM_GRID_CX,near=depths[0],far=depths[rows-1];
    float half_width=(UI_PM_FIELD_RIGHT-PM_GRID_CX)*near/f;
    pm_grid_count=0;
    for(int i=0;i<rows;i++){
        float y=UI_PM_HORIZON_Y+f*UI_PM_CAMERA_HEIGHT/depths[i],half=f*half_width/depths[i];
        /* Whole steps either side of the centre keep each row symmetric. */
        float start=-floorf(half/UI_PM_GRID_DOT_STEP)*UI_PM_GRID_DOT_STEP;
        for(float x=start;x<=-start+.01f;x+=UI_PM_GRID_DOT_STEP)pm_grid_add(cx+x,y);
    }
    for(int k=0;k<UI_PM_GRID_COLUMNS;k++){
        float gx=half_width*(2.0f*k/(UI_PM_GRID_COLUMNS-1)-1);
        float x0=cx+f*gx/near,y0=UI_PM_HORIZON_Y+f*UI_PM_CAMERA_HEIGHT/near;
        float x1=cx+f*gx/far,y1=UI_PM_HORIZON_Y+f*UI_PM_CAMERA_HEIGHT/far;
        int steps=(int)(hypotf(x1-x0,y1-y0)/UI_PM_GRID_DOT_STEP);
        for(int j=1;j<steps;j++){float u=(float)j/steps;pm_grid_add(x0+(x1-x0)*u,y0+(y1-y0)*u);}
    }
}
/* Opaque disc built from row spans; radius below one is a single pixel. */
static void pm_dot(int x,int y,float r){
    int ri=(int)r;
    for(int dy=-ri;dy<=ri;dy++){
        int half=(int)sqrtf(fmaxf(0,r*r-(float)(dy*dy)));
        rect(x-half,y+dy,2*half+1,1,false,0);
    }
}
static float pm_wave(int64_t tick,int speed,int seed){
    return sinf(TAU*(float)((tick*speed+seed*997)%PM_DRIFT_TICKS)/PM_DRIFT_TICKS);
}
static void pm_particle_field(void){
    if(pm_grid_count<0)pm_grid_build();
    for(int i=0;i<pm_grid_count;i++)rect(pm_grid_dots[i][0],pm_grid_dots[i][1],1,1,false,0);
    /* Each size class is weighted by its own channel's reading (PM1, PM2.5, PM4,
       PM10), so the field always shows a mix of sizes in the readings'
       proportions; the overall level sets only the density. Band differences
       (PM2.5-PM1, ...) would put every dot in one class whenever the channels
       read alike, as at full scale. */
    float cumulative[4],mass=0;
    for(int i=0;i<4;i++){
        float bin=fmaxf(0,pm_value(sensor,i));
        if(!isfinite(bin))bin=0;
        mass+=bin;cumulative[i]=mass;
    }
    static const float sizes[]={UI_PM_SIZE_PM1,UI_PM_SIZE_PM25,UI_PM_SIZE_PM4,UI_PM_SIZE_PM10};
    /* Drift advances in whole ticks, so the scene redraws only UI_PM_FIELD_FPS times a second. */
    int64_t tick=(int64_t)floor(anim_t*UI_PM_FIELD_FPS);
    float f=PM_FOCAL;
    pm_dust_count=0;
    for(int j=0;j<UI_PM_PARTICLE_MAX;j++){
        /* Deterministic admission thresholds prevent random per-frame flicker. */
        if(j+.5f>pm_density)break;
        float a=fmodf(j*.618034f+.2f,1),b=fmodf(j*.754878f+.37f,1),c=fmodf(j*.569840f+.61f,1);
        /* Bias toward the viewer so the far field does not crowd the horizon. */
        float home=UI_PM_DEPTH_NEAR+(UI_PM_DEPTH_FAR-UI_PM_DEPTH_NEAR)*b*b;
        float depth=home+UI_PM_DRIFT_DEPTH*pm_wave(tick,UI_PM_DRIFT_SPEED+j%5,j);
        /* Spread across the full field width at every depth. */
        float x=(a*2-1)*(UI_PM_FIELD_RIGHT-UI_PM_FIELD_CX-4)*depth/f+UI_PM_DRIFT_X*pm_wave(tick,UI_PM_DRIFT_SPEED+4+j%7,j+31);
        /* Fill the air between the floor and the top of the field at this depth. */
        float ceiling=UI_PM_CAMERA_HEIGHT+(UI_PM_HORIZON_Y-UI_PM_FIELD_TOP-4)*depth/f;
        float height=UI_PM_HEIGHT_MIN+(ceiling-UI_PM_HEIGHT_MIN)*c
            +UI_PM_DRIFT_Y*pm_wave(tick,UI_PM_DRIFT_SPEED+2+j%6,j+67);
        float quantile=fmodf(j*.819173f+.13f,1)*mass;int category=0;
        while(category<3 && quantile>cumulative[category])category++;
        float r=sizes[category]*UI_PM_DEPTH_NEAR/home; /* fixed per dot: no size popping */
        int ri=(int)r;
        int sx=(int)lroundf(UI_PM_FIELD_CX+f*x/depth);
        int sy=(int)lroundf(UI_PM_HORIZON_Y+f*(UI_PM_CAMERA_HEIGHT-height)/depth);
        sx=LV_CLAMP(UI_PM_FIELD_LEFT+ri,sx,UI_PM_FIELD_RIGHT-ri);
        sy=LV_CLAMP(UI_PM_FIELD_TOP+ri,sy,UI_PM_FIELD_BOTTOM-ri);
        pm_dot(sx,sy,r);
        pm_dust[pm_dust_count][0]=(int16_t)sx;pm_dust[pm_dust_count][1]=(int16_t)sy;
        pm_dust[pm_dust_count++][2]=(int16_t)lroundf(r);
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
/* Value and unit sit UI_TEXT_GAP apart, as on every page; the space left in
   the content band splits evenly above the value, between the unit and the
   cards, and below the cards. y[] gets the value ink top, unit ink
   top and card top; value_y[] the value and unit draw y. */
/* The reading and unit are centred on the gap between the PM1 and PM2.5 cards.
   The value uses the largest font whose widest reading ("999.9", or "1000")
   stays inside the left margin, the header rule's left edge. */
static int pm_value_center(void){return (2*(UI_MARGIN+UI_PM_CARD_WIDTH)+UI_PM_CARD_GAP)/2;}
static int pm_value_size(void){
    static const int sizes[]={112,96,80,UI_PM_VALUE_SIZE,64,60,40};
    for(int i=0;i<(int)(sizeof sizes/sizeof *sizes);i++)
        if(widest_number(4,true,sizes[i])<=2*(pm_value_center()-UI_MARGIN))return sizes[i];
    return 40;
}
static void pm_layout(int *y,int *value_y){
    lv_area_t digits=header_ink_bounds("0123456789.-",dashboard_font(pm_value_size()));
    lv_area_t unit=header_ink_bounds("\xc2\xb5" "g/m\xc2\xb3",dashboard_font(UI_FONT_UNIT));
    int dh=digits.y2-digits.y1+1,uh=unit.y2-unit.y1+1;
    int top=UI_HEADER_BOTTOM+UI_DIVIDER,spare=UI_HISTORY_PANEL_Y-top-dh-uh-UI_PM_CARD_HEIGHT;
    spare-=UI_TEXT_GAP;
    int gap=spare/3;
    y[0]=top+gap+(spare-3*gap)/2;y[1]=y[0]+dh+UI_TEXT_GAP;y[2]=y[1]+uh+gap;
    value_y[0]=y[0]-digits.y1;value_y[1]=y[1]-unit.y1;
}
/* The selected card's fill wipes like the underlines (old card clears, then the
   new one fills, both left to right), behind a 45-degree edge with its top leading. */
static void pm_card_fill(int x,int y,int card){
    int w=UI_PM_CARD_WIDTH-2,h=UI_PM_CARD_HEIGHT-2,r=UI_PM_CARD_RADIUS-1,d0,d1;
    if(!layer->draw_buf || layer->draw_buf->header.cf!=LV_COLOR_FORMAT_I1){
        /* Inversion needs a 1-bit layer; other formats show outlines only. */
        return;
    }
    if(!underline_span(card,pm_selected,w+h,&d0,&d1))return;
    x++;y++;
    for(int i=0;i<h;i++){
        float dy=fmaxf(0,fmaxf(r-(i+.5f),(i+.5f)-(h-r)));
        int inset=dy>0?(int)lroundf(r-sqrtf(fmaxf(0,r*r-dy*dy))):0;
        int lo=LV_MAX(inset,d0-i),hi=LV_MIN(w-inset,d1-i); /* sweep distance is x + row */
        if(hi>lo)direct_fill(x+lo,y+i+(int)offset,hi-lo,1,FILL_INVERT);
    }
}
static void pm_cards(int top){
    const char *unit="\xc2\xb5" "g/m\xc2\xb3";char value[32];
    /* Card rows: equal gaps between the card edges and each row's ink. */
    static const char *const card_canon[]={"PM1PM2.5PM4PM10","0123456789","\xc2\xb5" "g/m\xc2\xb3"};
    static const int card_size[]={UI_PM_LABEL_SIZE,UI_PM_CARD_VALUE_SIZE,UI_PM_CARD_UNIT_SIZE};
    int row[3];stack_rows(3,card_canon,card_size,UI_PM_CARD_ROW_GAP,top+UI_PM_CARD_HEIGHT/2,row,NULL);
    for(int i=0;i<4;i++){
        int x=UI_MARGIN+i*(UI_PM_CARD_WIDTH+UI_PM_CARD_GAP),mid=x+UI_PM_CARD_WIDTH/2;
        /* Draw every card unselected, then invert the filled part of its inside, so
           the text flips to paper exactly where the fill covers it. */
        pill(x,top,UI_PM_CARD_WIDTH,UI_PM_CARD_HEIGHT,UI_PM_CARD_RADIUS,false);
        pill(x+1,top+1,UI_PM_CARD_WIDTH-2,UI_PM_CARD_HEIGHT-2,UI_PM_CARD_RADIUS-1,true);
        centered_text(pm_labels[i],mid,row[0],UI_PM_LABEL_SIZE,false);
        format_reading(value,sizeof value,pm_value(sensor,i)<0?NAN:pm_value(sensor,i),1,false);
        centered_text(value,mid,row[1],UI_PM_CARD_VALUE_SIZE,false);
        centered_text(unit,mid,row[2],UI_PM_CARD_UNIT_SIZE,false);
        pm_card_fill(x,top,i);
    }
}
/* Everything below the particle field: the cards and the history chart. Selection
   changes redraw only this box (and the field on its own ticks), not the page. */
static lv_area_t pm_lower_area(void){return (lv_area_t){0,UI_PM_FIELD_BOTTOM+1,UI_WIDTH-1,UI_HEIGHT-1};}
static void draw_pm_lower(void){
    int y[3],value_y[2];pm_layout(y,value_y);
    rect(0,0,UI_WIDTH,UI_HEIGHT,true,0);pm_cards(y[2]);pm_history_chart();
}
static void draw_pm_detail(void){
    const char *unit="\xc2\xb5" "g/m\xc2\xb3";
    detail_header("PARTICLES",rating_text(rate_pm(pm_selected)));
    char value[32];
    format_reading(value,sizeof value,pm_value(sensor,pm_selected)<0?NAN:pm_value(sensor,pm_selected),1,false);
    int y[3],value_y[2];
    pm_layout(y,value_y);
    int center=pm_value_center();
    centered_text(value,center,value_y[0],pm_value_size(),false);
    centered_text(unit,center,value_y[1],UI_FONT_UNIT,false);pm_particle_field();
    pm_cards(y[2]);
    pm_history_chart();
}
static void climate_chart(int key,int top){
    static const char *const name[]={"Temperature","Humidity","Pressure"};
    static const char *const unit[]={"\xC2\xB0" "C","%","hPa"};
    static const float range[]={UI_CLIMATE_MIN_TEMP_RANGE,UI_CLIMATE_MIN_RH_RANGE,UI_CLIMATE_MIN_PRESSURE_RANGE};
    (void)top;
    history_chart(key,name[key-4],unit[key-4],range[key-4],UI_CLIMATE_PADDING);
}
/* Three rows, one per reading, separated by rules across the content width.
   Each row reads left to right: label, value, unit, divider, icon. Label, value
   and unit share the value's baseline, so the value can take most of the row
   height. Labels start at the left margin; each value is centred on the display
   with its unit just after it. The divider and icon fill the space right of the
   widest value and unit (at the sensor maximums), in three equal gaps. */
struct climate_layout_t{
    char value[3][32];
    int label_x,value_center,divider_x,icon_x;
    int ink_top[3],baseline[3],rule[2],row_center[3];
};
typedef struct climate_layout_t climate_layout_t;
static const char *const climate_labels[3]={"Temperature","Humidity","Pressure"};
static const char *const climate_units[3]={"\xC2\xB0" "C","%","hPa"};
static int font_baseline(int size){const lv_font_t *f=dashboard_font(size);return f->line_height-f->base_line;}
static climate_layout_t climate_layout(void){
    climate_layout_t l;
    const float reading[3]={sensor.temp,sensor.rh,sensor.pressure};
    /* "60.0" for temperature, "100" for humidity, "1100" for pressure. */
    const int widest[3]={widest_number(3,true,UI_CLIMATE_VALUE_SIZE),widest_number(3,false,UI_CLIMATE_VALUE_SIZE),
        widest_number(4,false,UI_CLIMATE_VALUE_SIZE)};
    l.label_x=UI_MARGIN;l.value_center=UI_WIDTH/2;
    int reach=0;
    for(int row=0;row<3;row++){
        format_reading(l.value[row],sizeof l.value[row],reading[row],row==1?0:1,false);
        reach=LV_MAX(reach,l.value_center+(widest[row]+1)/2+UI_CLIMATE_UNIT_GAP+
            ink_width(climate_units[row],UI_CLIMATE_UNIT_SIZE));
    }
    int spare=UI_WIDTH-UI_MARGIN-reach-UI_DIVIDER-UI_CLIMATE_ICON_SIZE;
    l.divider_x=reach+spare/3;
    /* Icons centre between the divider and the right margin. */
    l.icon_x=(l.divider_x+UI_DIVIDER+UI_WIDTH-UI_MARGIN)/2;
    /* Equal space above and below every row's value ink, between the header
       rule, the two row rules and the history panel. */
    lv_area_t digits=header_ink_bounds("0123456789.",dashboard_font(UI_CLIMATE_VALUE_SIZE));
    int h=digits.y2-digits.y1+1,top=UI_HEADER_BOTTOM+UI_DIVIDER;
    int free=UI_HISTORY_PANEL_Y-top-3*h-2*UI_DIVIDER,g=free/6;
    int y=top+(free-6*g)/2;
    int value_baseline=font_baseline(UI_CLIMATE_VALUE_SIZE)-digits.y1;
    for(int row=0;row<3;row++){
        y+=g;l.ink_top[row]=y;l.baseline[row]=y+value_baseline;l.row_center[row]=y+h/2;y+=h+g;
        if(row<2){l.rule[row]=y;y+=UI_DIVIDER;}
    }
    return l;
}
static int climate_row_at(int y){
    climate_layout_t l=climate_layout();
    return y<l.rule[0]?0:y<l.rule[1]?1:2;
}
/* Opaque disc from spans: a pixel is inside when its centre lies within r+0.5,
   so the outline stays round with no anti-aliased edge to lose when thresholded. */
static void disc(int cx,int cy,float r,bool paper){
    float reach=r+.5f;
    for(int dy=-(int)reach;dy<=(int)reach;dy++){
        int w=(int)floorf(sqrtf(fmaxf(0,reach*reach-dy*dy)));
        rect(cx-w,cy+dy,2*w+1,1,paper,0);
    }
}
/* Thick segment whose half-width tapers from ra at (ax,ay) to rb at (bx,by),
   filled where pixel centres lie within it: smooth edges at any angle, unlike
   stepping a square brush along a Bresenham line. Runs become one-row spans. */
static void segment(float ax,float ay,float bx,float by,float ra,float rb){
    float dx=bx-ax,dy=by-ay,len2=fmaxf(dx*dx+dy*dy,1e-6f),r=fmaxf(ra,rb);
    int x0=(int)floorf(fminf(ax,bx)-r),x1=(int)ceilf(fmaxf(ax,bx)+r);
    int y0=(int)floorf(fminf(ay,by)-r),y1=(int)ceilf(fmaxf(ay,by)+r);
    for(int y=y0;y<=y1;y++){
        int run=-1;
        for(int x=x0;x<=x1+1;x++){
            bool in=false;
            if(x<=x1){
                float u=clamp(((x-ax)*dx+(y-ay)*dy)/len2,0,1);
                float ex=x-(ax+u*dx),ey=y-(ay+u*dy),w=ra+(rb-ra)*u;
                in=ex*ex+ey*ey<=w*w;
            }
            if(in && run<0)run=x;
            if(!in && run>=0){rect(run,y,x-run,1,false,0);run=-1;}
        }
    }
}
/* Formaldehyde page: the CO2 layout. The reading's column is the title's ink width,
   in the largest font whose widest reading ("999.9", or "1000" from 1000 ppb) fits;
   H2CO molecules float in the field to its right. */
static int hcho_title_width(void){return ink_width("FORMALDEHYDE",UI_FONT_TITLE);}
static int hcho_field_left(void){return UI_MARGIN+hcho_title_width()+UI_HCHO_FIELD_GAP;}
static int hcho_value_size(void){
    static const int sizes[]={UI_CO2_VALUE_SIZE,72,64,60,48,40};
    for(int i=0;i<(int)(sizeof sizes/sizeof *sizes);i++)
        if(LV_MAX(widest_number(4,true,sizes[i]),widest_number(4,false,sizes[i]))<=hcho_title_width())return sizes[i];
    return 40;
}
/* Molecule slots in joining order: home offset from the field's top-left corner,
   bond length, and a seed for drift and spin. Packed offline so the ink of any two
   stays at least 2 px apart through their drift and spin; homes are clamped so
   the full extent stays inside the field. */
typedef struct { float x,y,bond,seed; } hcho_slot;
static const hcho_slot hcho_slots[UI_HCHO_MOLECULES]={
    {28,40,17,0},{120,116,15,1},{120,8,14,2},{40,116,13,3},{84,64,12,4},
    {72,8,12,5},{124,64,11,6},{80,116,11,7},{8,84,10,8},{48,76,10,9}};
static float hcho_extent(int j){return hcho_slots[j].bond*1.36f+UI_HCHO_WANDER+1;}
/* Molecules shown: from UI_HCHO_MIN near zero to every slot at UI_HCHO_FULL, on a
   log scale so each step of the range still adds molecules. */
static int hcho_target(void){
    float v=isfinite(sensor.hcho)?sensor.hcho:isinf(sensor.hcho) && sensor.hcho>0?UI_HCHO_FULL:0;
    float f=clamp(logf(1+fmaxf(v,0)/UI_HCHO_LOG_KNEE)/logf(1+UI_HCHO_FULL/UI_HCHO_LOG_KNEE),0,1);
    return UI_HCHO_MIN+(int)lroundf(f*(UI_HCHO_MOLECULES-UI_HCHO_MIN));
}
/* Joining molecules grow from their carbon and leaving ones shrink into it, so a
   reading change never pops one in or out. */
static void hcho_grow_step(double dt){
    int target=hcho_target();
    for(int j=0;j<UI_HCHO_MOLECULES;j++)
        hcho_grow[j]=clamp(hcho_grow[j]+(j<target?1:-1)*(float)dt/UI_HCHO_GROW_TIME,0,1);
}
static void hcho_ring(int x,int y,float r,float stroke){disc(x,y,r,false);if(r>stroke)disc(x,y,r-stroke,true);}
static void hcho_molecule(float x,float y,float angle,float bond,float scale){
    float l=bond*scale;bool big=bond>=15;
    float half=fmaxf(.8f,(big?1.5f:1.2f)*scale);
    float ox=x+cosf(angle)*l,oy=y+sinf(angle)*l;
    float hx[2],hy[2];
    for(int k=0;k<2;k++){
        float a=angle+(k?2.0944f:-2.0944f);
        hx[k]=x+cosf(a)*l*.8f;hy[k]=y+sinf(a)*l*.8f;
    }
    segment(x,y,ox,oy,half,half);
    for(int k=0;k<2;k++)segment(x,y,hx[k],hy[k],half,half);
    disc((int)lroundf(ox),(int)lroundf(oy),.36f*l,false);
    if(hcho_atom_count<UI_HCHO_MOLECULES){
        hcho_atoms[hcho_atom_count][0]=ox;hcho_atoms[hcho_atom_count][1]=oy;hcho_atoms[hcho_atom_count++][2]=.36f*l;
    }
    float stroke=big?2.0f:1.5f;
    hcho_ring((int)lroundf(x),(int)lroundf(y),.3f*l,stroke);
    for(int k=0;k<2;k++)hcho_ring((int)lroundf(hx[k]),(int)lroundf(hy[k]),.22f*l,stroke);
}
static void hcho_molecules(void){
    lv_area_t clip=layer->_clip_area;
    int left=hcho_field_left();
    hcho_atom_count=0;
    layer->_clip_area=(lv_area_t){LV_MAX(clip.x1,left),LV_MAX(clip.y1,UI_HCHO_FIELD_TOP),
        LV_MIN(clip.x2,UI_HCHO_FIELD_RIGHT-1),LV_MIN(clip.y2,UI_HCHO_FIELD_BOTTOM-1)};
    for(int j=0;j<UI_HCHO_MOLECULES;j++){
        float s=ease(hcho_grow[j]);
        const hcho_slot *m=&hcho_slots[j];
        if(s*m->bond<3)continue;
        float e=hcho_extent(j);
        float x=clamp(left+m->x,left+e,UI_HCHO_FIELD_RIGHT-e),y=clamp(UI_HCHO_FIELD_TOP+m->y,UI_HCHO_FIELD_TOP+e,UI_HCHO_FIELD_BOTTOM-e);
        /* Slow drift and spin; time phases wrap in double before converting to float. */
        float wx=(float)fmod(anim_t*(.41+.05*m->seed),TAU),wy=(float)fmod(anim_t*(.29+.04*m->seed),TAU);
        float spin=(float)fmod(anim_t*((int)m->seed%2?-.22:.18),TAU);
        x+=sinf(wx+m->seed*1.7f)*UI_HCHO_WANDER;y+=sinf(wy+m->seed*2.3f)*UI_HCHO_WANDER;
        hcho_molecule(x,y,m->seed*2.4f+spin,m->bond,s);
    }
    layer->_clip_area=clip;
}
static void draw_hcho_detail(void){
    int size=hcho_value_size();
    char value[32];format_reading(value,sizeof value,sensor.hcho,1,false);
    detail_header("FORMALDEHYDE",rating_text(rate_hcho()));
    /* Centred like the CO2 value/unit block, on canonical ink bounds. */
    lv_area_t digits=header_ink_bounds("0123456789.",dashboard_font(size));
    lv_area_t unit=header_ink_bounds("PPB",dashboard_font(UI_HCHO_UNIT_SIZE));
    int content_top=UI_HEADER_BOTTOM+UI_DIVIDER;
    int unit_offset=digits.y2+1+UI_CO2_UNIT_GAP-unit.y1;
    int block_height=unit_offset+unit.y2-digits.y1+1;
    int value_y=content_top+(UI_HISTORY_PANEL_Y-content_top-block_height)/2-digits.y1;
    int center=UI_MARGIN+hcho_title_width()/2;
    centered_text(value,center,value_y,size,false);
    centered_text("PPB",center,value_y+unit_offset,UI_HCHO_UNIT_SIZE,false);
    hcho_molecules();
    history_panel();
    history_chart(7,"HCHO","PPB",UI_HCHO_CHART_MIN_RANGE,UI_CHART_PADDING);
}
static void climate_icons(const climate_layout_t *l){
    const int s=UI_CLIMATE_ICON_SIZE,x=l->icon_x;
    /* The row rules cross the icon column: drawn here so partial redraws keep them. */
    for(int i=0;i<2;i++)rect(UI_MARGIN,l->rule[i],UI_WIDTH-2*UI_MARGIN,UI_DIVIDER,false,0);

    /* Thermometer: outlined stem and bulb, mercury filled to the temperature. */
    int top=l->row_center[0]-s/2,bulb=top+s-1-UI_CLIMATE_BULB_RADIUS;
    pill(x-UI_CLIMATE_STEM_HALF,top,2*UI_CLIMATE_STEM_HALF+1,bulb-top,UI_CLIMATE_STEM_HALF,false);
    disc(x,bulb,UI_CLIMATE_BULB_RADIUS,false);
    pill(x-UI_CLIMATE_STEM_HALF+UI_CLIMATE_STROKE,top+UI_CLIMATE_STROKE,2*(UI_CLIMATE_STEM_HALF-UI_CLIMATE_STROKE)+1,
        bulb-top,UI_CLIMATE_STEM_HALF-UI_CLIMATE_STROKE,true);
    disc(x,bulb,UI_CLIMATE_BULB_RADIUS-UI_CLIMATE_STROKE,true);
    float n=isfinite(sensor.temp)?norm(sensor.temp,UI_CLIMATE_TEMP_MIN,UI_CLIMATE_TEMP_MAX):0;
    int column=bulb-top-UI_CLIMATE_STEM_HALF,fill=(int)lroundf(column*n);
    /* Rounded top; the bottom end is hidden in the bulb. */
    pill(x-UI_CLIMATE_MERCURY_HALF,bulb-fill,2*UI_CLIMATE_MERCURY_HALF+1,fill+1,UI_CLIMATE_MERCURY_HALF,false);
    disc(x,bulb,UI_CLIMATE_BULB_RADIUS-2*UI_CLIMATE_STROKE,false);

    /* Droplet: pointed top, round base, water surface waving at the humidity level. */
    int height=UI_CLIMATE_DROP_HEIGHT-1,r=UI_CLIMATE_DROP_RADIUS;
    top=l->row_center[1]-UI_CLIMATE_DROP_HEIGHT/2;
    float level=top+height-(isfinite(sensor.rh)?norm(sensor.rh,0,100):0)*height;
    float phase=(float)fmod(anim_t*UI_CLIMATE_WAVE_SPEED,TAU);
    float surface_at[2*UI_CLIMATE_DROP_RADIUS+1];
    for(int xx=-r;xx<=r;xx++)
        surface_at[xx+r]=level+UI_CLIMATE_WAVE_AMPLITUDE*sinf(xx*UI_CLIMATE_WAVE_LENGTH+phase);
    /* A round base of radius r with straight sides tangent to it from the tip. */
    float base=height-r+.5f,tangent=base-r*r/base,slope=r/sqrtf(base*base-r*r);
    for(int yy=0;yy<=height;yy++){
        float half=yy<tangent?yy*slope:sqrtf(fmaxf(0,(r+.5f)*(r+.5f)-(yy-base)*(yy-base)));
        int w=(int)lroundf(half);
        rect(x-w,top+yy,2*w+1,1,false,0);
        for(int xx=-w+UI_CLIMATE_STROKE;xx<=w-UI_CLIMATE_STROKE;xx++)
            if(top+yy<surface_at[xx+r] && yy<height-UI_CLIMATE_STROKE)rect(x+xx,top+yy,1,1,true,0);
    }

    /* Gauge: ring, ticks over a 270-degree dial, needle at the pressure with a slight tremor. */
    int cy=l->row_center[2];
    float outer=s/2.0f-1,inner=outer-UI_CLIMATE_STROKE;
    disc(x,cy,outer,false);disc(x,cy,inner,true);
    for(int i=0;i<UI_CLIMATE_GAUGE_TICKS;i++){
        float a=UI_CLIMATE_GAUGE_START+i*UI_CLIMATE_GAUGE_SWEEP/(UI_CLIMATE_GAUGE_TICKS-1);
        segment(x+cosf(a)*(inner-UI_CLIMATE_GAUGE_TICK),cy-sinf(a)*(inner-UI_CLIMATE_GAUGE_TICK),
            x+cosf(a)*(inner-1.5f),cy-sinf(a)*(inner-1.5f),1,1);
    }
    float p=isfinite(sensor.pressure)?norm(sensor.pressure,UI_CLIMATE_PRESSURE_MIN,UI_CLIMATE_PRESSURE_MAX):0;
    float a=UI_CLIMATE_GAUGE_START+p*UI_CLIMATE_GAUGE_SWEEP+
        UI_CLIMATE_GAUGE_TREMOR*sinf((float)fmod(anim_t*UI_CLIMATE_GAUGE_TREMOR_SPEED,TAU));
    float needle=inner-UI_CLIMATE_GAUGE_TICK-3;
    segment(x,cy,x+cosf(a)*needle,cy-sinf(a)*needle,UI_CLIMATE_NEEDLE_BASE,UI_CLIMATE_NEEDLE_TIP);
    disc(x,cy,UI_CLIMATE_GAUGE_HUB,false);
}
static void draw_climate_detail(void){
    detail_header("CLIMATE",rating_text(rate_climate(climate_selected)));
    climate_layout_t l=climate_layout();
    int label_base=font_baseline(UI_CLIMATE_LABEL_SIZE),unit_base=font_baseline(UI_CLIMATE_UNIT_SIZE),
        value_base=font_baseline(UI_CLIMATE_VALUE_SIZE);
    for(int row=0;row<3;row++){
        int b=l.baseline[row];
        lv_area_t ink=header_ink_bounds(climate_labels[row],dashboard_font(UI_CLIMATE_LABEL_SIZE));
        dashboard_text(climate_labels[row],l.label_x-ink.x1,b-label_base,UI_CLIMATE_LABEL_SIZE,false);
        /* As on Gases, the underlined label names the reading the history chart shows. */
        /* Centred in the space between the label's descenders and the row's rule
           (or the history panel), so it touches neither. */
        int x0,x1;
        if(underline_span(row,climate_selected,ink.x2-ink.x1+1,&x0,&x1)){
            int below=b-label_base+ink.y2+1,limit=row<2?l.rule[row]:UI_HISTORY_PANEL_Y;
            rect(l.label_x+x0,below+(limit-below-UI_GAS_UNDERLINE)/2,x1-x0,UI_GAS_UNDERLINE,false,0);
        }
        ink=header_ink_bounds(l.value[row],dashboard_font(UI_CLIMATE_VALUE_SIZE));
        int value_x=l.value_center-(ink.x1+ink.x2+1)/2;
        dashboard_text(l.value[row],value_x,b-value_base,UI_CLIMATE_VALUE_SIZE,false);
        int unit_left=value_x+ink.x2+1+UI_CLIMATE_UNIT_GAP;
        ink=header_ink_bounds(climate_units[row],dashboard_font(UI_CLIMATE_UNIT_SIZE));
        dashboard_text(climate_units[row],unit_left-ink.x1,b-unit_base,UI_CLIMATE_UNIT_SIZE,false);
        rect(l.divider_x,l.row_center[row]-UI_CLIMATE_ICON_SIZE/2,UI_DIVIDER,UI_CLIMATE_ICON_SIZE,false,0);
    }
    climate_icons(&l);history_panel();
    climate_chart(4+climate_selected,UI_CHART_TOP);
}
/* Ribbon weight follows the same cutoffs as the rating badge: half weight at
   Fair, full weight at Poor, so each ribbon visibly tracks its index readout. */
static float gas_level(int lane){
    float value=lane?sensor.nox:sensor.voc,fair=band_start(ATMOS_VOC+lane,1,lane?UI_NOX_FAIR:UI_VOC_FAIR);
    float poor=fmaxf(band_start(ATMOS_VOC+lane,2,lane?UI_NOX_POOR:UI_VOC_POOR),fair+1);
    if(!isfinite(value))return 0;
    return value<fair?.5f*norm(value,0,fair):.5f+.5f*norm(value,fair,poor);
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
/* Furthest any ribbon or mote pixel reaches from content_center(). */
#define UI_GAS_ART_EXTENT ((int)(UI_GAS_RIBBON_GAP+UI_GAS_WAVE_AMPLITUDE+UI_GAS_HALF_WIDTH+UI_GAS_WIDTH_GAIN+UI_GAS_PARTICLE_OFFSET)+6)
static void gas_ribbons(void){
    const int left=UI_GAS_RIBBON_LEFT,right=UI_GAS_RIBBON_RIGHT;
    /* The animated bend makes the S asymmetric, so centre the drawn envelope
       (top ribbon's upper edge to bottom ribbon's lower edge) on content_center()
       each frame. The wave and taper depend only on the column: compute once. */
    float wave[UI_GAS_RIBBON_RIGHT-UI_GAS_RIBBON_LEFT+1],taper[UI_GAS_RIBBON_RIGHT-UI_GAS_RIBBON_LEFT+1];
    float half[2]={UI_GAS_HALF_WIDTH+UI_GAS_WIDTH_GAIN*gas_intensity[0],UI_GAS_HALF_WIDTH+UI_GAS_WIDTH_GAIN*gas_intensity[1]};
    float high=INFINITY,low=-INFINITY;
    for(int x=left;x<=right;x++){
        float u=(float)(x-left)/(right-left);
        wave[x-left]=gas_wave(u);
        /* Both edges meet at the tips; broad shoulders carry visual weight. */
        /* sin(pi) rounds slightly below zero; pow() of a negative is NaN. */
        taper[x-left]=pow(fmax(0,sin(TAU*u/2)),.8f);
        high=fmin(high,wave[x-left]-half[0]*taper[x-left]);
        low=fmax(low,wave[x-left]+half[1]*taper[x-left]);
    }
    float ribbon_top=content_center()-UI_GAS_RIBBON_GAP-(high+low)/2;
    gas_ribbon_top=ribbon_top;
    for(int lane=0;lane<3;lane++){
        float center=ribbon_top+lane*UI_GAS_RIBBON_GAP;
        for(int x=left;x<=right;x++){
            float y=center+wave[x-left];
            if(lane==1){
                if(x>left)line(x-1,center+wave[x-1-left],x,y,1,false);
                continue;
            }
            float w=half[lane/2]*taper[x-left];
            int top=(int)ceil(y-w),bottom=(int)floor(y+w);
            if(bottom>=top)rect(x,top,1,bottom-top+1,false,0);
        }
    }
    /* Sparse suspended motes trace the outer edge of the outer ribbons at a
       fixed clearance, so they follow the tapering silhouette, never between
       ribbons. Geometry shrinks at the ends, avoiding a visible wrap/reset. */
    for(int i=0;i<UI_GAS_PARTICLES;i++){
        float u=fmod(anim_t*UI_GAS_PARTICLE_SPEED/(right-left)+i*.618034f,1);
        float x=left+u*(right-left);
        float r=(i%3==0?2:1)*fmin(1,8*fmin(u,1-u));
        int lower=i%2;
        /* Each ribbon carries more motes as its index rises. */
        if(r<.65f || i/2>=1+(int)round((UI_GAS_PARTICLES/2-1)*gas_intensity[lower]))continue;
        float edge=(UI_GAS_HALF_WIDTH+UI_GAS_WIDTH_GAIN*gas_intensity[lower])*pow(fmax(0,sin(TAU*u/2)),.8f);
        float reach=edge+UI_GAS_PARTICLE_OFFSET+r;
        float y=ribbon_top+gas_wave(u)+(lower?2*UI_GAS_RIBBON_GAP+reach:-reach);
        circle(x,y,(int)round(r),1,false);
    }
}
static void gas_chart(int key,int top){
    (void)top;
    history_chart(key,key==2?"VOC":"NOx","Index",
        key==2?UI_GAS_CHART_MIN_VOC:UI_GAS_CHART_MIN_NOX,UI_GAS_CHART_PADDING);
}
/* Label, value and unit form one block centred on content_center(), with equal
   gaps between their visible ink. Canonical digit bounds keep rows stable as
   readings change; each string is centred on its column by its own ink. */
static void draw_gas_detail(void){
    detail_header("GASES",rating_text(rate_gas(gas_selected)));
    lv_area_t label=header_ink_bounds("VOCNOx",dashboard_font(UI_GAS_LABEL_SIZE));
    lv_area_t digits=header_ink_bounds("0123456789.-",dashboard_font(UI_GAS_VALUE_SIZE));
    lv_area_t unit=header_ink_bounds("Index",dashboard_font(UI_FONT_UNIT));
    int label_h=label.y2-label.y1+1,digits_h=digits.y2-digits.y1+1,unit_h=unit.y2-unit.y1+1;
    int block=label_h+digits_h+unit_h+2*UI_TEXT_GAP;
    int top=content_center()-block/2;
    int value_top=top+label_h+UI_TEXT_GAP,unit_top=value_top+digits_h+UI_TEXT_GAP;
    /* Each column is as wide as its widest visible ink, so the text starts at
       the same inset from the left edge as the ribbons end from the right. */
    char values[2][32];int width[2];
    for(int col=0;col<2;col++){
        format_reading(values[col],sizeof values[col],col?sensor.nox:sensor.voc,0,false);
        lv_area_t a=header_ink_bounds(col?"NOx":"VOC",dashboard_font(UI_GAS_LABEL_SIZE));
        width[col]=LV_MAX(LV_MAX(a.x2-a.x1+1,widest_number(3,false,UI_GAS_VALUE_SIZE)),unit.x2-unit.x1+1);
    }
    /* Spread the leftover width evenly: column, gap, divider, gap, column,
       gap, ribbons, where each gap equals the space on both sides of the divider. */
    int gap=(UI_GAS_RIBBON_LEFT-UI_GAS_SIDE_INSET-width[0]-width[1]-UI_DIVIDER)/3;
    gas_divider_x=UI_GAS_SIDE_INSET+width[0]+gap;
    gas_line(gas_divider_x,top,gas_divider_x,top+block-1,UI_DIVIDER,false);
    for(int col=0;col<2;col++){
        int left=col?gas_divider_x+UI_DIVIDER+gap:UI_GAS_SIDE_INSET;
        int center=left+width[col]/2;
        centered_text(col?"NOx":"VOC",center,top-label.y1,UI_GAS_LABEL_SIZE,false);
        /* The underlined label names the channel the history chart shows. */
        lv_area_t a=header_ink_bounds(col?"NOx":"VOC",dashboard_font(UI_GAS_LABEL_SIZE));
        int w=a.x2-a.x1+1,x0,x1;
        if(underline_span(col,gas_selected,w,&x0,&x1)){
            rect(center-w/2+x0,top+label_h+UI_GAS_UNDERLINE_GAP,x1-x0,UI_GAS_UNDERLINE,false,0);
        }
        centered_text(values[col],center,value_top-digits.y1,UI_GAS_VALUE_SIZE,false);
        centered_text("Index",center,unit_top-unit.y1,UI_CLIMATE_UNIT_SIZE,false);
    }
    gas_ribbons();history_panel();
    gas_chart(gas_selected?3:2,UI_CHART_TOP);
}
static void draw_scene(int which){
    rect(0,0,UI_WIDTH,UI_HEIGHT,true,0);
    switch(which){case 0:draw_dashboard();break;case 1:draw_co2_detail();break;
        case 2:draw_pm_detail();break;case 3:draw_gas_detail();break;case 4:draw_climate_detail();break;default:draw_hcho_detail();break;}
}
/* Fixed-point transition maths: Q16 progress/scale, Q8 screen positions,
   one exact integer square root per circle per row. No floating point per pixel. */
#define FX 65536
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
    if((unsigned)x>=UI_WIDTH || (unsigned)y>=UI_HEIGHT)return;
    uint8_t mask=(uint8_t)(0x80>>(x&7));uint8_t *b=&bits[y*ROW_BYTES+(x>>3)];
    *b=paper?(uint8_t)(*b|mask):(uint8_t)(*b&~mask);
}
/* Transitions are chosen by the page being left: each page opens onto the next through
   its own artwork, so any page can be reached from any other. */
enum{EFFECT_PORTAL,EFFECT_DUST,EFFECT_RIBBONS,EFFECT_DROPLET,EFFECT_ATOM};
static int transition_effect(int from){
    static const int effects[ATMOS_SCENES]={EFFECT_PORTAL,EFFECT_PORTAL,EFFECT_DUST,EFFECT_RIBBONS,EFFECT_DROPLET,EFFECT_ATOM};
    return effects[from];
}
/* The overview zooms into its status circle; CO2 into one of its visible bubbles. */
static void select_portal(int from){
    portal_x=UI_FLOWER_X<<8;portal_y=UI_FLOWER_Y<<8;portal_radius=25<<8;
    if(from!=1)return;
    int best=-1;
    for(int j=0;j<UI_BUBBLE_COUNT;j++){
        if(!co2_bubble_visible(j))continue;
        float x,y;co2_bubble_position(j,&x,&y);int r=co2_bubble_radius(j);
        if(y-r>=UI_BUBBLE_TOP && y+r<=UI_BUBBLE_BOTTOM && r>best){
            best=r;portal_x=(int32_t)lround(x*256);portal_y=(int32_t)lround(y*256);portal_radius=r<<8;
        }
    }
}
/* Reference compositors (the original per-pixel versions). The self-test requires
   the fast versions below to match them bit for bit. */
static void compose_portal_ref(int from,int to,int32_t progress){
    (void)to;
    /* The portal page is the one being left: zoom into its symbol. */
    bool reverse=false;
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
    bool ring_on=from==1 && q>0 && q<FX;
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
    (void)to;
    /* The portal page is the one being left: zoom into its symbol. */
    bool reverse=false;
    int32_t q=fx_ease(reverse?FX-progress:progress);
    const uint8_t *outer=SCENE_BITS(reverse?1:0),*inner=SCENE_BITS(reverse?0:1);
    int32_t cx=portal_x+(int32_t)(((int64_t)((200<<8)-portal_x)*q)>>16);
    int32_t cy=portal_y+(int32_t)(((int64_t)((150<<8)-portal_y)*q)>>16);
    int32_t q2=(int32_t)(((int64_t)q*q)>>16);
    int64_t zoom=FX+18LL*q2,arrival=FX+18LL*(FX-q)/100;
    int64_t inv_zoom=((int64_t)FX<<16)/zoom,inv_arrival=((int64_t)FX<<16)/arrival;
    int32_t opening=fx_ease((int32_t)((int64_t)(q-FX*8/100)*100/92));
    int64_t radius=((int64_t)opening*430)>>8,radius2=opening>0?radius*radius:-1;
    bool ring_on=from==1 && q>0 && q<FX;
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
/* Effect anchors, taken from the incoming snapshot by prepare_effect(). Q8 pixels. */
#define BLOOM_MAX 32
typedef struct{int32_t x,y,radius,delay;} bloom_t;
static bloom_t blooms[BLOOM_MAX];
static int bloom_count;
static int32_t ribbon_wave[UI_WIDTH],ribbon_top;
static int drop_x,drop_top,drop_base;
static int16_t drop_half[UI_CLIMATE_DROP_HEIGHT];
static int32_t drop_reach,atom_x,atom_y,atom_reach;
/* Distance from (x, y) to the farthest frame corner, plus a margin, in Q8. */
static int32_t farthest_corner(float x,float y){
    float dx=fmaxf(x,UI_WIDTH-x),dy=fmaxf(y,UI_HEIGHT-y);
    return (int32_t)(sqrtf(dx*dx+dy*dy)*256)+(4<<8);
}
/* Called right after the outgoing scene is rasterised (anim_t still at its snapshot
   time), so anchors match the artwork the transition opens through. */
static void prepare_effect(int from){
    switch(transition_effect(from)){
    case EFFECT_PORTAL:select_portal(from);break;
    case EFFECT_DUST:{
        /* The page's largest specks bloom first, then a jittered grid seeds
           blooms across the frame, later with distance from the field, so the
           blooms always cover the frame by the end. */
        int order[UI_PM_PARTICLE_MAX];
        for(int i=0;i<pm_dust_count;i++){
            int j=i;
            while(j>0 && pm_dust[order[j-1]][2]<pm_dust[i][2]){order[j]=order[j-1];j--;}
            order[j]=i;
        }
        bloom_count=0;
        for(int i=0;i<LV_MIN(pm_dust_count,10);i++){
            const int16_t *d=pm_dust[order[i]];
            blooms[bloom_count++]=(bloom_t){d[0]<<8,d[1]<<8,70<<8,FX*3*i/100};
        }
        const float fx=UI_PM_FIELD_CX,fy=(UI_PM_FIELD_TOP+UI_PM_FIELD_BOTTOM)/2.0f;
        for(int r=0;r<4;r++)for(int c=0;c<5;c++){
            int x=40+80*c+(r*7+c*13)%25-12,y=37+75*r+(r*11+c*5)%25-12;
            float distance=hypotf(x-fx,y-fy);
            int32_t delay=FX*15/100+(int32_t)(FX*.3f*fminf(distance/450.0f,1));
            blooms[bloom_count++]=(bloom_t){x<<8,y<<8,80<<8,delay};
        }
    }break;
    case EFFECT_RIBBONS:
        ribbon_top=(int32_t)lroundf(gas_ribbon_top*256);
        for(int x=0;x<UI_WIDTH;x++){
            float u=(float)(x-UI_GAS_RIBBON_LEFT)/(UI_GAS_RIBBON_RIGHT-UI_GAS_RIBBON_LEFT);
            ribbon_wave[x]=(int32_t)lroundf(gas_wave(u)*256);
        }
        break;
    case EFFECT_DROPLET:{
        /* The humidity icon's droplet outline: a round base with straight sides to the tip. */
        climate_layout_t l=climate_layout();
        int height=UI_CLIMATE_DROP_HEIGHT-1,r=UI_CLIMATE_DROP_RADIUS;
        float base=height-r+.5f,tangent=base-r*r/base,slope=r/sqrtf(base*base-r*r);
        for(int yy=0;yy<=height;yy++){
            float half=yy<tangent?yy*slope:sqrtf(fmaxf(0,(r+.5f)*(r+.5f)-(yy-base)*(yy-base)));
            drop_half[yy]=(int16_t)lroundf(half);
        }
        drop_x=l.icon_x;drop_top=l.row_center[1]-UI_CLIMATE_DROP_HEIGHT/2;drop_base=drop_top+(int)lroundf(base);
        drop_reach=farthest_corner(drop_x,drop_base);
    }break;
    case EFFECT_ATOM:{
        /* The largest molecule's oxygen; the field centre if none is shown. */
        float x=(hcho_field_left()+UI_HCHO_FIELD_RIGHT)/2.0f,y=(UI_HCHO_FIELD_TOP+UI_HCHO_FIELD_BOTTOM)/2.0f,best=-1;
        for(int i=0;i<hcho_atom_count;i++)
            if(hcho_atoms[i][2]>best){best=hcho_atoms[i][2];x=hcho_atoms[i][0];y=hcho_atoms[i][1];}
        atom_x=(int32_t)lroundf(x*256);atom_y=(int32_t)lroundf(y*256);atom_reach=farthest_corner(x,y);
    }break;
    }
}
/* Mask compositors: each output row is first coded per column (outgoing, incoming,
   ink or paper), then assembled a byte at a time. */
enum{CODE_OUT,CODE_IN,CODE_INK,CODE_PAPER};
static uint8_t row_code[UI_WIDTH];
static void code_span(int lo,int hi,uint8_t code){
    lo=LV_MAX(lo,0);hi=LV_MIN(hi,UI_WIDTH-1);
    if(lo<=hi)memset(row_code+lo,code,(size_t)(hi-lo+1));
}
/* Columns of row y inside a circle (centre and radius in Q8). */
static void code_disc(int y,int32_t cx,int32_t cy,int32_t radius,uint8_t code){
    if(radius<=0)return;
    int64_t dy=((int64_t)y<<8)-cy,limit=(int64_t)radius*radius-dy*dy;
    int lo,hi;circle_span(limit,cx,&lo,&hi);code_span(lo,hi,code);
}
static void emit_row(int y){
    const uint8_t *in=SCENE_BITS(1)+y*ROW_BYTES,*out=SCENE_BITS(0)+y*ROW_BYTES;
    uint8_t *o=transition_bits+y*ROW_BYTES;
    for(int xb=0;xb<ROW_BYTES;xb++){
        unsigned incoming=0,keep=0,paper=0;
        for(int k=0;k<8;k++){
            unsigned c=row_code[xb*8+k];
            incoming=incoming<<1|(c==CODE_IN);keep=keep<<1|(c==CODE_OUT);paper=paper<<1|(c==CODE_PAPER);
        }
        o[xb]=(uint8_t)((in[xb]&incoming)|(out[xb]&keep)|paper);
    }
}
/* Eased progress through the window [start, start+length) of the transition. */
static int32_t fx_window(int32_t progress,int32_t start,int32_t length){
    if(progress<=start)return 0;
    if(progress>=start+length)return FX;
    return fx_ease((int32_t)(((int64_t)(progress-start)<<16)/length));
}
static int floor_div(int32_t a,int32_t b){return a>=0?a/b:-((-a+b-1)/b);}
/* Particles: dust specks swell into openings onto the page, each with an ink rim. */
static void compose_dust(int32_t progress){
    int32_t radius[BLOOM_MAX];
    for(int i=0;i<bloom_count;i++)
        radius[i]=(int32_t)(((int64_t)blooms[i].radius*fx_window(progress,blooms[i].delay,FX-blooms[i].delay))>>16);
    for(int y=0;y<UI_HEIGHT;y++){
        memset(row_code,CODE_OUT,UI_WIDTH);
        /* A rim shows only where no other bloom has opened. */
        for(int i=0;i<bloom_count;i++)if(radius[i]>0)code_disc(y,blooms[i].x,blooms[i].y,radius[i]+(2<<8),CODE_INK);
        for(int i=0;i<bloom_count;i++)code_disc(y,blooms[i].x,blooms[i].y,radius[i],CODE_IN);
        emit_row(y);
    }
}
/* Gases: the page's ribbons, continued across the frame and repeated at the ribbon
   gap, swell from the middle lane outward until they merge. */
static void compose_ribbons(int32_t progress){
    const int32_t gap=UI_GAS_RIBBON_GAP<<8,full=(UI_GAS_RIBBON_GAP/2+2)<<8,rim=2<<8;
    const int reach=UI_GAS_WAVE_AMPLITUDE+4+UI_GAS_RIBBON_GAP;
    int lo=floor_div(-(reach<<8)-ribbon_top,gap),hi=floor_div(((UI_HEIGHT+reach)<<8)-ribbon_top,gap)+1;
    int32_t half[64];
    if(hi-lo>=64)hi=lo+63;
    for(int k=lo;k<=hi;k++){
        int32_t delay=FX*45/100*LV_MIN(LV_ABS(k-1),5)/5;
        half[k-lo]=(int32_t)(((int64_t)full*fx_window(progress,delay,FX*55/100))>>16);
    }
    for(int y=0;y<UI_HEIGHT;y++){
        for(int x=0;x<UI_WIDTH;x++){
            int32_t rel=((int32_t)y<<8)-ribbon_top-ribbon_wave[x];
            int k0=floor_div(rel+gap/2,gap);uint8_t code=CODE_OUT;
            for(int k=LV_MAX(k0-1,lo);k<=LV_MIN(k0+1,hi);k++){
                int32_t h=half[k-lo],d=LV_ABS(rel-k*gap);
                if(h<=0)continue;
                if(d<h){code=CODE_IN;break;}
                if(d<h+rim)code=CODE_INK;
            }
            row_code[x]=code;
        }
        emit_row(y);
    }
}
/* Climate: a droplet falls onto the humidity icon and its splash opens the page. */
#define DROP_FALL (FX*35/100)
static void compose_droplet(int32_t progress){
    int height=UI_CLIMATE_DROP_HEIGHT-1,lift=0;int32_t reveal=0;
    if(progress<DROP_FALL){
        /* Falls from above the frame under gravity. */
        int32_t u=(int32_t)(((int64_t)progress<<16)/DROP_FALL);
        lift=(int)(((FX-(((int64_t)u*u)>>16))*(drop_top+height+4))>>16);
    }else reveal=(int32_t)(((int64_t)drop_reach*fx_window(progress,DROP_FALL,FX-DROP_FALL))>>16);
    int top=drop_top-lift;
    for(int y=0;y<UI_HEIGHT;y++){
        memset(row_code,CODE_OUT,UI_WIDTH);
        int yy=y-top;
        /* A paper outline keeps the falling droplet visible over ink. */
        if(yy>=-1 && yy<=height+1){
            int w=drop_half[LV_CLAMP(0,yy,height)];
            code_span(drop_x-w-1,drop_x+w+1,CODE_PAPER);
        }
        if(yy>=0 && yy<=height)code_span(drop_x-drop_half[yy],drop_x+drop_half[yy],CODE_INK);
        if(reveal>0){
            code_disc(y,drop_x<<8,drop_base<<8,reveal+(3<<8),CODE_INK);
            code_disc(y,drop_x<<8,drop_base<<8,reveal,CODE_IN);
        }
        emit_row(y);
    }
}
/* Formaldehyde: an oxygen atom's ink swells over the frame and opens like an iris. */
static void compose_atom(int32_t progress){
    int32_t ink=(int32_t)(((int64_t)atom_reach*fx_window(progress,0,FX*55/100))>>16);
    int32_t hole=(int32_t)(((int64_t)atom_reach*fx_window(progress,FX*25/100,FX*75/100))>>16);
    hole=LV_MIN(hole,ink);
    for(int y=0;y<UI_HEIGHT;y++){
        memset(row_code,CODE_OUT,UI_WIDTH);
        if(ink>0){
            /* A paper halo keeps the disc distinct from ink beneath it. */
            code_disc(y,atom_x,atom_y,ink+(2<<8),CODE_PAPER);
            code_disc(y,atom_x,atom_y,ink,CODE_INK);
            code_disc(y,atom_x,atom_y,hole,CODE_IN);
        }
        emit_row(y);
    }
}
static void compose_transition(int from,int to,int32_t progress){
    /* Exact endpoints avoid a one-frame jump when returning to direct rendering. */
    if(progress<=0){memcpy(transition_bits,SCENE_BITS(0),FRAME_BYTES);return;}
    if(progress>=FX){memcpy(transition_bits,SCENE_BITS(1),FRAME_BYTES);return;}
    switch(transition_effect(from)){
    case EFFECT_PORTAL:compose_portal(from,to,progress);break;
    case EFFECT_DUST:compose_dust(progress);break;
    case EFFECT_RIBBONS:compose_ribbons(progress);break;
    case EFFECT_DROPLET:compose_droplet(progress);break;
    default:compose_atom(progress);break;
    }
}
static void render_scene_buffer(int index,int which){
    lv_layer_t offscreen;lv_canvas_init_layer(scene_canvas[index],&offscreen);
    layer=&offscreen;offset=0;draw_scene(which);
    lv_canvas_finish_layer(scene_canvas[index],&offscreen);
}
static void paint_splash(void);
static float transition_duration(int from,int to);
static void splash_portal(void);
static void render_splash_buffer(void){
    lv_layer_t offscreen;lv_canvas_init_layer(scene_canvas[0],&offscreen);
    layer=&offscreen;offset=0;paint_splash();
    lv_canvas_finish_layer(scene_canvas[0],&offscreen);
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
        case 2:pm_particle_field();break;case 3:gas_ribbons();break;case 5:hcho_molecules();break;
        default:{climate_layout_t l=climate_layout();climate_icons(&l);}break;}
}
static void draw(lv_event_t *event){
    lv_layer_t *output=lv_event_get_layer(event);layer=output;offset=0;transition_drawn=false;
    if(!moving){
        anim_t=t;
        lv_area_t box=animated_area(scene);
        lv_area_t lower=pm_lower_area();
        if(area_within(&output->_clip_area,&box))draw_animated(scene);
        else if(scene==2 && area_within(&output->_clip_area,&lower))draw_pm_lower();
        else draw_scene(scene);
        return;
    }
    if(!snapshots_ready){
        double start=t-movement;
        anim_t=start;
        if(intro){render_splash_buffer();splash_portal();}else{render_scene_buffer(0,scene);prepare_effect(scene);}
        anim_t=start+duration;render_scene_buffer(1,destination);
        anim_t=t;snapshots_ready=true;
    }
    uint32_t compose_start=clock_us?clock_us():0;
    compose_transition(intro?0:scene,destination,(int32_t)(clamp(movement/duration,0,1)*FX));
    if(clock_us){compose_us_total+=clock_us()-compose_start;compose_count++;}
    layer=output;blit_transition(output);transition_drawn=true;
}
const uint8_t *atmos_transition_frame(void){return transition_drawn?transition_bits:NULL;}
/* Screen box containing everything that moves in a still scene. Anything that
   changes only with readings, history or selection triggers a full redraw instead.
   The simulator self-test checks every partial redraw against a full one. */
static lv_area_t animated_area(int which){
    switch(which){
    case 0: return (lv_area_t){UI_FLOWER_X-UI_FLOWER_RADIUS-2,UI_FLOWER_Y-UI_FLOWER_RADIUS-2,
                               UI_FLOWER_X+UI_FLOWER_RADIUS+2,UI_FLOWER_Y+UI_FLOWER_RADIUS+2};
    case 1: return (lv_area_t){UI_BUBBLE_LEFT-2,UI_BUBBLE_TOP-2,UI_BUBBLE_RIGHT+2,UI_BUBBLE_BOTTOM+2};
    case 2: return (lv_area_t){UI_PM_FIELD_LEFT,UI_PM_FIELD_TOP,UI_PM_FIELD_RIGHT,UI_PM_FIELD_BOTTOM};
    case 3: return (lv_area_t){UI_GAS_RIBBON_LEFT-4,LV_MAX(content_center()-UI_GAS_ART_EXTENT,UI_HEADER_BOTTOM+UI_DIVIDER),
                               UI_GAS_RIBBON_RIGHT+4,LV_MIN(content_center()+UI_GAS_ART_EXTENT,UI_HISTORY_PANEL_Y-1)};
    case 5: return (lv_area_t){hcho_field_left(),UI_HCHO_FIELD_TOP,UI_HCHO_FIELD_RIGHT-1,UI_HCHO_FIELD_BOTTOM-1};
    default:{climate_layout_t l=climate_layout();
        return (lv_area_t){l.icon_x-UI_CLIMATE_ICON_SIZE/2-2,l.row_center[0]-UI_CLIMATE_ICON_SIZE/2-2,
                           l.icon_x+UI_CLIMATE_ICON_SIZE/2+2,l.row_center[2]+UI_CLIMATE_ICON_SIZE/2+2};}
    }
}
static void invalidate_frame(void){
    /* The Particles selection blend (card fill and chart) stays below the field. */
    bool pm_lower=scene==2 && (pm_blend_age<UI_PM_TRANSITION_TIME || underline_age<UI_UNDERLINE_TIME);
    if(moving || full_dirty || (!pm_lower && (pm_blend_age<UI_PM_TRANSITION_TIME ||
       underline_age<UI_UNDERLINE_TIME)) || chart_blend_age<UI_CHART_BLEND_TIME || chart_rise_age<UI_CHART_BLEND_TIME){
        lv_obj_invalidate(surface);full_dirty=false;
    }else if(pm_lower){
        lv_area_t a=pm_lower_area();lv_obj_invalidate_area(surface,&a);
        if((int64_t)floor(t*UI_PM_FIELD_FPS)!=pm_last_tick){a=animated_area(2);lv_obj_invalidate_area(surface,&a);}
    }else if(scene!=2 || (int64_t)floor(t*UI_PM_FIELD_FPS)!=pm_last_tick){
        lv_area_t a=animated_area(scene);lv_obj_invalidate_area(surface,&a);
    }
    pm_last_tick=(int64_t)floor(t*UI_PM_FIELD_FPS);
}
/* Keep one sample older than the span, so the trace reaches the chart's left edge;
   drop older ones. */
static void prune_history(void){
    while(count>1 && t-history[1].at>=history_window){memmove(history,history+1,(--count)*sizeof *history);}
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
            mix(origin.pm10,target.pm10,p)+sin(t*.11f+2.4f)*.38f,
            mix(origin.pressure,target.pressure,p)+sin(t*.07f+3.1f)*.3f,
            fmaxf(0,mix(origin.hcho,target.hcho,p)+sin(t*.17f+5.6f)*.6f)};
        sensor.co2=fminf(sensor.co2,UI_SENSOR_CO2_MAX);sensor.voc=fminf(sensor.voc,UI_SENSOR_INDEX_MAX);
        sensor.nox=fminf(sensor.nox,UI_SENSOR_INDEX_MAX);sensor.temp=fminf(sensor.temp,UI_SENSOR_TEMP_MAX);
        sensor.rh=fminf(sensor.rh,UI_SENSOR_RH_MAX);sensor.pm=fminf(sensor.pm,UI_SENSOR_PM_MAX);
        sensor.pm1=fminf(sensor.pm1,UI_SENSOR_PM_MAX);sensor.pm4=fminf(sensor.pm4,UI_SENSOR_PM_MAX);
        sensor.pm10=fminf(sensor.pm10,UI_SENSOR_PM_MAX);sensor.pressure=fminf(sensor.pressure,UI_SENSOR_PRESSURE_MAX);
        sensor.hcho=fminf(sensor.hcho,UI_SENSOR_HCHO_MAX);
    }
    prune_history();
    /* After the span grows the buffer can fill before the oldest sample expires: drop
       the sample whose neighbours are closest, so the trace keeps its full extent. */
    if(count==HISTORY_CAP){
        int drop=1;
        for(int i=2;i<count-1;i++)if(history[i+1].at-history[i-1].at<history[drop+1].at-history[drop-1].at)drop=i;
        memmove(history+drop,history+drop+1,(--count-drop)*sizeof *history);
    }
    history[count++]=(sample){t,sensor};full_dirty=true;data_version++;
    if(count==1)chart_rise_age=0;
}
/* Boot splash. The ink of the name and the badge forms one group centred on the screen. */
static const char *splash_name="Atmos",*splash_subtitle="Visualizing Air Quality";
static lv_obj_t *splash;
static void ink_text(const char *text,int center,int ink_top,int size){
    centered_text(text,center,ink_top-header_ink_bounds(text,dashboard_font(size)).y1,size,false);
}
static int ink_height(const char *text,int size){
    lv_area_t a=header_ink_bounds(text,dashboard_font(size));return a.y2-a.y1+1;
}
/* Splash layout, shared by the splash screen and the intro transition's anchor. */
/* The name uses the largest font whose ink fits the content width (x 12-387). */
static int splash_name_size(void){
    static const int sizes[]={60,48,40,32,24,16};
    for(int i=0;i<5;i++)if(ink_width(splash_name,sizes[i])<=UI_WIDTH-24)return sizes[i];
    return 16;
}
/* Optional subtitle under the name: 16 px, or 12 px when that is too wide. */
static int splash_subtitle_size(void){return ink_width(splash_subtitle,16)<=UI_WIDTH-24?16:12;}
static int splash_subtitle_gap(void){return UI_TEXT_GAP;}
/* The block (name ink top to badge bottom) is centred on the screen by its ink,
   descenders included; the text sits close above the badge. */
static void splash_layout(int *name_top,int *bx,int *by){
    const int gap=18;
    int name_h=ink_height(splash_name,splash_name_size());
    if(splash_subtitle){
        const lv_font_t *f=dashboard_font(splash_subtitle_size());
        name_h+=splash_subtitle_gap()+header_ink_bounds(splash_subtitle,f).y2-header_ink_bounds("H",f).y1+1;
    }
    *name_top=(UI_HEIGHT-(name_h+gap+SPLASH_BADGE_H))/2;
    *bx=(UI_WIDTH-SPLASH_BADGE_W)/2;*by=*name_top+name_h+gap;
}
static void paint_splash(void){
    rect(0,0,UI_WIDTH,UI_HEIGHT,true,0);
    int top,bx,by;splash_layout(&top,&bx,&by);
    ink_text(splash_name,UI_WIDTH/2,top,splash_name_size());
    if(splash_subtitle){
        /* Placed on the capital height, so descenders do not move it. */
        int size=splash_subtitle_size(),caps=header_ink_bounds("H",dashboard_font(size)).y1;
        int y=top+ink_height(splash_name,splash_name_size())+splash_subtitle_gap();
        centered_text(splash_subtitle,UI_WIDTH/2,y-caps,size,false);
    }
    /* The badge bitmap, drawn as row spans of ink. */
    for(int y=0;y<SPLASH_BADGE_H;y++)for(int x=0;x<SPLASH_BADGE_W;){
        if(!(splash_badge[y][x>>3]&(0x80>>(x&7)))){x++;continue;}
        int run=x;while(run<SPLASH_BADGE_W && splash_badge[y][run>>3]&(0x80>>(run&7)))run++;
        rect(bx+x,by+y,run-x,1,false,0);x=run;
    }
}
static void draw_splash(lv_event_t *event){layer=lv_event_get_layer(event);offset=0;paint_splash();}
/* The intro portal opens through the badge's house logo: the first run of inked
   columns inside the outline wider than a few pixels (the outline's rounded
   corners leave narrow slivers near the edges). */
static void splash_portal(void){
    int top,bx,by;splash_layout(&top,&bx,&by);
    for(int x=4;x<SPLASH_BADGE_W-4;){
        int x2=x,y1=SPLASH_BADGE_H,y2=-1;
        for(;x2<SPLASH_BADGE_W-4;x2++){
            bool any=false;
            for(int y=4;y<SPLASH_BADGE_H-4;y++)if(splash_badge[y][x2>>3]&(0x80>>(x2&7))){any=true;y1=LV_MIN(y1,y);y2=LV_MAX(y2,y);}
            if(!any)break;
        }
        if(x2-x>8){
            portal_x=(bx*2+x+x2)<<7;portal_y=(by*2+y1+y2+1)<<7;portal_radius=((y2-y1+1)/2)<<8;
            return;
        }
        x=x2+1;
    }
    portal_x=200<<8;portal_y=150<<8;portal_radius=25<<8;
}
void atmos_set_splash_subtitle(const char *text){splash_subtitle=text && *text?text:NULL;}
void atmos_show_splash(const char *name){
    if(name && *name)splash_name=name;
    splash=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(splash);
    lv_obj_set_size(splash,UI_WIDTH,UI_HEIGHT);not_scrollable(splash);
    lv_obj_add_event_cb(splash,draw_splash,LV_EVENT_DRAW_MAIN,NULL);
}
void atmos_init(void){
    bool from_splash=splash!=NULL;
    if(splash){lv_obj_delete(splash);splash=NULL;}
    axis_label_canvas=lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(axis_label_canvas,axis_label_buf,AXIS_W,AXIS_H,LV_COLOR_FORMAT_I1);
    hide_obj(axis_label_canvas);
    sensor=origin=target=profiles[0];count=scenario=gas_selected=climate_selected=0;scene=destination=CYCLE_FIRST;queued=-1;pending_advance=0;
    pm_selected=1;pm_blend_age=UI_PM_TRANSITION_TIME;chart_blend_age=chart_rise_age=UI_CHART_BLEND_TIME;chart_from_key=chart_to_key=-1;underline_age=UI_UNDERLINE_TIME;underline_from=-1;underline_from_end=1;for(int i=0;i<4;i++)pm_from_weights[i]=pm_to_weights[i]=i==1?1:0;pm_density=6;
    gas_intensity[0]=gas_level(0);gas_intensity[1]=gas_level(1);
    for(int j=0;j<UI_HCHO_MOLECULES;j++)hcho_grow[j]=j<hcho_target()?1:0;
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
    /* Open from the splash through the overview's portal, zooming into the badge's logo. */
    intro=from_splash;
    if(intro){destination=scene;movement=0;duration=transition_duration(0,scene);moving=true;}
}
static void select_gas(int lane){
    if(lane==gas_selected)return;
    start_chart_blend(3,2+gas_selected,2+lane);start_underline(3,gas_selected);gas_selected=lane;full_dirty=true;data_version++;lv_obj_invalidate(surface);
}
static void select_climate(int row){
    if(row==climate_selected)return;
    start_chart_blend(4,4+climate_selected,4+row);start_underline(4,climate_selected);climate_selected=row;full_dirty=true;data_version++;lv_obj_invalidate(surface);
}
/* Without touch, automatic cycling steps through each page's readings in reading
   order (Gases VOC then NOx, Particles left to right, Climate top to bottom),
   splitting the hold evenly between them. */
static int auto_items(int s){return s==3?2:s==2?4:s==4?3:1;}
static int auto_selected(int s){return s==3?gas_selected:s==2?pm_selected:s==4?climate_selected:0;}
static void auto_select(int s,int item){
    if(s==3)select_gas(item);else if(s==2)atmos_select_pm(item);else if(s==4)select_climate(item);
}
int atmos_page(int position){return page_order[(position%ATMOS_SCENES+ATMOS_SCENES)%ATMOS_SCENES];}
static int cycle_next(int from,int direction){
    int p=0;while(p<ATMOS_SCENES && page_order[p]!=from)p++;
    int n=ATMOS_SCENES-CYCLE_FIRST,i=p<CYCLE_FIRST?(direction>0?-1:0):p-CYCLE_FIRST;
    return page_order[CYCLE_FIRST+((i+direction)%n+n)%n];
}
static float transition_duration(int from,int to){
    (void)to;
    static const float seconds[ATMOS_SCENES]={1.4f,1.4f,1.5f,1.3f,1.6f,1.4f};
    return seconds[from];
}
static void prerender_step(void){
    if(!automatic || pm_blend_age<UI_PM_TRANSITION_TIME || chart_blend_age<UI_CHART_BLEND_TIME ||
       chart_rise_age<UI_CHART_BLEND_TIME || underline_age<UI_UNDERLINE_TIME){pre_stage=0;return;}
    double start=t+(hold-age);
    int to=cycle_next(scene,1);
    if(pre_from!=scene || pre_to!=to || pre_version!=data_version || fabs(pre_start-start)>1e-4){
        pre_stage=0;pre_from=scene;pre_to=to;pre_version=data_version;pre_start=start;
    }
    /* Wait for the last sample before the transition so the snapshot shows its data. */
    if(pre_stage==2 || next_sample<=start+1e-4)return;
    if(pre_stage==0){anim_t=start;render_scene_buffer(0,scene);prepare_effect(scene);prerendered_last_step=scene;}
    else{anim_t=start+transition_duration(scene,to);render_scene_buffer(1,to);prerendered_last_step=to;}
    anim_t=t;pre_stage++;
}
void atmos_step(double dt){
    if(paused || hidden || dt<=0)return;
    /* Limit catch-up after suspension; normal operation advances at active time. */
    dt=fmin(dt,.1f);t+=dt;prerendered_last_step=-1;
    pm_blend_age=fmin(UI_PM_TRANSITION_TIME,pm_blend_age+dt);chart_blend_age=fminf(UI_CHART_BLEND_TIME,chart_blend_age+(float)dt);
    chart_rise_age=fminf(UI_CHART_BLEND_TIME,chart_rise_age+(float)dt);
    underline_age=fminf(UI_UNDERLINE_TIME,underline_age+(float)dt);
    float concentration=isfinite(sensor.pm10)?clamp(sensor.pm10,0,UI_PM_DENSITY_LIMIT):0;
    float density_target=UI_PM_DENSITY_MIN+(UI_PM_PARTICLE_MAX-UI_PM_DENSITY_MIN)*
        concentration/(concentration+UI_PM_DENSITY_HALF);
    pm_density+=(density_target-pm_density)*(1-exp(-dt/UI_PM_DENSITY_TIME));
    for(int lane=0;lane<2;lane++){
        gas_intensity[lane]+=(gas_level(lane)-gas_intensity[lane])*(1-exp(-dt/UI_GAS_INTENSITY_TIME));
    }
    hcho_grow_step(dt);
    if(t+1e-7f>=next_sample){take_sample();next_sample+=sample_every();}
    prune_history();
    if(moving){
        movement+=dt;
        if(movement>=duration){
            scene=destination;moving=intro=false;age=0;full_dirty=true;chart_blend_age=UI_CHART_BLEND_TIME;underline_age=UI_UNDERLINE_TIME;
            /* Reset the next page now, before its snapshot is pre-rendered, so
               automatic cycling arrives on its first reading. */
            if(automatic){int next=cycle_next(scene,1);if(next!=scene)auto_select(next,0);}
            if(queued>=0){int requested=queued;queued=-1;atmos_set_scene(requested);}
            /* Key presses made during the transition now step the arrived page. */
            else while(pending_advance && !moving){int d=pending_advance>0?1:-1;pending_advance-=d;atmos_advance(d);}}
    }else{age+=dt;
        if(automatic){
            int n=auto_items(scene),item=LV_MIN(n-1,(int)(age*n/hold));
            if(item>auto_selected(scene))auto_select(scene,item);
        }
        prerender_step();if(automatic && age>=hold)atmos_navigate(1);}
    invalidate_frame();
}
void atmos_set_scene(int requested){
    requested=(requested%ATMOS_SCENES+ATMOS_SCENES)%ATMOS_SCENES;
    if(moving){queued=requested;return;}
    if(requested==scene)return;
    /* The outgoing snapshot is still for the whole transition, so finish selection
       animations first: a half-filled card would otherwise look frozen. */
    if(underline_age<UI_UNDERLINE_TIME || pm_blend_age<UI_PM_TRANSITION_TIME || chart_blend_age<UI_CHART_BLEND_TIME){
        underline_age=UI_UNDERLINE_TIME;pm_blend_age=UI_PM_TRANSITION_TIME;chart_blend_age=UI_CHART_BLEND_TIME;
        data_version++;
    }
    duration=transition_duration(scene,requested);
    /* Use the hold-time snapshots when they match; the transition clock then starts
       at their time so the end still meets the live scene exactly. */
    bool ready=pre_stage==2 && pre_from==scene && pre_to==requested && pre_version==data_version && pre_start<=t+1e-4;
    double start=ready?pre_start:t;
    destination=requested;movement=t-start;moving=true;snapshots_ready=ready;pre_stage=0;
    if(ready)prerendered_transitions++;
}
/* Hardware keys (no touch): step the underlined reading within the page first, and
   move to the neighbouring page past its last (or before its first) reading. The
   page is entered on its first reading going forward, its last going back. Each
   press restarts the hold, so automatic cycling gives the reader a full hold. */
void atmos_advance(int direction){
    direction=direction<0?-1:1;
    /* A press during a transition waits for the arriving page rather than skipping it. */
    if(moving){pending_advance+=direction;return;}
    int item=auto_selected(scene)+direction;
    if(item>=0 && item<auto_items(scene)){auto_select(scene,item);age=0;return;}
    int to=cycle_next(scene,direction);
    if(to!=scene)auto_select(to,direction>0?0:auto_items(to)-1);
    age=0;atmos_set_scene(to);
}
void atmos_navigate(int direction){atmos_set_scene(cycle_next(queued>=0?queued:moving?destination:scene,direction));}
void atmos_set_scenario(int value){
    if(value<0 || value>=(int)(sizeof profiles/sizeof profiles[0]))return;
    scenario=value;origin=sensor;target=profiles[value];changed=t;external_data=false;
}
void atmos_set_readings(atmos_readings value){sensor=value;external_data=true;full_dirty=true;data_version++;}
void atmos_set_wifi(int bars){
    bars=bars<0?-1:bars>3?3:bars;
    if(bars==wifi_bars)return;
    wifi_bars=bars;full_dirty=true;data_version++;if(surface)lv_obj_invalidate(surface);
}
int atmos_wifi(void){return wifi_bars;}
int atmos_wifi_bars(float dbm){return isnan(dbm)?-1:dbm>=-60.0f?3:dbm>=-70.0f?2:dbm>=-80.0f?1:0;}
void atmos_toggle_pause(void){paused=!paused;}
void atmos_toggle_auto(void){automatic=!automatic;}
void atmos_set_auto(bool on){automatic=on;}
bool atmos_auto(void){return automatic;}
void atmos_set_hold(float seconds){if(!isnan(seconds))hold=clamp(seconds,ATMOS_HOLD_MIN,ATMOS_HOLD_MAX);}
float atmos_hold(void){return hold;}
void atmos_set_history_window(float seconds){
    if(isnan(seconds))return;
    history_window=clamp(seconds,ATMOS_HISTORY_MIN,ATMOS_HISTORY_MAX);
    prune_history();
    /* The next sample follows the newest at the new interval (now, if that has passed). */
    if(count)next_sample=fmax(t,history[count-1].at+sample_every());
    full_dirty=true;data_version++;if(surface)lv_obj_invalidate(surface);
}
float atmos_history_window(void){return history_window;}
/* Each reading's page and its position there (the underline/chart selection). */
static const int8_t reading_scene[ATMOS_READINGS]={1,3,3,5,2,2,2,2,4,4,4};
static const int8_t reading_item[ATMOS_READINGS]={0,0,1,0,0,1,2,3,0,1,2};
void atmos_select_reading(int r){
    if(r<0 || r>=ATMOS_READINGS)return;
    int s=reading_scene[r];
    auto_select(s,reading_item[r]);age=0;pending_advance=0;
    atmos_set_scene(s);
}
int atmos_reading(void){
    int s=queued>=0?queued:moving?destination:scene,item=auto_selected(s);
    for(int r=0;r<ATMOS_READINGS;r++)if(reading_scene[r]==s && reading_item[r]==item)return r;
    return -1;
}
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
    if(count<60 || count>62 || t-history[0].at<120 || t-history[1].at>=120)return 6;
    atmos_set_readings((atmos_readings){800,2,40,1,23,50,1.1f,3.4f,5.8f,1013.2f,12});
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
    for(int from=0;from<ATMOS_SCENES;from++)for(int to=0;to<ATMOS_SCENES;to++)if(from!=to){
        render_scene_buffer(0,from);prepare_effect(from);render_scene_buffer(1,to);
        compose_transition(from,to,0);
        if(memcmp(transition_bits,SCENE_BITS(0),FRAME_BYTES))return 8;
        compose_transition(from,to,FX);
        if(memcmp(transition_bits,SCENE_BITS(1),FRAME_BYTES))return 9;
        static uint8_t reference[FRAME_BYTES];
        const int32_t probes[]={1,FX/9,FX/4,FX/3,FX/2,FX*2/3,FX*3/4,FX*9/10,FX-1};
        for(unsigned k=0;k<sizeof probes/sizeof *probes && transition_effect(from)==EFFECT_PORTAL;k++){
            compose_portal_ref(from,to,probes[k]);
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
    {
        /* Home Assistant controls: reading selection, hold, history span, rating bands. */
        atmos_select_reading(ATMOS_HUMIDITY);
        while(moving)atmos_step(.025f);
        if(scene!=4 || climate_selected!=1 || atmos_reading()!=ATMOS_HUMIDITY)return 28;
        atmos_select_reading(ATMOS_NOX);
        if(atmos_reading()!=ATMOS_NOX)return 29;
        while(moving)atmos_step(.025f);
        if(scene!=3 || gas_selected!=1)return 29;
        atmos_set_hold(1e6f);if(atmos_hold()!=ATMOS_HOLD_MAX)return 30;atmos_set_hold(6.5f);
        /* Shrinking discards samples older than the new span. */
        atmos_set_history_window(60);
        if(count<2 || t-history[1].at>=60)return 31;
        /* Growing lets the chart fill to the new span. */
        atmos_set_history_window(600);int held=count;
        for(int i=0;i<4000;i++)atmos_step(.1f);
        if(count<=held || count>HISTORY_CAP || t-history[0].at<380)return 32;
        atmos_set_history_window(120);
        for(int i=0;i<1300;i++)atmos_step(.1f);
        if(count<60 || count>62)return 33;
        atmos_band custom[2]={{NAN,500,0,"Fine"},{500,NAN,3,"Terrible"}};
        atmos_band saved[ATMOS_BANDS_MAX];int saved_n=bands[ATMOS_CO2].n;memcpy(saved,bands[ATMOS_CO2].band,sizeof saved);
        if(!atmos_set_bands(ATMOS_CO2,custom,2) || atmos_set_bands(ATMOS_CO2,custom,ATMOS_BANDS_MAX+1))return 34;
        if(strcmp(rating_text(rate_co2()),sensor.co2<500?"Fine":"Terrible"))return 35;
        if(sensor.co2>=500 && (!warning() || strcmp(rating_text(rate_overall()),"Terrible")))return 35;
        atmos_set_bands(ATMOS_CO2,saved,saved_n);
    }
    {
        /* A long span still samples within seconds of a fresh start. */
        int kept=count;sample saved[HISTORY_CAP];memcpy(saved,history,sizeof saved);
        double next=next_sample;count=0;next_sample=t+2;
        atmos_set_history_window(3600);
        for(int i=0;i<200;i++)atmos_step(.1f);
        if(count<5)return 36;
        atmos_set_history_window(120);memcpy(history,saved,sizeof saved);count=kept;next_sample=next;
    }
    puts("Controls: reading selection, hold bound, history span shrink/grow, fast fill of long spans and rating bands passed.");
    puts("Prerender: hold-time transition snapshots match fresh renders and are used.");
    puts("PM: independent history channels, rapid selection, fixed transition window and missing-sample gaps passed.");
    puts("Model: sampling, history, pruning, navigation, pause, visibility and external data passed.");
    puts("Transition endpoints: all 30 directed scene pairs match their live LVGL frames exactly.");
    puts("Compositor: the fast portal matches the reference compositor at 9 progress points for every pair leaving a portal page.");
    return 0;
}

