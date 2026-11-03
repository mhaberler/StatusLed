# StatusLed

Non-blocking status LED patterns for ESP32 (Arduino core 3.x, FreeRTOS).

- **Patterns are data:** a list of `{color, duration}` steps with optional fades, played once,
  a fixed number of times or forever.
- **Two layers:** a background *status* pattern, and one-shot *events* that play over it and
  then hand back.
- **Own task:** a small FreeRTOS task drives the LED. Your code only posts requests and never
  waits. The LED keeps its timing while `loop()` is blocked, and any task can trigger an event
  (e.g. a BLE callback).
- **Backends:** GPIO LED, NeoPixel (WS2812/SK6812), M5Stack power LED via M5Unified, or your
  own one-line write function.

```cpp
#include <StatusLed.h>
#include <StatusLedAdapters.h>
using namespace statusled;

NeoPixelLed hw(20, NEO_GRB + NEO_KHZ800, 1, 19);   // M5Stack NanoC6: data GPIO20, power GPIO19
StatusLed led;

constexpr Step WIFI_DOWN[] = {{0xFF0000, 200}, {0, 200}};   // fast red blink
constexpr Step CONNECTED[] = {{0x00FF00, 1000}};             // solid green

void setup() {
  hw.begin();
  led.begin(NeoPixelLed::write, &hw);
  led.blink(0xFFFFFF, 100, 5);           // boot: 5 white blinks, returns immediately
  led.setStatus(pattern(WIFI_DOWN));
}

void loop() {
  if (WiFi.isConnected())
    led.setStatus(pattern(CONNECTED));   // same pattern again = no-op, safe every pass
}
```

## Concepts

**Step:** show one color for a time.

```cpp
struct Step { uint32_t rgb; uint16_t ms; bool fade = false; };
```

- `rgb` is `0xRRGGBB`; `0` means off. Helper: `rgb(r, g, b)`.
- `ms` is the duration (1–65535 ms; 0 is treated as 1).
- `fade = true` ramps linearly from the previous color to `rgb` over the step, updated every
  20 ms. Without it, the color switches at the start of the step.

**Pattern:** a step array plus a repeat count.

```cpp
struct Pattern { const Step *steps; uint8_t count; uint8_t repeat; };
template <size_t N> constexpr Pattern pattern(const Step (&steps)[N], uint8_t repeat = 0);
```

`repeat` = number of passes through the steps, `0` = forever. Build one with
`pattern(MY_STEPS)` or `pattern(MY_STEPS, 3)`.

Examples:

```cpp
constexpr Step SOLID_GREEN[]  = {{0x00FF00, 1000}};                          // steady on
constexpr Step SLOW_ORANGE[]  = {{0xFF8800, 500}, {0, 500}};                  // 1 Hz blink
constexpr Step HEARTBEAT[]    = {{0x00FF00, 40}, {0, 2960}};                  // short blip every 3 s
constexpr Step BREATHE_BLUE[] = {{0x0000FF, 1000, true}, {0, 1000, true}};    // fade in and out
constexpr Step SOS[] = {{0xFF0000,150},{0,150},{0xFF0000,150},{0,150},{0xFF0000,150},{0,450},
                        {0xFF0000,450},{0,150},{0xFF0000,450},{0,150},{0xFF0000,450},{0,450},
                        {0xFF0000,150},{0,150},{0xFF0000,150},{0,150},{0xFF0000,150},{0,1500}};
```

**Layers:**

- **Status:** the background pattern, normally looping (`repeat` 0). It reflects a state:
  Wi-Fi down, broker down, healthy.
- **Event:** a one-shot overlay for something that just happened: provisioning succeeded, an
  error, an identify request. While an event plays, the status is hidden. When the event ends,
  the status pattern restarts from its first step. A new event replaces a running one.

## API

All calls except `begin()` / `end()` only post a request to the LED task and return immediately.

