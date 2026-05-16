#pragma once

#include <Adafruit_GFX.h>
#include <SPI.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include "ui.h"

// ---------------------------------------------------------------------------
// Display driver — ST7735 144 green tab for the Spotpear integrated board,
// ST7789 for the standalone 240×240 module.
// ---------------------------------------------------------------------------
#if defined(USE_ST7735_144)
  #include <Adafruit_ST7735.h>
  // Hardware SPI — ESP32-C3 routes the SPI peripheral to any GPIOs via
  // the GPIO matrix, so non-default pins are fine and far faster than
  // bit-banging 32 KB through software SPI.
  static Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, PIN_CS, PIN_DC, PIN_RST);
  // Big-number font has to fit horizontally on a 128 px panel.
  #define FONT_BIG     (&FreeSansBold18pt7b)
  #define FONT_LABEL   (&FreeSans9pt7b)
  #define FONT_DETAIL  ((const GFXfont*)nullptr)   // default 5x7
#else
  #include <Adafruit_ST7789.h>
  static Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, PIN_CS, PIN_DC, PIN_RST);
  #define FONT_BIG     (&FreeSansBold24pt7b)
  #define FONT_LABEL   (&FreeSans12pt7b)
  #define FONT_DETAIL  (&FreeSans9pt7b)
#endif

static char     g_lastBig[16]    = {0};
static char     g_lastBottom[96] = {0};
static uint16_t g_lastBigColor   = 0;

// ---------------------------------------------------------------------------
// Backlight: PWM when wired to a GPIO; no-op when hardwired to 3V3.
// ---------------------------------------------------------------------------
inline void backlightInit() {
#if PIN_BLK >= 0
  ledcSetup(BLK_PWM_CHANNEL, BLK_PWM_FREQ, BLK_PWM_BITS);
  ledcAttachPin(PIN_BLK, BLK_PWM_CHANNEL);
  ledcWrite(BLK_PWM_CHANNEL, BLK_FULL);
#endif
}

inline void backlightSet(uint8_t v) {
#if PIN_BLK >= 0
  ledcWrite(BLK_PWM_CHANNEL, v);
#else
  (void)v;
#endif
}

inline void displayInit() {
#if PIN_BLK >= 0
  pinMode(PIN_BLK, OUTPUT);
  digitalWrite(PIN_BLK, LOW);
#endif

  SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
#if defined(USE_ST7735_144)
  tft.initR(INITR_144GREENTAB);
  tft.setSPISpeed(27000000UL);     // ST7735 is happy up to ~32 MHz
  tft.setRotation(2);
#else
  tft.init(TFT_W, TFT_H, SPI_MODE0);
  tft.setSPISpeed(40000000UL);
  tft.setRotation(2);
#endif
  tft.fillScreen(ST77XX_BLACK);

  backlightInit();
}

// ---------------------------------------------------------------------------
// Centered-text helper.
// ---------------------------------------------------------------------------
inline void drawCenteredAt(int16_t cx, int16_t topY,
                           const char* text, uint16_t color,
                           const GFXfont* font = nullptr, uint8_t size = 1) {
  tft.setFont(font);
  tft.setTextSize(size);
  tft.setTextColor(color);
  int16_t bx, by;
  uint16_t bw, bh;
  tft.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  tft.setCursor(cx - bw / 2 - bx, topY - by);
  tft.print(text);
}

inline void truncate(char* dst, size_t dstSize, const char* src, size_t maxLen) {
  size_t srcLen = strlen(src);
  if (srcLen <= maxLen || dstSize < 4) {
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = 0;
    return;
  }
  size_t cap = (maxLen > dstSize - 4) ? dstSize - 4 : maxLen;
  memcpy(dst, src, cap);
  dst[cap]     = '.';
  dst[cap + 1] = '.';
  dst[cap + 2] = '.';
  dst[cap + 3] = 0;
}

inline void drawDotGrid(uint16_t color) {
  for (int y = 12; y < TFT_H; y += 16) {
    for (int x = 12; x < TFT_W; x += 16) {
      tft.drawPixel(x, y, color);
    }
  }
}

