#ifndef LV_CONF_H
#define LV_CONF_H
/* Shared by the desktop simulator and the ESPHome device build (the atmos component
   copies this file into the firmware). The application owns LVGL and creates a 1-bit
   I1 display; ESPHome's lvgl: component is not used. */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
#define LV_USE_CANVAS 1
#define LV_DRAW_SW_SUPPORT_I1 1
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DEF_REFR_PERIOD 25
#define LV_USE_OS LV_OS_NONE
/* No fixed LVGL pool. The simulator uses the C library heap. On the device the
   atmos component allocates from PSRAM (falling back to internal RAM): rendering a
   scene into a canvas queues thousands of draw tasks, which exhaust internal RAM. */
#ifdef ESP_PLATFORM
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM
#else
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#endif
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
/* The UI uses its own fonts (ui_fonts.c); LVGL requires a default. */
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#endif
