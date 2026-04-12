import esphome.codegen as cg
from esphome.components import light
import esphome.config_validation as cv

from .. import CONF_DOMETIC_FJX7_ID, FJX7_CLIENT_SCHEMA, dometic_fjx7_ns

DEPENDENCIES = ["dometic_fjx7"]

DometicFJX7Light = dometic_fjx7_ns.class_(
    "DometicFJX7Light", light.LightOutput, cg.Component
)

CONF_LIGHT_TYPE = "light_type"

CONFIG_SCHEMA = (
    light.light_schema(DometicFJX7Light, light.LightType.BINARY)
    .extend(
        {
            cv.Required(CONF_LIGHT_TYPE): cv.one_of("interior", "exterior", lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(FJX7_CLIENT_SCHEMA)
)


async def to_code(config):
    var = await light.new_light(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_DOMETIC_FJX7_ID])
    cg.add(var.set_parent(parent))
    if config[CONF_LIGHT_TYPE] == "interior":
        cg.add(var.set_param(0x05))
        cg.add(parent.set_interior_light(var))
    else:
        cg.add(var.set_param(0x0E))
        cg.add(parent.set_exterior_light(var))