inline void clearCachedRegions() {
  g_lastBig[0]    = 0;
  g_lastBottom[0] = 0;
  g_lastBigColor  = 0;
}

inline void renderBigNumber(int16_t cx, int16_t topY, const char* text,
                            uint16_t fg, uint16_t bg, bool force) {
  if (!force && strcmp(g_lastBig, text) == 0 && g_lastBigColor == fg) return;
  // Big enough rect to wipe FreeSansBold24pt7b (~40 px) on 240; ~28 px on 128.
#if defined(USE_ST7735_144)
  tft.fillRect(0, topY - 4, TFT_W, 32, bg);
#else
  tft.fillRect(0, topY - 6, TFT_W, 64, bg);
#endif
  drawCenteredAt(cx, topY, text, fg, FONT_BIG);
  strncpy(g_lastBig, text, sizeof(g_lastBig) - 1);
  g_lastBig[sizeof(g_lastBig) - 1] = 0;
  g_lastBigColor = fg;
}

inline void renderBottomStrap(int16_t topY, const char* text, uint16_t fg,
                              uint16_t bg, bool force) {
  if (!force && strcmp(g_lastBottom, text) == 0) return;
  tft.fillRect(0, topY - 4, TFT_W, 24, bg);
  drawCenteredAt(TFT_W / 2, topY, text, fg, FONT_DETAIL);
  strncpy(g_lastBottom, text, sizeof(g_lastBottom) - 1);
  g_lastBottom[sizeof(g_lastBottom) - 1] = 0;
}

// ---------------------------------------------------------------------------
// State renderers. Layouts diverge between 240×240 (rich) and 128×128
// (compact) — both kept in this file so the small-panel path stays
// honest about the same data, just truncated for space.
// ---------------------------------------------------------------------------

inline void drawIdle(const MeetingData& m, bool fresh) {
  Palette p = paletteFor(STATE_IDLE);
  if (fresh) {
    tft.fillScreen(p.bg);
#if !defined(USE_ST7735_144)
    drawDotGrid(p.accent);
#endif
    clearCachedRegions();
  }

  char clock[8];
  fmtClock(clock, sizeof(clock));

#if defined(USE_ST7735_144)
  renderBigNumber(TFT_W / 2, 28, clock, p.primary, p.bg, fresh);
  if (fresh) {
    char date[16];
    fmtDate(date, sizeof(date));
    drawCenteredAt(TFT_W / 2, 72, date, p.accent, FONT_LABEL);
    tft.drawFastHLine(20, 90, TFT_W - 40, p.accent);
  }
  char bottom[64] = "";
  if (m.valid && strcmp(m.status, "clear") != 0) {
    char hhmm[8];
    fmtClock(hhmm, sizeof(hhmm), m.startTime);
    char title[16];
    truncate(title, sizeof(title), m.title, 11);
    snprintf(bottom, sizeof(bottom), "%s  %s", hhmm, title);
  } else {
    snprintf(bottom, sizeof(bottom), "no upcoming");
  }
  renderBottomStrap(100, bottom, p.primary, p.bg, fresh);
#else
  renderBigNumber(TFT_W / 2, 80, clock, p.primary, p.bg, fresh);
  if (fresh) {
    char date[16];
    fmtDate(date, sizeof(date));
    drawCenteredAt(TFT_W / 2, 152, date, p.accent, FONT_DETAIL);
    tft.drawFastHLine(40, 188, TFT_W - 80, p.accent);
  }
  char bottom[96] = "";
  if (m.valid && strcmp(m.status, "clear") != 0) {
    char hhmm[8];
    fmtClock(hhmm, sizeof(hhmm), m.startTime);
    char title[40];
    truncate(title, sizeof(title), m.title, 20);
    snprintf(bottom, sizeof(bottom), "Next  %s  %s", title, hhmm);
  } else {
    snprintf(bottom, sizeof(bottom), "no upcoming meetings");
  }
  renderBottomStrap(204, bottom, p.primary, p.bg, fresh);
#endif
}

