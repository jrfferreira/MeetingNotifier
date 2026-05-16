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
static MeetingData  meeting   = { false, "", "", "", 0, 0, 0 };

static uint32_t lastCalendarFetch     = 0;
static uint32_t lastDisplayRefresh    = 0;
static uint32_t lastStateChange       = 0;
static uint32_t lastSuccessfulFetchMs = 0;
static uint8_t  pulsePhase            = 0;
static bool     dimmed                = false;

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
  if (lastSuccessfulFetchMs > 0 &&
      millis() - lastSuccessfulFetchMs > STALE_THRESHOLD_MS) {
    state = STATE_NO_CONNECTION;
    return;
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
static void doFetch() {
  if (!wifiOnline()) return;
  MeetingData fresh = meeting;
  if (calendarFetch(fresh)) {
    meeting = fresh;
    lastSuccessfulFetchMs = millis();   // exit STATE_NO_CONNECTION on next tick
  }
}

// ---------------------------------------------------------------------------
// K1 button handlers (Spotpear board only; no-op on standalone module).
// ---------------------------------------------------------------------------
static void onK1ShortPress() {
  log_w("K1 short press → forcing immediate calendar refresh");
  // Reset the cadence so the loop fires doFetch() on the next iteration
  // (any positive elapsed since 0 will exceed POLL_INTERVAL_MS). Single
  // shot — lastCalendarFetch is updated to millis() inside the fire path.
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
  drawLoading(true);

  buttonsInit(onK1ShortPress, onK1LongPress);

  wifiBoot();        // connect from NVS or start captive portal

  if (wifiOnline()) {
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
