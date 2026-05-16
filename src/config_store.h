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

// In-RAM cache of the calendar URL so the fetcher doesn't have to open
// NVS on every poll. Initialised lazily on first read; the captive
// portal save handler invalidates it (cfgSetCalendarUrl() resets the
// cached value alongside the NVS write).
static bool   g_cfgUrlCached = false;
static String g_cfgUrlCache;

inline void cfgInvalidateCache() {
  g_cfgUrlCached = false;
  g_cfgUrlCache  = String();
}

inline String cfgGetCalendarUrl() {
  if (g_cfgUrlCached) return g_cfgUrlCache;

  Preferences p;
  p.begin(CFG_NS, true);
  String url = p.getString(CFG_KEY_CAL_URL, "");
  p.end();

  if (url.length() == 0) {
#ifdef CALENDAR_URL
    url = CALENDAR_URL;
#endif
  }
  // Sentinel scrubbing: a default-build CALENDAR_URL contains "REPLACE_ME"
  // and should be treated as "not set" so first-boot drops to the portal.
  if (url.indexOf("REPLACE_ME") >= 0) url = "";

  g_cfgUrlCache  = url;
  g_cfgUrlCached = true;
  return g_cfgUrlCache;
}

inline void cfgSetCalendarUrl(const String& url) {
  Preferences p;
  p.begin(CFG_NS, false);
  p.putString(CFG_KEY_CAL_URL, url);
  p.end();
  cfgInvalidateCache();
}

inline bool cfgHasCalendarUrl() {
  return cfgGetCalendarUrl().length() > 0;
}
