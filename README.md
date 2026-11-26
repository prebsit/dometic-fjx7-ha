# Dometic FreshJet FJX / FJZ — ESPHome Bridge for Home Assistant, Victron and your phone

A working integration for Dometic FreshJet FJX and FJZ roof air conditioners. An ESP32 talks to the unit over Bluetooth and puts it on your van's WiFi: in **Home Assistant**, on a **Victron GX** screen (Cerbo, Ekrano) via Node-RED, or on its own **web page** on your phone. Confirmed on the FJX7, FJX4 and FJZ7.

**No cloud. No Dometic app. No wiring.**

> **Current release: v1.0.0-rc.1** (release candidate). It is going into its first customer vans now; v1.0.0 follows once it has proved itself there. Pin a release tag in your config (`ref: v1.0.0-rc.1`), not `main`, so an update never surprises you.

## What You Get

- **Climate control** — Cool, Heat, Auto, Dry, Fan Only, Off; target temperature 16–31 °C
- **Fan speed** — Low, Medium, High, Turbo, Auto
- **Interior & exterior lights** — on/off
- **Sleep mode** — the FJX7's Sleep preset (moon icon, dimmed display, quiet fan)
- **Power limit** *(FJZ only)* — cap the unit's current draw at 4/5/6/7 A or unlimited
- **Instant sync** — changes made on the AC's own panel or remote show up everywhere immediately
- **Auto-reconnect** — recovers from power cycles and the AC being switched off at the isolator
- **Four ways in** — Home Assistant (ESPHome API), MQTT (Victron GX via Node-RED, or any broker), the board's own web page, and an iPhone shortcut. Use any or all of them.
- **A page of its own** — control and setup from any phone on the van WiFi, with a password if you want one
- **A log that says who asked** — every command with where it came from (the page, the shortcut, MQTT, Home Assistant), and anything the aircon did that nobody here asked for. About a week is kept, through restarts and power cuts

**Don't fancy building your own?** The ready-made version is called **ice9**: no soldering, no flashing, just plug it in and set it up from your phone. See [ice9.cool](https://ice9.cool).

## Pick your route

