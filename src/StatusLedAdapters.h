#pragma once
// Ready-made StatusLed backends. Each provides begin() and a static
// write(rgb, ctx) to pass to StatusLed::begin():
//
//   statusled::GpioLed led{GPIO_NUM_7};
//   led.begin();
//   status.begin(statusled::GpioLed::write, &led);
//
// NeoPixelLed and M5PowerLed are only available when their libraries
// (Adafruit NeoPixel, M5Unified) are in the build.

#include "StatusLed.h"

namespace statusled {

// Plain on/off LED on a GPIO. Any color other than 0 switches it on.
struct GpioLed {
  int pin;
  bool activeLow = false;

  void begin() {
    pinMode(pin, OUTPUT);
    write(0, this);
  }
  static void write(uint32_t rgb, void *ctx) {
    auto *self = static_cast<GpioLed *>(ctx);
    const bool on = rgb != 0;
    digitalWrite(self->pin, (on != self->activeLow) ? HIGH : LOW);
  }
};

// Perceived brightness of a color, 0..255 (for single-channel LEDs).
inline uint8_t luminance(uint32_t rgb) {
  return uint8_t((((rgb >> 16) & 0xFF) * 77 + ((rgb >> 8) & 0xFF) * 150 +
                  (rgb & 0xFF) * 29) >>
                 8);
}

} // namespace statusled

#if __has_include(<Adafruit_NeoPixel.h>)
#include <Adafruit_NeoPixel.h>
namespace statusled {

// WS2812 / SK6812 pixel(s); all pixels show the same color.
// powerPin: optional GPIO that powers the LED (e.g. NanoC6 GPIO19), -1 = none.
struct NeoPixelLed {
  Adafruit_NeoPixel pixel;
  int powerPin;
  int powerOn;

  NeoPixelLed(int pin, neoPixelType type = NEO_GRB + NEO_KHZ800,
              uint16_t count = 1, int powerPin = -1, int powerOn = HIGH)
      : pixel(count, pin, type), powerPin(powerPin), powerOn(powerOn) {}

  void begin() {
    if (powerPin >= 0) {
      pinMode(powerPin, OUTPUT);
      digitalWrite(powerPin, powerOn);
    }
    pixel.begin();
    pixel.clear();
    pixel.show();
  }
  static void write(uint32_t rgb, void *ctx) {
    auto *self = static_cast<NeoPixelLed *>(ctx);
    self->pixel.fill(rgb);
    self->pixel.show();
  }
};

} // namespace statusled
#endif

#if __has_include(<M5Unified.h>)
#include <M5Unified.h>
namespace statusled {

// M5Stack power/status LED via M5Unified (brightness only). Call M5.begin()
// before StatusLed::begin().
struct M5PowerLed {
  void begin() { M5.Power.setLed(0); }
  static void write(uint32_t rgb, void *) { M5.Power.setLed(luminance(rgb)); }
};

} // namespace statusled
#endif
