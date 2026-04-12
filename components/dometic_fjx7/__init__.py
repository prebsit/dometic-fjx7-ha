import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client
from esphome.const import CONF_ID

CODEOWNERS = ["@prebsit"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["climate", "sensor", "light"]
MULTI_CONF = False

CONF_DOMETIC_FJX7_ID = "dometic_fjx7_id"

dometic_fjx7_ns = cg.esphome_ns.namespace("dometic_fjx7")
DometicFJX7 = dometic_fjx7_ns.class_(
    "DometicFJX7", ble_client.BLEClientNode, cg.Component
)

CONFIG_SCHEMA = (
    cv.Schema({cv.GenerateID(): cv.declare_id(DometicFJX7)})
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)

FJX7_CLIENT_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_DOMETIC_FJX7_ID): cv.use_id(DometicFJX7)}
)


async def register_fjx7_child(var, config):
    parent = await cg.get_variable(config[CONF_DOMETIC_FJX7_ID])
    cg.add(var.set_parent(parent))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
