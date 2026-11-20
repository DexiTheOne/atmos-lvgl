"""ESPHome external component running the platform-independent Atmos LVGL UI.

The component owns LVGL itself (ESPHome's lvgl: component is not used): it adds the
LVGL library, uses the project's shared desktop/lv_conf.h, renders into a native
1-bit frame and hands it to the ST7305 driver. The UI sources live in desktop/ (the
simulator is the source of truth) and are copied into the build's src/atmos_ui/.
"""

from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components import binary_sensor, number, select, sensor, switch
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INITIAL_OPTION,
    CONF_INITIAL_VALUE,
    CONF_LEVEL,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_TEXT,
    ENTITY_CATEGORY_CONFIG,
    UNIT_MINUTE,
    UNIT_SECOND,
)
from esphome.core import CORE
from esphome.helpers import copy_file_if_changed

DEPENDENCIES = ["display"]
AUTO_LOAD = ["binary_sensor", "number", "select", "sensor", "switch"]

CONF_DISPLAY_ID = "display_id"
CONF_FRAME_PERIOD = "frame_period"
CONF_REPORT_INTERVAL = "report_interval"
CONF_FRAME_STATS = "frame_stats"
CONF_DISPLAY_TEST = "display_test"
CONF_KEY_INTERVAL = "key_interval"
CONF_SPLASH_NAME = "splash_name"
CONF_SPLASH_SUBTITLE = "splash_subtitle"
CONF_SPLASH_TIMEOUT = "splash_timeout"
CONF_READING_SELECT = "reading_select"
CONF_TEMPERATURE_UNIT = "temperature_unit"
CONF_PRESSURE_UNIT = "pressure_unit"
CONF_AUTO_ADVANCE = "auto_advance"
CONF_HOLD_TIME = "hold_time"
CONF_ALERT = "alert"
CONF_ALERT_FOCUS = "alert_focus"
CONF_ALERT_ACTIVE = "alert_active"
CONF_ALERT_SOUND = "alert_sound"
CONF_OVERVIEW_LOW_LIGHT = "overview_low_light"
CONF_ALERT_ENTER = "alert_enter"
CONF_ALERT_LEAVE = "alert_leave"
CONF_ALERT_SOUND_COOLDOWN = "alert_sound_cooldown"
CONF_ON_ALERT_START = "on_alert_start"
CONF_ON_ALERT_CLEAR = "on_alert_clear"
CONF_HISTORY_TIME = "history_time"
CONF_RATINGS = "ratings"
CONF_SENSORS = "sensors"
CONF_SENSOR = "sensor"
CONF_STARTUP_DELAY = "startup_delay"
CONF_LED_COLOR = "led_color"
CONF_MARKER = "marker"
# Overview tile markers (atmos.h ATMOS_MARK_*): "level" picks check / ringed ! / triangle.
MARKERS = {"level": 0, "check": 1, "snowflake": 2, "cool": 3, "sun": 4, "flame": 5, "dry": 6, "drop": 7, "drops": 8}


def _rgb(value):
    """A status LED colour: "#RRGGBB" (or "RRGGBB")."""
    value = cv.string_strict(value).strip().lstrip("#")
    if len(value) != 6:
        raise cv.Invalid("led_color must be #RRGGBB")
    try:
        return int(value, 16)
    except ValueError as err:
        raise cv.Invalid("led_color must be #RRGGBB") from err

# Readings in page order, as atmos.h numbers them (ATMOS_CO2 ...): YAML key, select option.
READINGS = (
    ("co2", "CO2"),
    ("voc", "VOC"),
    ("nox", "NOx"),
    ("formaldehyde", "Formaldehyde"),
    ("pm1", "PM1.0"),
    ("pm2_5", "PM2.5"),
    ("pm4", "PM4.0"),
    ("pm10", "PM10"),
    ("temperature", "Temperature"),
    ("humidity", "Humidity"),
    ("pressure", "Pressure"),
)
# A reading's sensor: an id, or {sensor: id, startup_delay: true} for a sensor that
# reports nothing for a while after boot (SEN6x): its history waits until it is ready.
SENSOR_LINK = cv.Any(
    cv.Schema({
        cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_STARTUP_DELAY, default=False): cv.boolean,
    }),
    cv.use_id(sensor.Sensor),
)
# Bounds from atmos.h (ATMOS_HOLD_*, ATMOS_HISTORY_*, ATMOS_BANDS_MAX, ATMOS_BAND_TEXT).
HOLD_LIMITS = (2.0, 600.0)  # seconds
HISTORY_LIMITS = (1.0, 1440.0)  # minutes
BANDS_MAX = 8
BAND_TEXT_MAX = 15