inline void drawSoon(const MeetingData& m, bool fresh) {
  Palette p = paletteFor(STATE_SOON);
  if (fresh) {
    tft.fillScreen(p.bg);
#if defined(USE_ST7735_144)
    drawCenteredAt(TFT_W / 2, 4, "Meeting soon", p.accent, FONT_DETAIL);
#else
    drawCenteredAt(TFT_W / 2, 32, "Meeting soon", p.accent, FONT_DETAIL);
#endif
    clearCachedRegions();
  }

  long secs = (long)(m.startTime - time(nullptr));
  char cd[8];
  fmtCountdown(cd, sizeof(cd), secs);

#if defined(USE_ST7735_144)
  renderBigNumber(TFT_W / 2, 30, cd, p.primary, p.bg, fresh);
  if (fresh) {
    char title[16];
    truncate(title, sizeof(title), m.title, 14);
    drawCenteredAt(TFT_W / 2, 78, title, p.primary, FONT_LABEL);

    char start[8], end[8];
    fmtClock(start, sizeof(start), m.startTime);
    fmtClock(end,   sizeof(end),   m.endTime);
    char range[24];
    snprintf(range, sizeof(range), "%s-%s", start, end);
    drawCenteredAt(TFT_W / 2, 104, range, p.accent, FONT_DETAIL);
  }
#else
  renderBigNumber(TFT_W / 2, 80, cd, p.primary, p.bg, fresh);
  if (fresh) {
    char title[40];
    truncate(title, sizeof(title), m.title, 22);
    drawCenteredAt(TFT_W / 2, 168, title, p.primary, FONT_LABEL);

    char start[8], end[8];
    fmtClock(start, sizeof(start), m.startTime);
    fmtClock(end,   sizeof(end),   m.endTime);
    char range[24];
    snprintf(range, sizeof(range), "%s - %s", start, end);
    drawCenteredAt(TFT_W / 2, 200, range, p.accent, FONT_DETAIL);
  }
#endif
}

inline void drawImminent(const MeetingData& m, bool fresh, uint8_t /*pulsePhase*/) {
  Palette p = paletteFor(STATE_IMMINENT);
  if (fresh) {
    tft.fillScreen(p.bg);
    clearCachedRegions();
#if defined(USE_ST7735_144)
    int bw = 88, bh = 18, bx = (TFT_W - bw) / 2, by = 4;
    tft.drawRoundRect(bx, by, bw, bh, 4, p.primary);
    drawCenteredAt(TFT_W / 2, by + 4, "JOIN NOW", p.primary, FONT_DETAIL);
#else
    int bw = 130, bh = 26, bx = (TFT_W - bw) / 2, by = 14;
    tft.drawRoundRect(bx, by, bw, bh, 6, p.primary);
    drawCenteredAt(TFT_W / 2, by + 6, "JOIN NOW", p.primary, FONT_LABEL);
#endif
  }

  long secs = (long)(m.startTime - time(nullptr));
  char cd[8];
  fmtCountdown(cd, sizeof(cd), secs);

#if defined(USE_ST7735_144)
  renderBigNumber(TFT_W / 2, 34, cd, p.primary, p.bg, fresh);
  if (fresh) {
    char title[16];
    truncate(title, sizeof(title), m.title, 14);
    drawCenteredAt(TFT_W / 2, 80, title, p.primary, FONT_LABEL);
    char loc[20];
    truncate(loc, sizeof(loc), m.location[0] ? m.location : "-", 18);
    drawCenteredAt(TFT_W / 2, 106, loc, p.accent, FONT_DETAIL);
  }
#else
  renderBigNumber(TFT_W / 2, 84, cd, p.primary, p.bg, fresh);
  if (fresh) {
    char title[40];
    truncate(title, sizeof(title), m.title, 22);
    drawCenteredAt(TFT_W / 2, 168, title, p.primary, FONT_LABEL);
    char loc[40];
    truncate(loc, sizeof(loc), m.location[0] ? m.location : "-", 26);
    drawCenteredAt(TFT_W / 2, 200, loc, p.accent, FONT_DETAIL);
  }
#endif
}

