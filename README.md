# Dometic FreshJet FJX7 — ESPHome Component for Home Assistant

The first working Home Assistant integration for the Dometic FreshJet FJX7 roof-mounted air conditioning unit. Full bidirectional control over BLE via an ESP32 bridge.

**No cloud. No Dometic app. No wiring.**

## What You Get

- **Climate entity** — Cool, Heat, Auto, Dry, Fan Only, Off
- **Fan speed** — Low, Medium, High, Turbo, Auto
- **Temperature** — target and measured, 16–31°C
- **Interior & exterior lights** — on/off control
- **Instant sync** — state changes from the ADBD panel appear in HA immediately via BLE notifications
- **Auto-reconnect** — ESP32 recovers from power cycles and disconnections automatically

## Why an ESP32?

The FJX7's Microchip BLE module has a firmware quirk: it requires encrypted BLE (Just Works bonding) and does not send ATT Write Responses to Linux's BlueZ stack. This breaks every Linux-based BLE implementation — the connection drops on every write attempt. We tested multiple adapters (Pi 3B+ onboard, TP-Link UB500) across multiple BlueZ versions. All fail identically.

Apple's CoreBluetooth handles this silently (macOS works fine with bleak/Python), and Espressif's ESP-IDF/NimBLE stack also handles it correctly. So an ESP32 running ESPHome acts as a BLE-to-WiFi bridge: it connects to the FJX7 over BLE and exposes entities to Home Assistant over WiFi.

If you happen to run Home Assistant on a Mac Mini, a Python/bleak custom component approach may work for you via CoreBluetooth — but the ESPHome component is the recommended and tested path.

## Hardware Required

- **ESP32-S3 development board** — we use the ESP32-S3-DevKitC-1 (N16R8). Any ESP32-S3 board with WiFi and BLE should work. The S3's dual-core handles BLE and WiFi concurrently without watchdog issues.
- **USB-C cable** for initial flashing
- **5V USB power source** in the van for permanent installation

No wiring to the FJX7 — communication is entirely wireless over BLE.

**Boards we've tested:**

| Board | Status | Notes |
|-------|--------|-------|
| ESP32-S3-DevKitC-1 (N16R8) | ✅ Confirmed | Recommended. Dual-core, plenty of RAM |
| ESP32-C3 SuperMini | Untested | Single-core — may struggle with BLE+WiFi |
| ESP32-C6 | Untested | BLE 5.3, dual-core — should work well |

## Installation

### 1. Install ESPHome

If you don't have ESPHome installed:

```bash
pip install esphome
```

### 2. Create your configuration

Create a file called `fjx7-bridge.yaml`:

```yaml
esphome:
  name: fjx7-bridge
  friendly_name: "Dometic FJX7 Bridge"

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

logger:
  level: DEBUG

api:
  encryption:
    key: !secret api_key

ota:
  - platform: esphome
    password: !secret ota_password

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

# Optional: creates a WiFi hotspot if the configured network isn't available
captive_portal:

esp32_ble_tracker:
  scan_parameters:
    active: true

ble_client:
  - mac_address: "XX:XX:XX:XX:XX:XX"  # Replace with your FJX7's MAC address
    id: fjx7_ble
    auto_connect: true

external_components:
  - source:
      type: git
      url: https://github.com/prebsit/dometic-fjx7-ha
      ref: main
    components: [dometic_fjx7]

dometic_fjx7:
  ble_client_id: fjx7_ble

climate:
  - platform: dometic_fjx7
    name: "FJX7 Air Conditioning"

light:
  - platform: dometic_fjx7
    name: "FJX7 Interior Light"
    light_type: interior
  - platform: dometic_fjx7
    name: "FJX7 Exterior Light"
    light_type: exterior

sensor:
  - platform: dometic_fjx7
    measured_temperature:
      name: "FJX7 Temperature"
    fan_speed_percent:
      name: "FJX7 Fan Speed"

button:
  - platform: restart
    name: "FJX7 Bridge Restart"
```

Create a `secrets.yaml` in the same directory:

```yaml
wifi_ssid: "Your WiFi Name"
wifi_password: "your-wifi-password"
api_key: "generate-a-random-key-here"
ota_password: "pick-a-password"
```

### 3. Find your FJX7's MAC address

Before you can connect, you need your FJX7's Bluetooth MAC address. The quickest way: flash the ESP32 with the config above (leaving the MAC as `XX:XX:XX:XX:XX:XX`). It won't connect, but the BLE scanner will log every device it finds:

```
Found device 02:00:00:12:34:56 RSSI=-85
  Name: 'SHE_123456'
```

