#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

#include "ui.h"
#include "config_store.h"

// ---------------------------------------------------------------------------
// Persistent WiFi credentials live in NVS namespace CFG_NS (config_store.h),
// alongside the calendar URL. Captive portal SSID is "MeetingNotifier-Setup".
// ---------------------------------------------------------------------------
#define WM_AP_SSID       "MeetingNotifier-Setup"
#define WM_CONNECT_MS    10000UL
#define WM_DNS_PORT      53

static Preferences  wmPrefs;
static WebServer    wmHttp(80);
static DNSServer    wmDns;
static bool         wmPortalActive = false;
static bool         wmConnected    = false;
static char         wmTzBuf[64]    = "UTC0";

inline bool wifiOnline() { return wmConnected && WiFi.status() == WL_CONNECTED; }

inline bool wifiPortalActive() { return wmPortalActive; }

// ---------------------------------------------------------------------------
// NVS helpers (WiFi creds; calendar URL lives in config_store.h)
// ---------------------------------------------------------------------------
inline void wmLoadCreds(String& ssid, String& pass) {
  wmPrefs.begin(CFG_NS, true);
  ssid = wmPrefs.getString(CFG_KEY_SSID, "");
  pass = wmPrefs.getString(CFG_KEY_PASS, "");
  wmPrefs.end();
}

inline void wmSaveCreds(const String& ssid, const String& pass) {
  wmPrefs.begin(CFG_NS, false);
  wmPrefs.putString(CFG_KEY_SSID, ssid);
  wmPrefs.putString(CFG_KEY_PASS, pass);
  wmPrefs.end();
}

// ---------------------------------------------------------------------------
// Captive portal HTML — SSID, password, calendar URL.
// ---------------------------------------------------------------------------
static const char WM_PORTAL_HTML[] PROGMEM =
  "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
  "<title>MeetingNotifier setup</title>"
  "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;"
  "padding:24px;max-width:380px;margin:auto}h1{font-size:1.2rem}"
  "label{display:block;margin-top:10px;font-size:.9rem;color:#aaa}"
  "input{width:100%;padding:8px;margin:4px 0;border-radius:6px;border:1px solid #444;"
  "background:#222;color:#eee;box-sizing:border-box}"
  "small{color:#888;font-size:.8rem;display:block;margin-top:2px}"
  "button{width:100%;padding:10px;margin-top:14px;border-radius:6px;border:0;"
  "background:#4af;color:#000;font-weight:600;font-size:1rem}</style>"
  "</head><body><h1>MeetingNotifier setup</h1>"
  "<form method=POST action=/save>"
  "<label>WiFi network<input name=ssid required></label>"
  "<label>WiFi password<input name=pass type=password></label>"
  "<label>Calendar URL<input name=cal required>"
  "<small>Apps Script /exec URL, iCal secret URL, or local helper "
  "http://host:8080/</small></label>"
  "<button type=submit>Save and reboot</button></form></body></html>";

inline void wmHandleRoot()  { wmHttp.send_P(200, "text/html", WM_PORTAL_HTML); }

inline void wmHandleSave() {
  String ssid = wmHttp.arg("ssid");
  String pass = wmHttp.arg("pass");
  String cal  = wmHttp.arg("cal");
  if (ssid.length() == 0 || cal.length() == 0) {
    wmHttp.send(400, "text/plain", "ssid and cal required");
    return;
  }
  wmSaveCreds(ssid, pass);
  cfgSetCalendarUrl(cal);
  wmHttp.send(200, "text/html",
              "<html><body style='font-family:sans-serif;background:#111;color:#eee;"
              "padding:24px'>Saved. Rebooting…</body></html>");
  delay(400);
  ESP.restart();
}

inline void wmHandleNotFound() {
  wmHttp.sendHeader("Location", "/", true);
  wmHttp.send(302, "text/plain", "");
}

inline void wmStartPortal() {
  if (wmPortalActive) return;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WM_AP_SSID);
  IPAddress ip = WiFi.softAPIP();
  log_i("Captive portal up: SSID=%s ip=%s", WM_AP_SSID, ip.toString().c_str());

  wmDns.start(WM_DNS_PORT, "*", ip);
  wmHttp.on("/", wmHandleRoot);
  wmHttp.on("/save", HTTP_POST, wmHandleSave);
  wmHttp.onNotFound(wmHandleNotFound);
  wmHttp.begin();
  wmPortalActive = true;
  wmConnected    = false;
}

