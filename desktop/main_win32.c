#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "atmos.h"
#include "ui_theme.h"
#include <math.h>
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define LVGL_VERSION_TEXT STRINGIFY(LVGL_VERSION_MAJOR) "." STRINGIFY(LVGL_VERSION_MINOR) "." STRINGIFY(LVGL_VERSION_PATCH)

/* As on the device, LVGL renders a 1-bit I1 frame: an 8-byte palette, then 50-byte
   MSB-first rows, bit 1 = paper. */
static uint8_t draw_buffer[8+400/8*300];
static uint8_t frame_bits[400/8*300];
static uint32_t pixels[400*300];
/* Simulator-only display tones for ink (bit 0) and paper (bit 1). Override with
   --ink=RRGGBB / --paper=RRGGBB or ATMOS_INK / ATMOS_PAPER. */
static uint32_t ink_rgb=0x303030, paper_rgb=0xc6c6c6;
static HWND window;
/* --hw-pace (or H): hold each frame on screen for as long as the same kind of frame
   takes on the device, and advance the model the way the device loop does. Measured
   on a Seeed XIAO ESP32-S3 (240 MHz) with esphome/atmos-bench.yaml on 2026-10-18:
   own LVGL, 1-bit display, direct fills, table compositor, ST7305 SPI at 40 MHz
   (40 MHz is not yet verified on a real panel; at 10 MHz add about 9 ms per frame).
   Milliseconds of work per frame: still frames per scene, transition frames per
   source scene, and hold-time pre-render steps per rasterised scene (from the
   device's PROFILE log line). hw_loop_ms is the device loop overhead between frames
   (frame interval minus frame work, averaged over the run). */
static bool hw_pace;
static double boot_ms=1500; /* simulated boot time the splash holds for (--boot=ms) */
/* Formaldehyde (last entry) is not yet measured: the CO2 page's costs stand in. */
static const double hw_still_ms[ATMOS_SCENES]={70.9,33.4,46.4,31.2,29.0,37.5};
static const double hw_trans_ms[ATMOS_SCENES]={26.0,27.7,36.5,63.6,19.4,21.9};
static const double hw_prerender_ms[ATMOS_SCENES]={71.8,38.3,57.0,55.4,33.5,41.9};
static const double hw_loop_ms=4.0;
static void update_title(void);
static unsigned flushes,transitions_checked;
static lv_display_t *display;
static BITMAPINFO bitmap;
/* Direct render mode, as on the device: LVGL redraws only invalidated areas in a
   persistent frame buffer, so partial redraws are exercised here. */
