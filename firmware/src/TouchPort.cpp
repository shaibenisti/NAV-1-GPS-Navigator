#include "TouchPort.h"

#include <Arduino.h>
#include <Wire.h>
#include <TAMC_GT911.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../config.h"

namespace {

constexpr int SCREEN_W = 800, SCREEN_H = 480;
constexpr uint8_t GT911_ADDR = 0x5D;
constexpr int QUEUE_LEN = 32;

TAMC_GT911 s_gt(TOUCH_SDA_PIN, TOUCH_SCL_PIN, (uint8_t)TOUCH_INT_PIN, (uint8_t)TOUCH_RST_PIN,
                TOUCH_RAW_W, TOUCH_RAW_H);
bool s_present = false;

// ---- queue (producer: touch task on core 0, consumer: UI loop on core 1) ----------------
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
TouchPort::Event s_queue[QUEUE_LEN];
int s_head = 0, s_count = 0;
bool s_tailIsMove = false;          // newest queued event is a move (may be overwritten)
TouchPort::Event s_current = {};    // latest state, returned when the queue is empty
TouchPort::Stats s_stats = {};

void push(const TouchPort::Event &e, bool move) {
  portENTER_CRITICAL(&s_mux);
  s_current = e;
  if (move && s_count > 0 && s_tailIsMove) {
    s_queue[(s_head + s_count - 1) % QUEUE_LEN] = e;       // merge consecutive moves
  } else if (s_count < QUEUE_LEN) {
    s_queue[(s_head + s_count) % QUEUE_LEN] = e;
    s_count++;
    s_tailIsMove = move;
    if ((uint32_t)s_count > s_stats.queueMax) s_stats.queueMax = s_count;
  } else {
    s_stats.overflows++;
  }
  portEXIT_CRITICAL(&s_mux);
}

// ---- GT911 access (same transactions as tools/TOUCH_DIAG) --------------------------------
bool gtRead(uint16_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(GT911_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(GT911_ADDR, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool gtWrite8(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(GT911_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

int16_t mapX(int rx) { return constrain((int)((long)rx * (SCREEN_W - 1) / TOUCH_RAW_W), 0, SCREEN_W - 1); }
int16_t mapY(int ry) { return constrain((int)((long)ry * (SCREEN_H - 1) / TOUCH_RAW_H), 0, SCREEN_H - 1); }

void touchTask(void *) {
  bool pressed = false;
  int16_t lastX = 0, lastY = 0;
  uint32_t silentPolls = 0;
  TickType_t wake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(TOUCH_POLL_MS));
    const uint32_t t0 = micros();
    uint8_t st = 0;
    bool ok = gtRead(0x814E, &st, 1);
    bool report = ok && (st & 0x80);
    bool touched = false;
    int16_t x = lastX, y = lastY;
    if (report) {
      touched = (st & 0x0F) > 0;
      if (touched) {
        uint8_t p[8];
        if (gtRead(0x814F, p, 8)) {
          x = mapX(p[1] | (p[2] << 8));
          y = mapY(p[3] | (p[4] << 8));
        } else {
          ok = report = false;                         // keep the previous state
        }
      }
      if (!gtWrite8(0x814E, 0)) ok = false;            // release the buffer for the next report
    }
    const uint32_t dt = micros() - t0;

    portENTER_CRITICAL(&s_mux);
    s_stats.polls++;
    if (report) s_stats.reports++;
    if (!ok) s_stats.i2cErrors++;
    if (dt > s_stats.pollUsMax) s_stats.pollUsMax = dt;
    portEXIT_CRITICAL(&s_mux);

    if (!report) {
      // While a finger is down the GT911 reports every 10 ms. A long silence means a
      // lost release (I2C error): release at the last position instead of sticking.
      if (pressed && ++silentPolls >= TOUCH_STUCK_POLLS) {
        pressed = false;
        silentPolls = 0;
        push({lastX, lastY, false, micros()}, false);
        portENTER_CRITICAL(&s_mux);
        s_stats.stuckReleases++;
        portEXIT_CRITICAL(&s_mux);
      }
      continue;
    }
    silentPolls = 0;

    if (touched) {
      if (!pressed) {
        pressed = true;
        push({x, y, true, t0}, false);                  // press edge: exact first point
      } else if (x != lastX || y != lastY) {
        push({x, y, true, t0}, true);                   // move
      }
      lastX = x;
      lastY = y;
    } else if (pressed) {
      pressed = false;
      push({lastX, lastY, false, t0}, false);           // release edge
    }
  }
}

}  // namespace

bool TouchPort::begin() {
  // TAMC_GT911 1.0.2 begin() reads the 185-byte config block in ONE Wire request;
  // the default 128-byte Wire RX buffer (heap) then overflows by 57 bytes and
  // corrupts the heap (found in M1, 2026-09-27). The buffer must be enlarged first.
  Wire.setBufferSize(256);
  Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
  Wire.setClock(TOUCH_I2C_HZ);
  // Reset first, then look for the chip: probing before the reset missed it right after a
  // USB flash (2026-09-29, still starting up). Retries, and one more reset if it stays silent.
  auto probe = []() {
    for (int i = 0; i < 10; i++) {
      Wire.beginTransmission(GT911_ADDR);
      if (Wire.endTransmission() == 0) return true;
      delay(10);
    }
    return false;
  };
  s_gt.begin();                                         // reset + config, as verified in M1
  s_present = probe();
  if (!s_present) {
    Serial.println("[TOUCH] GT911 silent after reset, resetting again");
    s_gt.begin();
    s_present = probe();
  }
  // From here on only the touch task uses Wire.
  return xTaskCreatePinnedToCore(touchTask, "touch", 3072, nullptr, TOUCH_TASK_PRIO, nullptr,
                                 TOUCH_TASK_CORE) == pdPASS;
}

bool TouchPort::present() { return s_present; }

bool TouchPort::pop(Event &e, bool &more) {
  portENTER_CRITICAL(&s_mux);
  const bool got = s_count > 0;
  if (got) {
    e = s_queue[s_head];
    s_head = (s_head + 1) % QUEUE_LEN;
    s_count--;
    if (s_count == 0) s_tailIsMove = false;
  } else {
    e = s_current;
  }
  more = s_count > 0;
  portEXIT_CRITICAL(&s_mux);
  return got;
}

namespace {
TouchPort::Stats s_total = {};              // everything already taken, since boot

void fold(TouchPort::Stats &t, const TouchPort::Stats &w) {
  t.polls += w.polls;
  t.reports += w.reports;
  t.i2cErrors += w.i2cErrors;
  t.overflows += w.overflows;
  t.stuckReleases += w.stuckReleases;
  if (w.pollUsMax > t.pollUsMax) t.pollUsMax = w.pollUsMax;
  if (w.queueMax > t.queueMax) t.queueMax = w.queueMax;
}
}  // namespace

TouchPort::Stats TouchPort::takeStats() {
  portENTER_CRITICAL(&s_mux);
  const Stats s = s_stats;
  fold(s_total, s);
  s_stats = {};
  portEXIT_CRITICAL(&s_mux);
  return s;
}

TouchPort::Stats TouchPort::totals() {
  portENTER_CRITICAL(&s_mux);
  Stats t = s_total;
  fold(t, s_stats);
  portEXIT_CRITICAL(&s_mux);
  return t;
}
