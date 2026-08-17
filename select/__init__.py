import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from .. import CONF_DOMETIC_FJX7_ID, FJX7_CLIENT_SCHEMA, dometic_fjx7_ns

DEPENDENCIES = ["dometic_fjx7"]

DometicFJX7Select = dometic_fjx7_ns.class_(
    "DometicFJX7Select", select.Select, cg.Component
)

# Order matches ADAPTIVE_POWER_VALUES {0, 1, 2, 3, 7} in dometic_fjx7.h.
# Values 4/5/6 are reserved (not used on FJX7 2200; possibly used on
# higher-current models) and are intentionally not exposed here, mirroring
# the official Dometic app UI.
ADAPTIVE_POWER_OPTIONS = ["4A", "5A", "6A", "7A", "Unlimited"]

CONFIG_SCHEMA = (
    select.select_schema(DometicFJX7Select)
    .extend(cv.COMPONENT_SCHEMA)
    .extend(FJX7_CLIENT_SCHEMA)
)


async def to_code(config):
    var = await select.new_select(config, options=ADAPTIVE_POWER_OPTIONS)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_DOMETIC_FJX7_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_adaptive_power_select(var))
