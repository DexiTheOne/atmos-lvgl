/* Settings page (settings.c) and the few atmos.c drawing hooks it uses.
   Internal to the UI: not part of the atmos.h interface. */
#ifndef SETTINGS_INTERNAL_H
#define SETTINGS_INTERNAL_H
#include <stdbool.h>

/* settings.c, called by atmos.c */
bool settings_draw(void);            /* draws the frame while the sheet shows; false otherwise */
bool settings_step(double dt);       /* advances the slide; true while the page behind is frozen */
void settings_transition_done(void); /* a page transition ended: apply a queued press */

/* atmos.c, for settings.c. Coordinates are screen pixels, shifted by ui_set_offset(). */
void ui_wifi_header(const char *title); /* page title, rule and Wi-Fi box, no rating box */
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
