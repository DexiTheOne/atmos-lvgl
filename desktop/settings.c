/* Settings page: a circular vertical list of the device's settings in groups (Display,
   Lighting, Alerts, Sensors, Wi-Fi), split by rules and ending in Exit. The header's
   rating box names the selected row's group. The left key moves up, the right key
   down, the middle key (GPIO1) opens the page, then changes the selected setting:
   toggles flip, choices step to the next option, sliders step by a quarter (wrapping),
   actions run, information rows do nothing, Exit closes. The sheet drops in from the
   top over the frozen page and slides back up to close. */
#include "atmos.h"
#include "settings_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 400
#define SCREEN_H 300
#define MARGIN 12         /* content ink x 12..387, as on every page */
#define SLIDE_TIME .45f   /* seconds, each way */
#define EDGE 2            /* ink edge under the moving sheet */
#define LIST_TOP 52       /* below the header rule (y 48) */
#define LIST_BOTTOM 296   /* rows lie wholly inside LIST_TOP..LIST_BOTTOM */
#define ROW_H 25
#define GROUP_GAP 9       /* between groups: a 1 px rule in the middle */
#define FONT 16
#define SWITCH_W 30
#define SWITCH_H 16
#define SLIDER_W 96
#define SLIDER_STEPS 4    /* quarters: 0, 25, 50, 75, 100 % */
#define TEXT_MAX 34        /* an SSID is at most 32 bytes */
#define BAR_W 5           /* scroll bar at the right edge of the content (ink to x 387) */
#define BAR_GAP 6         /* between the rows and the scroll bar */
#define ROW_RIGHT (SCREEN_W-MARGIN-BAR_W-BAR_GAP) /* rows end here (x 376) */

enum{KIND_TOGGLE,KIND_CHOICE,KIND_SLIDER,KIND_ACTION,KIND_INFO,KIND_EXIT};
enum{G_DISPLAY,G_LIGHTING,G_ALERTS,G_SENSORS,G_WIFI,G_EXIT};
static const char *const group_names[]={"Display","Lighting","Alerts","Sensors","Wi-Fi","Exit"};
/* Rows follow atmos.h's ATMOS_SETTING_* order, then Exit. Labels are the Home
   Assistant entity names. */
#define S_EXIT ATMOS_SETTING_COUNT
#define S_COUNT (ATMOS_SETTING_COUNT+1)
static const struct{const char *label;int kind,group;}items[S_COUNT]={
    [ATMOS_SETTING_AUTO]={"Auto Advance",KIND_TOGGLE,G_DISPLAY},
    [ATMOS_SETTING_TEMPERATURE_UNIT]={"Temperature Unit",KIND_CHOICE,G_DISPLAY},
    [ATMOS_SETTING_PRESSURE_UNIT]={"Pressure Unit",KIND_CHOICE,G_DISPLAY},
    [ATMOS_SETTING_THEME]={"Theme",KIND_CHOICE,G_DISPLAY},
    [ATMOS_SETTING_HISTORY]={"History Time",KIND_SLIDER,G_DISPLAY},
    [ATMOS_SETTING_OVERVIEW_DARK]={"Overview in Low Light",KIND_TOGGLE,G_DISPLAY},
    [ATMOS_SETTING_LEDS]={"Status LEDs",KIND_TOGGLE,G_LIGHTING},
    [ATMOS_SETTING_LED_BRIGHTNESS]={"LED Brightness",KIND_SLIDER,G_LIGHTING},
    [ATMOS_SETTING_LED_DIMMING]={"Status LED Auto Dimming",KIND_TOGGLE,G_LIGHTING},
    [ATMOS_SETTING_ALERT_SOUND]={"Alert Sound",KIND_TOGGLE,G_ALERTS},
    [ATMOS_SETTING_VOLUME]={"Alert Volume",KIND_SLIDER,G_ALERTS},
    [ATMOS_SETTING_ALERT_FOCUS]={"Alert Focus",KIND_TOGGLE,G_ALERTS},
    [ATMOS_SETTING_CO2_ASC]={"CO2 Automatic Calibration",KIND_TOGGLE,G_SENSORS},
    [ATMOS_SETTING_CO2_CALIBRATE]={"Calibrate CO2",KIND_ACTION,G_SENSORS},
    [ATMOS_SETTING_WIFI_STATUS]={"Status",KIND_INFO,G_WIFI},
    [ATMOS_SETTING_WIFI_SSID]={"SSID",KIND_INFO,G_WIFI},
    [ATMOS_SETTING_WIFI_SIGNAL]={"RSSI",KIND_INFO,G_WIFI},
    [ATMOS_SETTING_IP_ADDRESS]={"IP Address",KIND_INFO,G_WIFI},
    [S_EXIT]={"Exit",KIND_EXIT,G_EXIT}};
