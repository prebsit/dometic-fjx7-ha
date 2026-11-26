"""The ice9's kept log: what the box did, who asked, and what the aircon did on its own.

Lifted out of F451 (its log lives inside esphome/components/wbus/wbus.cpp there), in the same
formats, so the page reads both the same way:

  GET /ice9/log      since the box last started, as plain text, oldest first: "Booted: ..."
                     first (never scrolled off; with the date when the clock knows it), then the
                     last 100 lines, "HH:MM:SS text" ("+M:SS text", time since the start, until
                     the clock is set), and "--- Thu 26 Nov ---" whenever the day changes.
  GET /ice9/log/all  the history kept in flash, through restarts, updates and power cuts: up to
                     1200 lines (about a week), oldest first, after one line "History: 812 lines,
                     oldest first (room for 1200; then the oldest go)". Sent a piece at a time.

A '~' in a line marks where the detail starts (where a command came from, network reasons,
memory): the page shows a line up to it unless asked for the detail. Both are behind the page
password like everything else on the web server. A factory reset wipes the history with the
rest of the settings.

What goes in:
  - every command to the aircon, its lights and its power limit, and every button pressed,
    with who asked. Worked out from which part of ESPHome is carrying the command out when it
    lands (Application::get_current_component()): the API server is Home Assistant, the MQTT
    client is MQTT, the web server is the web. A web request can say who it is with a header,
    "X-Ice9-From: page" or "X-Ice9-From: shortcut"; one that doesn't is "another app on the
    Wi-Fi". The asker's address goes in the detail.
  - what the aircon reports that nobody here asked for ("Not asked by the ice9: its own panel,
    remote or app"): a change to a setting no command touched in the 8 s before. A change is
    written once the aircon has been still for 2 s, so one press of its remote is one line.
  - the Bluetooth link to the aircon coming and going; Wi-Fi and MQTT coming and going, why,
    and the free memory when they went (as F451)
  - what the ice9's own parts log at INFO or worse: the page password (ice9.*), pairing
    (pair), the broker setting (mqtt_cfg), resets (reset), the packages' logger.log lines (main)
  - how the box started ("Booted: power on", "restart", "CRASH (panic)", "BROWNOUT: ..."),
    and whether the firmware is new since the last start

No change to components/dometic_fjx7: the log listens with ESPHome's own hooks (the climate's
control and state callbacks, the lights' listeners, the select's state, button presses), and
for the power limit, whose select has no hook for a command, the component's own "Set param
0x2d" line as it goes to the aircon.

The clock: none of its own (no battery). Times come from SNTP (webpage.yaml) or Home Assistant
once either has set it; until then lines carry the time since the start.

Needs ESPHome 2026.11 or later (the entity lists and listener interfaces it uses).
"""
import esphome.codegen as cg
from esphome.components import ble_client, web_server
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

DEPENDENCIES = ["web_server_base", "web_server", "ble_client", "dometic_fjx7"]

# Spelled out here: where ESPHome keeps these names has moved between releases.
CONF_WEB_SERVER_ID = "web_server_id"
CONF_BLE_CLIENT_ID = "ble_client_id"

log_ns = cg.esphome_ns.namespace("ice9_log")
KeptLog = log_ns.class_("KeptLog", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(KeptLog),
        # The web server: a command it carries out came in by the web. Found by itself.
        cv.GenerateID(CONF_WEB_SERVER_ID): cv.use_id(web_server.WebServer),
        # The Bluetooth link to the aircon (core.yaml's fjx7_ble). Found by itself.
        cv.GenerateID(CONF_BLE_CLIENT_ID): cv.use_id(ble_client.BLEClient),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_web_server(await cg.get_variable(config[CONF_WEB_SERVER_ID])))
    cg.add(var.set_ble_client(await cg.get_variable(config[CONF_BLE_CLIENT_ID])))
    # A log-listener slot: the ice9's own parts' lines, the Wi-Fi's and MQTT's reasons, and the
    # power limit going to the aircon.
    if "logger" in CORE.loaded_integrations:
        from esphome.components import logger

        logger.request_log_listener()
        cg.add(var.listen_to_logs())