# Keep in step with desktop/build.ps1 (the simulator's pinned LVGL).
LVGL_VERSION = "9.5.0"
# The repository root: components/atmos/__init__.py -> ../../desktop. ESPHome clones the
# whole repository for a git source, so this also works from github://.
UI_DIR = Path(__file__).resolve().parents[2] / "desktop"
UI_FILES = ("atmos.c", "atmos.h", "ui_fonts.c", "ui_fonts.h", "ui_theme.h", "splash_badge.h", "lv_conf.h")

atmos_ns = cg.esphome_ns.namespace("atmos")
AtmosComponent = atmos_ns.class_("AtmosComponent", cg.Component)
AdvanceAction = atmos_ns.class_("AdvanceAction", automation.Action)
AtmosReadingSelect = atmos_ns.class_("AtmosReadingSelect", select.Select)
AtmosUnitSelect = atmos_ns.class_("AtmosUnitSelect", select.Select, cg.Component)
AtmosAutoSwitch = atmos_ns.class_("AtmosAutoSwitch", switch.Switch, cg.Component)
AtmosAlertSwitch = atmos_ns.class_("AtmosAlertSwitch", switch.Switch, cg.Component)
AtmosNumber = atmos_ns.class_("AtmosNumber", number.Number, cg.Component)
ST7305RLCD = cg.esphome_ns.namespace("st7305_rlcd").class_("ST7305RLCD")



# Display units (atmos.h ATMOS_TEMP_* / ATMOS_PRESSURE_*, in order). Only the screen
# converts: linked sensors, ratings and what Home Assistant receives stay degC and hPa.
TEMPERATURE_UNITS = ["°C", "°F"]
PRESSURE_UNITS = ["hPa", "kPa", "mmHg", "inHg"]


def _unit_select_schema(options, icon):
    return select.select_schema(AtmosUnitSelect, entity_category=ENTITY_CATEGORY_CONFIG, icon=icon).extend(
        {cv.Optional(CONF_INITIAL_OPTION, default=options[0]): cv.one_of(*options)}
    ).extend(cv.COMPONENT_SCHEMA)


def _number_schema(unit, limits, default_max, default_initial):
    lo, hi = limits

    def validate(config):
        if not lo <= config[CONF_MIN_VALUE] < config[CONF_MAX_VALUE] <= hi:
            raise cv.Invalid(f"min_value/max_value must lie within {lo:g}..{hi:g} {unit}")
        if not config[CONF_MIN_VALUE] <= config[CONF_INITIAL_VALUE] <= config[CONF_MAX_VALUE]:
            raise cv.Invalid("initial_value must lie within min_value..max_value")
        return config

    return cv.All(
        number.number_schema(AtmosNumber, unit_of_measurement=unit, entity_category=ENTITY_CATEGORY_CONFIG)
        .extend(
            {
                cv.Optional(CONF_MIN_VALUE, default=lo): cv.float_,
                cv.Optional(CONF_MAX_VALUE, default=default_max): cv.float_,
                cv.Optional(CONF_INITIAL_VALUE, default=default_initial): cv.float_,
            }
        )
        .extend(cv.COMPONENT_SCHEMA),
        validate,
    )


def _band_keys(value):
    # Short keys min/max for min_value/max_value.
    if isinstance(value, dict):
        value = {{"min": CONF_MIN_VALUE, "max": CONF_MAX_VALUE}.get(k, k): v for k, v in value.items()}
    return value


def _validate_bands(bands):
    if len(bands) > BANDS_MAX:
        raise cv.Invalid(f"at most {BANDS_MAX} bands per reading")
    for i, band in enumerate(bands):
        band.setdefault(CONF_LEVEL, i)
        band.setdefault(CONF_ALERT, band[CONF_LEVEL] >= 2)
        if CONF_MIN_VALUE in band and CONF_MAX_VALUE in band and band[CONF_MIN_VALUE] >= band[CONF_MAX_VALUE]:
            raise cv.Invalid("min must be below max")
    return bands


