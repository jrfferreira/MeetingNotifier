#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include "ui.h"

// ---------------------------------------------------------------------------
// Display instance — write-only SPI, no MISO.
// ---------------------------------------------------------------------------
static Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, PIN_CS, PIN_DC, PIN_RST);

// Cached fields so we only repaint the dynamic regions on each tick.
static char     g_lastBig[16]    = {0};
static char     g_lastBottom[96] = {0};
static uint16_t g_lastBigColor   = 0;

inline void backlightInit() {
  ledcSetup(BLK_PWM_CHANNEL, BLK_PWM_FREQ, BLK_PWM_BITS);
  ledcAttachPin(PIN_BLK, BLK_PWM_CHANNEL);
  ledcWrite(BLK_PWM_CHANNEL, BLK_FULL);
}

inline void backlightSet(uint8_t v) {
  ledcWrite(BLK_PWM_CHANNEL, v);
}

inline void displayInit() {
  pinMode(PIN_BLK, OUTPUT);
  digitalWrite(PIN_BLK, LOW);

  SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
  tft.init(TFT_W, TFT_H, SPI_MODE0);
  tft.setSPISpeed(40000000UL);   // 40 MHz — ST7789 handles it
  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);

  backlightInit();
}

// ---------------------------------------------------------------------------
// Centered-text helper — relies on the currently selected font.
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

// Truncate `src` into `dst` with an ellipsis when longer than `maxLen` chars.
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

// ---------------------------------------------------------------------------
// Dot grid — ambient background for IDLE.
// ---------------------------------------------------------------------------
inline void drawDotGrid(uint16_t color) {
  for (int y = 12; y < TFT_H; y += 16) {
    for (int x = 12; x < TFT_W; x += 16) {
      tft.drawPixel(x, y, color);
    }
  }
}

// Concentric pulse rings — ambient background for IMMINENT.
inline void drawPulseRings(uint16_t color, int8_t phase) {
  int cx = TFT_W / 2;
  int cy = 110;
  for (int i = 0; i < 3; i++) {
    int r = 30 + i * 35 + (phase % 8);
    if (r > 0 && r < TFT_W) tft.drawCircle(cx, cy, r, color);
  }
}


// ---------------------------------------------------------------------------
// State renderers.
// Each takes `fresh = true` on state change → full repaint.
// Otherwise only the dynamic regions (countdown number + bottom strap) repaint.
// ---------------------------------------------------------------------------

inline void renderBigNumber(int16_t cx, int16_t topY, const char* text,
                            uint16_t fg, uint16_t bg, bool force) {
  if (!force && strcmp(g_lastBig, text) == 0 && g_lastBigColor == fg) return;
  // Erase a rectangle large enough for FreeSansBold24pt7b (~40 px tall, scale to 56)
  tft.fillRect(0, topY - 6, TFT_W, 64, bg);
  drawCenteredAt(cx, topY, text, fg, &FreeSansBold24pt7b);
  strncpy(g_lastBig, text, sizeof(g_lastBig) - 1);
  g_lastBig[sizeof(g_lastBig) - 1] = 0;
  g_lastBigColor = fg;
}

inline void renderBottomStrap(int16_t topY, const char* text, uint16_t fg,
                              uint16_t bg, bool force) {
  if (!force && strcmp(g_lastBottom, text) == 0) return;
  tft.fillRect(0, topY - 4, TFT_W, 32, bg);
  drawCenteredAt(TFT_W / 2, topY, text, fg, &FreeSans9pt7b);
  strncpy(g_lastBottom, text, sizeof(g_lastBottom) - 1);
  g_lastBottom[sizeof(g_lastBottom) - 1] = 0;
}

inline void clearCachedRegions() {
  g_lastBig[0]    = 0;
  g_lastBottom[0] = 0;
  g_lastBigColor  = 0;
}

