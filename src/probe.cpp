// Display pin-map probe for integrated "ESP32-C3 + 1.44 inch TFT" boards.
//
// Cycles through a handful of candidate pin / driver combinations every few
// seconds. Each attempt fills the screen with a distinct colour and labels
// itself with a combo number. The one that renders readably tells you which
// pin map matches your board — report the combo number back and we'll bake
// it into a permanent build env.
//
// Built into a separate firmware variant (esp32-c3-probe) so it can be
// flashed via the same web flasher as the main firmware.

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_ST7789.h>

struct Combo {
  const char* label;
  int8_t      sclk;
  int8_t      mosi;
  int8_t      dc;
  int8_t      cs;     // -1 means tied / not used
  int8_t      rst;    // -1 means tied / not used
  int8_t      bl;     // -1 means tied / always-on
  bool        is7789; // false → ST7735 144 green tab (128x128)
  uint16_t    bg;
};

static const Combo kCombos[] = {
  {"C1 ST35  SCLK=2 MOSI=3 DC=4 CS=- RST=5 BL=11",
     2, 3, 4, -1,  5, 11, false, ST77XX_BLUE},
  {"C2 ST35  SCLK=2 MOSI=3 DC=5 CS=- RST=4 BL=11",
     2, 3, 5, -1,  4, 11, false, ST77XX_MAGENTA},
  {"C3 ST35  SCLK=4 MOSI=5 DC=3 CS=- RST=2 BL=11",
     4, 5, 3, -1,  2, 11, false, ST77XX_GREEN},
  {"C4 ST35  SCLK=2 MOSI=3 DC=4 CS=5 RST=11 BL=0",
     2, 3, 4,  5, 11,  0, false, ST77XX_ORANGE},
  {"C5 ST35  SCLK=2 MOSI=3 DC=4 CS=- RST=5 BL=-",
     2, 3, 4, -1,  5, -1, false, ST77XX_CYAN},
  {"C6 ST89  SCLK=6 MOSI=7 DC=3 CS=10 RST=4 BL=5  (orig)",
     6, 7, 3, 10,  4,  5, true,  ST77XX_RED},
};

static const int  kNumCombos = sizeof(kCombos) / sizeof(kCombos[0]);
static const uint32_t kHoldMs = 7000;

static int           gIdx        = 0;
static uint32_t      gLastSwitch = 0;
static Adafruit_GFX* gTft        = nullptr;

static void freeTft() {
  if (!gTft) return;
  delete gTft;
  gTft = nullptr;
}

static void renderLabel(Adafruit_GFX* tft, const Combo& c, int w, int h) {
  tft->setTextWrap(false);
  tft->setTextColor(ST77XX_WHITE);
  tft->setTextSize(1);

  tft->setCursor(2, 2);
  tft->printf("Combo %d / %d", gIdx + 1, kNumCombos);

  // Label might be longer than the panel; the wrap-off setting clips it.
  tft->setCursor(2, 16);
  tft->print(c.label);

  tft->setCursor(2, h - 24);
  tft->print("If this is readable,");
  tft->setCursor(2, h - 12);
  tft->printf("note: C%d", gIdx + 1);
}

static void tryCombo(const Combo& c) {
  Serial.printf("\n[probe] %s\n", c.label);

  // Speculative backlight enable — drive a few common BL candidates HIGH so
  // we don't stay dark just because we guessed the BL pin wrong.
  for (int pin : { 0, 11 }) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  if (c.bl >= 0) {
    pinMode(c.bl, OUTPUT);
    digitalWrite(c.bl, HIGH);
  }

  freeTft();

  if (c.is7789) {
    auto* tft = new Adafruit_ST7789(c.cs, c.dc, c.mosi, c.sclk, c.rst);
    tft->init(240, 240, SPI_MODE0);
    tft->setRotation(2);
    tft->fillScreen(c.bg);
    renderLabel(tft, c, 240, 240);
    gTft = tft;
  } else {
    auto* tft = new Adafruit_ST7735(c.cs, c.dc, c.mosi, c.sclk, c.rst);
    tft->initR(INITR_144GREENTAB);
    tft->setRotation(2);
    tft->fillScreen(c.bg);
    renderLabel(tft, c, 128, 128);
    gTft = tft;
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("===========================================");
  Serial.println(" MeetingNotifier — display pin-map probe");
  Serial.println("===========================================");
  Serial.printf(" Cycling %d combos every %lus.\n",
                kNumCombos, (unsigned long)(kHoldMs / 1000));
  Serial.println(" Watch the screen. Note the combo number");
  Serial.println(" of the one that renders readable text.");
  Serial.println();

  tryCombo(kCombos[gIdx]);
  gLastSwitch = millis();
}

void loop() {
  if (millis() - gLastSwitch < kHoldMs) return;
  gIdx = (gIdx + 1) % kNumCombos;
  tryCombo(kCombos[gIdx]);
  gLastSwitch = millis();
}