static const char *const temp_units[]={"\xC2\xB0" "C","\xC2\xB0" "F"};
static const char *const pressure_units[]={"hPa","kPa","mmHg","inHg"};
static const char *const themes[]={"Light","Dark","Inverted"};
/* History slider stops, a quarter of the slider apart: the 2 minute default, then hours. */
static const float history_stops[SLIDER_STEPS+1]={120,3600,7200,10800,14400};
static const char *const history_text[SLIDER_STEPS+1]={"2 min","1 h","2 h","3 h","4 h"};

static bool open_,queued;     /* queued: a press waiting for a page transition to end */
static float pos;             /* 0 hidden, 1 fully down */
static int selected,scroll;   /* selected row (index into shown[]), list scroll in px */
/* Device-owned settings (percentages, toggles 0/1) and texts (Wi-Fi, calibration
   status): the device keeps them in step with its entities. Rows without one are hidden. */
static int device_value[ATMOS_SETTING_COUNT]={[ATMOS_SETTING_VOLUME]=50,[ATMOS_SETTING_LEDS]=1,
    [ATMOS_SETTING_LED_BRIGHTNESS]=100,[ATMOS_SETTING_LED_DIMMING]=1,[ATMOS_SETTING_CO2_ASC]=1};
static char device_text[ATMOS_SETTING_COUNT][TEXT_MAX];
static bool hidden_[ATMOS_SETTING_COUNT];
static uint32_t changed;      /* settings changed on screen, for atmos_poll_setting() */
static int shown[S_COUNT],shown_n,shown_y[S_COUNT],list_h;
_Static_assert(ATMOS_SETTING_COUNT<=32,"changed is a 32-bit mask");

static void list_items(void){
    shown_n=0;int y=0;
    for(int i=0;i<S_COUNT;i++){
        if(i<ATMOS_SETTING_COUNT && hidden_[i])continue;
        if(shown_n && items[i].group!=items[shown[shown_n-1]].group)y+=GROUP_GAP;
        shown_y[shown_n]=y;shown[shown_n++]=i;y+=ROW_H;
    }
    list_h=y;
}
/* Scroll so the selected row lies wholly inside the list, with its group's rule. */
static void keep_visible(void){
    int view=LIST_BOTTOM-LIST_TOP,y=shown_y[selected];
    if(selected==0)scroll=0;
    else if(y-GROUP_GAP<scroll)scroll=y-GROUP_GAP;
    if(y+ROW_H>scroll+view)scroll=y+ROW_H-view;
    if(scroll>list_h-view)scroll=list_h-view;
    if(scroll<0)scroll=0;
}
static int quarter(int percent){
    int q=100/SLIDER_STEPS,v=(percent+q/2)/q;
    return v<0?0:v>SLIDER_STEPS?SLIDER_STEPS:v;
}
static int history_step(void){
    float w=atmos_history_window();int best=0;
    for(int i=1;i<=SLIDER_STEPS;i++)if(fabsf(history_stops[i]-w)<fabsf(history_stops[best]-w))best=i;
    return best;
}
static bool toggle_value(int item){
    switch(item){
    case ATMOS_SETTING_AUTO:return atmos_auto();
    case ATMOS_SETTING_ALERT_FOCUS:return atmos_alert_focus();
    case ATMOS_SETTING_ALERT_SOUND:return atmos_alert_sound();
    case ATMOS_SETTING_OVERVIEW_DARK:return atmos_overview_dark();
    default:return device_value[item]!=0;
    }
}
static void activate(int item){
    switch(items[item].kind){
    case KIND_EXIT:atmos_set_settings(false);return;
    case KIND_INFO:return;
    default:break;
    }
    switch(item){
    case ATMOS_SETTING_AUTO:atmos_set_auto(!atmos_auto());break;
    case ATMOS_SETTING_ALERT_FOCUS:atmos_set_alert_focus(!atmos_alert_focus());break;
    case ATMOS_SETTING_ALERT_SOUND:atmos_set_alert_sound(!atmos_alert_sound());break;
    case ATMOS_SETTING_OVERVIEW_DARK:atmos_set_overview_dark(!atmos_overview_dark());break;
    case ATMOS_SETTING_HISTORY:atmos_set_history_window(history_stops[(history_step()+1)%(SLIDER_STEPS+1)]);break;
    case ATMOS_SETTING_TEMPERATURE_UNIT:atmos_set_temperature_unit((atmos_temperature_unit()+1)%2);break;
    case ATMOS_SETTING_PRESSURE_UNIT:atmos_set_pressure_unit((atmos_pressure_unit()+1)%4);break;
    case ATMOS_SETTING_THEME:atmos_set_theme((atmos_theme()+1)%ATMOS_THEME_COUNT);break;
    case ATMOS_SETTING_VOLUME:
        device_value[item]=(quarter(device_value[item])+1)%(SLIDER_STEPS+1)*(100/SLIDER_STEPS);break;
    case ATMOS_SETTING_LED_BRIGHTNESS: /* 25-100 %: off is the Status LEDs switch */
        device_value[item]=quarter(device_value[item])%SLIDER_STEPS*(100/SLIDER_STEPS)+100/SLIDER_STEPS;break;
    case ATMOS_SETTING_CO2_CALIBRATE:break; /* the device starts it */
    default:device_value[item]=!device_value[item];break;
    }
    changed|=1u<<item;
}

