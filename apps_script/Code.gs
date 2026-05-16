/**
 * MeetingNotifier — Google Apps Script web app.
 *
 * Deploy:
 *   1. script.google.com → New project, paste this file.
 *   2. Deploy → New deployment → Web app.
 *   3. Execute as: Me   |   Who has access: Anyone.
 *   4. Copy the /exec URL into CALENDAR_URL in the firmware.
 *
 * Response shapes:
 *   { status: "clear" }                          // nothing left today
 *   { status: "upcoming", title, start, end, location, next_title, next_start, remaining_today }
 *   { status: "in_meeting", title, start, end, location, next_title, next_start, remaining_today }
 */

function doGet() {
  const now = new Date();
  const eod = new Date();
  eod.setHours(23, 59, 59, 0);

  const cal = CalendarApp.getDefaultCalendar();
  const events = cal.getEvents(now, eod).filter(e => !e.isAllDayEvent());

  if (events.length === 0) {
    return json({ status: 'clear' });
  }

  const current = events.find(e =>
    e.getStartTime() <= now && e.getEndTime() >= now
  );
  const next = events.find(e => e.getStartTime() > now);
  const focus = current || next;

  return json({
    status:          current ? 'in_meeting' : 'upcoming',
    title:           focus.getTitle(),
    start:           focus.getStartTime().toISOString(),
    end:             focus.getEndTime().toISOString(),
    location:        focus.getLocation() || '',
    // The event AFTER the focus one, when there is a current meeting AND a
    // queued upcoming. The firmware's K1 "dismiss current" feature uses
    // these to advance the display past a meeting that ended early.
    next_title:      current && next ? next.getTitle() : '',
    next_start:      current && next ? next.getStartTime().toISOString() : '',
    next_end:        current && next ? next.getEndTime().toISOString()   : '',
    remaining_today: events.filter(e => e.getStartTime() > now).length,
  });
}

function json(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
