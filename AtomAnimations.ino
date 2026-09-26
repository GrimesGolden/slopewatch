#include <M5Unified.h>     // AtomS3R: use M5Unified, not a bare M5GFX object

#include "frames.h"
#include "frames2.h"
#include "frames3.h"
#include "nyan.h"

static constexpr int ANI_W = 128;
static constexpr int ANI_H = 128;

const uint16_t* const stills[] = { frame[0], carlton[0], rick[0] };

// let the compiler count for you — no hardcoded 18 needed
constexpr int STILL_COUNT = sizeof(stills) / sizeof(stills[0]);
constexpr int NYAN_FRAMES = sizeof(nyan)   / sizeof(nyan[0]);
constexpr int NYAN_FPS    = 12;

inline void drawFrame(const uint16_t* src) {
  M5.Display.pushImage(0, 0, ANI_W, ANI_H, src);   // uint16_t* -> RGB565
}

void setup() {
  M5.begin();
  M5.Display.setColorDepth(16);     // must be set OUTSIDE write mode
  M5.Display.setSwapBytes(true);    // flip to false if colours look wrong
  M5.Display.setBrightness(128);    // AtomS3R backlight: LP5562 -> FET, PWM ~500 Hz
  M5.Display.fillScreen(TFT_BLACK);
  Serial.printf("panel %ldx%ld\n", (long)M5.Display.width(), (long)M5.Display.height());
}

void loop() {
  for (int i = 0; i < STILL_COUNT; ++i) {   // frame, carlton, rick
    drawFrame(stills[i]);
    delay(1000);
  }

  uint32_t next = millis();                 // nyan sequence
  for (int i = 0; i < NYAN_FRAMES; ++i) {
    drawFrame(nyan[i]);
    next += 1000 / NYAN_FPS;
    int32_t wait = (int32_t)(next - millis());
    if (wait > 0) delay(wait);
  }
}

