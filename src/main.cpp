// MeetingNotifier — ESP32-C3 + ST7789 ambient display.
//
// Polls Google Calendar via an Apps Script web app every 60 s and
// transitions through five display states based on meeting proximity.
// Loop runs free — no delay() — FreeRTOS feeds the watchdog.

#include <Arduino.h>
#include <Preferences.h>
#include <esp_sleep.h>

#include "ui.h"
#include "display.h"
#include "wifi_mgr.h"
#include "calendar.h"
#include "config_store.h"
#include "buttons.h"

// Override Arduino-ESP32's weak symbol so loopTask gets a 32 KB stack.
// The 8 KB default is tight once we add USB-CDC + ArduinoJson + the iCal
// heap copy + the WiFi event handlers that fire repeatedly on AUTH_FAIL,
// and -DCONFIG_ARDUINO_LOOP_STACK_SIZE build flags don't reliably override
// the SDK config in PlatformIO. This weak-symbol path does.
size_t getArduinoLoopTaskStackSize() { return 32 * 1024; }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static DisplayState state     = STATE_LOADING;
static DisplayState prevState = STATE_LOADING;
static MeetingData  meeting   = {};

// User-dismissed meeting. While set, any fetch returning an event with
// this startTime either advances to the calendar's next-event (when one
// is known) or is forced to "clear". Cleared automatically once the
// focus event changes.
static time_t       dismissedMeetingStart = 0;

static uint32_t lastCalendarFetch     = 0;
static uint32_t lastDisplayRefresh    = 0;
static uint32_t lastStateChange       = 0;
static uint32_t lastSuccessfulFetchMs = 0;
static uint32_t lastReconnectAttempt  = 0;
static uint32_t lastNtpRetryMs        = 0;
static uint8_t  pulsePhase            = 0;
static bool     dimmed                = false;
static bool     g_fetching            = false;   // true while doFetch() is in flight

#define RECONNECT_INTERVAL_MS  30000UL    // try WiFi.reconnect() this often when offline
#define NTP_RETRY_INTERVAL_MS  60000UL    // re-attempt NTP if time still unset
#define NTP_VALID_EPOCH        1700000000 // any time before this is "not synced"

// ---------------------------------------------------------------------------
// State transitions — pure function of (wifi, meeting, now).
// ---------------------------------------------------------------------------
static void updateState() {
  if (!wifiOnline()) {
    state = STATE_NO_WIFI;
    return;
  }
  if (!meeting.valid) {
    state = STATE_LOADING;
    return;
  }
  // If fetches have been failing for too long, fall back to NO_CONNECTION
  // rather than letting a stale "clear" status look like "no more meetings".
  // Exception: if our last known data says we're in a meeting and that
  // meeting hasn't ended yet, keep showing the live elapsed timer. Better
  // to keep the running clock than to alarm in the middle of a real call.
  if (lastSuccessfulFetchMs > 0 &&
      millis() - lastSuccessfulFetchMs > STALE_THRESHOLD_MS) {
    time_t nowEpoch = time(nullptr);
    bool stillInMeeting =
      strcmp(meeting.status, "in_meeting") == 0 &&
      meeting.endTime > nowEpoch;
    if (!stillInMeeting) {
      state = STATE_NO_CONNECTION;
      return;
    }
  }
  if (strcmp(meeting.status, "clear") == 0) {
    state = STATE_ALL_CLEAR;
    return;
  }

  time_t now = time(nullptr);

  if (strcmp(meeting.status, "in_meeting") == 0) {
    state = STATE_IN_MEETING;
    if (now > meeting.endTime && meeting.remainingToday == 0) {
      state = STATE_ALL_CLEAR;
    }
    return;
  }

  long secsUntil = (long)(meeting.startTime - now);
  if      (secsUntil <= 0)                       state = STATE_IN_MEETING;
  else if (secsUntil <= IMMINENT_THRESHOLD_SECS) state = STATE_IMMINENT;
  else if (secsUntil <= SOON_THRESHOLD_SECS)     state = STATE_SOON;
  else                                            state = STATE_IDLE;
}

