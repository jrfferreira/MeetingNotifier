# Getting started with MeetingNotifier

This guide assumes **zero** familiarity with electronics or coding. Total time to a working device: **~30 minutes**, plus shipping for the parts.

If you're already comfortable with PlatformIO and just want to build from source, jump to the [README](README.md) instead.

---

## What you're building

A small desk display that shows whether you should be in a meeting right now. It glows quietly when nothing's happening, turns amber 15 minutes before your next meeting, red at 5 minutes, green while a meeting is running, and dims back down when your day is done.

## What you'll need

| | Item | Approx. cost | Where to buy |
|---|---|---|---|
| 1 | **ESP32-C3 dev board** (any "ESP32-C3-DevKitM-1" or pin-compatible "Super Mini" board with USB-C) | ~$5 | Amazon / AliExpress / Adafruit / Pimoroni |
| 2 | **1.4" ST7789 SPI display, 240×240** (no touchscreen) | ~$8 | Amazon / AliExpress |
| 3 | **8 jumper wires** (female–female, dupont) | ~$3 | Any electronics kit |
| 4 | **Half-size breadboard** *(optional, but easier than soldering)* | ~$3 | Any electronics kit |
| 5 | **USB-C cable** (data, not just charging — most are fine) | you have one | — |

Total: about **$15–25**. No soldering iron required if you go with the breadboard.

You also need:
- A computer with **Chrome, Edge, or Opera** (these are the only browsers that can talk to USB devices). macOS, Windows, and Linux all work.
- A phone, for setting up WiFi.
- A Google Calendar (personal Gmail or Google Workspace).

---

## Step 1 — Wire it up

Stick the ESP32-C3 into one half of the breadboard. Connect the display's pins to the ESP32-C3 with jumper wires:

| Display pin | ESP32-C3 pin |
| --- | --- |
| `SCLK` (or `SCK`)        | `GPIO 6` |
| `MOSI` (or `SDA`)        | `GPIO 7` |
| `CS`                     | `GPIO 10` |
| `DC` (or `RS`)           | `GPIO 3` |
| `RST`                    | `GPIO 4` |
| `BLK` (or `BL`)          | `GPIO 5` |
| `VCC`                    | `3V3` |
| `GND`                    | `GND` |

**That's it for hardware.** Nothing to power on yet — the next step does that for you when you plug in USB.

---

## Step 2 — Get a calendar URL

The device needs a URL it can hit to read your calendar. You have three options. Pick whichever your calendar setup allows. **If you only have a personal Gmail and don't know which to pick, choose Option A.**

### Option A — Apps Script (personal Google account)

The simplest. Takes 3 minutes.

1. Open [script.google.com](https://script.google.com) in a browser.
2. Click **New project**.
3. Delete the placeholder code, then paste the contents of [`apps_script/Code.gs`](apps_script/Code.gs) from this repo.
4. Click the **Deploy** dropdown → **New deployment** → click the gear ⚙ → choose **Web app**.
5. Configure:
   - **Execute as:** Me
   - **Who has access:** Anyone
6. Click **Deploy**, then **Authorize access** (Google will warn that the app isn't verified — that's fine, it's your script; click *Advanced* → *Go to (unsafe)*).
7. Copy the **Web app URL** that ends in `/exec`. **Save it somewhere** — you'll paste it in step 4.

### Option B — Calendar's secret iCal address (work calendar that allows it)

Even simpler when it's available.

1. Open [Google Calendar](https://calendar.google.com).
2. In the left sidebar, hover your calendar name → click the **⋮** → **Settings and sharing**.
3. Scroll down to **Integrate calendar**.
4. Look for **Secret address in iCal format**. **If this field is missing, your work admin disabled it — use Option C instead.**
5. Copy the URL (ends in `/basic.ics`). **Save it.** You'll paste it in step 4.

### Option C — Local helper (work calendar where everything is locked down)

Requires a Mac, Linux PC, or Raspberry Pi that stays online. ~10 minutes of one-time setup.

Follow [`tools/calendar_helper.py`](tools/calendar_helper.py) — instructions are at the top of the file. The summary:

```bash
git clone https://github.com/jrfferreira/MeetingNotifier
cd MeetingNotifier
pip install -r tools/requirements.txt
# (Set up a Google Cloud OAuth client, save as tools/client_secret.json)
python tools/calendar_helper.py auth
python tools/calendar_helper.py serve
```

The URL you'll use is **`http://<your-computer-name>.local:8080/`** — for example `http://johns-macbook.local:8080/`.

---

## Step 3 — Flash the firmware

1. Plug the ESP32-C3 into your computer with USB-C.
2. Open <https://jrfferreira.github.io/MeetingNotifier/install/> in **Chrome, Edge, or Opera**.
3. Pick the card that matches your choice from Step 2:
   - Options A and C → **Apps Script web app, or local helper**
   - Option B → **iCal secret URL**
4. Click **Install**, then in the dialog pick the serial port that matches your ESP32-C3 (it usually shows up as "USB JTAG/serial debug unit" or similar).
5. Click **Install** in the dialog and wait ~30 seconds.

That's it. The firmware is on the device.

> **Browser doesn't have a "Install" button?** You're probably on Safari or Firefox — those don't support the WebSerial API. Switch to Chrome / Edge / Opera. iOS and Android don't work either; use a desktop.

---

## Step 4 — Configure WiFi and your calendar URL

The device boots into setup mode the first time it powers on.

1. Pick up your **phone** (or any other device with WiFi).
2. Open **WiFi settings**. You should see a network called `MeetingNotifier-Setup` — join it. (No password.)
3. A setup page should pop up automatically. If not, open `http://192.168.4.1` in any browser.
4. Fill in:
   - Your home / office **WiFi name and password**
   - The **calendar URL** you copied in Step 2
5. Tap **Save and reboot**.

The device reboots, joins your WiFi, asks the internet what time zone you're in, syncs the clock, and starts polling your calendar. Within a minute the screen should show your next meeting.

---

## Troubleshooting

**Browser says "No matching devices found"**
Your ESP32-C3 isn't in flashing mode, or USB drivers are missing.
- On Windows: install [CP210x or CH340 drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) depending on your board's USB chip.
- On macOS: drivers are usually built-in; try a different USB-C cable (some are charge-only).
- Try holding the `BOOT` button on the board, then briefly tap `RST`, then release `BOOT`. This forces flashing mode.

**Display stays blank after flashing**
- Triple-check the wiring against the table above. `SCLK ↔ GPIO 6` and `MOSI ↔ GPIO 7` are the most commonly swapped.
- Confirm `VCC` is on `3V3`, *not* `5V`.

**Display shows "no wifi" forever**
- The captive portal failed. Erase the device and try again — see "Start over" below.

**Time / countdown is wrong**
- The device gets its timezone from your IP address. If your network uses a VPN to another country, it'll guess the VPN's location. Disconnect the VPN, power-cycle, and let it re-sync.

**Want to change WiFi or calendar URL later?**
Currently the only way is to factory-reset: hold `BOOT`, tap `RST`, then re-install via Step 3 (which prompts to erase). A "settings" button in the captive portal is on the roadmap.

### Start over

If anything goes weird, you can always erase the device completely and start from scratch:
1. Re-open the [install page](https://jrfferreira.github.io/MeetingNotifier/install/).
2. The "Install" dialog includes an **Erase device** option — pick that, then re-install.

---

## Going deeper

If you want to modify the firmware, see [README.md](README.md) for the architecture overview and the full PlatformIO build instructions.
