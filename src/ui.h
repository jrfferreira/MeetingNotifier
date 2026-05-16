#pragma once

#include <Arduino.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Pin map. Two boards supported, switched at compile time:
//
//   default (standalone ST7789 240×240 module, e.g. for ESP32-C3-DevKitM-1)
//   BOARD_SPOTPEAR_C3_144 — Spotpear / Fikra "ESP32-C3 com Display Integrado
//     1.44 Polegadas", confirmed via seller-supplied schematic. 128×128
//     ST7735, backlight hardwired to 3V3 (no PWM).
// ---------------------------------------------------------------------------
#if defined(BOARD_SPOTPEAR_C3_144)
  #define PIN_SCLK  3
  #define PIN_MOSI  4
  #define PIN_CS    2
  #define PIN_DC    0      // Confirmed via Spotpear's reference User_Setup.h;
                           // earlier reads of the schematic were off by 10
  #define PIN_RST   5
  #define PIN_BLK   -1     // hardwired via R11/L12_1, no GPIO control
  #define TFT_W     128
  #define TFT_H     128
  #define USE_ST7735_144
#else
  #define PIN_SCLK  6
  #define PIN_MOSI  7
  #define PIN_CS    10
  #define PIN_DC    3
  #define PIN_RST   4
  #define PIN_BLK   5
  #define TFT_W     240
  #define TFT_H     240
#endif

// ---------------------------------------------------------------------------
// Backlight PWM
// ---------------------------------------------------------------------------
#define BLK_PWM_CHANNEL  0
#define BLK_PWM_FREQ     5000
#define BLK_PWM_BITS     8
#define BLK_FULL         255
#define BLK_DIM          51        // ~20 %

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define POLL_INTERVAL_MS         30000UL    // calendar fetch cadence (30 s)
#define REFRESH_INTERVAL_MS       5000UL    // ambient display tick (5 s)
#define REFRESH_FAST_INTERVAL_MS  1000UL    // active-state display tick (1 s)
#define SOON_THRESHOLD_SECS         900     // 15 min
#define IMMINENT_THRESHOLD_SECS     300     // 5 min
#define DIM_TIMEOUT_MS           60000UL    // 1 min idle → dim
#define SLEEP_TIMEOUT_MS        300000UL    // 5 min idle → light sleep

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
typedef enum {
  STATE_LOADING,
  STATE_NO_WIFI,
  STATE_IDLE,
  STATE_SOON,
  STATE_IMMINENT,
  STATE_IN_MEETING,
  STATE_ALL_CLEAR,
} DisplayState;

inline const char* stateName(DisplayState s) {
  switch (s) {
    case STATE_LOADING:    return "LOADING";
    case STATE_NO_WIFI:    return "NO_WIFI";
    case STATE_IDLE:       return "IDLE";
    case STATE_SOON:       return "SOON";
    case STATE_IMMINENT:   return "IMMINENT";
    case STATE_IN_MEETING: return "IN_MEETING";
    case STATE_ALL_CLEAR:  return "ALL_CLEAR";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Meeting data (shared across modules)
// ---------------------------------------------------------------------------
struct MeetingData {
  bool   valid;
  char   status[16];     // "clear" | "upcoming" | "in_meeting"
  char   title[64];
  char   location[128];
  time_t startTime;
  time_t endTime;
  int    remainingToday;
};

// ---------------------------------------------------------------------------
// RGB565 helpers
// ---------------------------------------------------------------------------
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Palette per state — { bg, primary, accent }
struct Palette {
  uint16_t bg;
  uint16_t primary;
  uint16_t accent;
};

inline Palette paletteFor(DisplayState s) {
  switch (s) {
    case STATE_IDLE:       return { rgb565(0x0a,0x0a,0x0f), rgb565(0xe0,0xe0,0xff), rgb565(0x40,0x40,0x60) };
    case STATE_SOON:       return { rgb565(0x0f,0x0a,0x06), rgb565(0xf4,0xa2,0x61), rgb565(0x60,0x40,0x20) };
    case STATE_IMMINENT:   return { rgb565(0x0f,0x08,0x08), rgb565(0xff,0x70,0x70), rgb565(0x60,0x10,0x10) };
    case STATE_IN_MEETING: return { rgb565(0x06,0x0f,0x0a), rgb565(0x60,0xd0,0x90), rgb565(0x20,0x50,0x30) };
    case STATE_ALL_CLEAR:  return { rgb565(0x08,0x08,0x0e), rgb565(0x90,0x90,0xc0), rgb565(0x28,0x28,0x40) };
    case STATE_NO_WIFI:    return { rgb565(0x0a,0x0a,0x0a), rgb565(0x40,0x40,0x40), rgb565(0x28,0x28,0x28) };
    case STATE_LOADING:
    default:               return { rgb565(0x08,0x08,0x0e), rgb565(0x80,0x80,0xa0), rgb565(0x28,0x28,0x40) };
  }
}

// ---------------------------------------------------------------------------
// Time helpers — write 24h strings into caller-owned buffers.
// ---------------------------------------------------------------------------
inline void fmtClock(char* buf, size_t n, time_t ts = 0) {
  if (ts == 0) ts = time(nullptr);
  struct tm t;
  localtime_r(&ts, &t);
  strftime(buf, n, "%H:%M", &t);
}

inline void fmtDate(char* buf, size_t n, time_t ts = 0) {
  if (ts == 0) ts = time(nullptr);
  struct tm t;
  localtime_r(&ts, &t);
  strftime(buf, n, "%a %b %d", &t);
}

inline void fmtCountdown(char* buf, size_t n, long secs) {
  if (secs < 0) secs = 0;
  long m = secs / 60;
  long s = secs % 60;
  snprintf(buf, n, "%02ld:%02ld", m, s);
}

inline void fmtElapsedHMS(char* buf, size_t n, long secs) {
  if (secs < 0) secs = 0;
  long h = secs / 3600;
  long m = (secs % 3600) / 60;
  long s = secs % 60;
  if (h > 0) snprintf(buf, n, "%ld:%02ld:%02ld", h, m, s);
  else       snprintf(buf, n, "%02ld:%02ld", m, s);
}
