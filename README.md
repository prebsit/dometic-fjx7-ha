# Dometic FreshJet FJX7 — Home Assistant Integration

[![HACS Custom](https://img.shields.io/badge/HACS-Custom-41BDF5.svg)](https://github.com/hacs/integration)

> **⚠️ STATUS: Work in Progress — Not yet functional on Linux/BlueZ**
>
> The DDM protocol is fully decoded and bidirectional control is **proven working** from macOS (CoreBluetooth + bleak). However, the FJX7's Microchip BLE module has a firmware bug where it does not send ATT Write Responses to Linux's BlueZ stack, causing the connection to drop on every write attempt. This affects **all** Linux BLE adapters (tested: Pi 3B+ bcm43438, TP-Link UB500 RTL8761B). macOS works perfectly.
>
> **Next step:** Testing with ESPHome Bluetooth Proxy (ESP32, uses ESP-IDF stack instead of BlueZ). If you have an FJX7 and want to help test, open an issue!
>
> **If you're a Dometic engineer reading this:** your BLE firmware doesn't send ATT Write Response (opcode 0x13) for writes to characteristic `537a0401`. This breaks every BLE stack except Apple's CoreBluetooth, which silently handles the missing response. This is almost certainly why your Android app has terrible reviews.

Control your Dometic FreshJet FJX7 roof-mounted aircon from Home Assistant over Bluetooth Low Energy.

**No cloud. No Dometic app.**

**Currently requires:** ESPHome Bluetooth Proxy (ESP32) or macOS host. Linux/BlueZ is not compatible due to a Dometic firmware bug.

## Features

- **Climate entity** — full HVAC control: Cool, Heat, Auto, Dry, Fan Only, Off
- **Fan speed** — Auto, Low, Medium, High, Turbo
- **Target & measured temperature** — real-time readback from the unit
- **Interior & exterior lights** — on/off control
- **Auto-discovery** — HA detects `SHE_*` devices automatically via BLE
- **Push updates** — state changes arrive instantly via BLE notifications (no polling)
- **Reconnection** — automatic reconnect with exponential backoff

## Supported Devices

| Device | Firmware | Protocol | HA Integration |
|--------|----------|----------|----------------|
| FreshJet FJX7 | SHE_2.0.1 | ✅ Fully decoded | ⚠️ Blocked by BlueZ (ESP32 proxy untested) |
| FreshJet FJX5 | Unknown | 🔮 Likely same DDM | Untested |
| FreshJet FJX3 | Unknown | 🔮 Likely same DDM | Untested |

The DDM protocol is shared across the Dometic connected product range. If you have a different FJX model, please test and report back!

## Installation

### HACS (recommended)

1. Open HACS → Integrations → ⋮ → Custom repositories
2. Add `https://github.com/prebsit/dometic-fjx7-ha` as **Integration**
3. Install **Dometic FreshJet FJX7**
4. Restart Home Assistant

### Manual

Copy `custom_components/dometic_fjx7/` into your HA `custom_components/` directory.

## Setup

1. **Important:** Close the Dometic Climate app (the FJX7 only accepts one BLE connection at a time)
2. Go to Settings → Devices & Services
3. The FJX7 should appear automatically via Bluetooth discovery
4. Click **Configure** and confirm

If auto-discovery doesn't find your device, add it manually with the Bluetooth address.

## ESPHome Bluetooth Proxy (optional, for range)

If your HA host is too far from the FJX7, flash an ESP32 with standard ESPHome Bluetooth Proxy firmware. The integration automatically routes through any available proxy — no code changes needed.

```yaml
# esphome/bluetooth-proxy.yaml
esphome:
  name: ble-proxy-van

esp32:
  board: esp32-s3-devkitc-1

bluetooth_proxy:
  active: true
```

## Protocol

The FJX7 uses Dometic's DDM (Device Data Model) protocol over BLE GATT. The protocol was fully reverse-engineered from BLE traffic captures — see [protocol documentation](docs/protocol.md) for the complete decode.

Key facts:
- No encryption, no bonding, no authentication
- Single BLE service: `537a0400-0995-481f-926c-1604e23fd515`
- Write characteristic (host → device): `537a0401-...`
- Notify characteristic (device → host): `537a0402-...`
- Temperature encoded as millidegrees Celsius (LE uint32)

## Known Limitations

- **Single connection:** The FJX7 only accepts one BLE connection. The Dometic Climate app must be closed.
- **Dome control:** Not yet mapped over BLE (may not be exposed)
- **Wind direction:** Not yet mapped
- **Rain sensor:** Read-only, not yet decoded

## Contributing

PRs welcome! If you have a different Dometic connected product (Sharc, ShapeX, Inventilate, CFX), the DDM protocol layer is shared — your captures would help expand support.

## Credits

Protocol reverse-engineered by [@prebsit](https://github.com/prebsit) from a motorhome somewhere in Europe, April 2026.

## Licence

MIT
