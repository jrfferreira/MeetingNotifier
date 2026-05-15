# MeetingNotifier

An ambient ESP32-C3 desk display that tells you whether you should be in a meeting.

A 1.4" 240×240 TFT polls your Google Calendar once a minute and walks through five states based on how close the next meeting is — a quiet ambient clock when nothing is happening, a soft amber countdown 15 minutes out, a red "join now" alert at five, an in-meeting timer with a progress bar, and an all-clear when the day is done.

No buttons. No touch. Glance at it and know.

## States

| State        | Trigger                            | Look |
| ------------ | ---------------------------------- | ---- |
| `IDLE`       | Next meeting more than 15 min away | Navy, ambient clock + subtle next-meeting hint |
| `SOON`       | Next meeting ≤ 15 min              | Amber, `MM:SS` countdown + title + time range |
| `IMMINENT`   | Next meeting ≤ 5 min               | Red, `JOIN NOW` badge + countdown + meet link |
| `IN_MEETING` | Between start and end              | Green, elapsed timer + progress bar |
| `ALL_CLEAR`  | No more meetings today             | Dim, quiet clock + checkmark |

Plus `LOADING` while booting and `NO_WIFI` until credentials are configured.

## Hardware

- **MCU:** ESP32-C3-DevKitM-1 (or any ESP32-C3 board with the GPIOs below exposed)
- **Display:** 1.4" ST7789 TFT, 240×240, SPI, no touch / no MISO
- **Power:** USB-C, ~80 mA average at full backlight

Wiring (display → ESP32-C3 GPIO):

| Signal              | GPIO |
| ------------------- | ---- |
| SCLK                | 6    |
| MOSI / SDA          | 7    |
| CS                  | 10   |
| DC / RS             | 3    |
| RST                 | 4    |
| BLK (PWM backlight) | 5    |
| VCC                 | 3V3  |
| GND                 | GND  |

SPI clock runs at 40 MHz. Pin assignments live in `src/ui.h` if you need to remap.

## Setup

### 1. Pick a calendar source

| Source | When it fits | Build env | Setup time |
| --- | --- | --- | --- |
| **Apps Script web app** | Personal Google account, or any Workspace where Apps Script execution isn't blocked | `esp32-c3-devkitm-1` | ~3 min |
| **iCal secret URL** | Work calendar where *Integrate calendar → Secret address* is still available | `esp32-c3-ical` | ~1 min |
| **Local OAuth helper** | Work calendar where Apps Script *and* iCal export are disabled | `esp32-c3-devkitm-1` | ~10 min, plus a Mac / Pi that stays online |

Each option ends with a URL you'll wire into the firmware as `CALENDAR_URL`. Pick one and follow the matching subsection.

#### Option A — Apps Script web app

[`apps_script/Code.gs`](apps_script/Code.gs) exposes your next event as JSON.

