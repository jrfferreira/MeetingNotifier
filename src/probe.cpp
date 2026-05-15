// Display pin-map probe for integrated "ESP32-C3 + 1.44 inch TFT" boards.
//
// This board's K1/K2 header exposes GPIO 1, 6, 7, 10, 20, 21 — so the
// display is wired to some subset of the remaining usable GPIOs:
// {0, 2, 3, 4, 5, 8}. GPIO 11-17 are reserved for SPI flash on the
// MINI-1 module; 9 is BOOT; 18/19 are USB. So those are off-limits.
//
// Each combo lasts ~6s, fills the screen with a distinct background
// colour, and prints a large combo number on-screen and to USB-CDC.
// Note the combo number that lights up readably and report it back —
// we'll bake that pin map into a permanent build env.
//
// Build: `[env:esp32-c3-probe]` in platformio.ini.

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

struct Combo {
  uint8_t       num;
  const char*   label;
  int8_t        sclk;
  int8_t        mosi;
  int8_t        dc;
  int8_t        cs;    // -1 = tied / unused
  int8_t        rst;   // -1 = tied / unused
  int8_t        bl;    // -1 = tied / unused
  uint8_t       tab;   // INITR_*
  uint16_t      bg;
};

static const Combo kCombos[] = {
  // Most likely first: 2/3 are typical SPI pins on cheap C3 modules
  { 1, "SCLK=2 MOSI=3 DC=4 RST=5 BL=8 GREEN",
       2,  3,  4, -1,  5,  8, INITR_144GREENTAB, ST77XX_BLUE      },
  { 2, "SCLK=2 MOSI=3 DC=5 RST=4 BL=8 GREEN",
       2,  3,  5, -1,  4,  8, INITR_144GREENTAB, ST77XX_MAGENTA   },
  { 3, "SCLK=2 MOSI=3 DC=4 RST=5 CS=0 BL=8 GREEN",
       2,  3,  4,  0,  5,  8, INITR_144GREENTAB, ST77XX_GREEN     },
  { 4, "SCLK=2 MOSI=3 DC=4 RST=5 BL=8 RED",
       2,  3,  4, -1,  5,  8, INITR_REDTAB,      ST77XX_ORANGE    },
  { 5, "SCLK=4 MOSI=5 DC=3 RST=2 BL=8 GREEN",
       4,  5,  3, -1,  2,  8, INITR_144GREENTAB, ST77XX_CYAN      },
  { 6, "SCLK=2 MOSI=3 DC=4 CS=5 RST=0 BL=8 GREEN",
       2,  3,  4,  5,  0,  8, INITR_144GREENTAB, ST77XX_YELLOW    },
  { 7, "SCLK=2 MOSI=3 DC=4 RST=5 BL=0 GREEN",
       2,  3,  4, -1,  5,  0, INITR_144GREENTAB, ST77XX_WHITE     },
};

static const int      kNumCombos = sizeof(kCombos) / sizeof(kCombos[0]);
static const uint32_t kHoldMs    = 6000;

static int                 gIdx        = 0;
static uint32_t            gLastSwitch = 0;
static Adafruit_ST7735*    gTft        = nullptr;

static void forceCandidateBacklights() {
  // Drive every plausible backlight pin HIGH at startup. If the actual BL
  // pin is one of these and isn't used elsewhere by the current combo, the
  // panel will be lit even when the combo guesses BL wrong.
  for (int pin : { 0, 5, 8 }) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
}

static void freeTft() {
  if (gTft) { delete gTft; gTft = nullptr; }
}

static void renderCombo(const Combo& c) {
  if (!gTft) return;

  gTft->fillScreen(c.bg);

  // Big numeric label that's readable even if init/colour is slightly off.
  gTft->setTextWrap(false);
  gTft->setTextColor(ST77XX_WHITE);
  gTft->setTextSize(6);
  gTft->setCursor(28, 8);
  gTft->printf("C%u", c.num);

  // Pin map in tiny text below.
  gTft->setTextSize(1);
  gTft->setCursor(2, 70);
  gTft->print(c.label);

  // Instruction strap.
  gTft->setCursor(2, 100);
  gTft->print("If readable,");
  gTft->setCursor(2, 112);
  gTft->printf("note: C%u", c.num);
}

static void tryCombo(const Combo& c) {
  Serial.printf("\n[probe] C%u — %s\n", c.num, c.label);

  if (c.bl >= 0) {
    pinMode(c.bl, OUTPUT);
    digitalWrite(c.bl, HIGH);
  }

  freeTft();
  gTft = new Adafruit_ST7735(c.cs, c.dc, c.mosi, c.sclk, c.rst);
  gTft->initR(c.tab);
  gTft->setRotation(2);
  renderCombo(c);
}

void setup() {
  Serial.begin(115200);
  // USB-CDC needs a moment after enumeration before the host opens the port.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(50);

  Serial.println();
  Serial.println("============================================");
  Serial.println(" MeetingNotifier - display pin-map probe v2");
  Serial.println("============================================");
  Serial.printf(" Cycling %d combos x %lus each (~%lus total).\n",
                kNumCombos,
                (unsigned long)(kHoldMs / 1000),
                (unsigned long)((kHoldMs * kNumCombos) / 1000));
  Serial.println(" Pins probed: SCLK/MOSI/DC/RST/CS/BL from {0,2,3,4,5,8}");
  Serial.println(" Note the combo number (\"Cn\") that renders");
  Serial.println(" readable text on the panel.");
  Serial.println();

  forceCandidateBacklights();
  tryCombo(kCombos[gIdx]);
  gLastSwitch = millis();
}

void loop() {
  if (millis() - gLastSwitch < kHoldMs) return;
  gIdx = (gIdx + 1) % kNumCombos;
  tryCombo(kCombos[gIdx]);
  gLastSwitch = millis();
}