static void flush(lv_display_t *disp,const lv_area_t *area,uint8_t *map){
    const uint8_t *rows=map+8; /* skip the I1 palette; map is the whole frame */
    for(int y=area->y1;y<=area->y2;y++)for(int x=area->x1;x<=area->x2;x++){
        bool paper=rows[y*50+x/8]>>(7-(x&7))&1;
        uint8_t mask=(uint8_t)(0x80>>(x&7));
        if(paper)frame_bits[y*50+x/8]|=mask;else frame_bits[y*50+x/8]&=(uint8_t)~mask;
        pixels[y*400+x]=paper?paper_rgb:ink_rgb;
    }
    flushes++;
    if(window)InvalidateRect(window,NULL,FALSE);
    lv_display_flush_ready(disp);
}
static void save_bmp(const char *path){
    FILE *file=fopen(path,"wb");if(!file)return;
    BITMAPFILEHEADER header={0};header.bfType=0x4d42;
    header.bfOffBits=sizeof header+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+sizeof pixels;
    fwrite(&header,sizeof header,1,file);fwrite(&bitmap.bmiHeader,sizeof(BITMAPINFOHEADER),1,file);
    fwrite(pixels,sizeof pixels,1,file);fclose(file);
}
static bool present_frame(HDC dc,int width,int height){
    if(width<=0 || height<=0)return true;
    HDC back=CreateCompatibleDC(dc);
    HBITMAP image=CreateCompatibleBitmap(dc,width,height);
    if(!back || !image){if(back)DeleteDC(back);if(image)DeleteObject(image);return false;}
    HGDIOBJ previous=SelectObject(back,image);
    RECT client={0,0,width,height};
    FillRect(back,&client,(HBRUSH)GetStockObject(BLACK_BRUSH));
    int scale=width/400<height/300?width/400:height/300;
    if(scale<1)scale=1;
    int x=(width-400*scale)/2,y=(height-300*scale)/2;
    SetStretchBltMode(back,COLORONCOLOR);
    int copied=StretchDIBits(back,x,y,400*scale,300*scale,0,0,400,300,pixels,&bitmap,DIB_RGB_COLORS,SRCCOPY);
    /* Never clear the visible display. Publish the completed composition once. */
    bool ok=copied!=0 && copied!=(int)GDI_ERROR;
    if(ok)ok=BitBlt(dc,0,0,width,height,back,0,0,SRCCOPY)!=0;
    SelectObject(back,previous);DeleteObject(image);DeleteDC(back);
    return ok;
}
static int verify_presentation(void){
    const int sizes[][2]={{400,300},{800,600},{1000,700},{1200,900}};
    HDC screen=GetDC(NULL),target=CreateCompatibleDC(screen);
    if(!screen || !target){if(screen)ReleaseDC(NULL,screen);return 1;}
    int failed=0;
    for(unsigned size=0;size<sizeof sizes/sizeof *sizes;size++){
        int width=sizes[size][0],height=sizes[size][1];
        BITMAPINFO info=bitmap;info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        uint32_t *output=NULL;
        HBITMAP image=CreateDIBSection(screen,&info,DIB_RGB_COLORS,(void **)&output,NULL,0);
        if(!image){failed=1;break;}
        HGDIOBJ previous=SelectObject(target,image);
        if(!present_frame(target,width,height))failed=1;
        GdiFlush();
        int scale=width/400<height/300?width/400:height/300;
        int left=(width-400*scale)/2,top=(height-300*scale)/2;
        for(int y=0;y<height;y++)for(int x=0;x<width;x++){
            uint32_t expected=0;
            if(x>=left && x<left+400*scale && y>=top && y<top+300*scale)
                expected=pixels[((y-top)/scale)*400+(x-left)/scale];
            if((output[y*width+x]&0xffffff)!=expected)failed=1;
        }
        SelectObject(target,previous);DeleteObject(image);
    }
    DeleteDC(target);ReleaseDC(NULL,screen);return failed;
}
/* False while the boot splash shows: the UI model does not exist yet. */
static bool ui_started=true;
static double boot_end; /* when the simulated boot ends and the UI starts */
static double now_ms(void){LARGE_INTEGER f,c;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return c.QuadPart*1000.0/f.QuadPart;}
/* R: reboot the device. The UI is torn down and the boot splash shows again. */
static void reboot(void){
    lv_obj_clean(lv_screen_active());
    atmos_show_splash(NULL);ui_started=false;boot_end=now_ms()+boot_ms;
}
static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM key,LPARAM data){
    if(!ui_started && (message==WM_LBUTTONDOWN || message==WM_MOUSEWHEEL || message==WM_SIZE ||
        (message==WM_KEYDOWN && key!=VK_ESCAPE && key!='R')))return 0;
    switch(message){
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{
        PAINTSTRUCT paint;HDC dc=BeginPaint(hwnd,&paint);RECT client;GetClientRect(hwnd,&client);
        present_frame(dc,client.right,client.bottom);
        EndPaint(hwnd,&paint);return 0;
    }
    case WM_GETMINMAXINFO:{MINMAXINFO *info=(MINMAXINFO *)data;info->ptMinTrackSize.x=416;info->ptMinTrackSize.y=339;return 0;}
    case WM_SIZE:atmos_set_hidden(key==SIZE_MINIMIZED);return 0;
    case WM_LBUTTONDOWN:{
        RECT client;GetClientRect(hwnd,&client);
        int width=client.right,height=client.bottom;
        int scale=width/400<height/300?width/400:height/300;if(scale<1)scale=1;
        int x=((int)(short)LOWORD(data)-(width-400*scale)/2)/scale;
        int y=((int)(short)HIWORD(data)-(height-300*scale)/2)/scale;
        if(!atmos_select_pm_at(x,y))atmos_navigate(1);
        return 0;
    }
    case WM_MOUSEWHEEL:atmos_navigate(GET_WHEEL_DELTA_WPARAM(key)<0?1:-1);return 0;
    case WM_KEYDOWN:
        if(data & (1L<<30))return 0;
        if(key==VK_SPACE)atmos_toggle_pause();
        else if(key==VK_RIGHT || key==VK_DOWN)atmos_navigate(1);
        else if(key==VK_LEFT || key==VK_UP)atmos_navigate(-1);
        /* The device's two keys: reading by reading, then page by page. */
        else if(key==VK_OEM_PERIOD)atmos_advance(1);
        else if(key==VK_OEM_COMMA)atmos_advance(-1);
        else if(key>='1' && key<'1'+ATMOS_SCENES)atmos_set_scene(atmos_page((int)key-'1'));
        else if(key>=VK_F1 && key<=VK_F5)atmos_set_scenario((int)key-VK_F1);
        else if(key==VK_TAB || key==VK_OEM_6)atmos_cycle_pm(1);
        else if(key==VK_OEM_4)atmos_cycle_pm(-1);
        else if(key=='A')atmos_toggle_auto();
        else if(key=='W')atmos_set_wifi(atmos_wifi()<=-1?3:atmos_wifi()-1); /* 3, 2, 1, 0, off */
        else if(key=='H'){hw_pace=!hw_pace;update_title();}
        else if(key=='S')save_bmp("atmos-frame.bmp");
        else if(key=='R')reboot();
        else if(key==VK_ESCAPE)DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProc(hwnd,message,key,data);
}
static void advance(void){atmos_step(.025);lv_tick_inc(25);lv_timer_handler();}
/* A partial redraw must leave exactly the frame a full redraw of the same moment
   produces; otherwise something animates outside its scene's redraw area. */
static unsigned partial_checked;
static int verify_partial(void){
    static uint8_t partial[sizeof frame_bits];
    lv_refr_now(display); /* draw any invalidation the refresh timer has not reached yet */
    memcpy(partial,frame_bits,sizeof partial);
    lv_obj_invalidate(lv_screen_active());lv_refr_now(display);
    partial_checked++;
    int n=0,x1=999,y1=999,x2=-1,y2=-1;
    for(int i=0;i<400*300;i++){
        bool p=partial[i/8]>>(7-(i&7))&1,f=frame_bits[i/8]>>(7-(i&7))&1;
        if(p!=f){n++;int x=i%400,y=i/400;if(x<x1)x1=x;if(x>x2)x2=x;if(y<y1)y1=y;if(y>y2)y2=y;}
    }
    if(n){
        fprintf(stderr,"Partial redraw: %d pixels differ within %d,%d-%d,%d (full frame saved as partial-check-full.bmp)\n",
            n,x1,y1,x2,y2);
        save_bmp("partial-check-full.bmp");
    }
    return n!=0;
}
static int verify_frame(void){
    unsigned ink=0,paper=0;
    for(unsigned i=0;i<400*300;i++){
        bool bit=frame_bits[i/8]>>(7-(i&7))&1;
        if(pixels[i]!=(bit?paper_rgb:ink_rgb))return 1;
        if(bit)paper++;else ink++;
    }
    return ink<100 || paper<100;
}
static bool parse_rgb(const char *text,uint32_t *out){
    if(!text)return false;
    if(*text=='#')text++;
    char *end;unsigned long value=strtoul(text,&end,16);
    if(end-text!=6 || *end)return false;
    *out=(uint32_t)value;return true;
}
static void update_title(void){
    if(!window)return;
    char title[200];
    snprintf(title,sizeof title,"Atmos | LVGL " LVGL_VERSION_TEXT "%s | 1-6 scenes | F1-F5 scenarios | Space pause | A auto | W wifi | H hw pace | S save | R reboot",
        hw_pace?" | HW PACE (ESP32-S3, SPI 40 MHz)":"");
    SetWindowText(window,title);
}
int main(int argc,char **argv){
    parse_rgb(getenv("ATMOS_INK"),&ink_rgb);parse_rgb(getenv("ATMOS_PAPER"),&paper_rgb);
    /* Colour options may appear anywhere; the remaining argument selects the mode. */
    int kept=1,wifi=3; /* The simulator starts connected; W steps the strength down. */
    for(int i=1;i<argc;i++){
        if(strncmp(argv[i],"--ink=",6)==0){if(!parse_rgb(argv[i]+6,&ink_rgb)){fprintf(stderr,"Bad --ink\n");return 2;}}
        else if(strcmp(argv[i],"--hw-pace")==0)hw_pace=true;
        else if(strncmp(argv[i],"--boot=",7)==0)boot_ms=atof(argv[i]+7);
        else if(strncmp(argv[i],"--wifi=",7)==0)wifi=atoi(argv[i]+7);
        else if(strncmp(argv[i],"--paper=",8)==0){if(!parse_rgb(argv[i]+8,&paper_rgb)){fprintf(stderr,"Bad --paper\n");return 2;}}
        else argv[kept++]=argv[i];
    }
    argc=kept;
    bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth=400;bitmap.bmiHeader.biHeight=-300;
    bitmap.bmiHeader.biPlanes=1;bitmap.bmiHeader.biBitCount=32;bitmap.bmiHeader.biCompression=BI_RGB;
    lv_init();display=lv_display_create(400,300);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(display,draw_buffer,NULL,sizeof draw_buffer,LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display,flush);
    if(argc>1 && strcmp(argv[1],"--capture-splash")==0){
        if(argc>3)atmos_set_splash_subtitle(argv[3]);
        atmos_show_splash(argc>2?argv[2]:NULL);lv_refr_now(display);
        if(verify_frame())return 14;
        save_bmp("splash.bmp");puts("Splash saved as splash.bmp.");
        lv_deinit();return 0;
    }
    /* Interactive runs and the intro test start on the boot splash, as the device does. */
    bool intro_test=argc>1 && (strcmp(argv[1],"--test-intro")==0 || strcmp(argv[1],"--capture-intro")==0);
    atmos_set_wifi(wifi);
    if(argc<=1 || intro_test){atmos_show_splash(NULL);ui_started=false;}
    else atmos_init();
    if(intro_test){
        /* Splash, then the intro portal onto the first page: every presented frame must
           be the composed one, and the transition must land on the first page. */
        bool save=strcmp(argv[1],"--capture-intro")==0;
        lv_refr_now(display);
        if(verify_frame())return 40;
        if(save)save_bmp("intro-frame-00.bmp");
        atmos_init();ui_started=true;
        int checked=0;
        for(int i=1;i<=72;i++){
            advance();
            if(verify_frame())return 41;
            const uint8_t *composed=atmos_transition_frame();
            if(composed){checked++;if(memcmp(composed,frame_bits,sizeof frame_bits))return 42;}
            if(save && i%4==0){char path[80];snprintf(path,sizeof path,"intro-frame-%02d.bmp",i);save_bmp(path);}
        }
        if(checked<40 || atmos_is_moving() || atmos_scene()!=atmos_page(1))return 43;
        printf("Intro: splash, then %d portal frames matched the composed frame and landed on the first page.\n",checked);
        lv_deinit();return 0;
    }
    if(argc>1 && strcmp(argv[1],"--capture-stress")==0){
        atmos_toggle_auto();
        const atmos_readings cases[]={
            {NAN,NAN,NAN,NAN,NAN,NAN,NAN,NAN,NAN,NAN,NAN},
            {INFINITY,INFINITY,INFINITY,INFINITY,INFINITY,INFINITY,INFINITY,INFINITY,INFINITY,INFINITY,INFINITY},
            {9999,123.4,500,100,-10.5,100,82.7,151.9,206.8,1013.2f,999.9f}};
        for(int c=0;c<3;c++)for(int page=0;page<ATMOS_SCENES;page++){
            atmos_set_readings(cases[c]);atmos_set_scene(page);
            for(int i=0;i<80;i++){advance();if(verify_frame())return 28;}
            char path[80];snprintf(path,sizeof path,"stress-%d-%d.bmp",c,page);save_bmp(path);
        }
        puts("All scenes: missing, nonfinite and wide readings passed two-tone rendering.");
        lv_deinit();return 0;
    }
    if(argc>1 && strcmp(argv[1],"--capture-gases")==0){
        atmos_toggle_auto();atmos_set_readings((atmos_readings){617,3,128,4,24.2,58,2,4,5,1013.2f,12});atmos_set_scene(3);
        for(int i=0;i<60;i++)advance();
        save_bmp("gases-empty.bmp");
        for(int sample=0;sample<60;sample++){
            atmos_set_readings((atmos_readings){617,3,100+60*sin(sample*.12),10+6*sin(sample*.16),24.2,58,2,4,5,1013.2f,12});
            for(int i=0;i<80;i++)advance();
            if(sample==1)save_bmp("gases-short.bmp");
        }
        for(int i=0;i<40;i++){
            for(int j=0;j<8;j++)advance();
            if(verify_frame())return 25;
            char path[80];snprintf(path,sizeof path,"gases-motion-%02d.bmp",i);save_bmp(path);
        }
        atmos_set_readings((atmos_readings){1380,30,500,50,24,60,20,40,50,1013.2f,12});advance();save_bmp("gases-live.bmp");
        for(int i=0;i<120;i++)advance();
        save_bmp("gases-high.bmp");
        atmos_select_pm_at(130,UI_GAS_LABEL_Y+5);advance();save_bmp("gases-nox.bmp");
        atmos_select_pm_at(50,UI_DETAIL_LABEL_Y+5);advance();save_bmp("gases-voc.bmp");
        atmos_set_readings((atmos_readings){617,3,NAN,NAN,24,60,2,4,5,1013.2f,12});
        for(int i=0;i<80;i++)advance();
        save_bmp("gases-missing.bmp");
        for(int scenario=0;scenario<2;scenario++){
            atmos_set_readings((atmos_readings){scenario?1380:617,scenario?30:3,scenario?218:78,scenario?27:1,24,60,2,4,5,1013.2f,12});
            const int route[]={2,3,4,3,2,3};
            for(int leg=0;leg<6;leg++){
                atmos_set_scene(route[leg]);
                for(int i=0;i<64;i++){
                    advance();if(verify_frame())return 26;
                    if(i%8==0){char path[96];snprintf(path,sizeof path,"gases-travel-%d-%d-%02d.bmp",scenario,leg,i);save_bmp(path);}
                }
            }
        }
        puts("Gases: real histories, motion, live endpoint, missing readings and forward/reverse travel captured.");
        lv_deinit();return 0;
    }
    if(argc>1 && strcmp(argv[1],"--capture-climate")==0){
        atmos_toggle_auto();atmos_set_readings((atmos_readings){617,3,40,1,24.2,58,2,4,5,1013.2f,12});atmos_set_scene(4);
        for(int i=0;i<60;i++)advance();
        save_bmp("climate-empty.bmp");
        for(int sample=0;sample<60;sample++){
            atmos_set_readings((atmos_readings){617,3,40,1,24.2+2*sin(sample*.15),58+12*sin(sample*.19),2,4,5,1013.2f,12});
            for(int i=0;i<80;i++)advance();
        }
        for(int i=0;i<24;i++){
            for(int j=0;j<8;j++)advance();
            if(verify_frame())return 24;
            char path[80];snprintf(path,sizeof path,"climate-motion-%02d.bmp",i);save_bmp(path);
        }
        atmos_select_pm_at(50,127);advance();save_bmp("climate-humidity.bmp");
        atmos_select_pm_at(50,180);advance();save_bmp("climate-pressure.bmp");
        atmos_select_pm_at(50,UI_DETAIL_LABEL_Y+5);advance();save_bmp("climate-temperature.bmp");
        atmos_set_readings((atmos_readings){1380,30,100,4,-5,95,20,40,50,1013.2f,12});advance();save_bmp("climate-live.bmp");
        lv_deinit();return 0;
    }
    if(argc>1 && strcmp(argv[1],"--capture-pm")==0){
        atmos_toggle_auto();atmos_set_scene(2);
        for(int i=0;i<64;i++)advance();
        save_bmp("pm-empty.bmp");
        for(int sample=0;sample<60;sample++){
            double base=3.3+1.3*sin(sample*.19)+.6*sin(sample*.71);
            atmos_set_readings((atmos_readings){617,base,40,1,21,45,
                base*.6+.2*sin(sample*.31),base+1+.3*sin(sample*.27),base+2.6+.8*sin(sample*.13),1013.2f,12});
            for(int i=0;i<80;i++)advance();
        }
        atmos_set_readings((atmos_readings){617,3.3,40,1,21,45,2.3,3.9,4.7,1013.2f,12});
        for(int i=0;i<80;i++)advance();
        for(int selected=0;selected<4;selected++){
            atmos_select_pm(selected);
            for(int frame=0;frame<20;frame++){
                advance();if(verify_frame())return 19;
                if(frame%4==0){char path[80];snprintf(path,sizeof path,"pm-select-%d-frame-%02d.bmp",selected,frame);save_bmp(path);}
            }
            char path[80];snprintf(path,sizeof path,"pm-size-%d.bmp",selected);save_bmp(path);
        }
        atmos_select_pm(0);advance();atmos_select_pm(2);advance();atmos_select_pm(1);
        for(int i=0;i<20;i++)advance();
        save_bmp("pm-rapid.bmp");
        atmos_set_readings((atmos_readings){617,123.4,40,1,21,45,82.7,151.9,206.8,1013.2f,12});
        for(int i=0;i<80;i++)advance();
        save_bmp("pm-large.bmp");
        if(verify_frame())return 20;
        atmos_select_pm(2);
        atmos_set_readings((atmos_readings){617,3.3,40,1,21,45,2.3,NAN,4.7,1013.2f,12});
        for(int i=0;i<80;i++)advance();
        if(verify_frame())return 23;
        save_bmp("pm-missing.bmp");
        puts("PM: independent channels, all selections, graph blends, rapid reselection and overflow captured.");
        lv_deinit();return 0;
    }
    if(argc>1 && strcmp(argv[1],"--capture-co2")==0){
        atmos_toggle_auto();atmos_set_readings((atmos_readings){410,1,40,1,21,45,.6,1.5,2.2,1013.2f,12});atmos_set_scene(1);
        for(int i=0;i<60;i++)advance();
        save_bmp("co2-empty.bmp");
        for(int sample=0;sample<60;sample++){
            double value=410+sample*3.5+sin(sample*.3)*5;
            atmos_set_readings((atmos_readings){value,1,40,1,21,45,.6,1.5,2.2,1013.2f,12});
            for(int i=0;i<80;i++)advance();
            if(verify_frame())return 15;
            if(sample==1)save_bmp("co2-short.bmp");
        }
        save_bmp("co2-full.bmp");
        for(int frame=0;frame<8;frame++){
            for(int tick=0;tick<8;tick++)advance();
            char path[80];snprintf(path,sizeof path,"co2-motion-%02d.bmp",frame);save_bmp(path);
        }
        atmos_set_readings((atmos_readings){2500,1,40,1,21,45,.6,1.5,2.2,1013.2f,12});
        for(int i=0;i<80;i++)advance();
        save_bmp("co2-wide-range.bmp");
        if(verify_frame())return 16;
        puts("CO2: empty, short, full and wide-range histories captured in the approved palette.");
        lv_deinit();return 0;
    }
    if(argc>1 && strcmp(argv[1],"--capture-hcho")==0){
        /* Formaldehyde page from zero to the SEN68 maximum, with a short history each. */
        const float readings[]={0,8,24,40,95,250,999.9f,1000};
        atmos_toggle_auto();
        for(int c=0;c<(int)(sizeof readings/sizeof *readings);c++){
            atmos_set_readings((atmos_readings){617,3,78,1,22.4f,46,2.1f,4,5.3f,1013.2f,readings[c]});
            atmos_set_scene(5);
            for(int i=0;i<160;i++)advance();
            if(verify_frame())return 15;
            char path[80];snprintf(path,sizeof path,"hcho-%d.bmp",c);save_bmp(path);
        }
        puts("Formaldehyde: readings from zero to full scale captured.");
        lv_deinit();return 0;
    }
    if(argc>1 && strcmp(argv[1],"--capture-full-scale")==0){
        /* Readings at the sensor maximums, then just below the format steps. */
        const atmos_readings cases[]={{40000,1000,500,500,60,100,1000,1000,1000,1100,1000},
            {9999,999.9f,499,499,59.9f,99,999.9f,999.9f,999.9f,1013.2f,999.9f}};
        atmos_toggle_auto();
        for(int c=0;c<2;c++){
            atmos_set_readings(cases[c]);
            for(int i=0;i<62*80;i++)advance();
            for(int screen=0;screen<ATMOS_SCENES;screen++){
                atmos_set_scene(atmos_page(screen));
                for(int i=0;i<80;i++)advance();
                if(verify_frame())return 24;
                char path[80];snprintf(path,sizeof path,"full-scale-%d-scene-%d.bmp",c,screen);save_bmp(path);
            }
        }
        puts("Full scale: sensor maximums and near-maximum readings captured on every page.");
        lv_deinit();return 0;
    }
    if(argc>1 && strcmp(argv[1],"--capture-dashboard")==0){
        const atmos_readings cases[]={
            {435,1.2,40,1,20.7,42,.7,1.7,2.3,1013.2f,12},{950,12.4,40,1,23.1,45,7.8,15.1,20.9,1013.2f,12},
            {1200,38.7,100,4,25.5,58,20.1,48.2,64.9,1013.2f,12},{2500,125.8,100,4,-10.5,58,82.4,157.3,204.6,1013.2f,12}};
        atmos_toggle_auto();
        for(int c=0;c<4;c++){
            atmos_set_readings(cases[c]);
            for(int phase=0;phase<16;phase++){
                for(int tick=0;tick<20;tick++)advance();
                if(verify_frame())return 14;
                char path[80];snprintf(path,sizeof path,"dashboard-case-%d-phase-%02d.bmp",c,phase);
                save_bmp(path);
            }
        }
        puts("Dashboard: four reading combinations, sixteen phases each; exact two-tone frames passed.");
        lv_deinit();return 0;
    }
    bool capture=argc>1 && strcmp(argv[1],"--capture-transitions")==0;
    if(capture || (argc>1 && strcmp(argv[1],"--self-test")==0)){
        int result=atmos_self_test();if(result){fprintf(stderr,"Model test failed: %d\n",result);return result;}
        for(int scenario=0;scenario<4;scenario++){
            atmos_set_scenario(scenario);
            for(int i=0;i<160;i++)advance();
            for(int screen=0;screen<ATMOS_SCENES;screen++){
                atmos_set_scene(atmos_page(screen));
                for(int i=0;i<72;i++){
                    advance();
                    if(verify_frame()){fprintf(stderr,"Invalid palette or empty frame\n");return 10;}
                    /* The presented frame must be exactly the composed 1-bit transition. */
                    const uint8_t *composed=atmos_transition_frame();
                    if(composed){
                        transitions_checked++;
                        if(memcmp(composed,frame_bits,sizeof frame_bits)){fprintf(stderr,"Transition frame mismatch\n");return 29;}
                    }else if(verify_partial()){
                        fprintf(stderr,"Partial redraw differs from full redraw (scenario %d scene %d frame %d)\n",scenario,screen,i);
                        return 31;
                    }
                    if(capture && (scenario==0 || scenario==2) && i%8==0){
                        char path[100];snprintf(path,sizeof path,"transition-%d-to-%d-frame-%02d.bmp",scenario,screen,i);
                        save_bmp(path);
                    }
                }
                char path[80];snprintf(path,sizeof path,"scenario-%d-scene-%d.bmp",scenario,screen);save_bmp(path);
                if(verify_presentation()){fprintf(stderr,"Windows presentation test failed\n");return 12;}
            }
        }
        for(int screen=ATMOS_SCENES-1;screen>=0;screen--){
            atmos_set_scene(atmos_page(screen));
            for(int i=0;i<72;i++){advance();if(verify_frame())return 13;}
        }
        /* Automatic cycling through every transition twice, as on the device: exercises
           hold-time pre-rendering of every scene. */
        unsigned prerendered_before=atmos_prerendered_transitions();
        atmos_toggle_auto();
        for(int i=0;i<2*ATMOS_SCENES*(260+56);i++){
            advance();
            if(verify_frame())return 33;
            const uint8_t *composed=atmos_transition_frame();
            if(composed && memcmp(composed,frame_bits,sizeof frame_bits))return 34;
        }
        atmos_toggle_auto();
        if(atmos_prerendered_transitions()-prerendered_before<ATMOS_SCENES){
            fprintf(stderr,"Automatic cycle: only %u pre-rendered transitions\n",atmos_prerendered_transitions()-prerendered_before);return 35;}
        printf("Automatic cycle: %u transitions started from hold-time snapshots; %u clamped fills.\n",
            atmos_prerendered_transitions()-prerendered_before,atmos_clip_overruns());
        if(flushes<100)return 11;
        if(transitions_checked<100)return 30;
        if(partial_checked<100)return 32;
        printf("Partial redraw: %u still frames matched a full redraw exactly.\n",partial_checked);
        printf("Transitions: %u mid-transition frames matched the composed 1-bit frame exactly.\n",transitions_checked);
        printf("LVGL %d.%d.%d: %u flushes; all scenes/scenarios and transitions rendered in the approved palette.\n",LVGL_VERSION_MAJOR,LVGL_VERSION_MINOR,LVGL_VERSION_PATCH,flushes);
        puts("Windows GDI presentation: exact pixels at 1x/2x/3x and letterboxed sizes passed.");
        lv_deinit();return 0;
    }
    HINSTANCE instance=GetModuleHandle(NULL);
    SetProcessDPIAware();
    WNDCLASS wc={0};wc.lpfnWndProc=procedure;wc.hInstance=instance;
    wc.lpszClassName="AtmosLVGL";wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClass(&wc))return 20;
    RECT size={0,0,800,600};AdjustWindowRect(&size,WS_OVERLAPPEDWINDOW,FALSE);
    window=CreateWindow(wc.lpszClassName,"Atmos",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,size.right-size.left,size.bottom-size.top,NULL,NULL,instance,NULL);
    if(!window)return 21;
    update_title();
    /* The launcher hides the console via STARTUPINFO. Windows applies that
       setting to the first ShowWindow call; explicitly show the UI afterwards. */
    lv_refr_now(display);
    ShowWindow(window,SW_SHOW);ShowWindow(window,SW_SHOW);UpdateWindow(window);
    LARGE_INTEGER frequency,counter;QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    double previous=counter.QuadPart*1000.0/frequency.QuadPart,next_frame=previous,tick_carry=0;
    boot_end=previous+boot_ms;
    bool running=true;
    while(running){
        MSG message;
        while(PeekMessage(&message,NULL,0,0,PM_REMOVE)){
            if(message.message==WM_QUIT){running=false;break;}
            TranslateMessage(&message);DispatchMessage(&message);
        }
        if(!running)break;
        QueryPerformanceCounter(&counter);
        double now=counter.QuadPart*1000.0/frequency.QuadPart;
        if(hw_pace && now<next_frame){Sleep(1);continue;}
        double elapsed=now-previous;previous=now;
        tick_carry+=elapsed;uint32_t ticks=(uint32_t)tick_carry;tick_carry-=ticks;lv_tick_inc(ticks);
        /* Stand-in for the device's boot: the splash holds until boot_ms, then the UI starts. */
        if(!ui_started){
            if(now<boot_end){lv_timer_handler();Sleep(10);continue;}
            atmos_init();ui_started=true;continue;
        }
        atmos_step(elapsed/1000.0);
        if(!hw_pace){lv_timer_handler();Sleep(2);continue;}
        /* One device frame: step, render, then hold for the device's cost of that frame
           (never less than the device's 59 ms frame period, its 17 fps cap). */
        int pre=atmos_step_prerendered();
        lv_refr_now(display);
        double cost=atmos_is_moving()?hw_trans_ms[atmos_scene()]:hw_still_ms[atmos_scene()];
        if(pre>=0)cost+=hw_prerender_ms[pre];
        cost+=hw_loop_ms;
        next_frame=now+(cost>59?cost:59);
    }
    window=NULL;lv_deinit();return 0;
}

