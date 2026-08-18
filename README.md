# ATEM Web Tally

**Wireless tally lights for Blackmagic ATEM switchers — using the phones you
already have. No hardware required, no code to edit, guided setup in the
browser.**

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Buy Me a Coffee](https://img.shields.io/badge/☕_Buy_me_a_coffee-support_this_project-ffdd00)](https://buymeacoffee.com/jithinmathew)

> 🎯 **New here? Not technical?** Follow the step-by-step
> **[Getting Started Guide](docs/GETTING_STARTED.md)** — written for complete
> beginners, screenshots-level detail, ~15 minutes.

---

## What it does

One lightweight Node.js server holds a single connection to your ATEM and fans
tally state out to any number of clients:

- 📱 **Web tally (default)** — open a URL on any phone or tablet, tap your
  camera, get a full-screen red **L** (live) / green **P** (preview) tally.
  Zero hardware, unlimited devices, and it doesn't count against the ATEM's
  connection limit.
- 🔋 **Hardware tally (optional)** — flash the included firmware onto
  [M5StickS3](https://shop.m5stack.com/products/m5sticks3-esp32s3-mini-iot-dev-kit)
  units (~$21 ESP32 devices with built-in screen, battery, and magnetic case)
  for camera-mounted tallies with a hot-swap battery workflow, on-device
  camera assignment, and fleet battery monitoring.

```
                         ┌──────────────────────┐
  ATEM ──(1 connection)──│     Node server      │
                         │  (read-only default) │
                         └──────┬───────────┬───┘
                    WebSocket   │           │  UDP broadcast
             ┌──────────┬──────┘           └───────┬─────────┐
          📱 phone   📱 tablet                 🔋 StickS3  🔋 StickS3
```

## Feature highlights

| | |
|---|---|
| **Setup wizard** | First run opens a guided browser wizard: pick your ATEM model, enter its IP (with live *Test connection*), name your inputs, tune tally behavior, choose a mode. No JSON editing. |
| **Read-only by default** | Out of the box the server **never sends a command to the ATEM** — it cannot cut your show. |
| **Remote switcher mode (opt-in)** | Enable at setup with a mandatory password: logged-in users get click-to-preview + CUT/AUTO from the admin as a backup control surface. Everyone else stays read-only. |
| **Real auth** | scrypt-hashed passwords (never plaintext), in-memory session tokens (12 h). With a password set, all settings changes require login; *viewing* tally never does. The wizard locks itself after first run. |
| **Live input names** | Pulled from the ATEM (as named in ATEM Software Control), overridable per input. Names appear on phones, hardware units, and the admin. |
| **Honest tally** | Preview shows as on-air during transitions. Clients that lose the server show an amber **?** — never a stale tally. |
| **Fleet management** | Hardware units self-register by MAC, report battery % + signal, and warn with an estimated time-to-shutdown below 20 % (measured drain rate, not voltage guessing). |
| **Zero-touch swaps** | Assignment lives on the server, keyed by MAC. Swap a charged unit on and it resumes its camera — or press its button to pick an input by name, no laptop needed. |

## Quick start

Requires [Node.js](https://nodejs.org) 18+.

```bash
git clone https://github.com/YOURNAME/atem-web-tally.git
cd atem-web-tally/server
npm install
node server.js
```

Open `http://<server-ip>:3000` → the setup wizard takes it from there. Then:

| Page | URL | Who uses it |
|---|---|---|
| Web tally | `http://<server-ip>:3000/tally.html` | Camera operators — pick camera, go |
| Admin | `http://<server-ip>:3000/admin.html` | Whoever runs the system |

### macOS one-step install

```bash
./install-mac.sh              # installs Node if needed + all dependencies
./install-mac.sh --service    # also: start at login + restart on crash (launchd)
./install-mac.sh --uninstall-service
```

## Using the web tally

1. On the operator's phone, open `http://<server-ip>:3000/tally.html`.
2. Tap your camera — the picker buttons show **live tally colors**, so the
   selection screen doubles as a pocket multiview.
3. Full-screen tally: **red L** = you're live, **green P** = you're next,
   **amber ?** = check connection. Long-press anywhere to change camera.
4. iOS/Android: use **Add to Home Screen** for full-screen app mode.
   Deep-link a specific camera with `?cam=N`.

The page requests a screen wake lock; still, set phone auto-lock to *Never*
for shows longer than your OS honors the lock.

## Optional: M5StickS3 hardware tallies

Camera-mounted units with a battery-first state machine: full-screen **L**/**P**
when it matters, screen hard-off when idle (with an alive-blink heartbeat),
WiFi modem sleep — roughly 6–10 h per charge on the internal 250 mAh cell.

**Setup:** edit `firmware/src/config.h` (WiFi credentials), flash with
[PlatformIO](https://platformio.org) (`pio run -t upload`), then assign the
camera from the admin page — or on the unit itself: press the main button to
cycle inputs (shown by name), stop pressing for 4 s, and it saves to the
server with a green **SAVED** confirmation.

**Battery workflow:** units show charge % in the corner while lit, overlay it
every 15 min while idle, and below 20 % switch to every 5 min with an
estimated **~minutes left** — mirrored on the admin page so the swap crew
sees it first. Swap the whole unit; assignment follows the replacement's MAC
or one round of button presses.

**Verify before buying a fleet** (the StickS3 is recent hardware):
- Your M5Unified version detects the board and `M5.Power.getBatteryLevel()`
  returns sane values.
- `platformio.ini` board definition (`m5stack-stamps3` is the closest stock
  def; adjust if M5 publishes an official one).
- `LED_PIN` in `config.h` against the schematic (leave `-1` to use a dim
  screen pulse as the heartbeat).
- Your router delivers UDP broadcast promptly to power-saving clients
  (set AP DTIM to 1–3; disable "broadcast filtering" features).

## Protocol (for integrators)

Tally broadcast — server → `255.255.255.255:7411`, every 500 ms + on change:

```
byte 0-1  'T' '1'      magic
byte 2    seq          rolling counter
byte 3    camCount
byte 4+   camCount bytes: bit0 = program, bit1 = preview
```

Device status — device → broadcast `:7412` (JSON: `mac`, `batt`, `eta`,
`rssi`, `up`, `fw`, `cam`, optional `setCam`); server replies unicast with
that device's config (camera, name, brightness, input names, `maxCam`).
Anything that speaks UDP can join — the StickS3 firmware is a reference
client, not a requirement.

REST: `GET /api/state` (open) · `POST /api/devices/:mac` ·
`POST /api/inputs/:n` · `POST /api/atem` (token-gated once a password
exists) · `POST /api/switch` (always token-gated; refused outright in
read-only mode) · `POST /api/auth/login`. Live updates via WebSocket `/ws`.

## Notes & limitations

- Tally reflects M/E program/preview (including hot preview during
  transitions). Upstream/downstream keyer sources and SuperSource
  contributors are not tracked as on-air — PRs welcome.
- UDP broadcast does not cross subnets: hardware tallies must share the
  server's network segment. Web tallies (WebSocket) work across VLANs.
- Run on an isolated production network regardless of auth: traffic is plain
  HTTP/WS on your LAN, and the UDP device protocol is unauthenticated by
  design.
- Switcher mode covers preview select + CUT/AUTO on M/E 1 — a backup control
  surface, not an ATEM Software Control replacement.
- The server occupies one slot against the ATEM's client connection limit
  (that's the point — everything else connects to the server instead).

## License & author

MIT — see [LICENSE](LICENSE).

Built by **[Jithin Mathew](https://jithinmathew.com)** · Noedge Inc.

## Support this project

ATEM Web Tally is free and MIT-licensed, built and maintained in evenings
around a day job. If it saved your production budget (commercial tally
systems start in the hundreds of dollars per camera), you can:

- ⭐ **Star the repo** — helps others find it
- ☕ **[Buy me a coffee](https://buymeacoffee.com/jithinmathew)** — keeps the
  evenings caffeinated and the issues answered
- 🔧 **Open a PR** — keyer/SuperSource tally and new hardware clients are
  the most-wanted contributions
