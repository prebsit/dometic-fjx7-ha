# Node-RED flow: Dometic AC on a Victron GX

Puts the aircon on your GX device's screen (and in VRM) as a group of
virtual switches called **Aircon**:

| Control | Type | Does |
|---|---|---|
| AC active | Toggle | On (resumes the last mode, default Cool) / Off |
| AC mode | Dropdown | Cool / Heat / Auto / Fan / Dry |
| AC power limit | Dropdown | **FJZ only.** Adaptive Power: 4A / 5A / 6A / 7A / Unlimited (needs `packages/adaptive_power.yaml`) |
| AC sleep | Toggle | The AC's Sleep mode (moon icon, dimmed panel, quiet fan). Cool or Heat only, see below |
| AC speed | Dropdown | Auto / Low / Medium / High / Turbo |
| AC target | Temperature setpoint | 16–31 °C, with the measured cabin temperature shown alongside |
| AC unit light - exterior | Toggle | The AC's exterior light (harmless on units without one) |
| AC unit light - interior | Toggle | The AC's interior light |

The GX lists switches alphabetically by name, with no way to set the order,
so the names are chosen to sort with the main controls first and the lights last.

Changes made on the AC's own panel or remote show up on the GX too.

**Sleep needs a compressor mode.** The AC only accepts Sleep in Cool or Heat
(Dry and Auto untested). In Fan, or with the AC off, it silently ignores the
request, so the flow refuses it, warns on the *Sleep -> command* node and puts
the switch back. Same if the AC hasn't confirmed Sleep within 4 s.

There are two versions of the flow. Import the one for your unit:

| File | For | Differs by |
|---|---|---|
| `victron-ac-flow-fjx.json` | FJX (FJX7, FJX4, …) | No *AC power limit*: FJX units accept the setting but ignore it |
| `victron-ac-flow-fjz.json` | FJZ (FJZ7, …) | Includes *AC power limit* |

> **Status:** tested in a motorhome on an Ekrano GX (Venus OS Large) with a
> real FJX7: power, mode (cool and heat), speed (incl. Turbo) and target work in both
> directions, from the GX screen and from VRM. Written against
> node-red-contrib-victron 1.7.27. *AC sleep* confirmed on the same Ekrano:
> turns on in Cool, refused in Fan. If a Victron node looks wrong after import,
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
2. Menu → **Import** → **Clipboard** tab → paste `victron-ac-flow-fjx.json` or `victron-ac-flow-fjz.json` → **Import** → **Deploy**.
3. The **Route state** node shows the bridge's status (green = online).
4. The Aircon group appears on the GX display. Switches may read 0 until the
   bridge publishes its first state.

Updating the flow: delete the old **Dometic AC** tab and deploy before
importing the new one, or you'll end up with two sets of switches.

## How it works

```
AC ⇄ BLE ⇄ ESP32 bridge ⇄ MQTT (dometic/<name>/…) ⇄ Node-RED ⇄ Victron virtual switches ⇄ GX screen / VRM
```

The flow learns the bridge's topic prefix from its `status` topic and follows
whichever bridge last said `online`, so the bridge `name` doesn't need to match
anything, and a dead board's leftover messages can't take over. Commands are
only sent once a bridge has been seen online.

Every change on the GX produces a state update from the AC, which moves the
GX switch again. The flow ignores those echoes: a GX change is dropped only if
it matches a value the AC reported in the last 1.5 s, so a real press straight
after an update still goes through. The target temperature waits until the
slider has been still for 0.7 s and sends one command. Each command node shows
its last decision on the canvas (`sent …` / `echo ignored …`), which tells you
at a glance whether Node-RED sent a press.

**Only one system should run automations for the AC.** If Home Assistant is
also connected (`packages/ha.yaml`), let one of them own the logic and use the
other for display and manual control.
