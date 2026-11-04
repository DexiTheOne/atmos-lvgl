#ifndef ATMOS_UI_THEME_H
#define ATMOS_UI_THEME_H
/* Dashboard and CO2 typography use the shared DejaVu Sans Bold subset. */
#define UI_WIDTH 400
#define UI_HEIGHT 300
#define UI_MARGIN 12
#define UI_DIVIDER 1
#define UI_TEXT_GAP 12 /* Between stacked label, value and unit ink on detail pages. */
/* Logical 1-bit colours. The simulator maps them to display tones at runtime. */
#define UI_COLOR_INK 0x000000u
#define UI_COLOR_PAPER 0xffffffu
#define UI_FONT_TITLE 24
#define UI_FONT_LABEL 16
#define UI_FONT_VALUE 40
#define UI_FONT_VALUE_COMPACT 32
#define UI_FONT_UNIT 16
#define UI_FONT_SECONDARY 16
#define UI_FONT_AXIS 12
#define UI_DETAIL_LABEL_Y 84
#define UI_DETAIL_VALUE_Y 109
#define UI_DETAIL_UNIT_Y 152
#define UI_DETAIL_DIVIDER_TOP 78
#define UI_DETAIL_DIVIDER_BOTTOM 176
#define UI_COLUMN_WIDTH (UI_WIDTH/2)
#define UI_UNIT_GAP 5
/* Reusable badge and flower component parameters. */
#define UI_BADGE_PADDING 8
#define UI_BADGE_HEIGHT 30 /* Even, like the title and badge ink heights, so both centre exactly. */
#define UI_BADGE_WIDTH 128
#define UI_BADGE_X (UI_WIDTH-UI_MARGIN-UI_BADGE_WIDTH)
#define UI_WIFI_W 23 /* Wi-Fi icon, centred in its own black box at the badge's right end. */
#define UI_WIFI_H 16
#define UI_WIFI_X_SIZE 8 /* The disconnected X, in the icon's bottom-right corner. */
#define UI_WIFI_BOX_W 35 /* Odd like the icon, so it centres exactly. */
#define UI_WIFI_GAP 4 /* Paper between the rating box and the Wi-Fi box. */
#define UI_WIFI_BOX_X (UI_BADGE_X+UI_BADGE_WIDTH-UI_WIFI_BOX_W)
#define UI_WIFI_X (UI_WIFI_BOX_X+(UI_WIFI_BOX_W-UI_WIFI_W)/2)
#define UI_RATING_W (UI_WIFI_BOX_X-UI_WIFI_GAP-UI_BADGE_X) /* The rating's box, left of the gap. */
#define UI_FLOWER_RADIUS 69
#define UI_FLOWER_RING 6
#define UI_FLOWER_PETALS 6
#define UI_PETAL_CENTER 46
#define UI_PETAL_LENGTH 15
#define UI_PETAL_HALF_WIDTH 13
#define UI_SYMBOL_CLEARANCE 36
#define UI_FLOWER_SPEED .85f /* radians per active second */
/* Overview page anchors. */
#define UI_HEADER_TEXT_Y 6
#define UI_HEADER_BOTTOM 48
#define UI_FOOTER_TOP 205
#define UI_FOOTER_PADDING 11
#define UI_FOOTER_DIVIDER 2
#define UI_FLOWER_X 319 /* Ring ink ends at the right margin (x 387). */
#define UI_FLOWER_Y 127
#define UI_MAIN_VALUE_SIZE 64
#define UI_MAIN_GAP UI_TEXT_GAP
#define UI_FOOTER_GAP UI_TEXT_GAP
/* CO2 detail page and reusable time-series chart parameters. */
#define UI_CO2_TITLE_SIZE UI_FONT_TITLE
#define UI_CO2_VALUE_SIZE 80
#define UI_CO2_COMPACT_SIZE 32
#define UI_CO2_UNIT_SIZE 24
#define UI_CO2_TITLE_Y 6
#define UI_CO2_UNIT_GAP UI_TEXT_GAP
#define UI_CO2_DIVIDER_Y 162
#define UI_BUBBLE_LEFT UI_BADGE_X /* Bubbles span the rating badge (x 260-387). */
#define UI_BUBBLE_RIGHT 388 /* Bubble ink ends at the right margin (x 387). */
#define UI_BUBBLE_TOP 58
#define UI_BUBBLE_BOTTOM 195
#define UI_BUBBLE_COUNT 12
#define UI_BUBBLE_MIN 2 /* Bubbles shown at outdoor CO2. */
#define UI_CO2_BUBBLE_BASE 400.0f /* ppm with UI_BUBBLE_MIN bubbles */
#define UI_CO2_BUBBLE_FULL 3000.0f /* ppm with every bubble */
#define UI_BUBBLE_MAX_RADIUS 23
#define UI_BUBBLE_STROKE 3
#define UI_BUBBLE_SPEED 18.0f /* pixels per active second */
#define UI_BUBBLE_SWAY 3.0f
#define UI_HISTORY_PANEL_Y 205
/* Formaldehyde page: the CO2 layout, with floating H2CO molecules (central C ring,
   filled O, two small H rings) that fill in as the reading rises. */
