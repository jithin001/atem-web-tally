# Getting Started — the complete beginner's guide

This guide assumes **zero technical background**. If you can install an app
and type a web address, you can do this. Total time: about 15 minutes.

**What you'll end up with:** every camera operator opens a web page on their
phone, taps their camera number, and their screen turns **red when they're
live** and **green when they're next**. That's it.

---

## Part 1 — What you need

- **A computer that stays on during your event** (a Mac or Windows PC — an
  old laptop is fine). This runs the small "server" program.
- **Your ATEM switcher connected to your network** with an Ethernet cable,
  and you need to know its **IP address** (looks like `192.168.1.240`).
  Don't know it? See [Finding your ATEM's IP](#finding-your-atems-ip-address) below.
- **Phones on the same WiFi network** as that computer — one per camera.

> **The golden rule of this whole system:** the computer, the ATEM, and the
> phones must all be on the **same network**. If something doesn't connect,
> 9 times out of 10 this is why (e.g. phones on "Guest WiFi" while the
> computer is on the main network).

---

## Part 2 — Install the server

### On a Mac (easiest)

1. Download this project (green **Code** button on GitHub → **Download ZIP**)
   and unzip it. You'll get a folder called `atem-web-tally`.
2. Open the **Terminal** app (press `Cmd + Space`, type `terminal`, press Enter).
3. Type `cd ` (with a space after it), then **drag the unzipped folder into
   the Terminal window** — it fills in the path for you. Press Enter.
4. Type this and press Enter:

       ./install-mac.sh --service

   This installs everything needed (including Node.js if you don't have it)
   and sets the server to **start automatically** whenever the computer
   starts — you never have to remember to launch it.
5. When it finishes, it prints two web addresses. **Write them down** — one
   is for camera operators (`tally.html`), one is for you (`admin.html`).

> First time only: if a security warning appears when running scripts,
> right-click the file and choose **Open** instead of double-clicking.

### On Windows or Linux

1. Install **Node.js**: go to [nodejs.org](https://nodejs.org), download the
   **LTS** version, run the installer, accept all defaults.
2. Download and unzip this project (green **Code** button → **Download ZIP**).
3. Open **Command Prompt** (Windows: press `Win`, type `cmd`, Enter) and go
   into the project's `server` folder, e.g.:

       cd Downloads\atem-web-tally\server

4. Type these two lines, pressing Enter after each (the first one takes a
   minute):

       npm install
       node server.js

5. Leave that window open — **the server runs as long as that window is
   open.** The window shows the web addresses to use.

---

## Part 3 — The setup wizard (one time only)

On the server computer, open a web browser and go to:

    http://localhost:3000

The setup wizard opens automatically. Four short steps:

1. **Your switcher** — pick your ATEM model from the list (this fills in the
   number of inputs for you), type the ATEM's IP address, and click
   **Test connection**. You should see *✓ Connected* with your ATEM's name.
   If not, jump to [Troubleshooting](#part-6--troubleshooting).
2. **Name your inputs** — type what each camera actually is: *Pulpit*,
   *Stage Left*, *Balcony*… These names appear on every tally screen, so
   operators pick "Balcony", not "Input 3". (Leave blank to use the names
   already set on the ATEM.)
3. **Tally behavior** — only matters if you use the optional hardware
   tallies. Using phones only? Click **Next**.
4. **Mode & password** — leave **Read-only** selected (recommended). In
   read-only mode this system can *never* change what's on air — it only
   displays. Setting a password here is optional but smart: it stops anyone
   on the WiFi from renaming things or reassigning tallies.

Click **Finish setup**. Done — the wizard never runs again (re-running it
requires the password).

---

## Part 4 — Camera operators: using the tally (30 seconds to learn)

Each operator, on their phone:

1. Open the tally address in the phone's browser:

       http://THE-SERVER-IP:3000/tally.html

   (Replace `THE-SERVER-IP` with the address from Part 2 — for example
   `http://192.168.1.50:3000/tally.html`.)
2. **Tap your camera.** The buttons already glow red/green with live tally,
   so you can see the whole show at a glance before choosing.
3. That's it. Full screen:
   - 🔴 **Red L** — you are LIVE. Don't move.
   - 🟢 **Green P** — you're up next. Get ready.
   - ⚫ **Black with a small green blink** — idle, everything's working.
   - 🟠 **Amber ?** — connection problem. Wave at the person in the booth.
4. **Held the wrong camera?** Press and hold anywhere on the screen for a
   second — the camera picker comes back.

**Tips that save your Sunday:**
- Add it to the home screen (**Share → Add to Home Screen** on iPhone,
  browser menu → **Add to Home screen** on Android) — it opens full-screen
  like a real app.
- Set the phone's **auto-lock / screen timeout to Never** for the event, and
  start with a decent charge — a bright screen for 3 hours uses real battery.
- Bookmark your camera directly: add `?cam=2` to the address and that phone
  always opens as camera 2.

---

## Part 5 — The admin page (for whoever runs the system)

    http://THE-SERVER-IP:3000/admin.html

- **Live tally strip** — see all cameras' program/preview state at a glance.
- **Input names** — rename any input; every phone and hardware tally updates
  within seconds.
- **Devices** — only used with the optional hardware tallies: assign each
  unit to a camera, watch battery levels, and get a **"~minutes left"**
  estimate when one runs low.
- **Unlock** — if you set a password, changes require clicking **Unlock**
  and entering it once per session. Everyone can always *view*.

---

## Part 6 — Troubleshooting

**The wizard's "Test connection" fails**
- Double-check the IP (see below). Ping it if you know how.
- The ATEM must be connected by **Ethernet** to the same network as the
  server computer.
- ATEMs allow only a handful of simultaneous connections — close ATEM
  Software Control on other computers and try again.

**Phones show the amber ? / can't load the page**
- Phone and server on the same WiFi? Guest networks are usually isolated —
  that's the #1 cause.
- Is the server still running? (Windows/Linux: is that Command Prompt window
  still open? Mac with `--service`: it restarts itself, give it 10 seconds.)
- Firewall prompt on the server computer: click **Allow**.

**Tally feels delayed on phones**
- Normal delay is well under half a second. If it's worse, your WiFi is
  congested — a cheap dedicated router just for production gear fixes this
  permanently and is best practice anyway.

**I forgot the admin password**
- On the server computer, open `server/config.json` in a text editor, delete
  the line starting with `"auth":` … up to the matching `},`, save, and
  restart the server. (Or delete `config.json` entirely to re-run the wizard
  from scratch — you'll lose names and device assignments.)

**Finding your ATEM's IP address**<a name="finding-your-atems-ip-address"></a>
- Open **ATEM Software Control** on any computer that already connects to
  it → the IP is shown when selecting the switcher, or under
  *Blackmagic ATEM Setup* (the setup utility that came with the switcher).
- ATEMs ship with a default of `192.168.10.240` — if nobody ever changed it,
  try that.

**Everything worked last week and not today**
- Something got a new IP. Check the ATEM's IP first, then the server's.
  Best practice: give both **static IPs** (or DHCP reservations) in your
  router so this never happens again.

---

## Part 7 — Optional: hardware tally lights

If you want dedicated camera-mounted lights instead of phones (built-in
screen, battery, magnetic mount, ~$21 per unit), see the
**[hardware section of the README](../README.md#optional-m5sticks3-hardware-tallies)**.
The short version: buy an M5StickS3, flash the included firmware with your
WiFi details, and it appears on the admin page ready to be assigned — either
from the admin or by pressing the button on the unit itself.

---

*Part of [ATEM Web Tally](../README.md) · MIT ·
built by [Jithin Mathew](https://jithinmathew.com) ·
found it useful? [buy me a coffee ☕](https://buymeacoffee.com/jithinmathew)*
