#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
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

#ifndef CAL_ICAL_LINE_BUF
#define CAL_ICAL_LINE_BUF 512
#endif

namespace ical {

struct Event {
  time_t start;
  time_t end;
  char   title[64];
  char   location[128];
};

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

inline const char* matchProp(const char* line, const char* name) {
  size_t nlen = strlen(name);
  if (strncmp(line, name, nlen) != 0) return nullptr;
  char c = line[nlen];
  if (c != ':' && c != ';') return nullptr;
  const char* colon = strchr(line, ':');
  return colon ? colon + 1 : nullptr;
}

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

// ---------------------------------------------------------------------------
// Process one fully-unfolded iCal logical line. Updates the per-event state
// (cur, inEvent, …) and appends to events[] when an END:VEVENT closes a
// timed event that falls in today's window.
// ---------------------------------------------------------------------------
inline void handleLine(char* line,
                       Event& cur, bool& inEvent,
                       bool& curAllDay, bool& curHasStart, bool& curHasEnd,
                       Event* events, int& eventCount,
                       time_t now, time_t eod) {
  if (strcmp(line, "BEGIN:VEVENT") == 0) {
    inEvent     = true;
    cur         = {};
    curAllDay   = false;
    curHasStart = false;
    curHasEnd   = false;
    return;
  }
  if (strcmp(line, "END:VEVENT") == 0) {
    inEvent = false;
    if (curHasStart && curHasEnd && !curAllDay &&
        cur.end >= now && cur.start <= eod &&
        eventCount < CAL_ICAL_MAX_EVENTS) {
      events[eventCount++] = cur;
    }
    return;
  }
  if (!inEvent) return;

  const char* v;
  if ((v = matchProp(line, "DTSTART"))) {
    bool ad;
    if (parseValue(v, cur.start, ad)) {
      curHasStart = true;
      if (ad) curAllDay = true;
    }
  } else if ((v = matchProp(line, "DTEND"))) {
    bool ad;
    if (parseValue(v, cur.end, ad)) {
      curHasEnd = true;
      if (ad) curAllDay = true;
    }
  } else if ((v = matchProp(line, "SUMMARY"))) {
    copyText(cur.title, sizeof(cur.title), v);
  } else if ((v = matchProp(line, "LOCATION"))) {
    copyText(cur.location, sizeof(cur.location), v);
  }
}

}  // namespace ical

// ---------------------------------------------------------------------------
// Stream-parse the iCal feed instead of buffering it. Working memory stays
// at ~CAL_ICAL_LINE_BUF (512 B by default) regardless of feed size, so a
// 100 KB calendar export no longer OOMs the C3.
//
// iCal RFC 5545 §3.1 "line folding": physical lines can be split with
// CRLF + (space|tab). We re-assemble logical lines on the fly by peeking
// after each \n.
// ---------------------------------------------------------------------------
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

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    log_w("ical: no stream");
    http.end();
    return false;
  }

  log_d("ical: streaming, free heap=%u", ESP.getFreeHeap());

  time_t now = time(nullptr);
  struct tm eodTm;
  localtime_r(&now, &eodTm);
  eodTm.tm_hour = 23; eodTm.tm_min = 59; eodTm.tm_sec = 59;
  eodTm.tm_isdst = -1;
  time_t eod = mktime(&eodTm);

  ical::Event events[CAL_ICAL_MAX_EVENTS];
  int eventCount = 0;

  bool inEvent     = false;
  ical::Event cur  = {};
  bool curAllDay   = false;
  bool curHasStart = false;
  bool curHasEnd   = false;

  char  line[CAL_ICAL_LINE_BUF];
  size_t lineLen   = 0;
  bool   truncated = false;
  uint32_t lastRead = millis();
  const uint32_t streamIdleMs = 5000;

  while (true) {
    if (!stream->available()) {
      if (!stream->connected() && !stream->available()) break;
      if (millis() - lastRead > streamIdleMs) {
        log_w("ical: stream idle timeout");
        break;
      }
      delay(1);
      continue;
    }
    lastRead = millis();
    int c = stream->read();
    if (c < 0) break;

    if (c == '\r') continue;

    if (c == '\n') {
      // Peek the next char to detect line folding. WiFiClient's peek()
      // doesn't auto-buffer, so wait briefly if nothing's there yet.
      uint32_t peekStart = millis();
      while (!stream->available() && stream->connected() &&
             millis() - peekStart < 200) {
        delay(1);
      }
      int peeked = stream->available() ? stream->peek() : -1;
      if (peeked == ' ' || peeked == '\t') {
        stream->read();   // consume the continuation marker
        continue;         // keep accumulating onto the same logical line
      }

      // Logical line complete.
      line[lineLen] = 0;
      if (truncated) {
        log_d("ical: line truncated to %u bytes", (unsigned)lineLen);
      }
      ical::handleLine(line, cur, inEvent, curAllDay, curHasStart, curHasEnd,
                       events, eventCount, now, eod);
      lineLen   = 0;
      truncated = false;
      continue;
    }

    if (lineLen + 1 < sizeof(line)) {
      line[lineLen++] = (char)c;
    } else {
      // Overflow: keep the leading chunk (good enough for SUMMARY/LOCATION
      // truncation; long DTSTART/DTEND lines never approach 512 bytes).
      truncated = true;
    }
  }
  // Flush trailing logical line if the feed didn't end with a newline.
  if (lineLen > 0) {
    line[lineLen] = 0;
    ical::handleLine(line, cur, inEvent, curAllDay, curHasStart, curHasEnd,
                     events, eventCount, now, eod);
  }

  http.end();

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
    log_i("ical: %d event(s), none current/future → clear", eventCount);
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