Look for a device named `SHE_XXXXXX` — that's your FJX7. Copy the MAC address into your YAML and re-flash.

Alternatively, use any BLE scanner app on your phone (like nRF Connect) and look for `SHE_` devices.

### 4. Flash the ESP32

```bash
esphome run fjx7-bridge.yaml
```

Select the USB port when prompted. The ESP32 will compile, flash, connect to WiFi, bond with the FJX7, and start reporting state.

### 5. Add to Home Assistant

Home Assistant should auto-discover the new ESPHome device. If not, go to Settings → Devices & Services → Add Integration → ESPHome, and enter the ESP32's IP address.

**Important:** Close the Dometic Climate app first. The FJX7 only accepts one BLE connection at a time.

## How It Works

The ESP32 connects to the FJX7 using Dometic's DDM (Device Data Model) protocol over BLE GATT. It subscribes to all climate parameters and receives instant push notifications when anything changes — mode, fan speed, temperature, lights. Commands from Home Assistant are sent as DDM Set commands over the same BLE connection.

The bond keys are stored in the ESP32's flash (NVS), so it reconnects automatically after power cycles without needing to re-pair.

## Supported Devices

| Device | Protocol | Status |
|--------|----------|--------|
| FreshJet FJX7 | DDM over BLE | ✅ Fully working |
| FreshJet FJX5 | DDM over BLE | 🔮 Likely compatible (untested) |
| FreshJet FJX3 | DDM over BLE | 🔮 Likely compatible (untested) |

The DDM protocol is shared across Dometic's connected product range. If you have a different FJX model, please test and report back.

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

### BLE Connection Requirements

The FJX7 **requires encrypted BLE** (Just Works bonding). Without bonding, all writes fail with ATT error `0x0F` (Insufficient Encryption). The ESP-IDF stack handles this by calling `esp_ble_set_encryption()` on connection, and bond keys persist in NVS across reboots.

## For Dometic Engineers

If you're from Dometic and reading this — hello. Here's what we found while reverse-engineering the FJX7's BLE interface, in case it's useful for firmware improvements:

**1. ATT Write Response is missing on some stacks.** The FJX7's Microchip BLE module does not send ATT Write Response (opcode 0x13) when accessed from Linux's BlueZ stack, even after successful bonding and encryption. The write *does* take effect on the device, but the missing response causes BlueZ to consider the write failed and eventually drop the connection. This does not occur with Apple's CoreBluetooth or Espressif's ESP-IDF/NimBLE. This is likely the root cause of many negative reviews of the Dometic Climate Android app.

**2. Write Request required, Write Command ignored.** The FJX7 only processes ATT Write Request (opcode 0x12). ATT Write Command (opcode 0x52) is silently ignored. Both are valid per the BLE specification; the firmware only implements one.

**3. Single BLE connection limit.** The FJX7 only accepts one BLE client at a time. If the Dometic app is connected, no other client can connect, and vice versa. This is worth documenting for users.

**4. Fan speed value 4 is unused.** The fan speed parameter accepts values 0–3 and 5, but not 4. Sending 4 appears to default to Auto mode.

We'd be happy to test patched firmware if you'd like to address any of these. Open an issue on this repo.

## Troubleshooting

**ESP32 won't connect to FJX7**
- Close the Dometic Climate app — the FJX7 only accepts one connection
- Check the MAC address in your YAML matches your FJX7
- Ensure the ESP32 is within BLE range (~10m, less through walls/metal)

**Connection drops or watchdog resets**
- The ESP32-S3 occasionally hits watchdog timeouts when BLE and WiFi are competing for radio time. The device recovers automatically. If it happens frequently, ensure good WiFi signal strength to reduce radio contention.

**Bonding fails**
- Delete the ESP32's NVS (full flash erase: `esphome run --device /dev/ttyUSB0 fjx7-bridge.yaml` with `esp_idf` framework erases NVS by default on first flash)
- Power cycle the FJX7

**State doesn't update in HA**
- Check ESPHome logs: `esphome logs fjx7-bridge.yaml`
- Look for `All parameters subscribed` — if missing, the BLE connection isn't completing

## Contributing

PRs welcome. If you have a different Dometic connected product (FJX5, FJX3, or anything else using DDM), your testing would help expand support. The DDM protocol layer is shared across the range.

## Credits

Protocol reverse-engineered and ESPHome component built by [@prebsit](https://github.com/prebsit) from a motorhome in Austria, Germany, France and UK, whilst the van's authority slept, April 2026.

Built with [Claude](https://claude.ai) (Anthropic) as coding partner — Si provided the hardware, the van, and the button-pressing; Claude wrote the code.

## Licence

MIT