#define UI_HCHO_UNIT_SIZE UI_CO2_UNIT_SIZE
#define UI_HCHO_FIELD_GAP 16 /* Between the value column and the molecule field. */
#define UI_HCHO_FIELD_RIGHT 388 /* Molecule ink ends at the right margin (x 387). */
#define UI_HCHO_FIELD_TOP UI_BUBBLE_TOP
#define UI_HCHO_FIELD_BOTTOM UI_BUBBLE_BOTTOM
#define UI_HCHO_MOLECULES 10
#define UI_HCHO_MIN 1 /* Molecules shown near zero. */
#define UI_HCHO_FULL UI_SENSOR_HCHO_MAX /* ppb with every molecule */
#define UI_HCHO_LOG_KNEE 10.0f /* ppb; the count rises fastest below it */
#define UI_HCHO_GROW_TIME .6f /* Seconds for a molecule to grow in or shrink away. */
#define UI_HCHO_WANDER 3.0f /* Drift amplitude around each home, pixels. */
#define UI_HCHO_CHART_MIN_RANGE 40.0f /* ppb */
#define UI_HISTORY_LEFT 62
#define UI_HISTORY_RIGHT 384
#define UI_HISTORY_TOP 214
#define UI_HISTORY_BOTTOM 264
#define UI_HISTORY_TICK_Y 272
#define UI_HISTORY_DUAL_HEIGHT 30
#define UI_HISTORY_PILL_PADDING 3 /* Cap ink to the pill outline, vertically. */
#define UI_HISTORY_PILL_PADDING_X 2 /* Extra horizontal room for the corners. */
#define UI_HISTORY_PILL_RADIUS 5
#define UI_HISTORY_DUAL_GAP 11
#define UI_CHART_LEFT UI_HISTORY_LEFT
#define UI_CHART_RIGHT UI_HISTORY_RIGHT
#define UI_CHART_TOP UI_HISTORY_TOP
#define UI_CHART_BOTTOM UI_HISTORY_BOTTOM
#define UI_CHART_FONT_SIZE 12
#define UI_CHART_Y_FONT_SIZE 12
#define UI_CHART_TICK_LENGTH 3
#define UI_CHART_LABEL_GAP 4 /* Between tick ends and label ink, and above the caption. */
#define UI_CHART_CHANNEL_X 368
#define UI_CHART_TICK_Y UI_HISTORY_TICK_Y
#define UI_CHART_X_INTERVALS 4
#define UI_CHART_Y_INTERVALS 2
/* Highest readings the sensors report (SCD4x CO2, SEN5x PM, VOC/NOx index,
   temperature, humidity); chart scales stop here. */
