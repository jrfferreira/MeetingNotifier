// Display pin-map probe v4 for integrated "ESP32-C3 + 1.44 inch TFT" boards.
//
// v4 changes vs v3:
// - Widened pin universe to {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11}. Pins
//   exposed on K1 can still be display pins (many boards run the
//   display SPI bus out to header pins for expansion). GPIO 11 is also
//   free here because this board uses external XMC flash on GPIO 12-17,
//   not on 11.
// - Backlight-search phase first: cycle through each candidate pin,
//   driving it LOW for 3 s while the rest stay HIGH. If the backlight
//   is GPIO-controlled, exactly one of those will visibly dim the
//   panel — report the number to me.
// - Combo phase uses Waveshare-style and similar layouts that involve
//   the pins v3 excluded (especially DC=6, CS=7, RST=11).

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

static const int kPinUniverse[] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11
};
static const int kNumPins = sizeof(kPinUniverse) / sizeof(kPinUniverse[0]);

struct Combo {
  uint8_t       num;
  const char*   label;
  int8_t        sclk;
  int8_t        mosi;
  int8_t        dc;
  int8_t        cs;
  int8_t        rst;
  int8_t        bl;
  uint8_t       tab;
  uint16_t      bg;
};

static const Combo kCombos[] = {
  // Waveshare ESP32-C3-LCD-1.44 schematic and close variants.
  { 1, "SCLK=2 MOSI=3 DC=6 CS=7 RST=11 BL=5",
       2, 3, 6, 7, 11, 5, INITR_144GREENTAB, ST77XX_BLUE     },
  { 2, "SCLK=2 MOSI=3 DC=6 CS=7 RST=10 BL=11",
       2, 3, 6, 7, 10, 11, INITR_144GREENTAB, ST77XX_MAGENTA  },
  { 3, "SCLK=2 MOSI=3 DC=10 CS=7 RST=11 BL=5",
       2, 3, 10, 7, 11, 5, INITR_144GREENTAB, ST77XX_GREEN    },
  { 4, "SCLK=2 MOSI=3 DC=6 CS=10 RST=11 BL=5",
       2, 3, 6, 10, 11, 5, INITR_144GREENTAB, ST77XX_ORANGE   },
  { 5, "SCLK=4 MOSI=5 DC=6 CS=7 RST=11 BL=8",
       4, 5, 6, 7, 11, 8, INITR_144GREENTAB, ST77XX_CYAN      },

  // SPI on 6/7 (default-ish hardware SPI pin assignment on C3).
  { 6, "SCLK=6 MOSI=7 DC=2 CS=3 RST=10 BL=11",
       6, 7, 2, 3, 10, 11, INITR_144GREENTAB, ST77XX_YELLOW   },
  { 7, "SCLK=6 MOSI=7 DC=10 CS=2 RST=11 BL=5",
       6, 7, 10, 2, 11, 5, INITR_144GREENTAB, ST77XX_WHITE    },
  { 8, "SCLK=6 MOSI=7 DC=4 CS=5 RST=11 BL=10",
       6, 7, 4, 5, 11, 10, INITR_144GREENTAB, ST77XX_RED      },

  // SCLK=2 / MOSI=3 with RST/BL on the previously-skipped pins.
  { 9, "SCLK=2 MOSI=3 DC=4 CS=5 RST=11 BL=10",
       2, 3, 4, 5, 11, 10, INITR_144GREENTAB, ST77XX_BLUE     },
  {10, "SCLK=2 MOSI=3 DC=6 CS=7 RST=11 BL=5 (REDTAB)",
       2, 3, 6, 7, 11, 5, INITR_REDTAB,       ST77XX_MAGENTA  },
  {11, "SCLK=2 MOSI=3 DC=6 CS=7 RST=11 BL=5 (BLACKTAB)",
       2, 3, 6, 7, 11, 5, INITR_BLACKTAB,     ST77XX_GREEN    },

  // Long-shots — SPI on header-only pins.
  {12, "SCLK=10 MOSI=6 DC=7 CS=11 RST=2 BL=5",
       10, 6, 7, 11, 2, 5, INITR_144GREENTAB, ST77XX_ORANGE   },
};

static const int      kNumCombos = sizeof(kCombos) / sizeof(kCombos[0]);
static const uint32_t kBlStepMs  = 3000;
static const uint32_t kComboMs   = 6000;

enum Phase { PHASE_BL_SEARCH, PHASE_COMBOS };

