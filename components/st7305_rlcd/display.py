"""ESPHome ST7305 RLCD Display Component Configuration."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import display, spi
from esphome.const import (
    CONF_DC_PIN,
    CONF_ID,
    CONF_LAMBDA,
    CONF_MODEL,
    CONF_RESET_PIN,
    CONF_UPDATE_INTERVAL,
    CONF_WIDTH,
    CONF_HEIGHT,
)

CODEOWNERS = []
DEPENDENCIES = ["spi"]
AUTO_LOAD = ["display"]

CONF_ORIENTATION = "orientation"
CONF_REFRESH_RATE = "refresh_rate"
CONF_TE_PIN = "te_pin"
CONF_PARTIAL_UPDATES = "partial_updates"
CONF_SOURCE_VOLTAGE_POSITIVE = "source_voltage_positive"
CONF_SOURCE_VOLTAGE_NEGATIVE = "source_voltage_negative"
REFRESH_RATES = {"16Hz": 32, "25.5Hz": 51, "32Hz": 64, "51Hz": 102}

st7305_rlcd_ns = cg.esphome_ns.namespace("st7305_rlcd")
ST7305RLCD = st7305_rlcd_ns.class_(
    "ST7305RLCD",
    display.DisplayBuffer,
    spi.SPIDevice,
)

ST7305Model = st7305_rlcd_ns.enum("ST7305Model")
MODELS = {
    "WAVESHARE_400X300": ST7305Model.ST7305_MODEL_WAVESHARE_400X300,
    "OSPTEK_200X200": ST7305Model.ST7305_MODEL_OSPTEK_200X200,
    "CUSTOM": ST7305Model.ST7305_MODEL_CUSTOM,
}

ST7305Orientation = st7305_rlcd_ns.enum("ST7305Orientation")
ORIENTATIONS = {
    "LANDSCAPE": ST7305Orientation.ST7305_ORIENTATION_LANDSCAPE,
    "PORTRAIT": ST7305Orientation.ST7305_ORIENTATION_PORTRAIT,
}


def validate_custom_panel(config):
    """Validate that custom panels have required dimensions."""
    if config.get(CONF_MODEL) == "CUSTOM":
        if CONF_WIDTH not in config or CONF_HEIGHT not in config:
            raise cv.Invalid(
                "Custom model requires 'width' and 'height' to be specified"
            )
        if CONF_ORIENTATION not in config:
            raise cv.Invalid(
                "Custom model requires 'orientation' (LANDSCAPE or PORTRAIT)"
            )
    return config


CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(ST7305RLCD),
            cv.Required(CONF_DC_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            # Tearing-effect output: frames can be sent at the start of the panel's blank.
            cv.Optional(CONF_TE_PIN): pins.internal_gpio_input_pin_schema,
            # Send only the changed window of each frame (false: the whole frame).
            cv.Optional(CONF_PARTIAL_UPDATES, default=True): cv.boolean,
            cv.Optional(CONF_MODEL, default="WAVESHARE_400X300"): cv.enum(
                MODELS, upper=True, space="_"
            ),
            cv.Optional(CONF_UPDATE_INTERVAL, default="never"): cv.update_interval,
            cv.Optional(CONF_WIDTH): cv.int_range(min=1, max=800),
            cv.Optional(CONF_HEIGHT): cv.int_range(min=1, max=800),
            cv.Optional(CONF_ORIENTATION): cv.enum(ORIENTATIONS, upper=True),
            # Panel refresh in high-power mode (the controller offers 16, 25.5, 32 or 51 Hz).
            cv.Optional(CONF_REFRESH_RATE, default="32Hz"): cv.one_of(*REFRESH_RATES),
            # High-power source drive (VSHP C1h, VSHN C4h; datasheet v0.2 8.2.11/8.2.13),
            # 0.02 V steps. -3.0 V balances the two 1-dot inversion polarities on our
            # panel at 32 Hz: Waveshare's -4.0 V left a fine checkerboard over black and
            # lower contrast; -2.8 V and -3.1 V brought the checkerboard back.
            cv.Optional(CONF_SOURCE_VOLTAGE_POSITIVE, default=5.8): cv.All(
                cv.float_, cv.Range(min=3.7, max=6.2)
            ),
            cv.Optional(CONF_SOURCE_VOLTAGE_NEGATIVE, default=-3.0): cv.All(
                cv.float_, cv.Range(min=-5.2, max=-2.5)
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema(cs_pin_required=True)),
    validate_custom_panel,
)


async def to_code(config):
    """Generate C++ code from configuration."""
    var = cg.new_Pvariable(config[CONF_ID])

    await display.register_display(var, config)
    await spi.register_spi_device(var, config)

    dc_pin = await cg.gpio_pin_expression(config[CONF_DC_PIN])
    cg.add(var.set_dc_pin(dc_pin))

    if reset_config := config.get(CONF_RESET_PIN):
        reset_pin = await cg.gpio_pin_expression(reset_config)
        cg.add(var.set_reset_pin(reset_pin))

    if te_config := config.get(CONF_TE_PIN):
        cg.add(var.set_te_pin(await cg.gpio_pin_expression(te_config)))

    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_partial_updates(config[CONF_PARTIAL_UPDATES]))
    cg.add(var.set_refresh_hz_x2(REFRESH_RATES[config[CONF_REFRESH_RATE]]))
    cg.add(var.set_source_voltages(
        round((config[CONF_SOURCE_VOLTAGE_POSITIVE] - 3.7) / 0.02),
        round((-2.5 - config[CONF_SOURCE_VOLTAGE_NEGATIVE]) / 0.02),
    ))

    if config[CONF_MODEL] == "CUSTOM":
        cg.add(var.set_width(config[CONF_WIDTH]))
        cg.add(var.set_height(config[CONF_HEIGHT]))
        cg.add(var.set_orientation(config[CONF_ORIENTATION]))

    if lambda_config := config.get(CONF_LAMBDA):
        lambda_ = await cg.process_lambda(
            lambda_config, [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