/* Text right-aligned (by ink) to x right, caps centred on cy. */
static void text_right(const char *text,int right,int cy,bool paper){
    int b[4],caps[4];ui_text_ink(text,FONT,b);ui_text_ink("H",FONT,caps);
    ui_text(text,right-b[2],cy-(caps[3]-caps[1]+1)/2-caps[1],FONT,paper);
}
static void text_left(const char *text,int left,int cy,bool paper){
    int b[4],caps[4];ui_text_ink(text,FONT,b);ui_text_ink("H",FONT,caps);
    ui_text(text,left-b[0],cy-(caps[3]-caps[1]+1)/2-caps[1],FONT,paper);
}
/* fg: the row's ink colour (paper on the selected row). */
static void toggle_switch(int right,int cy,bool on,bool fg){
    int x=right-SWITCH_W+1,y=cy-SWITCH_H/2,r=SWITCH_H/2,k=SWITCH_H-6;
    ui_pill(x,y,SWITCH_W,SWITCH_H,r,fg);
    if(!on)ui_pill(x+1,y+1,SWITCH_W-2,SWITCH_H-2,r-1,!fg);
    int kx=on?x+SWITCH_W-3-k:x+3;
    ui_pill(kx,y+3,k,k,k/2,on?!fg:fg);
}
static void slider(int right,int cy,int step,bool fg){
    int x=right-SLIDER_W+1,y=cy-3,fill=step*(SLIDER_W-2)/SLIDER_STEPS;
    ui_pill(x,y,SLIDER_W,7,3,fg);
    ui_pill(x+1,y+1,SLIDER_W-2,5,2,!fg);
    if(fill>0)ui_fill(x+1,y+1,fill,5,fg);
    for(int i=1;i<SLIDER_STEPS;i++)ui_fill(x+1+i*(SLIDER_W-2)/SLIDER_STEPS,y+7,1,2,fg);
    int kx=x+1+fill-4;if(kx<x)kx=x;if(kx>right-7)kx=right-7;
    ui_pill(kx,cy-6,8,13,4,fg);
    ui_pill(kx+2,cy-4,4,9,2,!fg);
}
/* Information texts: the device's, else what the simulator knows. */
static const char *info_text(int item){
    if(device_text[item][0])return device_text[item];
    int bars=atmos_wifi();
    switch(item){
    /* The device shows the measured RSSI; the simulator one typical of the bar count. */
    case ATMOS_SETTING_WIFI_SIGNAL:return bars<0?"--":bars>=3?"-55 dBm":bars==2?"-65 dBm":bars==1?"-75 dBm":"-85 dBm";
    case ATMOS_SETTING_WIFI_STATUS:return bars<0?"Disconnected":"Connected";
    case ATMOS_SETTING_WIFI_SSID:return bars<0?"--":"Simulator";
    case ATMOS_SETTING_CO2_CALIBRATE:return "Start";
    default:return "--";
    }
}
static void row(int index,int y){
    int item=shown[index];
    bool sel=index==selected,fg=!sel;
    int cy=y+ROW_H/2,right=ROW_RIGHT-6;
    if(sel)ui_pill(MARGIN,y+1,ROW_RIGHT-MARGIN+1,ROW_H-2,4,true); /* 4 px corners, like the other boxes */
    if(items[item].kind==KIND_EXIT){
        int b[4];ui_text_ink("Exit",FONT,b);
        text_left("Exit",(MARGIN+ROW_RIGHT+1-(b[2]-b[0]+1))/2,cy,sel);return;
    }
    text_left(items[item].label,MARGIN+8,cy,sel);
    char buf[16];
    switch(items[item].kind){
    case KIND_TOGGLE:toggle_switch(right,cy,toggle_value(item),fg);break;
    case KIND_CHOICE:
        text_right(item==ATMOS_SETTING_TEMPERATURE_UNIT?temp_units[atmos_temperature_unit()]:
                   item==ATMOS_SETTING_PRESSURE_UNIT?pressure_units[atmos_pressure_unit()]:themes[atmos_theme()],right,cy,sel);
        break;
    case KIND_SLIDER:
        if(item==ATMOS_SETTING_HISTORY){
            slider(right,cy,history_step(),fg);
            text_right(history_text[history_step()],right-SLIDER_W-8,cy,sel);
        }else{
            snprintf(buf,sizeof buf,"%d%%",device_value[item]);
            slider(right,cy,quarter(device_value[item]),fg);
            text_right(buf,right-SLIDER_W-8,cy,sel);
        }
        break;
    default:{
        /* Trim long texts (an SSID) with "..." to keep 12 px clear of the label. */
        int lb[4];ui_text_ink(items[item].label,FONT,lb);
        int room=right-(MARGIN+8+lb[2]-lb[0]+1)-12;
        char fit[TEXT_MAX+3];snprintf(fit,sizeof fit,"%s",info_text(item));
        for(int n=(int)strlen(fit);n>0;n--){
            int b[4];ui_text_ink(fit,FONT,b);
            if(b[2]-b[0]+1<=room)break;
            while(n>1 && (fit[n-1]&0xC0)==0x80)n--; /* whole UTF-8 characters */
            snprintf(fit+n-1,sizeof fit-(size_t)(n-1),"...");
        }
        text_right(fit,right,cy,sel);break;
    }
    }
}
/* Scroll bar: a 1 px track with a rounded thumb sized and placed by the visible share. */
static void scroll_bar(void){
    int view=LIST_BOTTOM-LIST_TOP,x=SCREEN_W-MARGIN-BAR_W;
    ui_fill(x+BAR_W/2,LIST_TOP,1,view,true);
    int h=list_h>view?view*view/list_h:view;if(h<BAR_W*3)h=BAR_W*3;
    int range=list_h-view,y=LIST_TOP+(range>0?(view-h)*scroll/range:0);
    ui_pill(x,y,BAR_W,h,BAR_W/2,true);
}
static void page(void){
    ui_fill(0,0,SCREEN_W,SCREEN_H,false);
    ui_header("SETTINGS",group_names[items[shown[selected]].group]);
    scroll_bar();
    for(int i=0;i<shown_n;i++){
        int y=LIST_TOP+shown_y[i]-scroll;
        if(y<LIST_TOP-GROUP_GAP || y+ROW_H>LIST_BOTTOM)continue;
        /* The rule above a group's first row, inset like the header rule. */
        if(i && items[shown[i]].group!=items[shown[i-1]].group && y-GROUP_GAP>=LIST_TOP)
            ui_fill(MARGIN,y-(GROUP_GAP+1)/2,ROW_RIGHT-MARGIN+1,1,true);
        if(y>=LIST_TOP)row(i,y);
    }
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
/* Left/right keys while the list is down (not while it slides): up/down, wrapping. */
bool settings_move(int direction){
    if(!open_)return pos>0;
    if(pos<1)return true;
    selected=(selected+direction+shown_n)%shown_n;
    keep_visible();
    ui_invalidate();
    return true;
}

/* A press during a page transition waits for it to end (the sheet slides over a still page). */
void atmos_set_settings(bool open){
    if(ui_moving()){queued=open!=open_;return;}
    queued=false;
    if(open==open_)return;
    open_=open;
    if(open){list_items();selected=scroll=0;}
    ui_invalidate();
}
void atmos_settings_key(void){
    if(ui_moving() || !open_){atmos_set_settings(ui_moving()?!(open_^queued):true);return;}
    if(pos<1)return;
    activate(shown[selected]);
    ui_redraw_page();ui_invalidate();
}
bool atmos_settings_open(void){return open_;}

int atmos_poll_setting(void){
    if(!changed)return -1;
    int i=0;while(!(changed>>i&1u))i++;
    changed&=~(1u<<i);return i;
}
void atmos_set_device_setting(int setting,int value){
    if(setting<0 || setting>=ATMOS_SETTING_COUNT || device_value[setting]==value)return;
    device_value[setting]=value;
    if(pos>0)ui_invalidate();
}
int atmos_device_setting(int setting){return setting>=0 && setting<ATMOS_SETTING_COUNT?device_value[setting]:0;}
void atmos_set_setting_text(int setting,const char *text){
    if(setting<0 || setting>=ATMOS_SETTING_COUNT)return;
    if(!text)text="";
    if(strncmp(device_text[setting],text,TEXT_MAX-1)==0)return;
    snprintf(device_text[setting],TEXT_MAX,"%s",text);
    if(pos>0)ui_invalidate();
}
void atmos_set_setting_shown(int setting,bool shown_row){
    if(setting>=0 && setting<ATMOS_SETTING_COUNT)hidden_[setting]=!shown_row;
}