inline void drawInMeeting(const MeetingData& m, bool fresh) {
  Palette p = paletteFor(STATE_IN_MEETING);
  if (fresh) {
    tft.fillScreen(p.bg);
    clearCachedRegions();
#if defined(USE_ST7735_144)
    drawCenteredAt(TFT_W / 2, 24, "In meeting", p.accent, FONT_DETAIL);
#else
    drawCenteredAt(TFT_W / 2, 60, "In meeting", p.accent, FONT_DETAIL);
#endif
  }

  time_t now = time(nullptr);
  long total   = (long)(m.endTime - m.startTime);
  long elapsed = (long)(now - m.startTime);
  if (total <= 0) total = 1;
  if (elapsed < 0) elapsed = 0;
  if (elapsed > total) elapsed = total;

#if defined(USE_ST7735_144)
  const int barX = 10, barY = 6, barW = TFT_W - 20, barH = 4;
#else
  const int barX = 20, barY = 28, barW = TFT_W - 40, barH = 6;
#endif
  int filled = (int)((long)barW * elapsed / total);
  tft.fillRect(barX,        barY, barW,   barH, p.accent);
  tft.fillRect(barX,        barY, filled, barH, p.primary);
  tft.drawCircle(barX,        barY + barH / 2, 3, p.primary);
  tft.drawCircle(barX + barW, barY + barH / 2, 3, p.primary);

  char elapsedStr[12];
  fmtElapsedHMS(elapsedStr, sizeof(elapsedStr), elapsed);

#if defined(USE_ST7735_144)
  renderBigNumber(TFT_W / 2, 38, elapsedStr, p.primary, p.bg, fresh);
  if (fresh) {
    char title[16];
    truncate(title, sizeof(title), m.title, 14);
    drawCenteredAt(TFT_W / 2, 84, title, p.primary, FONT_LABEL);
    char endTxt[8], endLine[16];
    fmtClock(endTxt, sizeof(endTxt), m.endTime);
    snprintf(endLine, sizeof(endLine), "ends %s", endTxt);
    drawCenteredAt(TFT_W / 2, 110, endLine, p.accent, FONT_DETAIL);
  }
#else
  renderBigNumber(TFT_W / 2, 96, elapsedStr, p.primary, p.bg, fresh);
  if (fresh) {
    char title[40];
    truncate(title, sizeof(title), m.title, 22);
    drawCenteredAt(TFT_W / 2, 178, title, p.primary, FONT_LABEL);
    char endTxt[16], endLine[24];
    fmtClock(endTxt, sizeof(endTxt), m.endTime);
    snprintf(endLine, sizeof(endLine), "ends %s", endTxt);
    drawCenteredAt(TFT_W / 2, 210, endLine, p.accent, FONT_DETAIL);
  }
#endif
}

inline void drawAllClear(bool fresh) {
  Palette p = paletteFor(STATE_ALL_CLEAR);
  if (fresh) {
    tft.fillScreen(p.bg);
    clearCachedRegions();
    int cx = TFT_W / 2;
#if defined(USE_ST7735_144)
    int cy = 18;
    tft.drawLine(cx - 10, cy,     cx - 2, cy + 8,  p.accent);
    tft.drawLine(cx - 10, cy + 1, cx - 2, cy + 9,  p.accent);
    tft.drawLine(cx - 2,  cy + 8, cx + 12, cy - 6, p.accent);
    tft.drawLine(cx - 2,  cy + 9, cx + 12, cy - 5, p.accent);
#else
    tft.drawLine(cx - 18, 64, cx - 4, 78, p.accent);
    tft.drawLine(cx - 18, 65, cx - 4, 79, p.accent);
    tft.drawLine(cx - 4,  78, cx + 20, 54, p.accent);
    tft.drawLine(cx - 4,  79, cx + 20, 55, p.accent);
#endif
  }

  char clock[8];
  fmtClock(clock, sizeof(clock));
#if defined(USE_ST7735_144)
  renderBigNumber(TFT_W / 2, 50, clock, p.primary, p.bg, fresh);
  if (fresh) {
    drawCenteredAt(TFT_W / 2, 100, "no more meetings", p.accent, FONT_DETAIL);
  }
#else
  renderBigNumber(TFT_W / 2, 110, clock, p.primary, p.bg, fresh);
  if (fresh) {
    drawCenteredAt(TFT_W / 2, 192, "no more meetings", p.accent, FONT_DETAIL);
  }
#endif
}