static Phase               gPhase      = PHASE_BL_SEARCH;
static int                 gPinIdx     = -1;     // -1 = initial "all HIGH" step
static int                 gComboIdx   = 0;
static uint32_t            gLastSwitch = 0;
static Adafruit_ST7735*    gTft        = nullptr;

static void allCandidatesHigh() {
  for (int p : kPinUniverse) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
}

static void freeTft() {
  if (gTft) { delete gTft; gTft = nullptr; }
}

// ---------------------------- Phase 1: BL search ----------------------------

static void blStep() {
  // Restore previous LOW pin back to HIGH.
  if (gPinIdx >= 0 && gPinIdx < kNumPins) {
    digitalWrite(kPinUniverse[gPinIdx], HIGH);
  }

  gPinIdx++;

  if (gPinIdx >= kNumPins) {
    Serial.println();
    Serial.println("[probe] BL search complete.");
    Serial.println("[probe] If the screen DIMMED during one of the above,");
    Serial.println("[probe] the corresponding pin is the backlight.");
    Serial.println("[probe] Starting combo cycle now.");
    Serial.println();
    allCandidatesHigh();
    gPhase     = PHASE_COMBOS;
    gComboIdx  = -1;     // first iteration will increment to 0
    gLastSwitch = millis();
    return;
  }

  int pin = kPinUniverse[gPinIdx];
  Serial.printf("[probe] BL test: pin %d  LOW  (others HIGH) for %lus\n",
                pin, (unsigned long)(kBlStepMs / 1000));
  digitalWrite(pin, LOW);
}

// ---------------------------- Phase 2: combos ------------------------------

static void renderCombo(const Combo& c) {
  if (!gTft) return;
  gTft->fillScreen(c.bg);
  gTft->setTextWrap(false);
  gTft->setTextColor(ST77XX_WHITE);
  gTft->setTextSize(6);
  gTft->setCursor(28, 8);
  gTft->printf("C%u", c.num);
  gTft->setTextSize(1);
  gTft->setCursor(2, 70);
  gTft->print(c.label);
  gTft->setCursor(2, 100);
  gTft->print("If readable,");
  gTft->setCursor(2, 112);
  gTft->printf("note: C%u", c.num);
}

static void tryCombo(const Combo& c) {
  Serial.printf("[probe] C%u STARTING - %s\n", c.num, c.label);

  if (c.bl >= 0) {
    pinMode(c.bl, OUTPUT);
    digitalWrite(c.bl, HIGH);
  }

  freeTft();
  gTft = new Adafruit_ST7735(c.cs, c.dc, c.mosi, c.sclk, c.rst);
  gTft->initR(c.tab);
  gTft->setRotation(2);
  renderCombo(c);

  Serial.printf("[probe] C%u DONE\n", c.num);
}

static void comboStep() {
  gComboIdx = (gComboIdx + 1) % kNumCombos;
  tryCombo(kCombos[gComboIdx]);
}

// ---------------------------- Arduino lifecycle ----------------------------

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(50);

  Serial.println();
  Serial.println("============================================");
  Serial.println(" MeetingNotifier - display pin-map probe v4");
  Serial.println("============================================");
  Serial.println();
  Serial.println(" Phase 1: BL search");
  Serial.printf  ("   %d candidate pins, %lus each (~%lus total)\n",
                  kNumPins, (unsigned long)(kBlStepMs / 1000),
                  (unsigned long)((kBlStepMs * kNumPins) / 1000));
  Serial.println("   Watch for the screen DIMMING — that pin is the BL.");
  Serial.println();
  Serial.println(" Phase 2: combo cycle");
  Serial.printf  ("   %d combos, %lus each (~%lus total)\n",
                  kNumCombos, (unsigned long)(kComboMs / 1000),
                  (unsigned long)((kComboMs * kNumCombos) / 1000));
  Serial.println("   Watch for a readable Cn label.");
  Serial.println();

  allCandidatesHigh();
  Serial.printf("[probe] All %d candidate pins HIGH for %lus (screen lit?).\n",
                kNumPins, (unsigned long)(kBlStepMs / 1000));
  gLastSwitch = millis();
}

void loop() {
  if (gPhase == PHASE_BL_SEARCH) {
    if (millis() - gLastSwitch < kBlStepMs) return;
    blStep();
    gLastSwitch = millis();
  } else {
    if (millis() - gLastSwitch < kComboMs) return;
    comboStep();
    gLastSwitch = millis();
  }
}
