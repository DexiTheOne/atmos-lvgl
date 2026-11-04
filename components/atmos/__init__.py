"""ESPHome external component running the platform-independent Atmos LVGL UI.

The component owns LVGL itself (ESPHome's lvgl: component is not used): it adds the
LVGL library, uses the project's shared desktop/lv_conf.h, renders into a native
1-bit frame and hands it to the ST7305 driver. The UI sources live in desktop/ (the
simulator is the source of truth) and are copied into the build's src/atmos_ui/.
"""

from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components import number, select, sensor, switch
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
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
AUTO_LOAD = ["number", "select", "sensor", "switch"]

CONF_DISPLAY_ID = "display_id"
CONF_FRAME_PERIOD = "frame_period"
CONF_REPORT_INTERVAL = "report_interval"
CONF_FRAME_STATS = "frame_stats"
CONF_KEY_INTERVAL = "key_interval"
CONF_SPLASH_NAME = "splash_name"
CONF_SPLASH_SUBTITLE = "splash_subtitle"
CONF_SPLASH_TIMEOUT = "splash_timeout"
CONF_READING_SELECT = "reading_select"
CONF_AUTO_ADVANCE = "auto_advance"
CONF_HOLD_TIME = "hold_time"
CONF_HISTORY_TIME = "history_time"
CONF_RATINGS = "ratings"
CONF_SENSORS = "sensors"

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
AtmosAutoSwitch = atmos_ns.class_("AtmosAutoSwitch", switch.Switch, cg.Component)
AtmosNumber = atmos_ns.class_("AtmosNumber", number.Number, cg.Component)
ST7305RLCD = cg.esphome_ns.namespace("st7305_rlcd").class_("ST7305RLCD")



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
        if CONF_MIN_VALUE in band and CONF_MAX_VALUE in band and band[CONF_MIN_VALUE] >= band[CONF_MAX_VALUE]:
            raise cv.Invalid("min must be below max")
    return bands


# A band: the badge text for readings in [min, max) (either end may be left open).
# The first matching band wins. level orders severity for the overview, which shows the
# worst; level 2 or more raises its warning. level defaults to the band's position.
BAND_SCHEMA = cv.All(
    _band_keys,
    cv.Schema(
        {
            cv.Optional(CONF_MIN_VALUE): cv.float_,
            cv.Optional(CONF_MAX_VALUE): cv.float_,
            cv.Required(CONF_TEXT): cv.All(cv.string_strict, cv.Length(min=1, max=BAND_TEXT_MAX)),
            cv.Optional(CONF_LEVEL): cv.int_range(min=0, max=100),
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
        cv.Optional(CONF_AUTO_ADVANCE): switch.switch_schema(
            AtmosAutoSwitch,
            default_restore_mode="RESTORE_DEFAULT_ON",
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:autorenew",
        ).extend(cv.COMPONENT_SCHEMA),
        # Seconds each page is held before automatic cycling moves on.
        cv.Optional(CONF_HOLD_TIME): _number_schema(UNIT_SECOND, HOLD_LIMITS, 60.0, 6.5),
        # Minutes of history the charts show.
        cv.Optional(CONF_HISTORY_TIME): _number_schema(UNIT_MINUTE, HISTORY_LIMITS, 60.0, 2.0),
        # Any ESPHome sensor per reading. Once one is linked the demo data stops, and
        # unlinked readings show "--". Units: CO2 ppm, VOC/NOx index, formaldehyde ppb,
        # PM ug/m3, temperature degC, humidity %, pressure hPa (convert with filters).
        cv.Optional(CONF_SENSORS, default={}): cv.Schema(
            {cv.Optional(key): cv.use_id(sensor.Sensor) for key, _ in READINGS}
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
    cg.add(var.set_key_interval_ms(config[CONF_KEY_INTERVAL].total_milliseconds))
    cg.add(var.set_splash_name(config[CONF_SPLASH_NAME]))
    cg.add(var.set_splash_subtitle(config[CONF_SPLASH_SUBTITLE]))
    cg.add(var.set_splash_timeout_ms(config[CONF_SPLASH_TIMEOUT].total_milliseconds))

    for index, (key, _) in enumerate(READINGS):
        if key in config[CONF_SENSORS]:
            cg.add(var.set_sensor(index, await cg.get_variable(config[CONF_SENSORS][key])))
    nan = cg.RawExpression("NAN")
    for index, (key, _) in enumerate(READINGS):
        for band in config[CONF_RATINGS].get(key, []):
            lo, hi = band.get(CONF_MIN_VALUE), band.get(CONF_MAX_VALUE)
            cg.add(var.add_band(index, nan if lo is None else lo, nan if hi is None else hi, band[CONF_LEVEL], band[CONF_TEXT]))
    if conf := config.get(CONF_READING_SELECT):
        sel = await select.new_select(conf, options=["Auto advance"] + [name for _, name in READINGS])
        await cg.register_parented(sel, var)
        cg.add(var.set_reading_select(sel))
    if conf := config.get(CONF_AUTO_ADVANCE):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
        await cg.register_parented(sw, var)
        cg.add(var.set_auto_switch(sw))
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