inline void drawNoWifi(bool fresh) {
  Palette p = paletteFor(STATE_NO_WIFI);
  if (!fresh) return;
  tft.fillScreen(p.bg);
#if defined(USE_ST7735_144)
  drawCenteredAt(TFT_W / 2, 8,  "no wifi",                  p.primary, FONT_LABEL);
  drawCenteredAt(TFT_W / 2, 36, "join network:",            p.accent,  FONT_DETAIL);
  drawCenteredAt(TFT_W / 2, 56, "MeetingNotifier-Setup",    p.primary, FONT_DETAIL);
  drawCenteredAt(TFT_W / 2, 84, "then open",                p.accent,  FONT_DETAIL);
  drawCenteredAt(TFT_W / 2, 100, "192.168.4.1",             p.primary, FONT_DETAIL);
#else
  drawCenteredAt(TFT_W / 2, 60,  "no wifi",                 p.primary, &FreeSansBold18pt7b);
  drawCenteredAt(TFT_W / 2, 120, "join the network:",       p.accent,  FONT_DETAIL);
  drawCenteredAt(TFT_W / 2, 144, "MeetingNotifier-Setup",   p.primary, FONT_DETAIL);
  drawCenteredAt(TFT_W / 2, 180, "open http://192.168.4.1", p.accent,  FONT_DETAIL);
#endif
}

inline void drawNoConnection(bool fresh) {
  Palette p = paletteFor(STATE_NO_CONNECTION);
  if (fresh) {
    tft.fillScreen(p.bg);
    clearCachedRegions();
#if defined(USE_ST7735_144)
    drawCenteredAt(TFT_W / 2, 4,  "no connection", p.primary, FONT_LABEL);
#else
    drawCenteredAt(TFT_W / 2, 32, "no connection", p.primary, FONT_LABEL);
#endif
  }

  // Clock keeps ticking so it's obvious from across the room that the
  // device is alive but its calendar data is stale.
  char clock[8];
  fmtClock(clock, sizeof(clock));
#if defined(USE_ST7735_144)
  renderBigNumber(TFT_W / 2, 30, clock, p.primary, p.bg, fresh);
  if (fresh) {
    drawCenteredAt(TFT_W / 2, 84,  "can't reach", p.accent, FONT_DETAIL);
    drawCenteredAt(TFT_W / 2, 96,  "calendar",    p.accent, FONT_DETAIL);
    drawCenteredAt(TFT_W / 2, 112, "retrying...", p.accent, FONT_DETAIL);
  }
#else
  renderBigNumber(TFT_W / 2, 80, clock, p.primary, p.bg, fresh);
  if (fresh) {
    drawCenteredAt(TFT_W / 2, 168, "can't reach the calendar", p.accent, FONT_DETAIL);
    drawCenteredAt(TFT_W / 2, 200, "retrying...",              p.accent, FONT_DETAIL);
  }
#endif
}

inline void drawLoading(bool fresh) {
  Palette p = paletteFor(STATE_LOADING);
  if (!fresh) return;
  tft.fillScreen(p.bg);
#if defined(USE_ST7735_144)
  drawCenteredAt(TFT_W / 2, 50,  "MeetingNotifier", p.primary, FONT_DETAIL);
  drawCenteredAt(TFT_W / 2, 70,  "loading...",      p.accent,  FONT_DETAIL);
#else
  drawCenteredAt(TFT_W / 2, 100, "MeetingNotifier", p.primary, FONT_LABEL);
  drawCenteredAt(TFT_W / 2, 140, "loading...",      p.accent,  FONT_DETAIL);
#endif
}
