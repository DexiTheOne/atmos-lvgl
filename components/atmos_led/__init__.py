"""Status LEDs for the Atmos UI: an ESPHome addressable light (any strip platform, e.g.
esp32_rmt_led_strip with chipset, num_leds and pin) shows the colour of the rating on
screen (each band's led_color under atmos: ratings:). On a page change the LEDs fade
out over the first half of the transition and into the new colour over the second.
atmos_led is itself a brightness-only light (Home Assistant: power and a brightness
slider). Make the strip light internal; it is held on at full brightness and only
carries the colours.
"""

import esphome.codegen as cg
from esphome.components import light
import esphome.config_validation as cv
from esphome.const import CONF_LIGHT_ID

from esphome.components.atmos import AtmosComponent

DEPENDENCIES = ["atmos", "light"]

CONF_ATMOS_ID = "atmos_id"
CONF_FADE_TIME = "fade_time"

atmos_led_ns = cg.esphome_ns.namespace("atmos_led")
AtmosLed = atmos_led_ns.class_("AtmosLed", cg.Component, light.LightOutput)

CONFIG_SCHEMA = light.light_schema(
    AtmosLed, light.LightType.BRIGHTNESS_ONLY, default_restore_mode="RESTORE_DEFAULT_ON"
).extend(
    {
        cv.GenerateID(CONF_ATMOS_ID): cv.use_id(AtmosComponent),
        cv.Required(CONF_LIGHT_ID): cv.use_id(light.AddressableLightState),
        # Cross-fade when the shown rating changes without a page transition.
        cv.Optional(CONF_FADE_TIME, default="500ms"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await light.new_light(config)
    await cg.register_component(var, config)
    cg.add(var.set_atmos(await cg.get_variable(config[CONF_ATMOS_ID])))
    cg.add(var.set_light(await cg.get_variable(config[CONF_LIGHT_ID])))
    cg.add(var.set_fade_ms(config[CONF_FADE_TIME].total_milliseconds))
