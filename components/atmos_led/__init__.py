"""Status LEDs for the Atmos UI: an ESPHome addressable light (any strip platform, e.g.
esp32_rmt_led_strip with chipset, num_leds and pin) shows the colour of the rating on
screen (each band's led_color under atmos: ratings:). On a page change the LEDs fade
out over the first half of the transition and into the new colour over the second.
atmos_led is itself a brightness-only light (Home Assistant: power and a brightness
slider). While the boot splash waits for the connection, boot_color glows on every
LED at the light's brightness with a full-brightness glow bouncing along it ("none" keeps it dark). Make the strip light internal; it is
held on at full brightness and only carries the colours.

auto_dimming: an ambient light sensor (any sensor, e.g. an LDR divider on an ADC)
limits the LEDs without changing the light entity's brightness, so Home Assistant's
recorder sees only user changes. The user's brightness is the maximum (reached at
bright_value and beyond); the output falls linearly to min_brightness at dark_value.
The reading is smoothed over smoothing. An optional switch turns it on and off.
The same smoothed reading reports low light to the UI (within 10 % of dark_value,
lit again from 30 %), whatever the switch: it mutes alert sounds and drives the
atmos overview_low_light option.
"""

import esphome.codegen as cg
from esphome.components import light, sensor, switch
import esphome.config_validation as cv
from esphome.const import CONF_LIGHT_ID

from esphome.components.atmos import AtmosComponent, _rgb

DEPENDENCIES = ["atmos", "light"]
AUTO_LOAD = ["switch", "sensor"]

CONF_ATMOS_ID = "atmos_id"
CONF_FADE_TIME = "fade_time"
CONF_BOOT_COLOR = "boot_color"
CONF_REVERSE = "reverse"
CONF_AUTO_DIMMING = "auto_dimming"
CONF_SENSOR = "sensor"
CONF_BRIGHT_VALUE = "bright_value"
CONF_DARK_VALUE = "dark_value"
CONF_MIN_BRIGHTNESS = "min_brightness"
CONF_SMOOTHING = "smoothing"
CONF_SWITCH = "switch"

atmos_led_ns = cg.esphome_ns.namespace("atmos_led")
AtmosLed = atmos_led_ns.class_("AtmosLed", cg.Component, light.LightOutput)
AutoDimSwitch = atmos_led_ns.class_("AutoDimSwitch", switch.Switch, cg.Component)

AUTO_DIMMING_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
        # Sensor values for full (user) brightness and for min_brightness; either order.
        cv.Required(CONF_BRIGHT_VALUE): cv.float_,
        cv.Required(CONF_DARK_VALUE): cv.float_,
        cv.Optional(CONF_MIN_BRIGHTNESS, default="13%"): cv.percentage,
        cv.Optional(CONF_SMOOTHING, default="5s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_SWITCH): switch.switch_schema(
            AutoDimSwitch, default_restore_mode="RESTORE_DEFAULT_ON", icon="mdi:brightness-auto"
        ).extend(cv.COMPONENT_SCHEMA),
    }
)

CONFIG_SCHEMA = light.light_schema(
    AtmosLed, light.LightType.BRIGHTNESS_ONLY, default_restore_mode="RESTORE_DEFAULT_ON"
).extend(
    {
        cv.GenerateID(CONF_ATMOS_ID): cv.use_id(AtmosComponent),
        cv.Required(CONF_LIGHT_ID): cv.use_id(light.AddressableLightState),
        # Eased cross-fade when the colour changes on a still page (another underlined
        # reading or a rating change); 800ms matches the underline and chart on screen.
        cv.Optional(CONF_FADE_TIME, default="800ms"): cv.positive_time_period_milliseconds,
        # Glow bouncing along the LEDs while the splash waits for the connection; "none" keeps them dark.
        cv.Optional(CONF_BOOT_COLOR, default="#18BCF2"): cv.Any(cv.one_of("none", lower=True), _rgb),
        # The strip's first LED is on the right: the boot glow starts from the left.
        cv.Optional(CONF_REVERSE, default=False): cv.boolean,
        cv.Optional(CONF_AUTO_DIMMING): AUTO_DIMMING_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await light.new_light(config)
    await cg.register_component(var, config)
    cg.add(var.set_atmos(await cg.get_variable(config[CONF_ATMOS_ID])))
    cg.add(var.set_light(await cg.get_variable(config[CONF_LIGHT_ID])))
    cg.add(var.set_fade_ms(config[CONF_FADE_TIME].total_milliseconds))
    boot = config[CONF_BOOT_COLOR]
    cg.add(var.set_boot_color(0 if boot == "none" else boot))
    cg.add(var.set_reverse(config[CONF_REVERSE]))
    if dim := config.get(CONF_AUTO_DIMMING):
        cg.add(var.set_ambient_sensor(await cg.get_variable(dim[CONF_SENSOR])))
        cg.add(var.set_ambient_range(dim[CONF_BRIGHT_VALUE], dim[CONF_DARK_VALUE], dim[CONF_MIN_BRIGHTNESS]))
        cg.add(var.set_smoothing_ms(dim[CONF_SMOOTHING].total_milliseconds))
        if sw_conf := dim.get(CONF_SWITCH):
            sw = await switch.new_switch(sw_conf)
            await cg.register_component(sw, sw_conf)
            cg.add(sw.set_parent(var))
