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

// ---------------------------------------------------------------------------
// Persistent WiFi credentials live in NVS namespace "wifi".
// Captive portal SSID is "MeetingNotifier-Setup", no password.
// ---------------------------------------------------------------------------
#define WM_NAMESPACE     "wifi"
#define WM_KEY_SSID      "ssid"
#define WM_KEY_PASS      "pass"

#define WM_AP_SSID       "MeetingNotifier-Setup"
#define WM_CONNECT_MS    20000UL
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
// NVS helpers
// ---------------------------------------------------------------------------
inline void wmLoadCreds(String& ssid, String& pass) {
  wmPrefs.begin(WM_NAMESPACE, true);
  ssid = wmPrefs.getString(WM_KEY_SSID, "");
  pass = wmPrefs.getString(WM_KEY_PASS, "");
  wmPrefs.end();
}

inline void wmSaveCreds(const String& ssid, const String& pass) {
  wmPrefs.begin(WM_NAMESPACE, false);
  wmPrefs.putString(WM_KEY_SSID, ssid);
  wmPrefs.putString(WM_KEY_PASS, pass);
  wmPrefs.end();
}

// ---------------------------------------------------------------------------
// Captive portal HTML — minimal SSID/pass form.
// ---------------------------------------------------------------------------
static const char WM_PORTAL_HTML[] PROGMEM =
  "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
  "<title>MeetingNotifier</title>"
  "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;"
  "padding:24px;max-width:340px;margin:auto}h1{font-size:1.2rem}"
  "input{width:100%;padding:8px;margin:4px 0;border-radius:6px;border:1px solid #444;"
  "background:#222;color:#eee}button{width:100%;padding:10px;margin-top:12px;"
  "border-radius:6px;border:0;background:#4af;color:#000;font-weight:600}</style>"
  "</head><body><h1>MeetingNotifier setup</h1>"
  "<form method=POST action=/save>"
  "<label>SSID<input name=ssid required></label>"
  "<label>Password<input name=pass type=password></label>"
  "<button type=submit>Save and reboot</button></form></body></html>";

inline void wmHandleRoot()  { wmHttp.send_P(200, "text/html", WM_PORTAL_HTML); }

inline void wmHandleSave() {
  String ssid = wmHttp.arg("ssid");
  String pass = wmHttp.arg("pass");
  if (ssid.length() == 0) {
    wmHttp.send(400, "text/plain", "ssid required");
    return;
  }
  wmSaveCreds(ssid, pass);
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
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WM_CONNECT_MS) {
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
// Falls back to UTC if the call fails.
// ---------------------------------------------------------------------------
inline bool wmConfigureTimeViaIpApi() {
  if (!wifiOnline()) return false;

  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin("http://ip-api.com/json")) return false;

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

  const char* tz = doc["timezone"] | "";
  if (tz[0] == 0) return false;
  strncpy(wmTzBuf, tz, sizeof(wmTzBuf) - 1);
  wmTzBuf[sizeof(wmTzBuf) - 1] = 0;

  log_i("ip-api: city=%s country=%s tz=%s",
        (const char*)(doc["city"] | ""),
        (const char*)(doc["countryCode"] | ""),
        wmTzBuf);

  // POSIX-style IANA name doesn't load DST rules on its own; setenv("TZ", ...)
  // with an IANA name only works because esp-idf bundles tzdata.
  configTzTime(wmTzBuf, "pool.ntp.org", "time.nist.gov");
  return true;
}

inline bool wmAwaitNtp(uint32_t timeoutMs = 10000) {
  uint32_t start = millis();
  time_t now = 0;
  while (millis() - start < timeoutMs) {
    now = time(nullptr);
    if (now > 1700000000) {     // some plausible epoch after 2023
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
// Boot-time entry point: try stored creds, otherwise start portal.
// ---------------------------------------------------------------------------
inline void wifiBoot() {
  if (wmConnectStored()) {
    wmConfigureTimeViaIpApi();
    wmAwaitNtp();
    return;
  }
  log_w("no stored creds or connect failed → portal");
  wmStartPortal();
}