// ---------------------------------------------------------------------------
// Render dispatch.
// ---------------------------------------------------------------------------
static void renderScreen() {
  bool fresh = (state != prevState);
  if (fresh) {
    log_i("state: %s → %s", stateName(prevState), stateName(state));
    prevState        = state;
    lastStateChange  = millis();
    backlightSet(BLK_FULL);
    dimmed = false;
  }

  switch (state) {
    case STATE_LOADING:        drawLoading(fresh);                       break;
    case STATE_NO_WIFI:        drawNoWifi(fresh);                        break;
    case STATE_NO_CONNECTION:  drawNoConnection(fresh);                  break;
    case STATE_IDLE:           drawIdle(meeting, fresh);                 break;
    case STATE_SOON:           drawSoon(meeting, fresh);                 break;
    case STATE_IMMINENT:       drawImminent(meeting, fresh, pulsePhase); break;
    case STATE_IN_MEETING:     drawInMeeting(meeting, fresh);            break;
    case STATE_ALL_CLEAR:      drawAllClear(fresh);                      break;
  }
  // Re-overlay the fetching icon after the state-specific render, since a
  // fresh repaint wipes the corner. drawFetchingIcon clears + redraws so
  // it's safe to call every tick even when nothing changed.
  if (fresh) drawFetchingIcon(g_fetching, paletteFor(state));
  pulsePhase = (pulsePhase + 1) & 0x7;
}

// ---------------------------------------------------------------------------
// "Awake" means: countdown is live or meeting is running. Other states are
// quiet enough to dim/sleep through.
// ---------------------------------------------------------------------------
static bool shouldStayAwake() {
  return state == STATE_SOON ||
         state == STATE_IMMINENT ||
         state == STATE_IN_MEETING;
}

// ---------------------------------------------------------------------------
// Power management — runs each tick, no sleep while a meeting is imminent.
// ---------------------------------------------------------------------------
static void powerTick() {
  if (shouldStayAwake()) {
    if (dimmed) { backlightSet(BLK_FULL); dimmed = false; }
    return;
  }

  uint32_t idleMs = millis() - lastStateChange;
  if (idleMs > DIM_TIMEOUT_MS && !dimmed) {
    backlightSet(BLK_DIM);
    dimmed = true;
  }
  // SLEEP_TIMEOUT_MS handled passively: when display is dim, idle current draw
  // is already low; explicit esp_light_sleep is left as a future enhancement
  // since the ST7789 needs full reinit after deep sleep and would flicker.
}

// ---------------------------------------------------------------------------
// Calendar refresh + state recompute.
// ---------------------------------------------------------------------------
// Swap the "focus" event fields with the "next" event fields. After this,
// the data represents the upcoming meeting that was queued behind the
// dismissed one.
static void advanceToNextEvent(MeetingData& m) {
  strncpy(m.title, m.nextTitle, sizeof(m.title) - 1);
  m.title[sizeof(m.title) - 1] = 0;
  m.location[0] = 0;     // Apps Script doesn't surface next.location; iCal can,
                         // but for now we drop it on advance for symmetry.
  m.startTime = m.nextStartTime;
  m.endTime   = m.nextEndTime;
  strncpy(m.status, "upcoming", sizeof(m.status) - 1);
  m.status[sizeof(m.status) - 1] = 0;
  m.nextTitle[0]  = 0;
  m.nextStartTime = 0;
  m.nextEndTime   = 0;
}

static void doFetch() {
  if (!wifiOnline()) return;

  // Surface the fetching indicator immediately — calendarFetch() is a
  // 2–3 s blocking HTTPS call and otherwise the device would look frozen.
  g_fetching = true;
  drawFetchingIcon(true, paletteFor(state));

  MeetingData fresh = meeting;
  if (calendarFetch(fresh)) {
    // Reset any dismissal once the calendar's focus has actually moved on
    // (the meeting really ended, or got rescheduled).
    if (dismissedMeetingStart != 0 &&
        fresh.startTime != dismissedMeetingStart) {
      log_w("dismissal cleared: focus moved from %ld to %ld",
            (long)dismissedMeetingStart, (long)fresh.startTime);
      dismissedMeetingStart = 0;
    }
    // Still on the dismissed meeting → either advance to the queued
    // next-event or, if none is known, just force the status to clear.
    if (dismissedMeetingStart != 0 &&
        fresh.startTime == dismissedMeetingStart) {
      if (fresh.nextStartTime != 0 && fresh.nextEndTime != 0) {
        log_w("dismissal: still on %ld, advancing to next '%s' at %ld",
              (long)dismissedMeetingStart, fresh.nextTitle,
              (long)fresh.nextStartTime);
        advanceToNextEvent(fresh);
      } else {
        strncpy(fresh.status, "clear", sizeof(fresh.status) - 1);
        fresh.status[sizeof(fresh.status) - 1] = 0;
      }
    }
    meeting = fresh;
    lastSuccessfulFetchMs = millis();   // exit STATE_NO_CONNECTION on next tick
  }

  g_fetching = false;
  drawFetchingIcon(false, paletteFor(state));
}