#define UI_SENSOR_CO2_MAX 40000.0f
#define UI_SENSOR_PM_MAX 1000.0f
#define UI_SENSOR_INDEX_MAX 500.0f
#define UI_SENSOR_TEMP_MAX 60.0f
#define UI_SENSOR_RH_MAX 100.0f
#define UI_SENSOR_PRESSURE_MAX 1100.0f
#define UI_SENSOR_HCHO_MAX 1000.0f /* SEN68 formaldehyde, ppb */
#define UI_CHART_PADDING .04f
#define UI_CHART_MIN_RANGE 100.0f
#define UI_CHART_MIN_RANGE_RATIO .05f /* Minimum scale range as a share of the peak. */
#define UI_CHART_THOUSANDS 9000.0f
#define UI_CHART_LINE_WIDTH 2
#define UI_COLOR_CHART_FILL UI_COLOR_PAPER
/* PM detail page and its components. */
#define UI_PM_HEADER_Y 10
#define UI_PM_VALUE_SIZE 72
#define UI_PM_CARD_HEIGHT 59
#define UI_PM_CARD_ROW_GAP 5
#define UI_PM_CARD_GAP 4
#define UI_PM_CARD_WIDTH ((UI_WIDTH-2*UI_MARGIN-3*UI_PM_CARD_GAP)/4)
#define UI_PM_CARD_PADDING 6
#define UI_PM_CARD_RADIUS 4 /* Corner radius, drawn from row spans by pill(). */
#define UI_PM_LABEL_SIZE 12
#define UI_PM_CARD_VALUE_SIZE 24
#define UI_PM_CARD_UNIT_SIZE 12
/* Seconds a history chart takes to ease between readings when the selection changes. */
#define UI_CHART_BLEND_TIME .8f
#define UI_PM_TRANSITION_TIME UI_CHART_BLEND_TIME
/* Particle field: dotted floor grid with drifting dust, redrawn UI_PM_FIELD_FPS times a second. */
#define UI_PM_FIELD_LEFT (UI_MARGIN+2*(UI_PM_CARD_WIDTH+UI_PM_CARD_GAP)) /* PM4 card left edge, x 202. */
#define UI_PM_FIELD_RIGHT 387 /* Right margin. */
#define UI_PM_FIELD_TOP 54 /* Spans the value+unit ink rows. */
#define UI_PM_FIELD_BOTTOM 134
#define UI_PM_FIELD_CX ((UI_PM_FIELD_LEFT+UI_PM_FIELD_RIGHT)/2)
#define UI_PM_FIELD_FPS 20
#define UI_PM_HORIZON_Y 81
#define UI_PM_CAMERA_HEIGHT 1.0f
#define UI_PM_GRID_NEAR 1.0f
#define UI_PM_GRID_FAR 3.6f
#define UI_PM_GRID_COLUMNS 7
#define UI_PM_GRID_DOT_STEP 4.0f
#define UI_PM_GRID_DOT_MAX 600
#define UI_PM_DEPTH_NEAR 1.15f
#define UI_PM_DEPTH_FAR 3.6f
#define UI_PM_HEIGHT_MIN .15f
#define UI_PM_DRIFT_X .3f
#define UI_PM_DRIFT_Y .15f
#define UI_PM_DRIFT_DEPTH .35f
/* Drift cycles per ten minutes (higher is faster); each axis adds a per-dot offset. */
#define UI_PM_DRIFT_SPEED 22
/* Near-plane dot radius per size class: PM1, PM2.5, PM4, PM10. */
#define UI_PM_SIZE_PM1 1.0f
#define UI_PM_SIZE_PM25 1.8f
#define UI_PM_SIZE_PM4 2.6f
#define UI_PM_SIZE_PM10 3.6f
#define UI_PM_PARTICLE_MAX 80
#define UI_PM_DENSITY_MIN 8
#define UI_PM_DENSITY_TIME .8f
#define UI_PM_DENSITY_LIMIT 200.0f
#define UI_PM_DENSITY_HALF 40.0f
#define UI_PM_PANEL_Y UI_HISTORY_PANEL_Y
#define UI_PM_CHART_LEFT UI_HISTORY_LEFT
#define UI_PM_CHART_RIGHT UI_HISTORY_RIGHT
#define UI_PM_CHART_TOP UI_HISTORY_TOP
#define UI_PM_CHART_BOTTOM UI_HISTORY_BOTTOM
#define UI_PM_CHART_TICK_Y UI_HISTORY_TICK_Y
#define UI_PM_CHART_INTERVALS 4
#define UI_PM_CHART_Y_INTERVALS 2
#define UI_PM_CHART_PADDING .05f
#define UI_PM_CHART_MIN_RANGE 5
#define UI_PM_CHART_LINE_WIDTH 2
#define UI_PM_CHART_FONT_SIZE 12
#define UI_PM_READOUT_SLIDE 4
/* Climate: native landscape layout, two-tone icon and chart settings. */
#define UI_CLIMATE_TITLE_Y 6
#define UI_CLIMATE_LABEL_Y UI_DETAIL_LABEL_Y
#define UI_CLIMATE_VALUE_Y UI_DETAIL_VALUE_Y
#define UI_CLIMATE_VALUE_SIZE 48 /* Largest whose four-digit pressure fits the inline row. */
#define UI_CLIMATE_UNIT_Y UI_DETAIL_UNIT_Y
#define UI_CLIMATE_UNIT_GAP 6 /* Between a value and its unit. */
#define UI_CLIMATE_ICON_SIZE 41 /* Odd, so icons centre exactly on the value ink. */
#define UI_CLIMATE_STROKE 2
#define UI_CLIMATE_STEM_HALF 5
#define UI_CLIMATE_BULB_RADIUS 10
#define UI_CLIMATE_MERCURY_HALF 2
#define UI_CLIMATE_DROP_RADIUS 11
#define UI_CLIMATE_DROP_HEIGHT 41
#define UI_CLIMATE_GAUGE_TICKS 7
#define UI_CLIMATE_GAUGE_START 3.927f /* 225 degrees: lower left. */
#define UI_CLIMATE_GAUGE_SWEEP -4.712f /* 270 degrees, clockwise. */
#define UI_CLIMATE_GAUGE_TICK 4
#define UI_CLIMATE_GAUGE_HUB 3
#define UI_CLIMATE_NEEDLE_BASE 2.0f
#define UI_CLIMATE_NEEDLE_TIP .8f
#define UI_CLIMATE_GAUGE_TREMOR .04f
#define UI_CLIMATE_GAUGE_TREMOR_SPEED 2.3f
#define UI_CLIMATE_PRESSURE_MIN 960.0f
#define UI_CLIMATE_PRESSURE_MAX 1060.0f
#define UI_CLIMATE_MIN_PRESSURE_RANGE 4.0f
#define UI_CLIMATE_TEMP_MIN 0.0f
#define UI_CLIMATE_TEMP_MAX UI_SENSOR_TEMP_MAX
#define UI_CLIMATE_WAVE_SPEED 1.4f
#define UI_CLIMATE_WAVE_AMPLITUDE 2.0f
#define UI_CLIMATE_WAVE_LENGTH .18f
#define UI_CLIMATE_PANEL_Y UI_HISTORY_PANEL_Y
#define UI_CLIMATE_CHART_LEFT UI_HISTORY_LEFT
#define UI_CLIMATE_CHART_RIGHT UI_HISTORY_RIGHT
#define UI_CLIMATE_CHART_TOP UI_HISTORY_TOP
#define UI_CLIMATE_CHART_HEIGHT UI_HISTORY_DUAL_HEIGHT
#define UI_CLIMATE_CHART_GAP (UI_HISTORY_DUAL_HEIGHT+UI_HISTORY_DUAL_GAP)
#define UI_CLIMATE_TICK_Y UI_HISTORY_TICK_Y
#define UI_CLIMATE_FONT_SIZE 12
#define UI_CLIMATE_X_INTERVALS 4
#define UI_CLIMATE_MIN_TEMP_RANGE 4.0f
#define UI_CLIMATE_MIN_RH_RANGE 10.0f
#define UI_CLIMATE_PADDING .1f
#define UI_CLIMATE_LABEL_SIZE UI_FONT_SECONDARY
#define UI_CLIMATE_UNIT_SIZE UI_FONT_UNIT
#define UI_CLIMATE_FALLBACK_SIZE 32
#define UI_CLIMATE_AXIS_GAP 6
#define UI_CLIMATE_AXIS_CENTER 7
#define UI_CLIMATE_TICK_LENGTH 3
/* Gases: compact numeric columns and independent flowing artwork. */
#define UI_GAS_HEADER_Y 6
#define UI_GAS_BADGE_TEXT_INSET 4
#define UI_GAS_LABEL_Y UI_DETAIL_LABEL_Y
#define UI_GAS_VALUE_Y UI_DETAIL_VALUE_Y
#define UI_GAS_VALUE_SIZE UI_FONT_VALUE
#define UI_GAS_FALLBACK_SIZE 32
#define UI_GAS_SMALL_SIZE 24
/* Text columns are as wide as a three-digit value's ink, and the group
   (columns, divider, ribbons) has the same inset from both screen edges. */
