# Dometic FreshJet FJX / FJZ — ESPHome Bridge for Home Assistant, Victron and your phone

The first working integration for Dometic FreshJet roof air conditioners. An ESP32 talks to the unit over Bluetooth and puts it on your van's WiFi: in **Home Assistant**, on a **Victron GX** screen (Cerbo, Ekrano) via Node-RED, or on a plain **web page** on your phone. Confirmed on the FJX7, FJX4 and FJZ7.

**No cloud. No Dometic app. No wiring.**

> **Current release: v0.5.0-rc.1** (release candidate). The "board in a box" firmware is being field-tested. If you want something that won't change under you, pin `ref: v0.2.0` (component only) or a later tag in your config instead of `main`.

## What You Get

- **Climate control** — Cool, Heat, Auto, Dry, Fan Only, Off; target temperature 16–31 °C
- **Fan speed** — Low, Medium, High, Turbo, Auto
- **Interior & exterior lights** — on/off
- **Sleep mode** — the FJX7's Sleep preset (moon icon, dimmed display, quiet fan)
- **Adaptive Power Mode** *(optional, FJZ only)* — cap the unit's current draw at 4/5/6/7 A or unlimited
- **Instant sync** — changes made on the AC's own panel or remote show up everywhere immediately
- **Auto-reconnect** — recovers from power cycles and the AC being switched off at the isolator
- **Three ways in** — Home Assistant (ESPHome API), MQTT (Victron GX via Node-RED, or any broker), and the board's own web page. Use any or all of them.

## Pick your route

