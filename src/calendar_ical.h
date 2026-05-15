#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "config_store.h"

// CALENDAR_URL: read from NVS at fetch time (set via captive portal).
// Compile-time `-DCALENDAR_URL='"…"'` is honoured as a fallback.

#ifndef CAL_ICAL_MAX_EVENTS
#define CAL_ICAL_MAX_EVENTS 16
#endif

namespace ical {

struct Event {
  time_t start;
  time_t end;
  char   title[64];
  char   location[128];
};

// UTC epoch from broken-down UTC components.
inline time_t epochUTC(int Y, int M, int D, int h, int m, int s) {
  static const int dim[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
  long days = 0;
  for (int y = 1970; y < Y; y++) {
    days += 365;
    if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) days++;
  }
  for (int mo = 1; mo < M; mo++) {
    days += dim[mo - 1];
    if (mo == 2 && ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0)) days++;
  }
  days += D - 1;
  return (time_t)days * 86400 + (time_t)h * 3600 + (time_t)m * 60 + s;
}

// Parse "YYYYMMDD", "YYYYMMDDTHHMMSS", or "YYYYMMDDTHHMMSSZ".
// Sets isAllDay on the date-only form.
// Non-Z timed values are interpreted as LOCAL time using the device's
// configured timezone — pragmatic for work calendars in the user's home tz.
inline bool parseValue(const char* s, time_t& out, bool& isAllDay) {
  isAllDay = false;
  size_t n = strlen(s);
  if (n < 8) return false;

  int Y, M, D, h = 0, m = 0, sec = 0;
  if (sscanf(s, "%4d%2d%2d", &Y, &M, &D) != 3) return false;

  if (n == 8) {
    isAllDay = true;
    out = epochUTC(Y, M, D, 0, 0, 0);
    return true;
  }
  if (n < 15 || s[8] != 'T') return false;
  if (sscanf(s + 9, "%2d%2d%2d", &h, &m, &sec) != 3) return false;

  bool isUtc = (n >= 16 && s[15] == 'Z');
  if (isUtc) {
    out = epochUTC(Y, M, D, h, m, sec);
    return true;
  }

  struct tm t = {};
  t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D;
  t.tm_hour = h;        t.tm_min = m;     t.tm_sec = sec;
  t.tm_isdst = -1;
  out = mktime(&t);
  return out != (time_t)-1;
}

// iCal "line folding": continuation lines start with SPACE or TAB and join
// onto the previous line. Mutate `body` in place to undo it.
inline void unfoldInPlace(char* body) {
  char* read  = body;
  char* write = body;
  while (*read) {
    if (*read == '\r') { read++; continue; }
    if (*read == '\n' && (read[1] == ' ' || read[1] == '\t')) {
      read += 2;
      continue;
    }
    *write++ = *read++;
  }
  *write = 0;
}

// Match "NAME" or "NAME;…". Returns pointer to the value (after ':') or null.
inline const char* matchProp(const char* line, const char* name) {
  size_t nlen = strlen(name);
  if (strncmp(line, name, nlen) != 0) return nullptr;
  char c = line[nlen];
  if (c != ':' && c != ';') return nullptr;
  const char* colon = strchr(line, ':');
  return colon ? colon + 1 : nullptr;
}

// Copy with TEXT-value unescaping: \\ \, \; \n \N.
inline void copyText(char* dst, size_t dstSize, const char* src) {
  size_t i = 0;
  while (*src && i + 1 < dstSize) {
    if (*src == '\\' && src[1]) {
      char e = src[1];
      if      (e == 'n' || e == 'N')                dst[i++] = ' ';
      else if (e == ',' || e == ';' || e == '\\')   dst[i++] = e;
      else                                          dst[i++] = e;
      src += 2;
    } else {
      dst[i++] = *src++;
    }
  }
  dst[i] = 0;
}

}  // namespace ical