#define UI_GAS_SIDE_INSET UI_MARGIN
#define UI_GAS_COLUMN_WIDTH 80
#define UI_GAS_COLUMN_GAP 20
#define UI_GAS_COLUMN_STEP (UI_GAS_COLUMN_WIDTH+UI_GAS_COLUMN_GAP)

#define UI_GAS_DIVIDER_X (UI_GAS_SIDE_INSET+UI_GAS_COLUMN_WIDTH+UI_GAS_COLUMN_GAP/2)
#define UI_GAS_ART_DIVIDER_X (UI_GAS_RIBBON_LEFT-UI_GAS_COLUMN_GAP/2)
#define UI_GAS_DIVIDER_BOTTOM 176
#define UI_GAS_LABEL_SIZE UI_FONT_SECONDARY
#define UI_GAS_UNDERLINE 2
#define UI_GAS_UNDERLINE_GAP 4
/* Retract the old underline, then draw the new one (half each). */
#define UI_UNDERLINE_TIME UI_CHART_BLEND_TIME
#define UI_GAS_RIBBON_LABEL_X 230
#define UI_GAS_RIBBON_LABEL_Y 45
#define UI_GAS_RIBBON_LEFT (UI_GAS_SIDE_INSET+UI_GAS_COLUMN_STEP+UI_GAS_COLUMN_WIDTH+UI_GAS_COLUMN_GAP)
#define UI_GAS_RIBBON_RIGHT (UI_WIDTH-1-UI_GAS_SIDE_INSET)
#define UI_GAS_RIBBON_GAP 30
#define UI_GAS_WAVE_AMPLITUDE 20.0f
#define UI_GAS_WAVE_CYCLES 1.15f
#define UI_GAS_WAVE_SPEED .65f
#define UI_GAS_LANE_PHASE 1.1f
#define UI_GAS_HALF_WIDTH 3.0f
#define UI_GAS_WIDTH_GAIN 8.0f
#define UI_GAS_EDGE_TAPER 0.0f
#define UI_GAS_INTENSITY_TIME 1.5f
#define UI_GAS_ACCENTS 0
#define UI_GAS_ACCENT_SPEED 9.0f
#define UI_GAS_ACCENT_RADIUS 1
#define UI_GAS_PARTICLES 12
#define UI_GAS_PARTICLE_SPEED 5.0f
#define UI_GAS_PARTICLE_OFFSET 4.0f
#define UI_GAS_PARTICLE_SWAY 2.0f
#define UI_GAS_PANEL_Y UI_CLIMATE_PANEL_Y
#define UI_GAS_CHART_LEFT UI_HISTORY_LEFT
#define UI_GAS_CHART_RIGHT UI_CLIMATE_CHART_RIGHT
#define UI_GAS_CHART_TOP UI_CLIMATE_CHART_TOP
#define UI_GAS_CHART_HEIGHT UI_CLIMATE_CHART_HEIGHT
#define UI_GAS_CHART_GAP UI_CLIMATE_CHART_GAP
#define UI_GAS_CHART_FONT_SIZE UI_CLIMATE_FONT_SIZE
#define UI_GAS_CHART_LABEL_X UI_MARGIN
#define UI_GAS_CHART_LABEL_OFFSET 7
#define UI_GAS_CHART_MIN_VOC 50.0f
#define UI_GAS_CHART_MIN_NOX 10.0f
#define UI_GAS_CHART_PADDING .1f
#define UI_GAS_CHART_LINE_WIDTH 2
#define UI_GAS_CHART_AXIS_GAP 6
#define UI_GAS_CHART_TICK_LENGTH 3
#define UI_GAS_CHART_TICK_Y UI_CLIMATE_TICK_Y
#define UI_GAS_CHART_INTERVALS 4
/* Badge rating limits: PLACEHOLDERS, to be tuned. "Fair"/"Poor" begin at these
   values (upper-limit measurements); climate uses good/fair bands, else poor. */