// ---------------------------------------------------------------------------
// K1 button handlers (Spotpear board only; no-op on standalone module).
// ---------------------------------------------------------------------------
static void onK1ShortPress() {
  // In a meeting → mark it done and jump straight to the next event if
  // one is known. Outside a meeting → just force a calendar refresh.
  if (state == STATE_IN_MEETING && meeting.startTime != 0) {
    dismissedMeetingStart = meeting.startTime;
    log_w("K1 → dismissing '%s' (start=%ld)",
          meeting.title, (long)dismissedMeetingStart);
    if (meeting.nextStartTime != 0 && meeting.nextEndTime != 0) {
      log_w("K1 → advancing to next event '%s' (start=%ld)",
            meeting.nextTitle, (long)meeting.nextStartTime);
      advanceToNextEvent(meeting);
    } else {
      log_w("K1 → no next event known, showing clear");
      strncpy(meeting.status, "clear", sizeof(meeting.status) - 1);
      meeting.status[sizeof(meeting.status) - 1] = 0;
    }
    lastCalendarFetch = 0;       // also re-poll to surface anything queued later
    lastDisplayRefresh = 0;      // re-render now so the user sees the change
    return;
  }
  log_w("K1 short press → forcing immediate calendar refresh");
  lastCalendarFetch = 0;
}

static void onK1LongPress() {
  log_w("K1 long press → factory reset (clearing NVS)");
  Preferences p;
  p.begin(CFG_NS, false);
  p.clear();   // drops ssid, pass, cal — next boot drops to captive portal
  p.end();
  // Also wipe the WiFi stack's own persistent creds (set by
  // WiFi.persistent(true) elsewhere) so nothing tries to auto-reconnect
  // after the restart.
  WiFi.disconnect(true, true);
  delay(200);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(50);
  log_i("MeetingNotifier boot");

  displayInit();
  setLoadingPhase("starting up");
  drawLoading(true);

  cfgMigrateLegacyNamespace();   // one-time copy from "wifi" → "mnotif"
  buttonsInit(onK1ShortPress, onK1LongPress);

  setLoadingPhase("connecting WiFi");
  drawLoading(true);
  wifiBoot();        // connect from NVS or start captive portal

  if (wifiOnline()) {
    setLoadingPhase("fetching calendar");
    drawLoading(true);
    doFetch();
    updateState();
  } else {
    state = STATE_NO_WIFI;
  }
  renderScreen();

  lastCalendarFetch  = millis();
  lastDisplayRefresh = millis();
  lastStateChange    = millis();
}

// ---------------------------------------------------------------------------
// Self-healing housekeeping: re-arm WiFi when it drops and re-trigger NTP
// when the clock is still at the 1970 epoch. Both are no-ops on the happy
// path so they're cheap to call every loop iteration.
// ---------------------------------------------------------------------------
static void networkHousekeeping() {
  uint32_t nowMs = millis();

  // Manual reconnect nudge on top of Arduino's built-in auto-reconnect. The
  // auto path can stall after some failure modes; this guarantees we keep
  // trying.
  if (!wifiOnline() &&
      nowMs - lastReconnectAttempt > RECONNECT_INTERVAL_MS) {
    lastReconnectAttempt = nowMs;
    log_w("network: WiFi offline, calling WiFi.reconnect()");
    WiFi.reconnect();
  }

  // NTP retry. If wmAwaitNtp() timed out at boot, time(nullptr) is still
  // near zero and every state-machine calculation is broken. Keep retrying.
  if (wifiOnline() &&
      time(nullptr) < NTP_VALID_EPOCH &&
      nowMs - lastNtpRetryMs > NTP_RETRY_INTERVAL_MS) {
    lastNtpRetryMs = nowMs;
    log_w("network: NTP not yet synced, retrying ip-api + configTime");
    wmConfigureTimeViaIpApi();
  }
}

// ---------------------------------------------------------------------------
// Loop — no delay(); cadence is timer-driven.
// ---------------------------------------------------------------------------
void loop() {
  buttonsTick();   // even during captive portal — K1 long-press = re-reset

  // Captive portal: handle DNS + HTTP requests, skip the rest until the user
  // finishes setup (then we ESP.restart() from the save handler).
  if (wifiPortalActive()) {
    wmPortalLoop();
    return;
  }

  networkHousekeeping();

  uint32_t now = millis();

  // Active states (countdown / elapsed timer visible) tick at 1 s so the
  // MM:SS digits don't jump in 5 s steps. Ambient states (clock only
  // changes per-minute) stay at the cheaper 5 s.
  uint32_t refreshMs = (state == STATE_SOON ||
                        state == STATE_IMMINENT ||
                        state == STATE_IN_MEETING)
                       ? REFRESH_FAST_INTERVAL_MS
                       : REFRESH_INTERVAL_MS;

  if (now - lastDisplayRefresh >= refreshMs) {
    lastDisplayRefresh = now;
    updateState();
    renderScreen();
    powerTick();
  }

  if (now - lastCalendarFetch >= POLL_INTERVAL_MS) {
    lastCalendarFetch = now;
    doFetch();
  }
}