1. Open [script.google.com](https://script.google.com), create a project, paste the contents of `apps_script/Code.gs`.
2. **Deploy → New deployment → Web app**:
   - *Execute as:* **Me**
   - *Who has access:* **Anyone**
3. Copy the `/exec` URL. That's your `CALENDAR_URL`.

#### Option B — iCal secret URL

In Google Calendar: **Settings and sharing → Integrate calendar → Secret address in iCal format**. Copy the URL — that's your `CALENDAR_URL`. Build with the `esp32-c3-ical` env so the firmware uses the on-device `.ics` parser instead of the JSON one.

If the *Secret address* field is missing from settings, your admin disabled iCal export; use Option C.

> Caveat: the parser interprets non-UTC timestamps in the device's configured local timezone (auto-detected via ip-api). Events scheduled in *other* timezones may display at the wrong time. If that's a problem, Option C doesn't have this limitation.

#### Option C — Local OAuth helper

`tools/calendar_helper.py` runs on your Mac / Pi, OAuths into the Calendar API, and re-exposes the *same* JSON shape Option A produces — so the firmware doesn't care which one is on the other end of `CALENDAR_URL`.

One-time setup:

1. [console.cloud.google.com](https://console.cloud.google.com) → new project (free tier).
2. **APIs & Services → Library** → enable *Google Calendar API*.
3. **APIs & Services → Credentials → Create credentials → OAuth client ID → Desktop app**. Download the JSON to `tools/client_secret.json` (git-ignored).
4. ```bash
   pip install -r tools/requirements.txt
   python tools/calendar_helper.py auth      # opens a browser, grant read-only access
   ```

Daily use:

```bash
python tools/calendar_helper.py serve       # default: 0.0.0.0:8080
```

Your `CALENDAR_URL` is `http://<your-host>.local:8080/` (or an LAN IP if mDNS is flaky on your network). The helper needs to be reachable from the ESP32 — same WiFi is enough.

### 2. Build and flash

Requires [PlatformIO](https://platformio.org/install/cli).

```bash
git clone https://github.com/jrfferreira/MeetingNotifier
cd MeetingNotifier
```

Pass your `CALENDAR_URL` via `build_flags` in `platformio.ini` (keeps the URL out of git), or paste it directly into the matching header:

```ini
[env:esp32-c3-devkitm-1]      ; or [env:esp32-c3-ical] for Option B
build_flags =
  ${env.build_flags}
  -DCALENDAR_URL='"https://script.google.com/macros/s/.../exec"'
```

Then:

```bash
pio run -e esp32-c3-devkitm-1 -t upload   # Option A or C
# or
pio run -e esp32-c3-ical -t upload         # Option B
pio device monitor                          # optional, watch the log
```

### 3. First boot

On first power-up the device starts an open access point called **`MeetingNotifier-Setup`**. Join it from your phone, open `http://192.168.4.1`, fill in your home WiFi credentials, hit *Save and reboot*. The device stores them in NVS and never asks again.

Once online it queries [ip-api.com](http://ip-api.com) to detect your IANA timezone and feeds it to `configTzTime`, so the clock is correct without manual TZ setup. Then it syncs NTP from `pool.ntp.org` and starts polling the calendar.

To re-run the captive portal (e.g. moving to a new network), erase NVS:

```bash
pio run -e esp32-c3-devkitm-1 -t erase
```

## Architecture

State machine in `src/main.cpp`, recomputed every 5 s from `(wifi, meeting, now)`:

```
IDLE ──── ≤ 15 min ──── SOON ──── ≤ 5 min ──── IMMINENT
                                                   │
                                            start  ▼
                                                IN_MEETING
                                                   │
                                              end  ▼
                                                ALL_CLEAR
```

File map:

| File                       | What's in it |
| -------------------------- | ------------ |
| `src/main.cpp`             | `setup()`, `loop()`, `updateState()`, `renderScreen()`, power management |
| `src/ui.h`                 | Pin map, per-state palettes, `MeetingData`, time helpers, timing constants |
| `src/display.h`            | ST7789 + GFX init, PWM backlight, per-state renderers, dirty-region updates |
| `src/wifi_mgr.h`           | NVS-backed STA connect with SoftAP + DNS captive portal fallback; ip-api + NTP |
| `src/calendar.h`           | Dispatcher: includes `calendar_json.h` by default, `calendar_ical.h` with `-DUSE_ICAL_SOURCE` |
| `src/calendar_json.h`      | HTTPS GET to a JSON endpoint (Apps Script *or* local helper) + ISO 8601 parser |
| `src/calendar_ical.h`      | HTTPS GET to a `.ics` URL + VEVENT parser (line unfolding, `DTSTART/DTEND/SUMMARY/LOCATION`) |
| `apps_script/Code.gs`      | Deployable Apps Script web app source (Option A) |
| `tools/calendar_helper.py` | Local OAuth helper that mimics the Apps Script JSON contract (Option C) |

The loop runs free — no `delay()` — and is driven by `millis()` comparisons. The display refreshes every 5 s; the calendar is polled every 60 s. `renderBigNumber` and `renderBottomStrap` only repaint when their text actually changes, so a 5 s tick is cheap when nothing has moved.

## Configuration

The interesting knobs live as `#define`s in `src/ui.h`:

| Define                    | Default | Effect |
| ------------------------- | ------- | ------ |
| `POLL_INTERVAL_MS`        | 60 000  | Calendar fetch cadence |
| `REFRESH_INTERVAL_MS`     | 5 000   | Display tick |
| `SOON_THRESHOLD_SECS`     | 900     | Enter `SOON` at this many seconds out |
| `IMMINENT_THRESHOLD_SECS` | 300     | Enter `IMMINENT` at this many seconds out |
| `DIM_TIMEOUT_MS`          | 60 000  | Dim backlight to 20% after this much idle (ambient states only) |

`CALENDAR_URL` lives in `src/calendar_json.h` / `src/calendar_ical.h`; override via `build_flags` so the URL stays out of git.

## Releases

Every push to `main` triggers [`.github/workflows/release.yml`](.github/workflows/release.yml), which compiles both build environments and publishes a GitHub Release tagged `vYYYY.MM.DD-<run>` with both binaries attached:

| Asset                                                | Source |
| ---------------------------------------------------- | ------ |
| `MeetingNotifier-esp32-c3-devkitm-1-vYYYY.MM.DD-N.bin` | JSON (Apps Script or local helper) |
| `MeetingNotifier-esp32-c3-ical-vYYYY.MM.DD-N.bin`     | iCal (`.ics`) |

Both ship with the placeholder `CALENDAR_URL` baked in, so the binaries are useful only as build-validation smoke tests — you still need to rebuild locally with your own URL for a working device.

PRs trigger [`.github/workflows/build.yml`](.github/workflows/build.yml), which compile-checks both envs against the merge base.

## License

[MIT](LICENSE)