#define UI_CO2_FAIR 800.0f   /* ppm */
#define UI_CO2_POOR 1200.0f
#define UI_HCHO_FAIR 30.0f   /* ppb */
#define UI_HCHO_POOR 80.0f   /* about the WHO 0.1 mg/m3 guideline */
#define UI_PM1_FAIR 10.0f    /* ug/m3 */
#define UI_PM1_POOR 25.0f
#define UI_PM25_FAIR 12.0f
#define UI_PM25_POOR 35.0f
#define UI_PM4_FAIR 25.0f
#define UI_PM4_POOR 50.0f
#define UI_PM10_FAIR 54.0f
#define UI_PM10_POOR 154.0f
#define UI_VOC_FAIR 150.0f   /* index */
#define UI_VOC_POOR 250.0f
#define UI_NOX_FAIR 20.0f    /* index */
#define UI_NOX_POOR 150.0f
#define UI_TEMP_GOOD_LOW 20.0f /* degC */
#define UI_TEMP_GOOD_HIGH 25.0f
#define UI_TEMP_FAIR_LOW 18.0f
#define UI_TEMP_FAIR_HIGH 27.0f
#define UI_RH_GOOD_LOW 40.0f /* % */
#define UI_RH_GOOD_HIGH 60.0f
#define UI_RH_FAIR_LOW 30.0f
#define UI_RH_FAIR_HIGH 70.0f
#endif

