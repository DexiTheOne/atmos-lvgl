from esphome import automation, core
import esphome.codegen as cg
from esphome.components import i2c, sensirion_common, sensor, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ALTITUDE_COMPENSATION,
    CONF_AMBIENT_PRESSURE_COMPENSATION,
    CONF_AMBIENT_PRESSURE_COMPENSATION_SOURCE,
    CONF_AUTOMATIC_SELF_CALIBRATION,
    CONF_CO2,
    CONF_HUMIDITY,
    CONF_ID,
    CONF_TEMPERATURE,
    CONF_TEMPERATURE_OFFSET,
    CONF_UPDATE_INTERVAL,
    CONF_VALUE,
    DEVICE_CLASS_CARBON_DIOXIDE,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_TEMPERATURE,
    ICON_MOLECULE_CO2,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PARTS_PER_MILLION,
    UNIT_PERCENT,
)
from esphome.core import ID
from esphome.cpp_generator import MockObj, TemplateArgsType
from esphome.types import ConfigType

DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensirion_common", "text_sensor"]

# atmos: guarded forced calibration (start_calibration() / cancel_calibration()).
CONF_CALIBRATION = "calibration"
CONF_STATUS = "status"
CONF_REFERENCE = "reference"
CONF_SETTLE_TIME = "settle_time"
CONF_CO2_SPREAD = "co2_spread"
CONF_HUMIDITY_SPREAD = "humidity_spread"
CONF_CAL_TIMEOUT = "timeout"

CALIBRATION_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_REFERENCE, default=420): cv.int_range(min=400, max=2000),
        cv.Optional(CONF_SETTLE_TIME, default="10min"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=core.TimePeriod(minutes=1), max=core.TimePeriod(minutes=60)),
        ),
        cv.Optional(CONF_CAL_TIMEOUT, default="60min"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=core.TimePeriod(minutes=5), max=core.TimePeriod(hours=24)),
        ),
        cv.Optional(CONF_CO2_SPREAD, default=20.0): cv.float_range(min=1.0, max=500.0),
        cv.Optional(CONF_HUMIDITY_SPREAD, default=2.0): cv.float_range(min=0.1, max=50.0),
    }
)

scd30_ns = cg.esphome_ns.namespace("scd30")
SCD30Component = scd30_ns.class_(
    "SCD30Component", cg.Component, sensirion_common.SensirionI2CDevice
)

# Actions
ForceRecalibrationWithReference = scd30_ns.class_(
    "ForceRecalibrationWithReference", automation.Action
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SCD30Component),
            cv.Optional(CONF_CO2): sensor.sensor_schema(
                unit_of_measurement=UNIT_PARTS_PER_MILLION,
                icon=ICON_MOLECULE_CO2,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_CARBON_DIOXIDE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_HUMIDITY,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_AUTOMATIC_SELF_CALIBRATION, default=True): cv.boolean,
            cv.Optional(CONF_ALTITUDE_COMPENSATION): cv.All(
                cv.float_with_unit("altitude", "(m|m a.s.l.|MAMSL|MASL)"),
                cv.int_range(min=0, max=0xFFFF, max_included=False),
            ),
            cv.Optional(CONF_AMBIENT_PRESSURE_COMPENSATION, default=0): cv.pressure,
            # atmos: a pressure sensor in hPa (mbar), sent when it changes by 1 mbar.
            cv.Optional(CONF_AMBIENT_PRESSURE_COMPENSATION_SOURCE): cv.use_id(
                sensor.Sensor
            ),
            cv.Optional(CONF_CALIBRATION, default={}): CALIBRATION_SCHEMA,
            cv.Optional(CONF_TEMPERATURE_OFFSET): cv.All(
                cv.temperature_delta,
                cv.float_range(min=0, max=655.35),
            ),
            cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(
                    min=core.TimePeriod(seconds=2), max=core.TimePeriod(seconds=1800)
                ),
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x61))
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_automatic_self_calibration(config[CONF_AUTOMATIC_SELF_CALIBRATION]))
    if CONF_ALTITUDE_COMPENSATION in config:
        cg.add(var.set_altitude_compensation(config[CONF_ALTITUDE_COMPENSATION]))

    if CONF_AMBIENT_PRESSURE_COMPENSATION in config:
        cg.add(
            var.set_ambient_pressure_compensation(
                config[CONF_AMBIENT_PRESSURE_COMPENSATION]
            )
        )

    if CONF_AMBIENT_PRESSURE_COMPENSATION_SOURCE in config:
        sens = await cg.get_variable(config[CONF_AMBIENT_PRESSURE_COMPENSATION_SOURCE])
        cg.add(var.set_ambient_pressure_source(sens))

    cal = config[CONF_CALIBRATION]
    cg.add(var.set_calibration_reference(cal[CONF_REFERENCE]))
    cg.add(var.set_calibration_settle_time(cal[CONF_SETTLE_TIME].total_milliseconds))
    cg.add(var.set_calibration_timeout(cal[CONF_CAL_TIMEOUT].total_milliseconds))
    cg.add(var.set_calibration_co2_spread(cal[CONF_CO2_SPREAD]))
    cg.add(var.set_calibration_humidity_spread(cal[CONF_HUMIDITY_SPREAD]))
    if CONF_STATUS in cal:
        status = await text_sensor.new_text_sensor(cal[CONF_STATUS])
        cg.add(var.set_calibration_status(status))

    if CONF_TEMPERATURE_OFFSET in config:
        cg.add(var.set_temperature_offset(config[CONF_TEMPERATURE_OFFSET]))

    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))

    if CONF_CO2 in config:
        sens = await sensor.new_sensor(config[CONF_CO2])
        cg.add(var.set_co2_sensor(sens))

    if CONF_HUMIDITY in config:
        sens = await sensor.new_sensor(config[CONF_HUMIDITY])
        cg.add(var.set_humidity_sensor(sens))

    if CONF_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_TEMPERATURE])
        cg.add(var.set_temperature_sensor(sens))


@automation.register_action(
    "scd30.force_recalibration_with_reference",
    ForceRecalibrationWithReference,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(SCD30Component),
            cv.Required(CONF_VALUE): cv.templatable(
                cv.int_range(min=400, max=2000, max_included=True)
            ),
        },
        key=CONF_VALUE,
    ),
    synchronous=True,
)
async def scd30_force_recalibration_with_reference_to_code(
    config: ConfigType,
    action_id: ID,
    template_arg: cg.TemplateArguments,
    args: TemplateArgsType,
) -> MockObj:
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    template_ = await cg.templatable(config[CONF_VALUE], args, cg.uint16)
    cg.add(var.set_value(template_))
    return var
