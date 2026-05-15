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

### 1. Deploy the calendar web app

[`apps_script/Code.gs`](apps_script/Code.gs) is a small Google Apps Script that exposes the next event from your default calendar as JSON.

1. Open [script.google.com](https://script.google.com), create a new project, paste the contents of `apps_script/Code.gs`.
2. **Deploy → New deployment → Web app**:
   - *Execute as:* **Me**
   - *Who has access:* **Anyone**
3. Copy the `/exec` URL.

### 2. Build and flash

Requires [PlatformIO](https://platformio.org/install/cli).

```bash
git clone https://github.com/jrfferreira/MeetingNotifier
cd MeetingNotifier
```

Paste your `/exec` URL into `CALENDAR_URL` in `src/calendar.h`, or override it via `build_flags` in `platformio.ini`:

```ini
build_flags =
  -DCALENDAR_URL='"https://script.google.com/macros/s/.../exec"'
```

Then build and flash:

```bash
pio run -e esp32-c3-devkitm-1 -t upload
pio device monitor                # optional, watch the log
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

| File                  | What's in it |
| --------------------- | ------------ |
| `src/main.cpp`        | `setup()`, `loop()`, `updateState()`, `renderScreen()`, power management |
| `src/ui.h`            | Pin map, per-state palettes, `MeetingData`, time helpers, timing constants |
| `src/display.h`       | ST7789 + GFX init, PWM backlight, per-state renderers, dirty-region updates |
| `src/wifi_mgr.h`      | NVS-backed STA connect with SoftAP + DNS captive portal fallback; ip-api + NTP |
| `src/calendar.h`      | HTTPS GET to Apps Script + ISO 8601 parser (handles both `Z` and `±HH:MM`) |
| `apps_script/Code.gs` | Deployable Apps Script web app source |

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

`CALENDAR_URL` lives in `src/calendar.h`.

## Releases

Every push to `main` triggers [`.github/workflows/release.yml`](.github/workflows/release.yml), which compiles the firmware and publishes a GitHub Release tagged `vYYYY.MM.DD-<run>` with `firmware.bin` and `firmware.elf` attached.

The published binary is built with the placeholder `CALENDAR_URL` and is useful only as a build-validation smoke test — you still need to rebuild locally with your own URL for a working device.

PRs trigger [`.github/workflows/build.yml`](.github/workflows/build.yml), which just compile-checks against the merge base.

## License

[MIT](LICENSE)
