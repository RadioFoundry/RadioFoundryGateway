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
- **A POST or a WebSocket from a web page of another origin is refused** (403): the board takes a change, or opens
  a WebSocket, only for its own page or a client that sends no `Origin` header (curl, scripts).
- **A request whose Host isn't an address or a local name is refused** (403), whatever its method: the board
  answers to its IP address, a one-label name, and names ending in `.local`, `.lan`, `.home`, `.internal`,
  `.home.arpa` or `.localdomain`. To reach it by another name (a reverse proxy's, a DNS name of your own), add
  that name to the settings' `web_hosts` (`"web_hosts": "gw.example.org"`); rdofradio reads the same key.
- **The setup access point has a published default password** (`radiofoundry`). It comes up whenever the board
  has had no Wi-Fi connection for 20 s.
- **HBlink4 and MQTT passwords are stored on the board** in plain text (NVS), and the settings API returns them.
  Wi-Fi passwords are never returned.
- **DMR over the air has no authentication.** The IP gateway forwards what radios send, inside the routes you set.

## Supported versions

Only the latest release and `main`.
