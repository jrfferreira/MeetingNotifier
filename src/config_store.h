#pragma once

#include <Arduino.h>
#include <Preferences.h>

// NVS-backed configuration. All firmware modules talk to NVS through
// these helpers so we have one place to evolve the schema.
//
// History note: this used to use namespace "wifi" — close enough to
// names used internally by various WiFi-stack components that there
// was a risk of collision (e.g. the WiFi.persistent(true) machinery
// writes to its own NVS partition, but had a chance of stomping on
// keys here). Renamed to "mnotif" to be unambiguously ours; the boot
// path runs cfgMigrateLegacyNamespace() once to copy any pre-existing
// data over from the old name.

#ifndef CFG_NS
#define CFG_NS  "mnotif"
#endif
#define CFG_NS_LEGACY  "wifi"

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
  if (!p.begin(CFG_NS, false)) {
    log_e("cfg: failed to open NVS for write");
    return;
  }
  size_t wrote = p.putString(CFG_KEY_CAL_URL, url);
  p.end();
  cfgInvalidateCache();
  log_w("cfg: saved calendar URL (%u bytes)", (unsigned)wrote);
}

inline bool cfgHasCalendarUrl() {
  return cfgGetCalendarUrl().length() > 0;
}

// ---------------------------------------------------------------------------
// One-time migration from the pre-rename "wifi" namespace. Call from
// setup() before any cfgGet*() reads. Idempotent: skips when the new
// namespace already has a calendar URL.
// ---------------------------------------------------------------------------
inline void cfgMigrateLegacyNamespace() {
  // Cheap precheck: if the new namespace already has a URL, nothing to do.
  {
    Preferences p;
    p.begin(CFG_NS, true);
    bool already = p.getString(CFG_KEY_CAL_URL, "").length() > 0;
    p.end();
    if (already) return;
  }

  // Read whatever's in the legacy namespace.
  String oldSsid, oldPass, oldCal;
  {
    Preferences old;
    if (!old.begin(CFG_NS_LEGACY, true)) return;
    oldSsid = old.getString(CFG_KEY_SSID,    "");
    oldPass = old.getString(CFG_KEY_PASS,    "");
    oldCal  = old.getString(CFG_KEY_CAL_URL, "");
    old.end();
  }
  if (oldSsid.length() == 0 && oldCal.length() == 0) return;

  // Copy whatever was there into the new namespace.
  Preferences neu;
  if (!neu.begin(CFG_NS, false)) {
    log_w("cfg: legacy data found but couldn't open '%s' for write", CFG_NS);
    return;
  }
  if (oldSsid.length() > 0) neu.putString(CFG_KEY_SSID,    oldSsid);
  if (oldPass.length() > 0) neu.putString(CFG_KEY_PASS,    oldPass);
  if (oldCal.length()  > 0) neu.putString(CFG_KEY_CAL_URL, oldCal);
  neu.end();

  log_w("cfg: migrated config from legacy '%s' namespace to '%s'",
        CFG_NS_LEGACY, CFG_NS);
}

