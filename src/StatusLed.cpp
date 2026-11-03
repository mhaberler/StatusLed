#include "StatusLed.h"

namespace statusled {

static constexpr uint32_t FADE_TICK_MS = 20;
static constexpr UBaseType_t QUEUE_LEN = 8;

static uint32_t scale(uint32_t c, uint8_t b) {
  if (b == 255)
    return c;
  uint32_t r = (((c >> 16) & 0xFF) * (b + 1)) >> 8;
  uint32_t g = (((c >> 8) & 0xFF) * (b + 1)) >> 8;
  uint32_t bl = ((c & 0xFF) * (b + 1)) >> 8;
  return (r << 16) | (g << 8) | bl;
}

static uint32_t lerp(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
  uint32_t out = 0;
  for (int sh = 16; sh >= 0; sh -= 8) {
    int32_t ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
    int32_t c = ca + (cb - ca) * int32_t(num) / int32_t(den);
    out |= uint32_t(c & 0xFF) << sh;
  }
  return out;
}

static bool samePattern(const Pattern &a, const Pattern &b) {
  return a.steps == b.steps && a.count == b.count && a.repeat == b.repeat;
}

bool StatusLed::begin(WriteFn write, void *ctx, uint32_t stackSize,
                      UBaseType_t priority, BaseType_t core) {
  if (task_ || !write)
    return false;
  write_ = write;
  ctx_ = ctx;
  queue_ = xQueueCreate(QUEUE_LEN, sizeof(Request));
  if (!queue_)
    return false;
  running_ = true;
  if (xTaskCreatePinnedToCore(taskEntry, "statusled", stackSize, this, priority,
                              &task_, core) != pdPASS) {
    running_ = false;
    vQueueDelete(queue_);
    queue_ = nullptr;
    task_ = nullptr;
    return false;
  }
  return true;
}

void StatusLed::end() {
  if (!task_)
    return;
  Request r{};
  r.op = Op::Stop;
  xQueueSend(queue_, &r, portMAX_DELAY);
  while (running_)
    vTaskDelay(pdMS_TO_TICKS(5));
  task_ = nullptr;
  vQueueDelete(queue_);
  queue_ = nullptr;
  requested_ = Pattern{};
}

void StatusLed::post(const Request &r) {
  if (!queue_)
    return;
  if (xQueueSend(queue_, &r, 0) != pdTRUE) {
    log_w("StatusLed: request queue full, dropped");
  }
}

void StatusLed::setStatus(const Pattern &p) {
  if (!queue_)
    return; // not started
  // de-duplicate here, so calling this on every loop pass doesn't flood the queue
  portENTER_CRITICAL(&mux_);
  const bool same = samePattern(p, requested_);
  requested_ = p;
  portEXIT_CRITICAL(&mux_);
  if (same)
    return;
  Request r{};
  r.op = Op::Status;
  r.pattern = p;
  post(r);
}

void StatusLed::event(const Pattern &p) {
  Request r{};
  r.op = Op::Event;
  r.pattern = p;
  post(r);
}

void StatusLed::blink(uint32_t rgb, uint16_t ms, uint8_t times) {
  Request r{};
  r.op = Op::Blink;
  r.rgb = rgb;
  r.ms = ms;
  r.value = times ? times : 1;
  post(r);
}

void StatusLed::clearEvent() {
  Request r{};
  r.op = Op::ClearEvent;
  post(r);
}

void StatusLed::setBrightness(uint8_t brightness) {
  Request r{};
  r.op = Op::Brightness;
  r.value = brightness;
  post(r);
}

uint32_t StatusLed::stackHighWater() const {
  return task_ ? uxTaskGetStackHighWaterMark(task_) * sizeof(StackType_t) : 0;
}

void StatusLed::taskEntry(void *arg) {
  static_cast<StatusLed *>(arg)->run();
}

void StatusLed::startLayer(Layer &l, const Pattern &p, uint32_t now) {
  l.p = p;
  l.idx = 0;
  l.pass = 0;
  l.stepStart = now;
  l.from = current_;
  l.active = p.steps && p.count;
}

// Move past every step whose time is up.
void StatusLed::advance(Layer &l, uint32_t now) {
  while (l.active) {
    const Step &s = l.p.steps[l.idx];
    const uint32_t ms = s.ms ? s.ms : 1; // 0 would spin forever
    if (now - l.stepStart < ms)
      return;
    l.stepStart += ms;
    l.from = s.rgb;
    if (++l.idx >= l.p.count) {
      l.idx = 0;
      if (l.p.repeat && ++l.pass >= l.p.repeat) {
        l.active = false;
        return;
      }
    }
    // after a long stall, resynchronise instead of replaying missed steps
    if (now - l.stepStart > 60000)
      l.stepStart = now;
  }
}

uint32_t StatusLed::colorOf(const Layer &l, uint32_t now) const {
  if (!l.active)
    return 0;
  const Step &s = l.p.steps[l.idx];
  if (!s.fade || s.ms == 0)
    return s.rgb;
  uint32_t t = now - l.stepStart;
  return lerp(l.from, s.rgb, t > s.ms ? s.ms : t, s.ms);
}

TickType_t StatusLed::nextWait(uint32_t now) const {
  const Layer &l = event_.active ? event_ : status_;
  if (!l.active)
    return portMAX_DELAY;
  const Step &s = l.p.steps[l.idx];
  uint32_t elapsed = now - l.stepStart;
  uint32_t left = elapsed >= s.ms ? 0 : s.ms - elapsed;
  if (s.fade && left > FADE_TICK_MS)
    left = FADE_TICK_MS;
  TickType_t t = pdMS_TO_TICKS(left);
  return t ? t : 1;
}

void StatusLed::render(uint32_t now) {
  current_ = colorOf(event_.active ? event_ : status_, now);
  uint32_t out = scale(current_, brightness_);
  if (out != shown_) {
    shown_ = out;
    write_(out, ctx_);
  }
}

void StatusLed::run() {
  render(millis()); // initial state: off
  for (;;) {
    Request r;
    bool got = xQueueReceive(queue_, &r, nextWait(millis())) == pdTRUE;
    uint32_t now = millis();
    // bring layers up to date before applying the request
    bool eventWasActive = event_.active;
    advance(event_, now);
    if (eventWasActive && !event_.active)
      startLayer(status_, status_.p, now); // event done: restart status
    else
      advance(status_, now);

    if (got) {
      switch (r.op) {
      case Op::Status:
        if (!samePattern(r.pattern, status_.p) || !status_.active)
          startLayer(status_, r.pattern, now);
        break;
      case Op::Event:
        startLayer(event_, r.pattern, now);
        break;
      case Op::Blink:
        blinkSteps_[0] = Step{r.rgb, r.ms};
        blinkSteps_[1] = Step{0, r.ms};
        startLayer(event_, Pattern{blinkSteps_, 2, r.value}, now);
        break;
      case Op::ClearEvent:
        if (event_.active) {
          event_.active = false;
          startLayer(status_, status_.p, now);
        }
        break;
      case Op::Brightness:
        brightness_ = r.value;
        shown_ = 0xFFFFFFFF; // force rewrite
        break;
      case Op::Stop:
        write_(0, ctx_);
        running_ = false;
        vTaskDelete(nullptr);
        return;
      }
    }
    render(now);
  }
}

} // namespace statusled
