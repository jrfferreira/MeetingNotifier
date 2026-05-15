// Display pin-map probe v3 for integrated "ESP32-C3 + 1.44 inch TFT" boards.
//
// v3 changes vs v2:
// - 12 combos instead of 7, covering SCLK/MOSI swap, DC on different pins,
//   RST on different pins, plus alternate ST7735 init tabs.
// - Forces every plausible BL/RST candidate HIGH at startup so the panel
//   isn't held in reset by a floating pin we never touch.
// - Logs both "[probe] Cn STARTING" and "[probe] Cn DONE" so we can tell
//   whether the firmware froze mid-combo (no DONE) vs cycled cleanly.
//
// The board's K1 header exposes GPIO 1, 6, 7, 10, 20, 21. The MINI-1
// module reserves 11-17 (flash), 9 (BOOT), 18/19 (USB). So display is
// constrained to {0, 2, 3, 4, 5, 8} — every combo here picks from that
// set.

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

struct Combo {
  uint8_t       num;
  const char*   label;
  int8_t        sclk;
  int8_t        mosi;
  int8_t        dc;
  int8_t        cs;     // -1 = tied / unused
  int8_t        rst;    // -1 = tied / unused
  int8_t        bl;     // -1 = tied / unused
  uint8_t       tab;    // INITR_*
  uint16_t      bg;
};

static const Combo kCombos[] = {
  // SCLK=2 / MOSI=3 — most common arrangement on cheap C3 boards.
  { 1, "SCLK=2 MOSI=3 DC=4 RST=5 BL=8",
       2, 3, 4, -1, 5, 8, INITR_144GREENTAB, ST77XX_BLUE      },
  { 2, "SCLK=2 MOSI=3 DC=5 RST=4 BL=8",
       2, 3, 5, -1, 4, 8, INITR_144GREENTAB, ST77XX_MAGENTA   },
  { 3, "SCLK=2 MOSI=3 DC=8 RST=5 BL=0",
       2, 3, 8, -1, 5, 0, INITR_144GREENTAB, ST77XX_GREEN     },
  { 4, "SCLK=2 MOSI=3 DC=4 RST=8 BL=0",
       2, 3, 4, -1, 8, 0, INITR_144GREENTAB, ST77XX_ORANGE    },
  { 5, "SCLK=2 MOSI=3 DC=4 RST=5 BL=8 (REDTAB)",
       2, 3, 4, -1, 5, 8, INITR_REDTAB,      ST77XX_CYAN      },

  // SCLK/MOSI swap — some panels reverse the FPC pinout.
  { 6, "SCLK=3 MOSI=2 DC=4 RST=5 BL=8",
       3, 2, 4, -1, 5, 8, INITR_144GREENTAB, ST77XX_YELLOW    },
  { 7, "SCLK=3 MOSI=2 DC=5 RST=4 BL=8",
       3, 2, 5, -1, 4, 8, INITR_144GREENTAB, ST77XX_WHITE     },

  // SPI on 4/5 — second most common arrangement.
  { 8, "SCLK=4 MOSI=5 DC=2 RST=3 BL=8",
       4, 5, 2, -1, 3, 8, INITR_144GREENTAB, ST77XX_BLUE      },
  { 9, "SCLK=5 MOSI=4 DC=2 RST=3 BL=8",
       5, 4, 2, -1, 3, 8, INITR_144GREENTAB, ST77XX_MAGENTA   },
  {10, "SCLK=4 MOSI=5 DC=3 RST=2 BL=8",
       4, 5, 3, -1, 2, 8, INITR_144GREENTAB, ST77XX_GREEN     },

  // SCLK on 0 or 8 — unusual but worth probing.
  {11, "SCLK=0 MOSI=4 DC=2 RST=3 BL=8",
       0, 4, 2, -1, 3, 8, INITR_144GREENTAB, ST77XX_ORANGE    },
  {12, "SCLK=8 MOSI=4 DC=2 RST=3 BL=0",
       8, 4, 2, -1, 3, 0, INITR_144GREENTAB, ST77XX_CYAN      },
};

static const int      kNumCombos = sizeof(kCombos) / sizeof(kCombos[0]);
static const uint32_t kHoldMs    = 6000;

static int                 gIdx        = 0;
static uint32_t            gLastSwitch = 0;
static Adafruit_ST7735*    gTft        = nullptr;

// Drive every plausible BL / RST pin HIGH at startup so the panel is lit
// and not held in reset by something we never touch in a given combo.
static void forceCandidatesHigh() {
  for (int pin : { 0, 2, 3, 4, 5, 8 }) {
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

  gTft->setTextWrap(false);
  gTft->setTextColor(ST77XX_WHITE);

  // Big numeric label — readable from across the room even if rotation
  // or colour-order is slightly off.
  gTft->setTextSize(6);
  gTft->setCursor(28, 8);
  gTft->printf("C%u", c.num);

  // Pin map in tiny text.
  gTft->setTextSize(1);
  gTft->setCursor(2, 70);
  gTft->print(c.label);

  gTft->setCursor(2, 100);
  gTft->print("If readable,");
  gTft->setCursor(2, 112);
  gTft->printf("note: C%u", c.num);
}

static void tryCombo(const Combo& c) {
  Serial.printf("[probe] C%u STARTING — %s\n", c.num, c.label);

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

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(50);

  Serial.println();
  Serial.println("============================================");
  Serial.println(" MeetingNotifier - display pin-map probe v3");
  Serial.println("============================================");
  Serial.printf(" Cycling %d combos x %lus each (~%lus total).\n",
                kNumCombos,
                (unsigned long)(kHoldMs / 1000),
                (unsigned long)((kHoldMs * kNumCombos) / 1000));
  Serial.println(" Pins probed: SCLK/MOSI/DC/RST/BL from {0,2,3,4,5,8}");
  Serial.println(" Watch the screen for a readable Cn label and");
  Serial.println(" report the number back.");
  Serial.println();

  forceCandidatesHigh();
  tryCombo(kCombos[gIdx]);
  gLastSwitch = millis();
}

void loop() {
  if (millis() - gLastSwitch < kHoldMs) return;
  gIdx = (gIdx + 1) % kNumCombos;
  tryCombo(kCombos[gIdx]);
  gLastSwitch = millis();
}
