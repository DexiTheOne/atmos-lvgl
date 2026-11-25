/* Settings page (settings.c) and the few atmos.c drawing hooks it uses.
   Internal to the UI: not part of the atmos.h interface. */
#ifndef SETTINGS_INTERNAL_H
#define SETTINGS_INTERNAL_H
#include <stdbool.h>

/* settings.c, called by atmos.c */
bool settings_draw(void);            /* draws the frame while the sheet shows; false otherwise */
bool settings_step(double dt);       /* advances the slide; true while the page behind is frozen */
void settings_transition_done(void); /* a page transition ended: apply a queued press */
bool settings_move(int direction);   /* left/right key; true when the settings page took it */

/* atmos.c, for settings.c. Coordinates are screen pixels, shifted by ui_set_offset(). */
void ui_header(const char *title,const char *badge); /* page title, rule, rating box (badge) and Wi-Fi box */
void ui_text(const char *text,int x,int y,int size,bool paper); /* font size 12/16/24..., line-box origin */
void ui_text_ink(const char *text,int size,int box[4]);          /* ink box x1,y1,x2,y2 at origin 0,0 */
void ui_pill(int x,int y,int w,int h,int radius,bool ink);
void ui_fill(int x,int y,int w,int h,bool ink);
void ui_invert(int x,int y,int w,int h);
void ui_set_offset(int y);
void ui_draw_page_behind(void);      /* the current page, still */
bool ui_dark(void);                  /* dark or inverted theme */
bool ui_moving(void);                /* a page transition is running */
void ui_invalidate(void);
void ui_redraw_page(void);           /* the page needs a full redraw */
void ui_flush_dirty(void);           /* invalidate if readings, Wi-Fi or settings changed */
#endif
