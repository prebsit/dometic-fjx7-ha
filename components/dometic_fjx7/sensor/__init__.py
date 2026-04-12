import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PERCENT,
)

from .. import CONF_DOMETIC_FJX7_ID, FJX7_CLIENT_SCHEMA, dometic_fjx7_ns, register_fjx7_child

DEPENDENCIES = ["dometic_fjx7"]

CONF_MEASURED_TEMP = "measured_temperature"
CONF_FAN_SPEED_PCT = "fan_speed_percent"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.Optional(CONF_MEASURED_TEMP): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_FAN_SPEED_PCT): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(FJX7_CLIENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DOMETIC_FJX7_ID])

    if CONF_MEASURED_TEMP in config:
        sens = await sensor.new_sensor(config[CONF_MEASURED_TEMP])
        cg.add(parent.set_measured_temp_sensor(sens))

    if CONF_FAN_SPEED_PCT in config:
        sens = await sensor.new_sensor(config[CONF_FAN_SPEED_PCT])
        cg.add(parent.set_fan_speed_pct_sensor(sens))
