import esphome.codegen as cg
from esphome.components import climate
import esphome.config_validation as cv

from .. import CONF_DOMETIC_FJX7_ID, FJX7_CLIENT_SCHEMA, dometic_fjx7_ns

DEPENDENCIES = ["dometic_fjx7"]

DometicFJX7Climate = dometic_fjx7_ns.class_(
    "DometicFJX7Climate", climate.Climate, cg.Component
)

CONFIG_SCHEMA = (
    climate.climate_schema(DometicFJX7Climate)
    .extend(cv.COMPONENT_SCHEMA)
    .extend(FJX7_CLIENT_SCHEMA)
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_DOMETIC_FJX7_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_climate(var))
