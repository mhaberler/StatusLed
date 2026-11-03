#pragma once
// StatusLed - non-blocking status LED patterns for ESP32 (Arduino / FreeRTOS).
//
// A dedicated task drives the LED; the application only posts requests, so
// nothing ever blocks the caller (loop(), callbacks, other tasks). See README.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stddef.h>
#include <stdint.h>

namespace statusled {

// One step of a pattern: show `rgb` (0xRRGGBB, 0 = off) for `ms` milliseconds.
// With `fade`, the color ramps linearly from the previous color to `rgb`
// over the step instead of switching at its start.
struct Step {
  uint32_t rgb;
  uint16_t ms;
  bool fade = false;
};

// A sequence of steps. `repeat` = number of passes; 0 = forever.
// `steps` must stay valid while the pattern plays (use static/constexpr arrays).
struct Pattern {
  const Step *steps;
  uint8_t count;
  uint8_t repeat;
};

// Build a Pattern from a Step array: pattern(MY_STEPS) or pattern(MY_STEPS, 3).
template <size_t N>
constexpr Pattern pattern(const Step (&s)[N], uint8_t repeat = 0) {
  return Pattern{s, static_cast<uint8_t>(N), repeat};
}

constexpr uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

// Backend: show this color (already scaled by setBrightness()). Called only
// from the StatusLed task.
using WriteFn = void (*)(uint32_t rgb, void *ctx);

class StatusLed {
public:
  StatusLed() = default;
  ~StatusLed() { end(); }
  StatusLed(const StatusLed &) = delete;
  StatusLed &operator=(const StatusLed &) = delete;

  // Start the LED task. Returns false if already started or on allocation
  // failure.
  bool begin(WriteFn write, void *ctx = nullptr, uint32_t stackSize = 2048,
             UBaseType_t priority = 1, BaseType_t core = tskNO_AFFINITY);

  // Switch the LED off and stop the task.
  void end();

  // Background pattern, usually repeat 0. Requesting the pattern that is
  // already running is a no-op, so this is safe to call on every loop pass.
  void setStatus(const Pattern &p);

  // One-shot overlay: plays once (its `repeat` passes), then the status
  // pattern restarts. A new event replaces a running one.
  void event(const Pattern &p);

  // Convenience event: `times` x (`rgb` for `ms`, off for `ms`). The steps
  // are copied, no lifetime rules.
  void blink(uint32_t rgb, uint16_t ms, uint8_t times = 1);

  // Abort a running event; the status pattern restarts.
  void clearEvent();

  // Global brightness 0..255 (default 255), applied to every color.
  void setBrightness(uint8_t brightness);

  bool started() const { return task_ != nullptr; }

  // Lowest free stack of the LED task in bytes (0 if not started).
  uint32_t stackHighWater() const;

private:
  enum class Op : uint8_t { Status, Event, Blink, ClearEvent, Brightness, Stop };
  struct Request {
    Op op;
    Pattern pattern;
    uint32_t rgb;
    uint16_t ms;
    uint8_t value;
  };
  struct Layer {
    Pattern p{};
    uint8_t idx = 0;
    uint8_t pass = 0;
    bool active = false;
    uint32_t stepStart = 0;
    uint32_t from = 0; // color at the start of the current step (fades)
  };

  static void taskEntry(void *arg);
  void run();
  void post(const Request &r);
  void startLayer(Layer &l, const Pattern &p, uint32_t now);
  void advance(Layer &l, uint32_t now);
  uint32_t colorOf(const Layer &l, uint32_t now) const;
  TickType_t nextWait(uint32_t now) const;
  void render(uint32_t now);

  WriteFn write_ = nullptr;
  void *ctx_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  volatile bool running_ = false;
  // last pattern requested via setStatus() (caller side, for de-duplication)
  Pattern requested_{};
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

  // owned by the LED task
  Layer status_, event_;
  Step blinkSteps_[2]{};
  uint8_t brightness_ = 255;
  uint32_t shown_ = 0xFFFFFFFF; // last written (scaled) color; forces first write
  uint32_t current_ = 0;        // last unscaled color
};

} // namespace statusled
