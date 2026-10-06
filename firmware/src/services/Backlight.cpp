#include "Backlight.h"

#include <lvgl.h>
#include "Settings.h"
#include "../../config.h"

namespace {

constexpr uint32_t PWM_HZ = 5000;        // well above visible flicker, below the LED driver's limits
constexpr uint8_t PWM_BITS = 8;
constexpr int MIN_PCT = 10, DIM_PCT = 15;

int s_pct = 100;
uint32_t s_dimAfterS = 0;
bool s_dimmed = false, s_swallow = false, s_pwm = false;
volatile bool s_off = false;         // forced off (firmware update), wins over everything

void apply(int pct) {
  if (!s_pwm) return;
  if (s_off) pct = 0;
  // 100 % = duty 2^bits: constantly on, exactly the previous digitalWrite(HIGH) behaviour
  const uint32_t duty = pct <= 0 ? 0 : pct >= 100 ? (1u << PWM_BITS) : (uint32_t)pct * ((1u << PWM_BITS) - 1) / 100;
  ledcWrite(LCD_BL_PIN, duty);
}

}  // namespace

void Backlight::begin() {
  s_pct = constrain(Settings::brightness(), MIN_PCT, 100);
  s_dimAfterS = Settings::dimAfterS();
  s_pwm = ledcAttach(LCD_BL_PIN, PWM_HZ, PWM_BITS);
  apply(s_pct);
  Serial.printf("[BL] brightness %d %%, dim after %s%s\n", s_pct, s_dimAfterS ? (String(s_dimAfterS) + " s").c_str() : "never",
                s_pwm ? "" : " (PWM FAILED: always on)");
}

void Backlight::update() {
  if (!s_dimAfterS || s_dimmed) return;
  if (lv_display_get_inactive_time(lv_display_get_default()) >= s_dimAfterS * 1000) {
    s_dimmed = true;
    apply(min(DIM_PCT, s_pct));
  }
}

void Backlight::setBrightness(int pct, bool save) {
  s_pct = constrain(pct, MIN_PCT, 100);
  if (save) Settings::setBrightness(s_pct);
  if (!s_dimmed) apply(s_pct);
}

int Backlight::brightness() { return s_pct; }

void Backlight::forceOff(bool off) {
  s_off = off;
  apply(s_dimmed ? min(DIM_PCT, s_pct) : s_pct);
}

void Backlight::setDimAfter(uint32_t seconds) {
  s_dimAfterS = seconds;
  Settings::setDimAfterS(seconds);
  wake();
}

uint32_t Backlight::dimAfter() { return s_dimAfterS; }
bool Backlight::dimmed() { return s_dimmed; }
bool Backlight::pwmOk() { return s_pwm; }

void Backlight::wake() {
  lv_display_trigger_activity(lv_display_get_default());   // restart the inactivity time
  if (!s_dimmed) return;
  s_dimmed = false;
  apply(s_pct);
}

bool Backlight::touchGate(bool pressed) {
  if (s_swallow) {                       // the waking touch: hide it from LVGL until release
    if (!pressed) s_swallow = false;
    return false;
  }
  if (pressed && s_dimmed) {
    wake();
    s_swallow = true;
    return false;
  }
  return true;
}
