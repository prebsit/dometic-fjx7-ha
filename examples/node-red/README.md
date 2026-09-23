# Node-RED flow: Dometic AC on a Victron GX

Puts the aircon on your GX device's screen (and in VRM) as a group of
virtual switches called **Aircon**:

| Control | Type | Does |
|---|---|---|
| AC power | Toggle | On (resumes the last mode, default Cool) / Off |
| AC mode | Dropdown | Cool / Heat / Auto / Fan / Dry |
| AC target | Temperature setpoint | 16–31 °C, with the measured cabin temperature shown alongside |
| AC power limit | Dropdown | Adaptive Power: 4A / 5A / 6A / 7A / Unlimited (needs `packages/adaptive_power.yaml`) |

Changes made on the AC's own panel or remote show up on the GX too.

> **Status:** tested on an Ekrano GX (Venus OS Large) with a simulated bridge:
> all four controls register, show state, and send commands in both
> directions. Not yet tested with a real bridge + AC. Written against
> node-red-contrib-victron 1.7.27. If a Victron node looks wrong after import,
> open it, re-select the switch type and deploy.

## Requirements

- GX device on **Venus OS Large** with Node-RED enabled
  (Settings → Integrations → Node-RED; the menu location varies by firmware).
- The bridge built with `packages/mqtt.yaml` (see `examples/victron-node-red.yaml`),
  `mqtt_broker` set to the GX's IP address.
- An MQTT broker the bridge and Node-RED can both reach. See below.

## Which broker?

**Option A: the GX's own broker (try this first).** Venus OS runs an MQTT
broker on port 1883 when **Settings → Services → MQTT (plaintext)** is on.
The flow is set up for this (`127.0.0.1:1883`, since Node-RED runs on the GX).
Confirmed on an Ekrano GX: Venus's broker carries the bridge's `dometic/...`
topics fine. If on your GX the flow's "Bridge state" node shows *connected*
but nothing arrives, go to option B.

**Option B: a broker inside Node-RED.** Install `node-red-contrib-aedes` from
the palette (needs internet), drop an *Aedes MQTT broker* node on port **1884**,
change the flow's broker config to port 1884, and rebuild the bridge with
`mqtt_port: "1884"`.

## Import

1. Open Node-RED on the GX (via VRM → Venus OS Large, or `https://<gx-ip>:1881`).
2. Menu → **Import** → paste `victron-ac-flow.json` → **Import** → **Deploy**.
3. The **Route state** node shows the bridge's status (green = online).
4. The Aircon group appears on the GX display. Switches may read 0 until the
   bridge publishes its first state.

## How it works

```
AC ⇄ BLE ⇄ ESP32 bridge ⇄ MQTT (dometic/<name>/…) ⇄ Node-RED ⇄ Victron virtual switches ⇄ GX screen / VRM
```

The flow learns the bridge's topic prefix from its messages, so the bridge
`name` doesn't need to match anything. Commands are only sent after the bridge
has been seen at least once.

**Only one system should run automations for the AC.** If Home Assistant is
also connected (`packages/ha.yaml`), let one of them own the logic and use the
other for display and manual control.