inline bool calendarFetch(MeetingData& out) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(15000);
  String url = cfgGetCalendarUrl();
  if (url.length() == 0) {
    log_w("ical: no URL configured");
    return false;
  }
  if (!http.begin(client, url)) {
    log_w("ical: http.begin failed");
    return false;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != 200) {
    log_w("ical: GET %d", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();
  if (body.length() == 0) {
    log_w("ical: empty body");
    return false;
  }

  // Heap-buffer for in-place unfolding (String's underlying buffer is
  // mutable but we don't depend on that here).
  size_t blen = body.length();
  char* buf = (char*)malloc(blen + 1);
  if (!buf) { log_w("ical: oom %u", (unsigned)blen); return false; }
  memcpy(buf, body.c_str(), blen + 1);
  body = String();      // free Arduino-String copy early
  ical::unfoldInPlace(buf);

  // Today's window in local time.
  time_t now = time(nullptr);
  struct tm eodTm;
  localtime_r(&now, &eodTm);
  eodTm.tm_hour = 23; eodTm.tm_min = 59; eodTm.tm_sec = 59;
  eodTm.tm_isdst = -1;
  time_t eod = mktime(&eodTm);

  ical::Event events[CAL_ICAL_MAX_EVENTS];
  int eventCount = 0;

  bool inEvent = false;
  ical::Event cur = {};
  bool curAllDay = false, curHasStart = false, curHasEnd = false;

  char* line = buf;
  while (*line) {
    char* nl = strchr(line, '\n');
    if (nl) *nl = 0;

    if (strcmp(line, "BEGIN:VEVENT") == 0) {
      inEvent = true;
      cur = {};
      curAllDay = curHasStart = curHasEnd = false;
    } else if (strcmp(line, "END:VEVENT") == 0) {
      inEvent = false;
      if (curHasStart && curHasEnd && !curAllDay &&
          cur.end >= now && cur.start <= eod &&
          eventCount < CAL_ICAL_MAX_EVENTS) {
        events[eventCount++] = cur;
      }
    } else if (inEvent) {
      const char* v;
      if      ((v = ical::matchProp(line, "DTSTART"))) {
        bool ad;
        if (ical::parseValue(v, cur.start, ad)) {
          curHasStart = true;
          if (ad) curAllDay = true;
        }
      } else if ((v = ical::matchProp(line, "DTEND"))) {
        bool ad;
        if (ical::parseValue(v, cur.end, ad)) {
          curHasEnd = true;
          if (ad) curAllDay = true;
        }
      } else if ((v = ical::matchProp(line, "SUMMARY"))) {
        ical::copyText(cur.title, sizeof(cur.title), v);
      } else if ((v = ical::matchProp(line, "LOCATION"))) {
        ical::copyText(cur.location, sizeof(cur.location), v);
      }
    }

    if (!nl) break;
    line = nl + 1;
  }

  free(buf);

  // Sort by start time (insertion sort — N ≤ 16).
  for (int i = 1; i < eventCount; i++) {
    ical::Event t = events[i];
    int j = i - 1;
    while (j >= 0 && events[j].start > t.start) {
      events[j + 1] = events[j];
      j--;
    }
    events[j + 1] = t;
  }

  int currentIdx = -1, nextIdx = -1;
  for (int i = 0; i < eventCount; i++) {
    if (events[i].start <= now && events[i].end >= now) {
      if (currentIdx < 0) currentIdx = i;
    } else if (events[i].start > now && nextIdx < 0) {
      nextIdx = i;
    }
  }

  if (currentIdx < 0 && nextIdx < 0) {
    out.valid = true;
    strncpy(out.status, "clear", sizeof(out.status) - 1);
    out.status[sizeof(out.status) - 1] = 0;
    out.title[0] = 0; out.location[0] = 0;
    out.startTime = 0; out.endTime = 0;
    out.remainingToday = 0;
    log_i("ical: %d event(s), all in past → clear", eventCount);
    return true;
  }

  const ical::Event& focus = events[currentIdx >= 0 ? currentIdx : nextIdx];
  const char* status = currentIdx >= 0 ? "in_meeting" : "upcoming";
  strncpy(out.status, status, sizeof(out.status) - 1);
  out.status[sizeof(out.status) - 1] = 0;
  strncpy(out.title, focus.title, sizeof(out.title) - 1);
  out.title[sizeof(out.title) - 1] = 0;
  strncpy(out.location, focus.location, sizeof(out.location) - 1);
  out.location[sizeof(out.location) - 1] = 0;
  out.startTime = focus.start;
  out.endTime   = focus.end;
  out.remainingToday = 0;
  for (int i = 0; i < eventCount; i++) {
    if (events[i].start > now) out.remainingToday++;
  }
  out.valid = true;

  log_i("ical: %d event(s); status=%s title='%s' start=%ld end=%ld",
        eventCount, out.status, out.title,
        (long)out.startTime, (long)out.endTime);
  return true;
}