# A band: the badge text for readings in [min, max) (either end may be left open).
# The first matching band wins. level orders severity for the overview, which shows the
# worst; level defaults to the band's position. Temperature, humidity and pressure are
# comfort readings: they never alert and stay out of the overview's rating and LED.
# alert (default: level 2 or more) raises
# the overview's warning and the Alert active sensor, and with alert focus on,
# automatic cycling shows only alerting readings.
BAND_SCHEMA = cv.All(
    _band_keys,
    cv.Schema(
        {
            cv.Optional(CONF_MIN_VALUE): cv.float_,
            cv.Optional(CONF_MAX_VALUE): cv.float_,
            cv.Required(CONF_TEXT): cv.All(cv.string_strict, cv.Length(min=1, max=BAND_TEXT_MAX)),
            cv.Optional(CONF_LEVEL): cv.int_range(min=0, max=100),
            cv.Optional(CONF_ALERT): cv.boolean,
            # Status LED colour (atmos_led) while this band is shown; default by level:
            # 0 green, 1 amber, 2+ red. "#000000" turns the LEDs off.
            cv.Optional(CONF_LED_COLOR): _rgb,
            # Overview tile marker: level (default), check, snowflake, cool, sun, flame,
            # dry, drop, drops.
            cv.Optional(CONF_MARKER, default="level"): cv.enum(MARKERS, lower=True),
        }
    ),
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AtmosComponent),
        cv.Required(CONF_DISPLAY_ID): cv.use_id(ST7305RLCD),
        # Frame period and frame-rate cap: 59 ms = about 17 fps, which nearly every frame meets,
        # leaving about a quarter of the CPU idle for Wi-Fi, the API and sensors.
        cv.Optional(CONF_FRAME_PERIOD, default="59ms"): cv.positive_time_period_microseconds,
        # Debug: log fps, frame times, CPU share, heap and the PROFILE lines (used by
        # bench.ps1 and the simulator's --hw-pace) every report_interval.
        cv.Optional(CONF_FRAME_STATS, default=False): cv.boolean,
        # Panel speed test: no UI; a counter and a moving box drawn straight into the
        # frame every frame_period, logged every report_interval.
        cv.Optional(CONF_DISPLAY_TEST, default=False): cv.boolean,
        # Minimum time between accepted atmos.next/atmos.previous presses.
        # Boot splash: this name over a "Made for ESPHome" badge, held until a Home
        # Assistant client connects (or the network, without the API), at most this long.
        cv.Optional(CONF_SPLASH_NAME, default="Atmos"): cv.All(cv.string, cv.Length(min=1, max=24)),
        # Line under the name; an empty string shows none.
        cv.Optional(CONF_SPLASH_SUBTITLE, default="Visualizing Air Quality"): cv.All(cv.string, cv.Length(max=40)),
        cv.Optional(CONF_SPLASH_TIMEOUT, default="20s"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(max=cv.TimePeriod(minutes=5))
        ),
        cv.Optional(CONF_KEY_INTERVAL, default="250ms"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(max=cv.TimePeriod(seconds=5))
        ),
        cv.Optional(CONF_REPORT_INTERVAL, default="10s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_READING_SELECT): select.select_schema(AtmosReadingSelect, icon="mdi:gauge"),
        # Units the screen shows temperature and pressure in (readings and charts).
        cv.Optional(CONF_TEMPERATURE_UNIT): _unit_select_schema(TEMPERATURE_UNITS, "mdi:thermometer"),
        cv.Optional(CONF_PRESSURE_UNIT): _unit_select_schema(PRESSURE_UNITS, "mdi:gauge"),
        cv.Optional(CONF_AUTO_ADVANCE): switch.switch_schema(
            AtmosAutoSwitch,
            default_restore_mode="RESTORE_DEFAULT_ON",
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:autorenew",
        ).extend(cv.COMPONENT_SCHEMA),
        # While any reading's band alerts, automatic cycling shows only alerting readings.
        cv.Optional(CONF_ALERT_FOCUS): switch.switch_schema(
            AtmosAlertSwitch,
            default_restore_mode="RESTORE_DEFAULT_OFF",
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:alert-circle-outline",
        ).extend(cv.COMPONENT_SCHEMA),
        # Alert sounds: fire on_alert_start / on_alert_clear where automatic cycling
        # enters / leaves the alert-focus cycle. Needs alert focus on.
        cv.Optional(CONF_ALERT_SOUND): switch.switch_schema(
            AtmosAlertSwitch,
            default_restore_mode="RESTORE_DEFAULT_OFF",
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:bell-ring-outline",
        ).extend(cv.COMPONENT_SCHEMA),
        # Low light (atmos_led auto_dimming's ambient sensor) parks the display on the
        # overview: at the next hold end while cycling, at once otherwise; lit again,
        # it resumes after one hold. Alerts do not override it. Low light always mutes alert sounds.
        cv.Optional(CONF_OVERVIEW_LOW_LIGHT): switch.switch_schema(
            AtmosAlertSwitch,
            default_restore_mode="RESTORE_DEFAULT_OFF",
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:weather-night",
        ).extend(cv.COMPONENT_SCHEMA),
        # Alert hysteresis for every reading: it joins the alert set after alerting this
        # long, and leaves it after being clear this long. Badges show the raw rating.
        cv.Optional(CONF_ALERT_ENTER, default="60s"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(max=cv.TimePeriod(hours=24))
        ),
        cv.Optional(CONF_ALERT_LEAVE, default="3min"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(max=cv.TimePeriod(hours=24))
        ),
        # After an alert clears, a new alert changes the cycle without a sound for this long.
        cv.Optional(CONF_ALERT_SOUND_COOLDOWN, default="20min"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(max=cv.TimePeriod(hours=24))
        ),
        cv.Optional(CONF_ON_ALERT_START): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_ALERT_CLEAR): automation.validate_automation(single=True),
        # On while the debounced alert set (alert_enter/alert_leave) is non-empty.
        cv.Optional(CONF_ALERT_ACTIVE): binary_sensor.binary_sensor_schema(device_class="problem"),
        # Seconds each page is held before automatic cycling moves on.
        cv.Optional(CONF_HOLD_TIME): _number_schema(UNIT_SECOND, HOLD_LIMITS, 60.0, 6.5),
        # Minutes of history the charts show.
        cv.Optional(CONF_HISTORY_TIME): _number_schema(UNIT_MINUTE, HISTORY_LIMITS, 60.0, 2.0),
        # Any ESPHome sensor per reading. Once one is linked the demo data stops, and
        # unlinked readings show "--". Units: CO2 ppm, VOC/NOx index, formaldehyde ppb,
        # PM ug/m3, temperature degC, humidity %, pressure hPa (convert with filters).
        cv.Optional(CONF_SENSORS, default={}): cv.Schema(
            {cv.Optional(key): SENSOR_LINK for key, _ in READINGS}
        ),
        cv.Optional(CONF_RATINGS, default={}): cv.Schema(
            {cv.Optional(key): cv.All(cv.ensure_list(BAND_SCHEMA), _validate_bands) for key, _ in READINGS}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    for name in UI_FILES:
        copy_file_if_changed(UI_DIR / name, Path(CORE.relative_src_path("atmos_ui", name)))
    # LVGL as an ESP-IDF component, configured by the shared lv_conf.h (as ESPHome's
    # own lvgl component does with its generated file).
    add_idf_sdkconfig_option("CONFIG_LV_BUILD_EXAMPLES", False)
    add_idf_sdkconfig_option("CONFIG_LV_BUILD_DEMOS", False)
    add_idf_component(name="lvgl/lvgl", ref=LVGL_VERSION)
    lv_conf = Path(CORE.relative_src_path("atmos_ui", "lv_conf.h")).as_posix()
    cg.add_build_flag("-DLV_CONF_H=1")
    cg.add_build_flag(f'-DLV_CONF_PATH=\\"{lv_conf}\\"')
    cg.add_build_flag("-DLV_KCONFIG_IGNORE")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_display(await cg.get_variable(config[CONF_DISPLAY_ID])))
    cg.add(var.set_frame_period_us(config[CONF_FRAME_PERIOD].total_microseconds))
    cg.add(var.set_report_interval_ms(config[CONF_REPORT_INTERVAL].total_milliseconds))
    cg.add(var.set_frame_stats(config[CONF_FRAME_STATS]))
    cg.add(var.set_display_test(config[CONF_DISPLAY_TEST]))
    cg.add(var.set_key_interval_ms(config[CONF_KEY_INTERVAL].total_milliseconds))
    cg.add(var.set_splash_name(config[CONF_SPLASH_NAME]))
    cg.add(var.set_splash_subtitle(config[CONF_SPLASH_SUBTITLE]))
    cg.add(var.set_splash_timeout_ms(config[CONF_SPLASH_TIMEOUT].total_milliseconds))

    for index, (key, _) in enumerate(READINGS):
        if (link := config[CONF_SENSORS].get(key)) is not None:
            if not isinstance(link, dict):
                link = {CONF_SENSOR: link, CONF_STARTUP_DELAY: False}
            cg.add(var.set_sensor(index, await cg.get_variable(link[CONF_SENSOR])))
            if link[CONF_STARTUP_DELAY]:
                cg.add(var.set_startup_delay(index, True))
    nan = cg.RawExpression("NAN")
    for index, (key, _) in enumerate(READINGS):
        for position, band in enumerate(config[CONF_RATINGS].get(key, [])):
            if CONF_LED_COLOR in band:
                cg.add(var.set_band_color(index, position, band[CONF_LED_COLOR]))
            lo, hi = band.get(CONF_MIN_VALUE), band.get(CONF_MAX_VALUE)
            cg.add(var.add_band(index, nan if lo is None else lo, nan if hi is None else hi, band[CONF_LEVEL], band[CONF_TEXT], band[CONF_ALERT], band[CONF_MARKER]))
    if conf := config.get(CONF_READING_SELECT):
        sel = await select.new_select(conf, options=["Auto Advance", "Overview"] + [name for _, name in READINGS])
        await cg.register_parented(sel, var)
        cg.add(var.set_reading_select(sel))
    for key, options, pressure in ((CONF_TEMPERATURE_UNIT, TEMPERATURE_UNITS, False), (CONF_PRESSURE_UNIT, PRESSURE_UNITS, True)):
        if conf := config.get(key):
            sel = await select.new_select(conf, options=options)
            await cg.register_component(sel, conf)
            cg.add(sel.set_pressure(pressure))
            cg.add(sel.set_initial(options.index(conf[CONF_INITIAL_OPTION])))
    if conf := config.get(CONF_AUTO_ADVANCE):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
        await cg.register_parented(sw, var)
        cg.add(var.set_auto_switch(sw))
    if conf := config.get(CONF_ALERT_FOCUS):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
    if conf := config.get(CONF_ALERT_SOUND):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
        cg.add(sw.set_kind(1))
    if conf := config.get(CONF_OVERVIEW_LOW_LIGHT):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
        cg.add(sw.set_kind(2))
    cg.add(var.set_alert_times(
        config[CONF_ALERT_ENTER].total_milliseconds / 1000.0,
        config[CONF_ALERT_LEAVE].total_milliseconds / 1000.0,
        config[CONF_ALERT_SOUND_COOLDOWN].total_milliseconds / 1000.0,
    ))
    for key, getter in ((CONF_ON_ALERT_START, var.get_alert_start_trigger()), (CONF_ON_ALERT_CLEAR, var.get_alert_clear_trigger())):
        if conf := config.get(key):
            await automation.build_automation(getter, [], conf)
    if conf := config.get(CONF_ALERT_ACTIVE):
        cg.add(var.set_alert_sensor(await binary_sensor.new_binary_sensor(conf)))
    for key, history, scale, step in ((CONF_HOLD_TIME, False, 1.0, 0.5), (CONF_HISTORY_TIME, True, 60.0, 1.0)):
        if conf := config.get(key):
            num = await number.new_number(conf, min_value=conf[CONF_MIN_VALUE], max_value=conf[CONF_MAX_VALUE], step=step)
            await cg.register_component(num, conf)
            await cg.register_parented(num, var)
            cg.add(num.set_history(history))
            cg.add(num.set_scale(scale))
            cg.add(num.set_initial_value(conf[CONF_INITIAL_VALUE]))


# atmos.next / atmos.previous: the device's two keys. Steps the underlined reading
# within the page, then moves to the next/previous page.
ADVANCE_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(AtmosComponent)})


async def _advance(config, action_id, template_arg, direction):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_direction(direction))
    return var


@automation.register_action("atmos.next", AdvanceAction, ADVANCE_SCHEMA, synchronous=True)
async def next_to_code(config, action_id, template_arg, args):
    return await _advance(config, action_id, template_arg, 1)


@automation.register_action("atmos.previous", AdvanceAction, ADVANCE_SCHEMA, synchronous=True)
async def previous_to_code(config, action_id, template_arg, args):
    return await _advance(config, action_id, template_arg, -1)