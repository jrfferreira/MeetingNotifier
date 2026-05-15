#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#include "ui.h"

// ---------------------------------------------------------------------------
// Configured by user — paste your Apps Script /exec URL here.
// The script source lives in docs / repo README.
// ---------------------------------------------------------------------------
#ifndef CALENDAR_URL
#define CALENDAR_URL "https://script.google.com/macros/s/REPLACE_ME/exec"
#endif

// ---------------------------------------------------------------------------
// UTC epoch from broken-down UTC components (no TZ side-effects).
// ---------------------------------------------------------------------------
inline time_t calToEpochUTC(int year, int mon, int day,
                            int hour, int min, int sec) {
  static const int dim[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
  long days = 0;
  for (int y = 1970; y < year; y++) {
    days += 365;
    if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) days++;
  }
  for (int M = 1; M < mon; M++) {
    days += dim[M - 1];
    if (M == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days++;
  }
  days += day - 1;
  return (time_t)days * 86400 + (time_t)hour * 3600 +
         (time_t)min * 60 + (time_t)sec;
}

// Parse ISO 8601:  "YYYY-MM-DDTHH:mm:ss[.fff](Z|±HH:MM)" → epoch (UTC).
// Apps Script's Date.toISOString() emits the "...sssZ" variant.
// Returns 0 on failure.
inline time_t calParseISO8601(const char* s) {
  if (!s || !s[0]) return 0;

  int year, mon, day, hour, min, sec;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d",
             &year, &mon, &day, &hour, &min, &sec) != 6) {
    return 0;
  }
  time_t utc = calToEpochUTC(year, mon, day, hour, min, sec);

  // Walk past "YYYY-MM-DDTHH:mm:ss" (19 chars).
  const char* p = s + 19;
  if (strlen(s) < 19) return utc;

  // Optional ".fractional"
  if (*p == '.') {
    p++;
    while (*p >= '0' && *p <= '9') p++;
  }

  if (*p == 0 || *p == 'Z' || *p == 'z') return utc;
  if (*p == '+' || *p == '-') {
    char sign = *p++;
    int tz_h = 0, tz_m = 0;
    if (sscanf(p, "%d:%d", &tz_h, &tz_m) < 1) return utc;
    long offset = (long)tz_h * 3600 + (long)tz_m * 60;
    if (sign == '+') utc -= offset;   // wall-clock ahead of UTC → subtract
    else             utc += offset;
  }
  return utc;
}

// ---------------------------------------------------------------------------
// Fetch + parse — populates the caller's MeetingData in place.
// Returns true if the HTTP call + JSON parse succeeded.
// ---------------------------------------------------------------------------
inline bool calendarFetch(MeetingData& out) {
  WiFiClientSecure client;
  client.setInsecure();      // Google Apps Script cert is well-known; skip CA bundle for v1

  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, CALENDAR_URL)) {
    log_w("calendar: http.begin failed");
    return false;
  }
  // Apps Script redirects to googleusercontent.com — follow it.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != 200) {
    log_w("calendar: GET %d", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<1536> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    log_w("calendar: parse failed: %s", err.c_str());
    return false;
  }

  const char* status = doc["status"] | "";
  strncpy(out.status, status, sizeof(out.status) - 1);
  out.status[sizeof(out.status) - 1] = 0;

  if (strcmp(status, "clear") == 0) {
    out.valid          = true;
    out.title[0]       = 0;
    out.location[0]    = 0;
    out.startTime      = 0;
    out.endTime        = 0;
    out.remainingToday = 0;
    return true;
  }

  const char* title    = doc["title"]    | "";
  const char* loc      = doc["location"] | "";
  const char* startIso = doc["start"]    | "";
  const char* endIso   = doc["end"]      | "";

  strncpy(out.title,    title, sizeof(out.title) - 1);
  out.title[sizeof(out.title) - 1] = 0;
  strncpy(out.location, loc,   sizeof(out.location) - 1);
  out.location[sizeof(out.location) - 1] = 0;

  out.startTime      = calParseISO8601(startIso);
  out.endTime        = calParseISO8601(endIso);
  out.remainingToday = doc["remaining_today"] | 0;
  out.valid          = (out.startTime > 0);

  log_i("calendar: status=%s title='%s' start=%ld end=%ld remaining=%d",
        out.status, out.title,
        (long)out.startTime, (long)out.endTime, out.remainingToday);
  return true;
}
