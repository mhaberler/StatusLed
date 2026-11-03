// StatusLed basic example: status patterns, events from another task,
// brightness, and an LED that keeps going while loop() blocks.
#include <Arduino.h>
#include <StatusLed.h>
#include <StatusLedAdapters.h>

using namespace statusled;

#ifndef RGB_POWER_PIN
#define RGB_POWER_PIN -1
#endif

NeoPixelLed hw(RGB_LED_PIN, NEO_GRB + NEO_KHZ800, 1, RGB_POWER_PIN);
StatusLed led;

// Patterns are plain data. Status patterns loop (repeat 0).
constexpr Step SOLID_GREEN[] = {{0x00FF00, 1000}};
constexpr Step SLOW_ORANGE[] = {{0xFF8800, 500}, {0, 500}};
constexpr Step FAST_RED[] = {{0xFF0000, 200}, {0, 200}};
constexpr Step BLUE_BREATHE[] = {{0x0000FF, 1000, true}, {0, 1000, true}};
// Event: played twice, then the status pattern resumes.
constexpr Step ERROR_RED[] = {{0xFF0000, 300}, {0, 200}};

struct Named {
  const char *name;
  Pattern pattern;
};
const Named STATUSES[] = {
    {"solid green", pattern(SOLID_GREEN)},
    {"slow orange", pattern(SLOW_ORANGE)},
    {"fast red", pattern(FAST_RED)},
    {"blue breathe", pattern(BLUE_BREATHE)},
};

// Backend wrapper: drive the pixel, optionally log each change.
static void writeLed(uint32_t rgb, void *ctx) {
  NeoPixelLed::write(rgb, ctx);
#ifdef LOG_LED
  Serial.printf("LED %lu %06lx\n", millis(), (unsigned long)rgb);
#endif
}

// Events can be posted from any task, e.g. a protocol callback.
static void eventTask(void *) {
  for (bool ok = true;; ok = !ok) {
    vTaskDelay(pdMS_TO_TICKS(7000));
    if (ok) {
      Serial.println("EVENT green x3");
      led.blink(0x00FF00, 100, 3);
    } else {
      Serial.println("EVENT error red x2");
      led.event(pattern(ERROR_RED, 2));
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  hw.begin();
#ifdef LOG_LED
  const uint32_t stack = 4096; // printf in the write callback needs stack
#else
  const uint32_t stack = 2048;
#endif
  if (!led.begin(writeLed, &hw, stack)) {
    Serial.println("StatusLed begin failed");
  }
  led.blink(0xFFFFFF, 100, 5); // boot blink, returns immediately
  xTaskCreate(eventTask, "events", 3072, nullptr, 1, nullptr);
}

void loop() {
  static size_t i = 0;
  static uint8_t round = 0;
  const Named &s = STATUSES[i];
  Serial.printf("STATUS %s (brightness %u, led stack free %u B)\n", s.name,
                round % 2 ? 32 : 255, (unsigned)led.stackHighWater());
  led.setStatus(s.pattern);
  i = (i + 1) % (sizeof(STATUSES) / sizeof(STATUSES[0]));
  if (i == 0) {
    round++;
    led.setBrightness(round % 2 ? 32 : 255);
  }
  if (i == 3) {
    Serial.println("loop() blocks for 10 s - the LED keeps running");
    delay(10000);
  } else {
    delay(5000);
  }
}
