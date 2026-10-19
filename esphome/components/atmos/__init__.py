"""ESPHome external component running the platform-independent Atmos LVGL UI.

The component owns LVGL itself (ESPHome's lvgl: component is not used): it adds the
LVGL library, uses the project's shared desktop/lv_conf.h, renders into a native
1-bit frame and hands it to the ST7305 driver. The UI sources live in desktop/ (the
simulator is the source of truth) and are copied into the build's src/atmos_ui/.
"""

from pathlib import Path

import esphome.codegen as cg
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE
from esphome.helpers import copy_file_if_changed

DEPENDENCIES = ["display"]

CONF_DISPLAY_ID = "display_id"
CONF_FRAME_PERIOD = "frame_period"
CONF_REPORT_INTERVAL = "report_interval"

# Keep in step with desktop/build.ps1 (the simulator's pinned LVGL).
LVGL_VERSION = "9.5.0"
UI_DIR = Path(__file__).resolve().parents[3] / "desktop"
UI_FILES = ("atmos.c", "atmos.h", "ui_fonts.c", "ui_fonts.h", "ui_theme.h", "lv_conf.h")

atmos_ns = cg.esphome_ns.namespace("atmos")
AtmosComponent = atmos_ns.class_("AtmosComponent", cg.Component)
ST7305RLCD = cg.esphome_ns.namespace("st7305_rlcd").class_("ST7305RLCD")

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AtmosComponent),
        cv.Required(CONF_DISPLAY_ID): cv.use_id(ST7305RLCD),
        # Model step cadence. 25 ms = 40 fps target.
        cv.Optional(CONF_FRAME_PERIOD, default="25ms"): cv.positive_time_period_microseconds,
        cv.Optional(CONF_REPORT_INTERVAL, default="10s"): cv.positive_time_period_milliseconds,
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
