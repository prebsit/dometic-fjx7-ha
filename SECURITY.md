# Security policy

## Reporting a vulnerability

Please **don't open a public issue** for security problems. Use GitHub's private reporting instead: the **Security** tab of this repo, then **Report a vulnerability**. Only the maintainer sees it until a fix is out.

This is a one-person hobby project, so replies are best-effort, but security reports get looked at first.

## Supported versions

The latest release and the latest release candidate. Older versions get no fixes; update instead.

## By design (not vulnerabilities)

- **The board's web page has no login, and neither does its firmware upload box.** The board is built to live on a private van network only. Never put it on a public or campsite WiFi.
- **The setup hotspot** only exists before first setup or after a factory reset. Its password is unique per board, derived from a key held by whoever built the firmware and the board's MAC address.
- **Pairing with an AC** needs physical access: someone has to hold + and − on the AC's own panel.
- **Home Assistant's connection** is encrypted with a key that HA sets on the board when it's added.

## Out of scope

Problems in the AC's own firmware or Bluetooth stack belong with Dometic, and problems in ESPHome itself with ESPHome. If you're unsure, report here and I'll pass it on.
