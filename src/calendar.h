#pragma once

// Dispatcher — selects the calendar source at compile time.
//
//   default                 → calendar_json.h
//     CALENDAR_URL points at a JSON endpoint (Apps Script web app or the
//     local Python helper in tools/calendar_helper.py). Both speak the
//     same { status, title, start, end, ... } shape.
//
//   -DUSE_ICAL_SOURCE      → calendar_ical.h
//     CALENDAR_URL points at a Google Calendar "Secret address in iCal
//     format". The device fetches the .ics directly and parses it on-board.
//
// Both expose:  bool calendarFetch(MeetingData& out);

#include "ui.h"

#if defined(USE_ICAL_SOURCE)
#  include "calendar_ical.h"
#else
#  include "calendar_json.h"
#endif