| You have… | Go to |
|---|---|
| An ESP32-S3 and want to build your own | [Building your own](#building-your-own) |
| A board already flashed with the board-in-a-box firmware | [Setting up a board-in-a-box](#setting-up-a-board-in-a-box) |
| Your own ESPHome config and just want the component | [Using the component directly](#using-the-component-directly) |

## Setting up a board-in-a-box

About ten minutes, all from your phone. You need the board's **ID** (the last six characters of its MAC, e.g. `f1de94`) and its **setup password**; `tools/ap_card.py` prints both, plus a WiFi QR code, when you flash it (see [Building your own](#building-your-own)).

1. **Plug the board in** near the AC (any 5 V USB supply).
2. **Join its setup hotspot.** Scan the QR code, or join the WiFi network `fjx7-bridge-<ID>` with the setup password.
3. **Open Safari / Chrome and go to `http://192.168.4.1`.** (Your phone may pop this page up by itself; if not, open it by hand.) Pick your van's WiFi, enter its password, save. The board joins your WiFi and the hotspot disappears.
   The board only ever joins **your van's private WiFi**. It has no login on its web page, so don't put it on a campsite network.
4. **Pair it with the AC.** On the AC's control panel, hold **+ and −** together for 3 seconds until the display shows `BL`. The board finds the unit and pairs with it by itself, usually within a few seconds.
5. **Open its page:** `http://fjx7-bridge-<ID>.local` on any phone or laptop on the van WiFi. Tip: add it to your home screen. The **Pairing** line should say *Connected to SHE_xxxxxx*.
   Android phones don't always understand `.local` addresses. If the page won't open, find the board's IP address in your router's list of devices and use that instead.

Then, depending on what else you run:

- **Victron GX (Cerbo, Ekrano…):** type the GX's IP address into **MQTT broker IP** on the board's page and press Enter. The board restarts (about 10 s) and the **MQTT connection** line changes to *Connected*. Then set up the GX side: see [Victron GX](#victron-gx-via-node-red).
- **Home Assistant:** it should find the board by itself (Settings → Devices & Services). Adopt it and HA sets its own encryption key.
- **Neither:** the web page *is* your remote.

**Starting again (factory reset).** Any of these wipes the WiFi, the AC pairing and the MQTT settings, and brings the setup hotspot back:
- **Unplug and plug back in 5 times**, unplugging within 10 s each time. The 5th time, leave it plugged in.
- Hold the board's **BOOT** button for 10 s while it's running (not while plugging it in).
- On the web page, press **Factory reset** twice within 10 s.

**Pairing with a different AC:** press **Forget AC** twice within 10 s on the web page, then hold + and − on the new unit.

**Known quirks (release candidate):**
- After changing an MQTT setting the page doesn't always notice the board coming back. Reload it after 20 s.
- The page's firmware upload box is not password-protected (neither is the page). That's by design for a board on a private van network, but it is why the board must never go on public WiFi.

## Building your own

The firmware is split into packages, so you only build what you use. Every example pins a release tag; bump `ref:` and `fjx7_ref:` together when you update.

| Example | What it builds |
|---|---|
| [`examples/home-assistant.yaml`](examples/home-assistant.yaml) | Home Assistant only. WiFi and the AC's address set in your config |
| [`examples/victron-node-red.yaml`](examples/victron-node-red.yaml) | MQTT for Victron / Node-RED (add `ha.yaml` for both) |
| [`examples/board-in-a-box.yaml`](examples/board-in-a-box.yaml) | Board-in-a-box: nothing about the van compiled in; set up from a phone (setup hotspot, auto-pair, web page, HA, MQTT, factory reset) |

| Package | Does |
|---|---|
| `core.yaml` | Required. The component, Bluetooth, and the climate/light/sensor entities |
| `ha.yaml` | Home Assistant (encrypted ESPHome API) |
| `mqtt.yaml` | MQTT, for Node-RED on a Victron GX or any broker |
| `adaptive_power.yaml` | FJZ only: the Adaptive Power select |
| `provisioning.yaml` | Setup hotspot + captive portal instead of compiled-in WiFi. Needs `ap_key` |
| `autopair.yaml` | Pairs with whichever unit is put into pairing mode, instead of a compiled-in MAC |
| `webpage.yaml` | The control and setup web page (MQTT settings live here) |
| `reset.yaml` | Factory reset by 5 power cycles or holding BOOT |

**Building the board-in-a-box firmware:** `board-in-a-box.yaml` needs two secrets, `encryption_key` (OTA) and `ap_key` (the hotspot master key; `openssl rand -hex 16`). The build stops with an error if `ap_key` is missing. After flashing over USB, print the board's hotspot details from the MAC address esptool shows:

```bash
AP_KEY=<your ap_key> python3 tools/ap_card.py <board MAC>
```

It prints the SSID, the password and the `WIFI:` string for a QR code. **Never publish firmware built with your `ap_key`**: anyone holding it and a board's MAC can work out that board's hotspot password.

### Hardware

- **ESP32-S3 board.** The S3 handles Bluetooth and WiFi together without trouble.
- USB-C cable for the first flash; a 5 V USB supply in the van.

| Board | Status | Notes |
|-------|--------|-------|
| ESP32-S3 SuperMini | ✅ Confirmed | Small, cheap, rock solid. Used for the board-in-a-box build. BOOT is the right-hand button with USB-C at the bottom |
| ESP32-S3-DevKitC-1 (N16R8) | ✅ Confirmed | Dual-core, plenty of RAM |
| ESP32-C3 SuperMini | Untested | Single-core — may struggle with BLE+WiFi |
| ESP32-C6 | Untested | BLE 5.3 — should work |

**ESPHome:** 2026.9.0 or newer for the packages (OTA encryption). On every push and weekly, CI builds the component against several ESPHome versions and every package config against the current and development ESPHome.

### Victron GX via Node-RED

[`examples/node-red/`](examples/node-red/) has a flow that puts the AC on the GX screen and in VRM as a group of virtual switches (power, mode, fan speed, target temperature, and the FJZ power limit), in both directions. It needs **Venus OS Large** with Node-RED switched on and the GX's MQTT enabled. The [Node-RED README](examples/node-red/README.md) covers setup and import.

## Why an ESP32?

The FJX7's Microchip BLE module has a firmware quirk: it requires encrypted BLE (Just Works bonding) and does not send ATT Write Responses to Linux's BlueZ stack. This breaks every Linux-based BLE implementation — the connection drops on every write attempt. We tested multiple adapters (Pi 3B+ onboard, TP-Link UB500) across multiple BlueZ versions. All fail identically.

Apple's CoreBluetooth handles this silently (macOS works fine with bleak/Python), and Espressif's ESP-IDF/NimBLE stack also handles it correctly. So an ESP32 running ESPHome acts as a BLE-to-WiFi bridge: it connects to the FJX7 over BLE and exposes entities to Home Assistant over WiFi.

If you happen to run Home Assistant on a Mac Mini, a Python/bleak custom component approach may work for you via CoreBluetooth — but the ESPHome component is the recommended and tested path.

## Using the component directly

If you'd rather write your own config, this is the minimal version (WiFi and the AC's address compiled in). Put your secrets in `secrets.yaml` (`wifi_ssid`, `wifi_password`, `api_key`, `ota_key`; generate the keys with `openssl rand -base64 32`).

```yaml
esphome:
  name: fjx7-bridge
  friendly_name: "Dometic FJX7 Bridge"

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

logger:

api:
  encryption:
    key: !secret api_key

ota:
  - platform: esphome
    encryption:
      key: !secret ota_key

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

captive_portal:

esp32_ble_tracker:

ble_client:
  - mac_address: "XX:XX:XX:XX:XX:XX"  # your AC's address, see below
    id: fjx7_ble

external_components:
  - source:
      type: git
      url: https://github.com/prebsit/dometic-fjx7-ha
      ref: v0.5.0-rc.1  # pin a release tag so updates don't surprise you
    components: [dometic_fjx7]

dometic_fjx7:
  ble_client_id: fjx7_ble

climate:
  - platform: dometic_fjx7
    name: "Air Conditioning"

light:
  - platform: dometic_fjx7
    name: "Interior Light"
    light_type: interior
  - platform: dometic_fjx7
    name: "Exterior Light"
    light_type: exterior

sensor:
  - platform: dometic_fjx7
    measured_temperature:
      name: "Temperature"
    fan_speed_percent:
      name: "Fan Speed"

# FJZ only: Adaptive Power. Without this the component never touches 0x2D.
# select:
#   - platform: dometic_fjx7
#     name: "Adaptive Power"
```

**Finding the AC's address.** The unit advertises as `SHE_xxxxxx`. Flash with any address first and watch the log for `Name: 'SHE_…'`, or use a phone BLE scanner (nRF Connect). The Bluetooth address is the unit's base MAC plus 2: `SHE_36bd48` is `…:36:BD:4A`.

**Pairing (first time only).** The AC only accepts a new bond in pairing mode: hold **+ and −** on the panel for 3 s until it shows `BL`, then restart the ESP32. The log should show `Connected — requesting encryption` then `All parameters subscribed`. The bond survives power cycles and updates. If you skip this, the log loops `Connected` / `Disconnected`.

**Close the Dometic app first**: the AC only accepts one Bluetooth connection at a time.

## How It Works

The ESP32 connects to the AC using Dometic's DDM (Device Data Model) protocol over BLE GATT. It subscribes to the climate parameters and gets push notifications whenever anything changes. Commands go back as DDM Set commands over the same connection. Bond keys live in the ESP32's flash, so it reconnects by itself.

## Supported Devices

| Device | Protocol | Status |
|--------|----------|--------|
| FreshJet FJX7 | DDM over BLE | ✅ Fully working (Adaptive Power not supported) |
| FreshJet FJX4 | DDM over BLE | ✅ Confirmed working (community-tested by Nige) |
| FreshJet FJZ7 2200 | DDM over BLE | ✅ Confirmed working incl. Adaptive Power (community-tested, [@DRAKS1000](https://github.com/DRAKS1000)) |
| FreshJet FJX5 | DDM over BLE | 🔮 Likely compatible (untested) |
| FreshJet FJX3 | DDM over BLE | 🔮 Likely compatible (untested) |

The DDM protocol is shared across Dometic's connected product range. If you have a different FJX model, please test and report back.

**Units without an exterior light (e.g. FJX4):** the exterior light entity is harmless even if your unit doesn't have the hardware — the FJX7 component still creates it, but the device simply reports `0x0E = 0` and reading it causes no errors or beeping. You can either ignore the entity or disable it in Home Assistant. The interior light works normally.

## DDM Protocol Reference

For anyone wanting to understand or extend the protocol. The FJX7 uses a simple binary protocol over two BLE GATT characteristics on service `537a0400-0995-481f-926c-1604e23fd515`:

- **Write** (`537a0401`): host → device commands
- **Notify** (`537a0402`): device → host state reports

### Frame format

```
Byte 0:    Command (0x10=Report, 0x11=Set, 0x12=Subscribe)
Byte 1:    Parameter ID
Byte 2:    0x00 (padding)
Byte 3-4:  Group ID (0x02 0x01 for Climate Zone 1)
Byte 5-8:  Value (LE uint32) — only present in Set/Report (9 bytes total)
```

Subscribe frames are 5 bytes (no value). Set/Report frames are 9 bytes.

### AC Mode (param 0x03)

| Value | Mode | HA Climate Mode |
|-------|------|------------------|
| 0 | Cooling | Cool |
| 1 | Heating | Heat |
| 2 | Ventilation | Fan only |
| 3 | Automatic | Heat/Cool |
| 4 | Dehumidify | Dry |

### Fan Speed (param 0x02)

| Value | Speed | ADBD Display |
|-------|-------|--------------|
| 0 | Low | 1 |
| 1 | Medium | 2 |
| 2 | High | 3 |
| 3 | Turbo | 4 |
| 5 | Auto | AA |

Value 4 is not used.

### Other Parameters

| Param | Type | Description |
|-------|------|-------------|
| 0x01 | R/W | Power (0=off, 1=on) |
| 0x04 | R/W | Target temperature (millidegrees Celsius, LE uint32) |
| 0x05 | R/W | Interior light (0=off, 1=on) |
| 0x06 | R | Fan speed readback (0–100%) |
| 0x0A | R | Measured temperature (millidegrees Celsius) |
| 0x0E | R/W | Exterior light (0=off, 1=on) |
| 0x1B | R/W | Sleep mode flag (0=off, 1=on). See Sleep Mode section below |

### Sleep Mode (param 0x1B)

The FJX7's Sleep mode is exposed via Home Assistant's `preset_mode: sleep`. Setting it writes `0x1B = 1` over BLE; the device lights the moon icon on the panel, dims the display, and forces `fan_speed` to Low. Setting preset back to `none` clears the flag, and the firmware automatically restores the previous `fan_speed` setting.

**Firmware constraint:** Sleep mode requires a compressor-using HVAC mode. Confirmed working in Cool and Heat. The Dometic firmware silently rejects `0x1B = 1` writes when the unit is in Fan Only — the BLE write completes successfully (ATT write ack returned), but the device reports `0x1B = 0` in the follow-up notification. Dry and Heat/Cool not yet tested but likely supported on the same logic (any mode that runs the compressor). This is a state-machine-level gate, not just a UI restriction.

If you specifically want Sleep behaviour in Fan Only mode (for off-grid quiet ventilation without battery-draining compressor cycles), a workaround is to set HVAC mode to Cool with target temperature 31°C and then engage Sleep. The compressor stays idle because the target is above measured temp, but you get the Sleep aesthetic — moon, dim, Low fan — that the firmware would otherwise reserve for compressor modes.

### Adaptive Power Mode (param 0x2D)

Limits how much current the unit draws from hookup or the inverter. Sniffed on an FJZ7 2200 by [@DRAKS1000](https://github.com/DRAKS1000) (PR #6).

| Value | Limit |
|-------|-------|
| 0 | 4 A |
| 1 | 5 A |
| 2 | 6 A |
| 3 | 7 A |
| 7 | Unlimited |

Values 4–6 are reserved and not exposed — possibly used on higher-current models. If your unit reports one, the log shows `Adaptive Power: received unknown raw value N`; please open an issue with the value and your model.

**Opt-in:** the component only subscribes to 0x2D when you configure the `select` platform.

**FJZ only.** Tested on an FJX7 (September 2026): the unit accepts the write (one beep, `Write OK` in the log) but never reports 0x2D back, and the current draw doesn't change. Leave the `select` out on FJX units.

### BLE Connection Requirements

The FJX7 **requires encrypted BLE** (Just Works bonding). Without bonding, all writes fail with ATT error `0x0F` (Insufficient Encryption). The ESP-IDF stack handles this by calling `esp_ble_set_encryption()` on connection, and bond keys persist in NVS across reboots.

## Troubleshooting

**Can't see the setup hotspot**
- Unplug the board for 5 seconds and plug it back in. If your phone has "forgotten" the network, iPhones sometimes hide it until the board restarts.
- The hotspot only exists before setup or after a factory reset. Once the board has joined your WiFi it never falls back to the hotspot, even if the router is off.

**The setup page doesn't pop up**
- Open a browser and go to `http://192.168.4.1` while joined to the hotspot.

**Pairing says "Not paired"**
- Hold + and − on the AC panel for 3 s until it shows `BL`. Pairing mode lasts about a minute.
- Close the Dometic app on every phone nearby: the AC only takes one connection.
- Board-in-a-box: if it still won't pair, press Forget AC twice on the page and try again.

**Connects then disconnects in a loop** (own config)
- Pairing mode not used: see [Pairing](#using-the-component-directly).
- Check the MAC address; check the ESP32 is within range (~10 m, less through metal).
- Power-cycle the AC at the isolator and try again.

**MQTT connection says "Not connected"**
- Check the IP in **MQTT broker IP**. On a Victron GX, MQTT must be on (Settings → Services → MQTT (plaintext)).

**Home Assistant offers "Reconfigure" instead of "Add"**
- HA remembers an older setup of a board with the same name (after a re-flash or a factory reset). Reconfiguring switches the connection to unencrypted. Delete the old device in HA first, then add the board fresh: HA then sets a new encryption key on it.
- A factory reset wipes the key HA set, so HA will need to add the board again afterwards.

**State doesn't update in HA**
- Check the logs (`esphome logs <your config>.yaml`) for `All parameters subscribed`.

## Contributing

PRs welcome. If you have another Dometic connected product (FJX5, FJX3, anything using DDM), your testing would help: the protocol layer is shared across the range. FJZ owners: a BLE scan of your unit's advert (nRF Connect, the manufacturer data under company 0x0845) would help the board tell FJX and FJZ apart.

## Changelog

### v0.5.0-rc.1 — board in a box (release candidate)
- **Board-in-a-box firmware** ([`examples/board-in-a-box.yaml`](examples/board-in-a-box.yaml)): flash once, then set everything up from a phone, with nothing about the van compiled in
- **Setup hotspot** with a per-board password derived from a secret key and the board's MAC (`tools/ap_card.py`). Once joined, the board never falls back to the hotspot, so a router switched off overnight doesn't strand it
- **Bluetooth waits for WiFi**: ESPHome's default full-time scanning starved the hotspot's WiFi handshake on the S3
- **Auto-pair**: pairs with whichever unit is put into pairing mode (the pairing flag is the first byte of the second manufacturer-data entry in the advert), saved to flash
- **Web page**: AC control with a fan-speed dropdown (ESPHome's page has none), Victron/MQTT settings, pairing and MQTT status, Forget AC, Factory reset, Restart
- **MQTT broker set on the page**, not compiled in. Blank = MQTT off
- **Factory reset** by 5 power cycles or holding BOOT for 10 s
- **Fix: MQTT spam** — the climate state is only published when something changed
- **Fix: Node-RED dropped and duplicated presses** — value-aware echo check, temperature slider debounce, follows only the online bridge
- **Safety:** the build refuses to run without a real `ap_key`; the WiFi password is never logged
- Tested on an FJX7 with an ESP32-S3 SuperMini, an iPhone and an Ekrano GX

### v0.4.0 — packages, MQTT and Victron
- **ESPHome packages**: `core`, `ha`, `mqtt`, `adaptive_power`; pick what you need
- **MQTT** for Node-RED, Signal K or any broker
- **Victron GX Node-RED flow**: the AC as virtual switches on the GX screen and VRM (power, mode, fan speed, target, FJZ power limit). Tested on an Ekrano with a real FJX7, cooling and heating
- CI builds the package configs too

### v0.3.0
- **FJZ support** — confirmed working on the FJZ7 2200 ([@DRAKS1000](https://github.com/DRAKS1000))
- **Adaptive Power Mode** — new optional `select` to cap the unit's current draw (4/5/6/7 A or unlimited). Protocol sniffed by [@DRAKS1000](https://github.com/DRAKS1000). Opt-in: FJX users who don't add the select see no change
- **ESPHome 2026.11 ready** — custom fan mode moved to the new API, with a fallback so older ESPHome still builds
- **Fixes** — sub-zero measured temperatures now read correctly; target temperature clamped to 16–31 °C; unknown AC modes logged instead of silently showing Cool
- **Quieter logs** — raw BLE hex dump moved to VERBOSE
- **CI** — every push and a weekly run compile an FJX config and an FJZ config against several ESPHome versions
- **Hardware-tested on an FJX7** — full control confirmed in the van; Adaptive Power confirmed FJZ-only

### v0.2.0
- Sleep preset mode

## Credits

Protocol reverse-engineered and ESPHome component built by [@prebsit](https://github.com/prebsit) from a motorhome in Austria, Germany, France and UK, whilst the van's authority slept, April 2026.

Built with [Claude](https://claude.ai) (Anthropic) as coding partner — Si provided the hardware, the van, and the button-pressing; Claude wrote the code.

### Contributors

- [@DRAKS1000](https://github.com/DRAKS1000) — FJZ7 2200 support, reverse-engineered Adaptive Power Mode (param 0x2D), ESPHome 2026.11 compatibility fix
- Nige — first community confirmation on the FJX4

## Licence

MIT