inline void drawIdle(const MeetingData& m, bool fresh) {
  Palette p = paletteFor(STATE_IDLE);
  if (fresh) {
    tft.fillScreen(p.bg);
    drawDotGrid(p.accent);
    clearCachedRegions();
  }

  char clock[8];
  fmtClock(clock, sizeof(clock));
  renderBigNumber(TFT_W / 2, 80, clock, p.primary, p.bg, fresh);

  if (fresh) {
    char date[16];
    fmtDate(date, sizeof(date));
    drawCenteredAt(TFT_W / 2, 152, date, p.accent, &FreeSans9pt7b);
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
}

inline void drawSoon(const MeetingData& m, bool fresh) {
  Palette p = paletteFor(STATE_SOON);
  if (fresh) {
    tft.fillScreen(p.bg);
    drawCenteredAt(TFT_W / 2, 32, "Meeting soon", p.accent, &FreeSans9pt7b);
    clearCachedRegions();
  }

  long secs = (long)(m.startTime - time(nullptr));
  char cd[8];
  fmtCountdown(cd, sizeof(cd), secs);
  renderBigNumber(TFT_W / 2, 80, cd, p.primary, p.bg, fresh);

  if (fresh) {
    char title[40];
    truncate(title, sizeof(title), m.title, 22);
    drawCenteredAt(TFT_W / 2, 168, title, p.primary, &FreeSans12pt7b);

    char start[8], end[8];
    fmtClock(start, sizeof(start), m.startTime);
    fmtClock(end,   sizeof(end),   m.endTime);
    char range[24];
    snprintf(range, sizeof(range), "%s - %s", start, end);
    drawCenteredAt(TFT_W / 2, 200, range, p.accent, &FreeSans9pt7b);
  }
}

inline void drawImminent(const MeetingData& m, bool fresh, uint8_t pulsePhase) {
  Palette p = paletteFor(STATE_IMMINENT);
  if (fresh) {
    tft.fillScreen(p.bg);
    clearCachedRegions();
    drawPulseRings(p.accent, pulsePhase);

    // JOIN NOW badge
    int bw = 130, bh = 26, bx = (TFT_W - bw) / 2, by = 14;
    tft.drawRoundRect(bx, by, bw, bh, 6, p.primary);
    drawCenteredAt(TFT_W / 2, by + 6, "JOIN NOW", p.primary, &FreeSans12pt7b);
  }

  long secs = (long)(m.startTime - time(nullptr));
  char cd[8];
  fmtCountdown(cd, sizeof(cd), secs);
  renderBigNumber(TFT_W / 2, 84, cd, p.primary, p.bg, fresh);

  if (fresh) {
    char title[40];
    truncate(title, sizeof(title), m.title, 22);
    drawCenteredAt(TFT_W / 2, 168, title, p.primary, &FreeSans12pt7b);

    char loc[40];
    truncate(loc, sizeof(loc), m.location[0] ? m.location : "—", 26);
    drawCenteredAt(TFT_W / 2, 200, loc, p.accent, &FreeSans9pt7b);
  }
}

inline void drawInMeeting(const MeetingData& m, bool fresh) {
  Palette p = paletteFor(STATE_IN_MEETING);
  if (fresh) {
    tft.fillScreen(p.bg);
    clearCachedRegions();
    drawCenteredAt(TFT_W / 2, 60, "In meeting", p.accent, &FreeSans9pt7b);
  }

  time_t now = time(nullptr);
  long total   = (long)(m.endTime - m.startTime);
  long elapsed = (long)(now - m.startTime);
  if (total <= 0) total = 1;
  if (elapsed < 0) elapsed = 0;
  if (elapsed > total) elapsed = total;

  // Progress bar across the top — refreshed each tick.
  const int barX = 20, barY = 28, barW = TFT_W - 40, barH = 6;
  int filled = (int)((long)barW * elapsed / total);
  tft.fillRect(barX,          barY, barW,   barH, p.accent);
  tft.fillRect(barX,          barY, filled, barH, p.primary);
  tft.drawCircle(barX,          barY + barH / 2, 4, p.primary);
  tft.drawCircle(barX + barW,   barY + barH / 2, 4, p.primary);

  char elapsedStr[12];
  fmtElapsedHMS(elapsedStr, sizeof(elapsedStr), elapsed);
  renderBigNumber(TFT_W / 2, 96, elapsedStr, p.primary, p.bg, fresh);

  if (fresh) {
    char title[40];
    truncate(title, sizeof(title), m.title, 22);
    drawCenteredAt(TFT_W / 2, 178, title, p.primary, &FreeSans12pt7b);

    char endTxt[16], endLine[24];
    fmtClock(endTxt, sizeof(endTxt), m.endTime);
    snprintf(endLine, sizeof(endLine), "ends %s", endTxt);
    drawCenteredAt(TFT_W / 2, 210, endLine, p.accent, &FreeSans9pt7b);
  }
}

inline void drawAllClear(bool fresh) {
  Palette p = paletteFor(STATE_ALL_CLEAR);
  if (fresh) {
    tft.fillScreen(p.bg);
    clearCachedRegions();

    // Dim check mark
    int cx = TFT_W / 2;
    tft.drawLine(cx - 18, 64,  cx - 4,  78, p.accent);
    tft.drawLine(cx - 18, 65,  cx - 4,  79, p.accent);
    tft.drawLine(cx - 4,  78,  cx + 20, 54, p.accent);
    tft.drawLine(cx - 4,  79,  cx + 20, 55, p.accent);
  }

  char clock[8];
  fmtClock(clock, sizeof(clock));
  renderBigNumber(TFT_W / 2, 110, clock, p.primary, p.bg, fresh);

  if (fresh) {
    drawCenteredAt(TFT_W / 2, 192, "no more meetings", p.accent, &FreeSans9pt7b);
  }
}

inline void drawNoWifi(bool fresh) {
  Palette p = paletteFor(STATE_NO_WIFI);
  if (!fresh) return;     // static screen, no dynamic content
  tft.fillScreen(p.bg);
  drawCenteredAt(TFT_W / 2, 60,  "no wifi",                 p.primary, &FreeSansBold18pt7b);
  drawCenteredAt(TFT_W / 2, 120, "join the network:",        p.accent,  &FreeSans9pt7b);
  drawCenteredAt(TFT_W / 2, 144, "MeetingNotifier-Setup",    p.primary, &FreeSans9pt7b);
  drawCenteredAt(TFT_W / 2, 180, "open http://192.168.4.1",  p.accent,  &FreeSans9pt7b);
}

inline void drawLoading(bool fresh) {
  Palette p = paletteFor(STATE_LOADING);
  if (!fresh) return;
  tft.fillScreen(p.bg);
  drawCenteredAt(TFT_W / 2, 100, "MeetingNotifier", p.primary, &FreeSans12pt7b);
  drawCenteredAt(TFT_W / 2, 140, "loading…",        p.accent,  &FreeSans9pt7b);
}
