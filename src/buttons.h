#pragma once

#include <Arduino.h>
#include "ui.h"

// Button handling — debounced edge detector with short/long press
// distinction. Active-low (button pulls GPIO to GND).
//
// Compiled out entirely when PIN_K1 < 0, so the standalone-module
// build has zero footprint here.

#ifndef BTN_DEBOUNCE_MS
#define BTN_DEBOUNCE_MS         30
#endif
#ifndef BTN_SHORT_PRESS_MAX_MS
#define BTN_SHORT_PRESS_MAX_MS  1500
#endif
#ifndef BTN_LONG_PRESS_MIN_MS
#define BTN_LONG_PRESS_MIN_MS   3000
#endif

typedef void (*ButtonCb)();

static ButtonCb gK1ShortCb = nullptr;
static ButtonCb gK1LongCb  = nullptr;

#if PIN_K1 >= 0
static bool     gK1Pressed     = false;
static uint32_t gK1DownMs      = 0;
static uint32_t gK1LastEdgeMs  = 0;
static bool     gK1LongFired   = false;
#endif

inline void buttonsInit(ButtonCb shortPress, ButtonCb longPress) {
  gK1ShortCb = shortPress;
  gK1LongCb  = longPress;
#if PIN_K1 >= 0
  pinMode(PIN_K1, INPUT_PULLUP);
#endif
}

// Call once per loop iteration. Cheap — single digitalRead, a few compares.
inline void buttonsTick() {
#if PIN_K1 >= 0
  uint32_t nowMs = millis();
  bool     raw   = digitalRead(PIN_K1) == LOW;

  // Debounce: ignore any edge less than BTN_DEBOUNCE_MS after the previous.
  if (raw != gK1Pressed) {
    if (nowMs - gK1LastEdgeMs < BTN_DEBOUNCE_MS) return;
    gK1LastEdgeMs = nowMs;

    if (raw) {
      // press
      gK1Pressed   = true;
      gK1DownMs    = nowMs;
      gK1LongFired = false;
    } else {
      // release
      gK1Pressed = false;
      if (gK1LongFired) {
        // long press already fired on the held branch — nothing more to do
        return;
      }
      uint32_t heldMs = nowMs - gK1DownMs;
      if (heldMs >= BTN_DEBOUNCE_MS && heldMs <= BTN_SHORT_PRESS_MAX_MS) {
        if (gK1ShortCb) gK1ShortCb();
      }
    }
    return;
  }

  // Held: fire the long-press callback once the threshold is crossed,
  // even before the user releases. Lets the user feel a hold-to-reset.
  if (gK1Pressed && !gK1LongFired) {
    if (nowMs - gK1DownMs >= BTN_LONG_PRESS_MIN_MS) {
      gK1LongFired = true;
      if (gK1LongCb) gK1LongCb();
    }
  }
#endif
}