| You have… | Go to |
|---|---|
| An ice9, or a board you've flashed with the box firmware | [Setting up an ice9](#setting-up-an-ice9) |
| An ESP32-S3 and want to build your own | [Building your own](#building-your-own) |
| Your own ESPHome config and just want the component | [Using the component directly](#using-the-component-directly) |

## Setting up an ice9

About ten minutes, all from your phone. You need three things about the box: its **serial** (six characters, e.g. `abcdef`), its **setup password**, and ideally the WiFi QR code that carries both.

- **Bought an ice9?** They are on the setup sheet that came with it.
- **Built your own?** These steps are for a board built from [`examples/ice9-page.yaml`](examples/ice9-page.yaml), which is the same firmware. No sheet comes with it: `tools/ap_card.py` prints the three details for your board, from your own key (see [Building your own](#building-your-own)). A board built from `board-in-a-box.yaml` sets up the same way with ESPHome's plainer screens.

1. **Plug it in** near the AC (any 5 V USB supply).
2. **Join its setup hotspot.** Scan the QR code on the sheet, or join the WiFi network `ice9-<serial>` with the setup password.
3. **Give it your van's WiFi.** Your phone opens the ice9's setup screen by itself (if it doesn't, open a browser and go to `http://192.168.4.1`). Pick your van's WiFi from the list, type its password and tap **Join**. Your phone drops off the hotspot while the ice9 tries your WiFi. When it has joined, usually within a minute, the hotspot disappears for good. If `ice9-<serial>` is still in your phone's WiFi list after two minutes, the password was wrong: join the hotspot again and have another go.
   Use **your van's own WiFi**, never a campsite's: anyone on the same WiFi can reach the ice9. A [page password](#page-password) keeps the rest of your own network out.
4. **Open its page:** `http://ice9-<serial>.local` on any phone or laptop on the van WiFi. Tip: add it to your home screen.
   If it won't open, try the same address without `.local`, or find the ice9's IP address in your router's list of devices and use that. See [Troubleshooting](#troubleshooting).
5. **Pair it with the AC.** On the AC's control panel, hold **+ and −** together for 3 seconds until the display shows `BL`. The ice9 finds the unit and pairs with it by itself, usually within a few seconds. In **Settings**, **Aircon pairing** should say *Connected to SHE_xxxxxx*.
   **FJX4:** the FJX4 doesn't announce pairing mode, so the ice9 can't spot it by itself. In **Settings**, find your unit under **Aircons nearby** (`SHE_xxxxxx`), type that name into **Aircon name to pair with** and tap **Pair**, then hold + and − on the panel. The ice9 pairs with that unit only, never a neighbour's. This works on any model.

Then, depending on what else you run:

- **Victron GX (Cerbo, Ekrano…):** in **Settings**, under **Victron GX or Node-RED**, type the GX's address (`venus.local` or its IP address) and tap **Save**. The ice9 restarts (about 20 s) and **Connection** changes to *Connected*. What else to fill in depends on the GX's local network security profile (Venus OS 3.x, under Access control in its settings):
  - **Unsecured:** leave the user name and password empty.
  - **Weak:** any user name, and the GX's own password.
  - **Secured:** not supported. On *Secured* the GX only takes encrypted MQTT, which the ice9 doesn't speak.

  Then set up the GX side: see [Victron GX](#victron-gx-via-node-red).
- **Home Assistant:** it should find the ice9 by itself (Settings → Devices & Services). Adopt it and HA sets its own encryption key.
- **Neither:** the web page *is* your remote.
- **iPhone:** add the **Aircon** shortcut for a one-tap remote (and Siri). See [iPhone shortcut](#iphone-shortcut).

### Page password

Off until you set one: **Settings**, **Page password**, 4 to 64 characters. From then on the page asks for it, on any phone, and so does everything else the ice9's web server offers, the iPhone shortcut included. The login's user name can be anything: only the password counts.

- A new password is **on trial** until someone logs in with it. If nobody does within 10 minutes, the ice9 goes back to how it was, so a typo can't lock you out.
- Changing or removing it needs the current one typed in, even on a phone that is already logged in.
- Home Assistant and the Victron screen don't use the page, so they carry on as they are.
- **Forgotten it?** [Start again](#starting-again-factory-reset): that takes your hands on the box, and wipes the password with everything else.

It is a lock for the door, not a safe: the page is plain HTTP, so the password keeps out everyone else on the WiFi, but not someone recording the WiFi's traffic.

### Updating

In **Settings**, under **Update**, choose the firmware file (its name ends in `.ota.bin`) and tap **Send**. Your WiFi, pairing, Victron settings, page password and log all carry over.

New firmware takes either the page password or your hands on the box:

- **With a page password set:** any time, from someone who has given it.
- **With none set:** only in the 10 minutes after the ice9 is plugged in. Unplug it, plug it back in, and send the file. A restart doesn't count.

If you built your own, the file is `.esphome/build/<name>/build/firmware.ota.bin`, beside your config. **Not `firmware.factory.bin`:** that one is for flashing a blank board over USB. Sent as an update it is answered "Update Successful!", and the ice9 restarts on the firmware it already had. The log's first line then says the update didn't take.

### The log

**Settings**, **Log**. Each line says what the aircon was asked to do and who asked:

```
14:02:13 Cool, 21°, fan Auto. Asked by: the page
22:15:02 Inside light off. Asked by: Home Assistant
07:10:44 The aircon turned on: Cool, 21°, fan Auto. Not asked by the ice9: its own panel, remote or app
```

- **Who asked** is one of: the page, the Shortcut, MQTT (the GX or Node-RED), Home Assistant, another app on the Wi-Fi, or the ice9 itself.
- **Not asked by the ice9** means the aircon reported a change that nothing above asked for: someone used its own panel, its remote or Dometic's app.
- It also records how the ice9 started (power on, a restart, a crash, a dip in its supply), new firmware, and the WiFi, the Victron connection and the Bluetooth link to the aircon coming and going.
- **Show the last week** brings back about a week, kept through restarts and power cuts. **Copy** and **Share** are for sending it to whoever is helping you.
- The times come from the internet over the van's WiFi. With no internet since it was plugged in, lines show the time since the ice9 started instead.
- No password is ever written to it. Starting again wipes it.

The same text is at `http://ice9-<serial>.local/ice9/log` (since the last start) and `/ice9/log/all` (the week).

### iPhone shortcut

One button that shows what the AC is doing ("AC COOL · set 21° · cabin 24° · fan AUTO") and lets you change mode, temperature, fan speed, sleep and the lights. After each change it reads the AC back and tells you what it actually did. Works with Siri: "Hey Siri, Aircon".

It talks straight to the ice9 over the van WiFi: no GX, no app, no cloud. That also means it only works when your phone is on the van WiFi. For control from anywhere you need a GX (Cerbo, Ekrano) and VRM; see [Victron GX](#victron-gx-via-node-red).

1. Open **[Aircon shortcut](https://www.icloud.com/shortcuts/c77180f334f9483399266d58d9ba7235)** on your iPhone and tap **Add Shortcut**. It asks for the ice9's page password: leave it blank if you haven't set one.
2. In the Shortcuts app, open **Aircon** for editing (tap ⋯ on it). Under the note that says **ADDRESS**, the box says `ice9-xxxxxx.local`. Change it to your ice9's address from step 4 above, e.g. `ice9-abcdef.local`, or its IP address.
3. Tap it to run. That's it.

Set a page password later? Type it into the box under the **PASSWORD** note. If the aircon isn't connected to the ice9, or the ice9 wants a password the shortcut doesn't have, the shortcut says so and stops.

The shortcut is built by [`shortcuts/build_aircon.py`](shortcuts/build_aircon.py), so you can read exactly what it sends. To build your own copy: `python3 shortcuts/build_aircon.py --host ice9-<serial>.local`, then sign it on a Mac with `shortcuts sign -m anyone -i Aircon.shortcut -o signed/Aircon.shortcut` (iPhones only import signed shortcuts).

Questions, problems or your own tweaks: see the [shortcut's discussion thread](https://github.com/prebsit/dometic-fjx7-ha/discussions/14).

### Starting again (factory reset)

Any of these wipes the WiFi, the AC pairing, the Victron settings, the page password and the log, and brings the setup hotspot back:

- **Unplug and plug back in 5 times**, unplugging within 10 s each time. The 5th time, leave it plugged in.
- Hold the board's **BOOT** button for 10 s while it's running (not while plugging it in).
- On the page, in **Settings**, tap **Start again from scratch** twice.

**Pairing with a different AC:** in **Settings**, tap **Forget this aircon** twice, then hold + and − on the new unit (FJX4: type its name into **Aircon name to pair with**).

## Building your own

The firmware is split into packages, so you only build what you use. Every example pins a release tag; bump `ref:` and `fjx7_ref:` together when you update.

> **Don't know your AC's Bluetooth MAC, or only have an iPhone?** Build one of the two box examples and pair by name: no MAC needed. The other two examples need the AC's MAC; see [Finding the AC's address](#finding-the-acs-address).

| Example | What it builds |
|---|---|
| [`examples/home-assistant.yaml`](examples/home-assistant.yaml) | Home Assistant only. WiFi and the AC's Bluetooth MAC set in your config |
| [`examples/victron-node-red.yaml`](examples/victron-node-red.yaml) | MQTT for Victron / Node-RED (add `ha.yaml` for both). AC's Bluetooth MAC set in your config |
| [`examples/ice9-page.yaml`](examples/ice9-page.yaml) | The box firmware as an ice9 runs it: nothing about the van compiled in, set up from a phone (setup hotspot, auto-pair, the ice9 page, page password, updates, the log, HA, MQTT, factory reset). Builds from a clone of this repository, because the page's two files are read from disk |
| [`examples/board-in-a-box.yaml`](examples/board-in-a-box.yaml) | The same firmware with ESPHome's own plain page in place of the ice9 page. Builds straight from GitHub, no clone needed |

| Package | Does |
|---|---|
| `core.yaml` | Required. The component, Bluetooth, and the climate/light/sensor entities |
| `ha.yaml` | Home Assistant (encrypted ESPHome API) |
| `mqtt.yaml` | MQTT, for Node-RED on a Victron GX or any broker |
| `adaptive_power.yaml` | The FJZ's power limit (the Adaptive Power select). An FJX never reports one, and the ice9 page only shows the control once the aircon has |
| `provisioning.yaml` | Setup hotspot + captive portal instead of compiled-in WiFi. Needs `ap_key` |
| `autopair.yaml` | Pairs with whichever unit is put into pairing mode, or the one whose name is typed on the web page, instead of a compiled-in MAC |
| `webpage.yaml` | The web server and what sits behind the page: the MQTT settings, the page password, firmware updates, the kept log and its clock |
| `reset.yaml` | Factory reset by 5 power cycles or holding BOOT |

**Building the box firmware:** both box examples need two secrets, `encryption_key` (OTA) and `ap_key` (the hotspot master key; `openssl rand -hex 16`). The build stops with an error if `ap_key` is missing. After flashing over USB, print the board's hotspot details from the MAC address esptool shows:

```bash
AP_KEY=<your ap_key> python3 tools/ap_card.py <board MAC>
```

It prints the SSID, the password and the `WIFI:` string for a QR code. **Never publish firmware built with your `ap_key`**: anyone holding it and a board's MAC can work out that board's hotspot password.

**With ESPHome's own page** (`board-in-a-box.yaml`) everything works the same underneath, but the settings appear as a list of entities under their own names, the setup hotspot shows ESPHome's setup page, and there is no screen for the page password (it can still be set: see the top of [`components/ice9_lock/__init__.py`](components/ice9_lock/__init__.py)). The log is at `/ice9/log`.

### Finding the AC's address

`home-assistant.yaml`, `victron-node-red.yaml` and your own configs need the AC's Bluetooth MAC in `ac_mac`, written `AA:BB:CC:DD:EE:FF`. **The `SHE_xxxxxx` name is not the address.** Put the name in and ESPHome stops with `MAC Address must consist of 6 : (colon) separated parts`.

- **From the ESP32:** flash with any address first (e.g. `00:00:00:00:00:00`) and watch the log for `Name: 'SHE_…'` and the address logged with it.
- **Android:** a BLE scanner (nRF Connect) shows the address next to the `SHE_` name.
- **iPhone:** no app can show it. iOS hides Bluetooth addresses and gives apps a random ID instead. Use the ESP32's log, or build a box example and pair by name.

The name is the last three bytes of the unit's base MAC, and the Bluetooth address is the base MAC plus 2: `SHE_a1b2c3` is `…:A1:B2:C5`. Handy for checking you've got the right unit.

### Hardware

- **ESP32-S3 board.** The S3 handles Bluetooth and WiFi together without trouble.
- USB-C cable for the first flash; a 5 V USB supply in the van.

| Board | Status | Notes |
|-------|--------|-------|
| ESP32-S3 SuperMini | ✅ Confirmed | Small, cheap, rock solid. What an ice9 is built on. BOOT is the right-hand button with USB-C at the bottom |
| ESP32-S3-DevKitC-1 (N16R8) | ✅ Confirmed | Dual-core, plenty of RAM |
| ESP32-C3 SuperMini | Untested | Single-core — may struggle with BLE+WiFi |
| ESP32-C6 | Untested | BLE 5.3 — should work |

**ESPHome:** 2026.11.0 or newer for the box firmware (the log uses what that release added); 2026.9.0 or newer for the other packages (OTA encryption). On every push and weekly, CI builds the component against several ESPHome versions and every package config against the current and development ESPHome.

### Victron GX via Node-RED

[`examples/node-red/`](examples/node-red/) has a flow (one for FJX, one for FJZ) that puts the AC on the GX screen and in VRM as a group of virtual switches (power, mode, fan speed, target temperature, and the FJZ power limit), in both directions. It needs **Venus OS Large** with Node-RED switched on and the GX's MQTT enabled. The [Node-RED README](examples/node-red/README.md) covers setup and import.

## Why an ESP32?

The FJX7's Microchip BLE module has a firmware quirk: it requires encrypted BLE (Just Works bonding) and does not send ATT Write Responses to Linux's BlueZ stack. This breaks every Linux-based BLE implementation — the connection drops on every write attempt. We tested multiple adapters (Pi 3B+ onboard, TP-Link UB500) across multiple BlueZ versions. All fail identically.

Apple's CoreBluetooth handles this silently (macOS works fine with bleak/Python), and Espressif's ESP-IDF/NimBLE stack also handles it correctly. So an ESP32 running ESPHome acts as a BLE-to-WiFi bridge: it connects to the FJX7 over BLE and exposes entities to Home Assistant over WiFi.

If you happen to run Home Assistant on a Mac Mini, a Python/bleak custom component approach may work for you via CoreBluetooth — but the ESPHome component is the recommended and tested path.

## Using the component directly

If you'd rather write your own config, this is the minimal version (WiFi and the AC's address compiled in). Put your secrets in `secrets.yaml` (`wifi_ssid`, `wifi_password`, `api_key`, `ota_key`; generate the keys with `openssl rand -base64 32`).

```yaml
esphome:
  name: dometic-ac
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
  - mac_address: "XX:XX:XX:XX:XX:XX"  # AC's Bluetooth MAC, not the SHE_ name: see below
    id: fjx7_ble

external_components:
  - source:
      type: git
      url: https://github.com/prebsit/dometic-fjx7-ha
      ref: v1.0.0-rc.1  # pin a release tag so updates don't surprise you
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

**Finding the AC's address.** `mac_address` is the AC's Bluetooth MAC, not its `SHE_xxxxxx` name: see [Finding the AC's address](#finding-the-acs-address).

**Pairing (first time only).** The AC only accepts a new bond in pairing mode: hold **+ and −** on the panel for 3 s until it shows `BL`, then restart the ESP32. The log should show `Connected — requesting encryption` then `All parameters subscribed`. The bond survives power cycles and updates. If you skip this, the log loops `Connected` / `Disconnected`.

**Close the Dometic app first**: the AC only accepts one Bluetooth connection at a time.

## How It Works

The ESP32 connects to the AC using Dometic's DDM (Device Data Model) protocol over BLE GATT. It subscribes to the climate parameters and gets push notifications whenever anything changes. Commands go back as DDM Set commands over the same connection. Bond keys live in the ESP32's flash, so it reconnects by itself.

## Supported Devices

| Device | Protocol | Status |
|--------|----------|--------|
| FreshJet FJX7 | DDM over BLE | ✅ Fully working (Adaptive Power not supported) |
| FreshJet FJX4 | DDM over BLE | ✅ Confirmed working (community-tested by Nige; box firmware field-tested, pairs by name) |
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

**`MAC Address must consist of 6 : (colon) separated parts`**
- `ac_mac` has the AC's name (`SHE_xxxxxx`) in it. It needs the Bluetooth MAC: see [Finding the AC's address](#finding-the-acs-address). Or build a box example and pair by name.

**Can't see the setup hotspot**
- Unplug the ice9 for 5 seconds and plug it back in. If your phone has "forgotten" the network, iPhones sometimes hide it until the ice9 restarts.
- The hotspot only exists before setup or after a factory reset. Once the ice9 has joined your WiFi it never falls back to the hotspot, even if the router is off.

**The setup screen doesn't pop up**
- Open a browser and go to `http://192.168.4.1` while joined to the hotspot.

**The page won't open at `http://ice9-<serial>.local`**
- Check the phone is on the van's WiFi, not mobile data or the setup hotspot.
- Try the same address without `.local` (`http://ice9-<serial>`): many routers answer to that name themselves.
- Or use the ice9's IP address, from your router's list of devices. Android phones don't always understand `.local` addresses.
- **iPhone, Safari says the connection is not secure or shows an "HTTPS-Only" error:** the ice9's page is plain `http://`, which is normal for a device on your own network. Tap through the warning if it offers to, or try the two addresses above. If Safari still refuses, the switch is under Settings, Apps, Safari, **Not Secure Connection Warning**.

**Forgotten the page password**
- It can only be cleared with your hands on the box: [start again](#starting-again-factory-reset), then set it up as new.

**The update was refused ("No permission")**
- The ice9 has no page password, and it wasn't plugged in within the last 10 minutes. Unplug it, plug it back in and send the file again; or set a page password, and it takes updates at any time.

**The update said "Successful" but nothing changed**
- The file was probably `firmware.factory.bin`. Send the one ending `.ota.bin`. The first line of the [log](#the-log) says when an update didn't take.

**The aircon came on, or changed, and nobody touched it**
- Look in the [log](#the-log). A line ending *Asked by: …* names what asked the ice9 to do it. A line saying *Not asked by the ice9* means it came from the aircon's own panel, its remote or Dometic's app.

**Pairing says "Not paired"**
- Hold + and − on the AC panel for 3 s until it shows `BL`. Pairing mode lasts about a minute.
- Close the Dometic app on every phone nearby: the AC only takes one connection.
- FJX4: holding + and − isn't enough on its own, because the FJX4 doesn't show pairing mode in its Bluetooth advert. Type its name into **Aircon name to pair with** (step 5 of [Setting up](#setting-up-an-ice9)).
- **Aircons nearby** says *None heard*: the ice9 can't hear the AC. Move it closer to the roof unit, and check no phone has the Dometic app open.
- If it still won't pair, tap **Forget this aircon** twice in Settings and try again.

**Connects then disconnects in a loop** (own config)
- Pairing mode not used: see [Pairing](#using-the-component-directly).
- Check the MAC address; check the ESP32 is within range (~10 m, less through metal).
- Power-cycle the AC at the isolator and try again.

**The Victron connection says "Not connected"**
- Check the address under **Victron GX or Node-RED**. On a Victron GX, MQTT must be on (Settings → Services → MQTT (plaintext)).
- Venus OS 3.x, by the GX's security profile: on **Unsecured**, leave the user name and password empty; on **Weak**, give any user name and the GX's own password; **Secured** is not supported.
- The [log](#the-log) helps: a line saying *the broker refused the ice9* means the GX answered and turned the login down (wrong or missing, on *Weak*). No such line, and still not connected, means nothing answered at that address: a wrong address, MQTT switched off, or *Secured*.

**Home Assistant offers "Reconfigure" instead of "Add"**
- HA remembers an older setup of a board with the same name (after a re-flash or a factory reset). Reconfiguring switches the connection to unencrypted. Delete the old device in HA first, then add the board fresh: HA then sets a new encryption key on it.
- A factory reset wipes the key HA set, so HA will need to add the board again afterwards.

**The iPhone shortcut can't reach the board**
- The phone must be on the van WiFi: the shortcut talks straight to the board, never over the internet.
- Check the box under **ADDRESS** in the shortcut matches the ice9's address on its setup sheet.
- "This ice9 wants its page password": type it into the box under **PASSWORD** in the shortcut.
- If `.local` addresses don't work on your phone, use the ice9's IP address instead (your router's list of devices shows it).

**The shortcut says it worked but the AC didn't change**
- The AC needs mains power: on hookup, or with the inverter on.
- The [log](#the-log) shows whether the command arrived (*Asked by: the Shortcut*).
- Sleep only works in Cool or Heat; in other modes the AC quietly ignores it.

**State doesn't update in HA**
- Check the logs (`esphome logs <your config>.yaml`) for `All parameters subscribed`.

## Contributing

PRs welcome. If you have another Dometic connected product (FJX5, FJX3, anything using DDM), your testing would help: the protocol layer is shared across the range. FJZ owners: a BLE scan of your unit's advert (nRF Connect, the manufacturer data under company 0x0845) would help the board tell FJX and FJZ apart.

## Changelog

### v1.0.0-rc.1 — ice9 (release candidate)
- **The ice9 page** in place of ESPHome's stock interface on the box firmware: the controls first, settings behind them, in plain words ([`page/`](page/), built by [`examples/ice9-page.yaml`](examples/ice9-page.yaml))
- **New boxes are `ice9-<serial>`**: setup hotspot, `.local` address and the name the router shows. Home Assistant calls the device *Dometic Aircon*. The serial is the last six characters of the board's MAC, and is shown in Settings with the firmware version
- **The ice9's own setup screen** on the setup hotspot: pick the van's WiFi from a list
- **Page password** ([`components/ice9_lock`](components/ice9_lock)): off until set, any user name, on trial until someone logs in with it, cleared by a factory reset
- **Firmware updates from the page**, behind the page password, or for 10 minutes after plug-in when none is set. **Update any box on earlier firmware**
- **A kept log** ([`components/ice9_log`](components/ice9_log)): what was asked of the aircon and who asked (the page, the shortcut, MQTT, Home Assistant), what the aircon did that nobody here asked for, and the WiFi, MQTT and Bluetooth links coming and going. About a week is kept in flash
- **One firmware for FJX and FJZ**: every box carries the FJZ's power limit, and the page shows it only once the aircon reports one
- **Victron GX on the *Weak* security profile** now works: any user name plus the GX's password. A box with a login uses a client ID of its own, because the GX refuses a known ID that once connected without one. *Secured* is still not supported
- **Saved MQTT password** is never handed back by the web API; the page shows whether one is saved
- **iPhone shortcut**: sends the page password when one is set, says so plainly when the aircon isn't connected, labels its two boxes ADDRESS and PASSWORD, and tells the ice9 who is asking. [New link](https://www.icloud.com/shortcuts/c77180f334f9483399266d58d9ba7235)
- **Needs ESPHome 2026.11.0** or newer for the box firmware
- **Docs:** `ac_mac` is the AC's Bluetooth MAC, not its `SHE_` name. Example and `core.yaml` comments fixed, [Finding the AC's address](#finding-the-acs-address) moved into Building your own (with the iPhone catch), pair-by-name pointed out up front, and a troubleshooting entry for the `MAC Address must consist of 6 : (colon) separated parts` error ([#16](https://github.com/prebsit/dometic-fjx7-ha/issues/16))

### v0.5.0-rc.2 — FJX4 pairing
- **Pair by name** for units whose advert doesn't show pairing mode. The FJX4 sends one 0x0845 entry (`04` + base MAC) and it doesn't change when + and − are held, so rc.1's auto-pair never fired on it. The web page now lists the units the board can hear under **AC units nearby**; type one into **AC name to pair with** and the board pairs with that unit only. Works on any model; FJX7/FJZ still pair with + and − as before
- **Web page:** pairing status, nearby units and the name box grouped under **AC pairing**
- **Docs:** Venus OS 3.x needs the GX's security profile on *Unsecured* for the board's MQTT
- Field-tested on an FJX4: pairing by name, the web page, the iPhone shortcut, and MQTT to a Cerbo GX (Venus OS 3.x)
- **iPhone shortcut** ([`shortcuts/build_aircon.py`](shortcuts/build_aircon.py), [iCloud link](https://www.icloud.com/shortcuts/bc17475521644550973b51cc794ed488)): one-button control of the AC and lights from an iPhone on the van WiFi, with Siri. No firmware change: works with any board on v0.5.0-rc.1 or later

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