| Call | Effect |
| --- | --- |
| `bool begin(WriteFn write, void *ctx = nullptr, uint32_t stackSize = 2048, UBaseType_t priority = 1, BaseType_t core = tskNO_AFFINITY)` | Starts the LED task; the LED begins off. `write(rgb, ctx)` is your backend. Returns `false` if already started or out of memory. |
| `void end()` | Switches the LED off and stops the task (waits for it). |
| `void setStatus(const Pattern &p)` | Sets the background pattern. Requesting the current status again (same steps, count, repeat) is filtered out in the caller and never reaches the queue, so it's fine to call on every `loop()` pass. Calls before `begin()` are ignored. |
| `void event(const Pattern &p)` | Plays `p` as a one-shot overlay (`p.repeat` passes; 0 would loop until `clearEvent()`). |
| `void blink(uint32_t rgb, uint16_t ms, uint8_t times = 1)` | Convenience event: `times` × (`rgb` for `ms`, off for `ms`). The steps are copied. |
| `void clearEvent()` | Aborts a running event; the status pattern restarts. |
| `void setBrightness(uint8_t b)` | Global brightness 0–255 (default 255), applied to every color before `write`. |
| `bool started() const` | Whether the task is running. |
| `uint32_t stackHighWater() const` | The task's lowest free stack in bytes, for sizing `stackSize`. |

`WriteFn` is `void (*)(uint32_t rgb, void *ctx)`. It is called from the LED task, only when the
displayed color changes, already scaled by `setBrightness()`.

### Threading and lifetime rules

- **Callers:** any task may call the API concurrently. ISRs may not: it uses `xQueueSend`, not
  the `FromISR` variant.
- **Request queue:** 8 requests deep. If it overflows (8 requests faster than the LED task
  runs), the request is dropped with a `log_w`.
- **Pattern lifetime:** `Pattern.steps` must stay valid while the pattern may play. Use
  `static` / `constexpr` arrays, not locals. `blink()` has no such restriction.
- **The write callback:** only the LED task calls it, so it needs no locking against
  StatusLed. Don't touch the same LED from elsewhere while the task runs.

## Backends (`StatusLedAdapters.h`)

| Adapter | Hardware | Notes |
| --- | --- | --- |
| `GpioLed{pin, activeLow}` | Plain LED on a GPIO | On for any color ≠ 0. |
| `NeoPixelLed(pin, type, count, powerPin, powerOn)` | WS2812 / SK6812 via Adafruit NeoPixel | All pixels show the color. `powerPin` optional (−1 = none), e.g. NanoC6 GPIO19. Available if Adafruit NeoPixel is a dependency. |
| `M5PowerLed` | M5Stack power LED via M5Unified | Brightness = color luminance. Call `M5.begin()` first. Available if M5Unified is a dependency. |

Each adapter has `begin()` (configure the hardware) and a static `write(rgb, ctx)`:

```cpp
statusled::GpioLed led7{GPIO_NUM_7};
led7.begin();
status.begin(statusled::GpioLed::write, &led7);
```

**Writing your own backend** takes one function:

```cpp
void myWrite(uint32_t rgb, void *ctx) {
  ledcWrite(MY_PIN, statusled::luminance(rgb));   // e.g. PWM-dimmed single LED
}
led.begin(myWrite);
```

## Memory and CPU

- **Task stack:** 2048 B by default. The engine uses about 550 B, measured on an ESP32-C6 with
  the NeoPixel backend. If your write callback logs or does more work, raise `stackSize`
  (4096 B with `Serial.printf` in the callback) and check with `stackHighWater()`.
- **Queue:** 8 requests of ~20 B each.
- **CPU:** the task sleeps on its queue until the current step ends, so a steady pattern
  costs nothing in between. Fades wake it every 20 ms.

## Migrating from a blocking `blinkLed()`

```cpp
// before: blocks the caller for 2 * d * times ms
void blinkLed(int d, int times, uint32_t color) { /* delay() loop */ }

// after: same signature, returns immediately
void blinkLed(int d, int times, uint32_t color) { led.blink(color, d, times); }
```

## Example

[`examples/basic`](examples/basic) builds for the M5Stack NanoC6 and StampS3 with PlatformIO. It:
- cycles four status patterns (solid, slow blink, fast blink, fade)
- posts events from a separate FreeRTOS task
- switches brightness
- blocks `loop()` for 10 s to show the LED keeps running

Enable `-DLOG_LED` in its `platformio.ini` to log every LED change with a timestamp.

```sh
cd examples/basic
pio run -e m5stack-nanoc6 -t upload && pio device monitor
```

## License

MIT
