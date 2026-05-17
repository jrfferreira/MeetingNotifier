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
// IcalParseSink — a Stream that HTTPClient::writeToStream() pumps the
// decoded body into one byte at a time. We accumulate physical iCal lines,
// re-fold continuations on the fly (RFC 5545 §3.1), and feed each logical
// line to ical::handleLine() as it's complete. Working memory is the line
// buffer (default 512 B).
//
// Using writeToStream() rather than reading the raw socket means HTTPClient
// does Content-Length / chunked-transfer / EOF detection for us — no more
// stream-idle-timeout games on keep-alive connections.
// ---------------------------------------------------------------------------
class IcalParseSink : public Stream {
 public:
  IcalParseSink(ical::Event* events, int& eventCount, time_t now, time_t eod)
    : events_(events), eventCount_(eventCount), now_(now), eod_(eod) {}

  // --- Stream::write hooks (the half we actually use) ----------------------
  size_t write(uint8_t b) override {
    process((char)b);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t size) override {
    for (size_t i = 0; i < size; i++) process((char)buf[i]);
    return size;
  }

  // --- Stream::read interface (unused, must be present) -------------------
  int  available() override { return 0;  }
  int  read()      override { return -1; }
  int  peek()      override { return -1; }
  void flush()     override {}

  // Call after writeToStream() returns to emit any trailing pending line.
  void finalize() {
    if (pendingNewline_ || lineLen_ > 0) emitLine();
  }

  size_t bytesIn()    const { return bytesIn_; }
  bool   inEvent()    const { return inEvent_; }

 private:
  void process(char c) {
    bytesIn_++;
    if (c == '\r') return;

    if (pendingNewline_) {
      pendingNewline_ = false;
      // RFC 5545 §3.1 line folding: a CRLF + (space|tab) inside a logical
      // line means "ignore the CRLF and the whitespace and keep going on
      // the same line".
      if (c == ' ' || c == '\t') return;
      emitLine();
      // fall through and treat c as the first char of the next line
    }

    if (c == '\n') {
      pendingNewline_ = true;
      return;
    }

    if (lineLen_ + 1 < sizeof(line_)) {
      line_[lineLen_++] = c;
    }
    // else: overflow → silently truncate; SUMMARY/LOCATION values longer
    // than CAL_ICAL_LINE_BUF lose their tail but the event is still kept.
  }

  void emitLine() {
    line_[lineLen_] = 0;
    ical::handleLine(line_, cur_, inEvent_, curAllDay_, curHasStart_, curHasEnd_,
                     events_, eventCount_, now_, eod_);
    lineLen_ = 0;
  }

  ical::Event* events_;
  int&         eventCount_;
  time_t       now_;
  time_t       eod_;

  ical::Event  cur_         = {};
  bool         inEvent_     = false;
  bool         curAllDay_   = false;
  bool         curHasStart_ = false;
  bool         curHasEnd_   = false;

  char         line_[CAL_ICAL_LINE_BUF];
  size_t       lineLen_        = 0;
  bool         pendingNewline_ = false;
  size_t       bytesIn_        = 0;
};

// ---------------------------------------------------------------------------
// Stream-parse the iCal feed via HTTPClient::writeToStream(). 512 B of
// working memory regardless of feed size; HTTPClient handles chunked
// transfer encoding and end-of-body detection.
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

  log_d("ical: streaming, free heap=%u, content-length=%d",
        ESP.getFreeHeap(), http.getSize());

  // Today's window in local time.
  time_t now = time(nullptr);
  struct tm eodTm;
  localtime_r(&now, &eodTm);
  eodTm.tm_hour = 23; eodTm.tm_min = 59; eodTm.tm_sec = 59;
  eodTm.tm_isdst = -1;
  time_t eod = mktime(&eodTm);

  ical::Event events[CAL_ICAL_MAX_EVENTS];
  int eventCount = 0;

  IcalParseSink sink(events, eventCount, now, eod);
  int written = http.writeToStream(&sink);
  sink.finalize();
  http.end();

  if (written <= 0) {
    log_w("ical: writeToStream returned %d (read %u bytes)",
          written, (unsigned)sink.bytesIn());
    return false;
  }
  log_w("ical: parsed %u bytes, %d event(s) in today's window",
        (unsigned)sink.bytesIn(), eventCount);

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
    out.nextTitle[0]   = 0;
    out.nextStartTime  = 0;
    out.nextEndTime    = 0;
    log_w("ical: %d event(s) total, none current/future → clear", eventCount);
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

  // Surface the next-after-focus event when there is one. K1 dismiss uses
  // these to skip the focus and jump to whatever's next.
  if (currentIdx >= 0 && nextIdx >= 0) {
    strncpy(out.nextTitle, events[nextIdx].title, sizeof(out.nextTitle) - 1);
    out.nextTitle[sizeof(out.nextTitle) - 1] = 0;
    out.nextStartTime = events[nextIdx].start;
    out.nextEndTime   = events[nextIdx].end;
  } else {
    out.nextTitle[0]  = 0;
    out.nextStartTime = 0;
    out.nextEndTime   = 0;
  }
  out.valid = true;

  log_w("ical: status=%s title='%s' start=%ld end=%ld remaining=%d",
        out.status, out.title,
        (long)out.startTime, (long)out.endTime, out.remainingToday);
  return true;
}