inline void wmPortalLoop() {
  if (!wmPortalActive) return;
  wmDns.processNextRequest();
  wmHttp.handleClient();
}

// ---------------------------------------------------------------------------
// Connect with stored creds. Returns true on success.
// ---------------------------------------------------------------------------
inline bool wmConnectStored() {
  String ssid, pass;
  wmLoadCreds(ssid, pass);
  if (ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("meetingnotifier");
  WiFi.setAutoReconnect(true);   // keep trying if the AP drops us mid-session
  WiFi.persistent(true);         // mirror creds into the WiFi stack's own flash
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  for (;;) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) break;
    // Bail early on definitive failures so we drop to the captive portal
    // within ~2 s of a wrong password instead of waiting out the full
    // timeout. WL_CONNECT_FAILED covers AUTH_FAIL/AUTH_EXPIRE; the SSID
    // checks cover networks the device can't see.
    if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) {
      log_w("WiFi gave up early: status=%d (likely bad password or hidden SSID)", (int)s);
      break;
    }
    if (millis() - start >= WM_CONNECT_MS) break;
    delay(200);
  }
  wmConnected = (WiFi.status() == WL_CONNECTED);
  if (wmConnected) {
    log_i("WiFi connected: ip=%s rssi=%d",
          WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
  return wmConnected;
}

// ---------------------------------------------------------------------------
// Detect location/timezone via ip-api.com and set system TZ.
//
// Arduino-ESP32's newlib doesn't bundle a `tzdata` blob, so passing an IANA
// name like "America/Sao_Paulo" to configTzTime() silently falls back to
// UTC. Instead we ask ip-api for its `offset` field (seconds east of UTC,
// already DST-adjusted to the current moment) and feed that to configTime()
// directly. DST transitions won't auto-update — but every device reboot
// re-fetches the current offset, so a power-cycle self-heals.
// ---------------------------------------------------------------------------
inline bool wmConfigureTimeViaIpApi() {
  if (!wifiOnline()) return false;

  HTTPClient http;
  http.setTimeout(5000);
  // Explicit fields so we get `offset` (not in default response).
  if (!http.begin("http://ip-api.com/json?fields=status,offset,timezone,city,countryCode")) return false;

  int code = http.GET();
  if (code != 200) {
    log_w("ip-api GET failed: %d", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    log_w("ip-api JSON parse failed: %s", err.c_str());
    return false;
  }

  long       offset_sec = doc["offset"]   | 0;     // seconds east of UTC
  const char* tz        = doc["timezone"] | "";    // IANA name (for logging)
  if (tz[0]) {
    strncpy(wmTzBuf, tz, sizeof(wmTzBuf) - 1);
    wmTzBuf[sizeof(wmTzBuf) - 1] = 0;
  }

  log_i("ip-api: city=%s country=%s tz=%s offset=%lds",
        (const char*)(doc["city"] | ""),
        (const char*)(doc["countryCode"] | ""),
        wmTzBuf, offset_sec);

  configTime(offset_sec, 0, "pool.ntp.org", "time.nist.gov");
  return true;
}

inline bool wmAwaitNtp(uint32_t timeoutMs = 10000) {
  uint32_t start = millis();
  time_t now = 0;
  while (millis() - start < timeoutMs) {
    now = time(nullptr);
    if (now > 1700000000) {
      log_i("NTP sync ok: %ld", (long)now);
      return true;
    }
    delay(200);
  }
  log_w("NTP sync timed out");
  return false;
}

inline const char* wmTimezone() { return wmTzBuf; }

// ---------------------------------------------------------------------------
// Boot-time entry point. We need BOTH WiFi creds and a calendar URL —
// missing either drops us into the captive portal.
// ---------------------------------------------------------------------------
inline void wifiBoot() {
  bool wifiOK = wmConnectStored();
  if (wifiOK) {
    wmConfigureTimeViaIpApi();
    wmAwaitNtp();
  }
  if (!wifiOK || !cfgHasCalendarUrl()) {
    log_w("setup needed (wifi=%d, url=%d) → portal",
          (int)wifiOK, (int)cfgHasCalendarUrl());
    wmStartPortal();
  }
}
