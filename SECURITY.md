# Security

## Reporting a problem

Use GitHub's private vulnerability reporting: the **Security** tab, then **Report a vulnerability**. Please don't
open a public issue for it.

Include:

- the firmware version or commit, and the board profile
- what an attacker can do, and from where (the Wi-Fi network, the setup access point, HBlink4, MQTT, over the air)
- steps to reproduce

We'll reply within a week. Fixes go into `main` and the next release.

## What to expect from the firmware

It's built for a trusted network. Know these before you deploy it:

- **The settings page and JSON API have no login.** Anyone who can reach the board can change its settings,
  upload firmware (OTA) and read its core dumps. Keep it on a network you control.
- **The setup access point has a published default password** (`radiofoundry`). It comes up whenever the board
  has had no Wi-Fi connection for 20 s.
- **HBlink4 and MQTT passwords are stored on the board** in plain text (NVS), and the settings API returns them.
  Wi-Fi passwords are never returned.
- **DMR over the air has no authentication.** The IP gateway forwards what radios send, inside the routes you set.

## Supported versions

Only the latest release and `main`.
