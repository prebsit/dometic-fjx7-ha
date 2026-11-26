"""The ice9 page password: off until the owner sets one. Ported from F451 (components/f451_lock).

None until the owner sets one on the page; kept in flash; gone again with a factory
reset, which is the way back in if it's forgotten (five power cycles, or BOOT held
10 s: both need hands on the box).

The check is the box's own, not ESPHome's web-server `auth:`. ESPHome's wants a user
name as well, fixed when the firmware is built, and nobody would know to type it.
Here the login box's user name can be anything, or nothing: only the password counts.

  - A gate is the first handler the web server asks about every request
    (page_lock.cpp, Gate). With a password set, a request without it gets a 401 and
    the browser's login box, and nothing else sees it. That covers the page, its REST
    calls and live events, /ice9/password, and the setup hotspot's pages too while a
    password exists (after a factory reset none does). MQTT and Home Assistant don't
    go through the web server.
  - A new password is ON TRIAL until someone logs in with it: nothing goes to flash
    until then, the old one (if any) still gets in meanwhile, and if nobody logs in
    with it within 10 minutes the box goes back to how it was. So setting a password
    can't lock anyone out for longer than that, typos included. Removing one is at
    once.
  - The firmware upload (POST /update, ESPHome's web_server OTA) takes either the
    page password or hands on the box. With a password set: any time, for someone
    who has given it. With none set: for 10 minutes after the box is plugged in
    (a restart from the page or after an update doesn't count), and 403 the rest of
    the time. The setup hotspot is always open: getting on needs the card's password.

  GET  /ice9/password   {"set":..,"pending":..,"window":..}   (pending: on trial;
                        window: seconds left of the plug-in window, 0 = shut)
  POST /ice9/password   form field new=<4 to 64 characters> sets or changes it;
                        new= (empty) removes it. Printable ASCII only: browsers
                        don't agree on how to send anything else in a login.
                        With a password set, also current=<it>, typed: a phone
                        that remembers the login can work the aircon but not take
                        the password over. 403 if wrong; after 5 wrong in a row,
                        403 for 10 minutes (the reason says which).

  GET  /ice9/about      {"name":"ice9-abcdef","serial":"abcdef","version":"1.0.0",
                        "esphome":"2026.11.0","window":..}   what this box is, for
                        the page's Settings and for support (behind the password
                        like everything else)

Plain HTTP: anyone sniffing the van's own Wi-Fi could read it. It keeps out whoever
else is on that Wi-Fi, not someone with a packet sniffer on it.
"""
import esphome.codegen as cg
from esphome.components import web_server
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["web_server_base", "web_server"]

# Spelled out here: where ESPHome keeps this name has moved between releases.
CONF_WEB_SERVER_ID = "web_server_id"

lock_ns = cg.esphome_ns.namespace("ice9_lock")
PageLock = lock_ns.class_("PageLock", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PageLock),
        # The web server that serves the page: the setup screen hands it the page's own requests
        # while the setup hotspot is up (page_lock.cpp, SetupScreen). Found by itself.
        cv.GenerateID(CONF_WEB_SERVER_ID): cv.use_id(web_server.WebServer),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_web_server(await cg.get_variable(config[CONF_WEB_SERVER_ID])))
