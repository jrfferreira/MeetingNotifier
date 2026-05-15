#pragma once

#include <Arduino.h>
#include <Preferences.h>

// NVS-backed configuration. All firmware modules talk to NVS through
// these helpers so we have one place to evolve the schema.
//
// Namespace "wifi" preserves credentials stored by earlier firmware that
// only knew about ssid/pass.

#ifndef CFG_NS
#define CFG_NS  "wifi"
#endif

#define CFG_KEY_SSID     "ssid"
#define CFG_KEY_PASS     "pass"
#define CFG_KEY_CAL_URL  "cal"

// Read-only accessor. Returns the NVS-stored URL if present; otherwise
// falls back to the compile-time CALENDAR_URL macro (so legacy builds
// with `-DCALENDAR_URL='"…"'` keep working). The placeholder URL counts
// as "not set" so first-boot still triggers the portal.
inline String cfgGetCalendarUrl() {
  Preferences p;
  p.begin(CFG_NS, true);
  String url = p.getString(CFG_KEY_CAL_URL, "");
  p.end();

  if (url.length() == 0) {
#ifdef CALENDAR_URL
    url = CALENDAR_URL;
#endif
  }
  if (url.indexOf("REPLACE_ME") >= 0) url = "";
  return url;
}

inline void cfgSetCalendarUrl(const String& url) {
  Preferences p;
  p.begin(CFG_NS, false);
  p.putString(CFG_KEY_CAL_URL, url);
  p.end();
}

inline bool cfgHasCalendarUrl() {
  return cfgGetCalendarUrl().length() > 0;
}
