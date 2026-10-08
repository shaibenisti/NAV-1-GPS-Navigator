// =============================================================================
//  Drive app: a dashboard to read at a glance in a car or on a bike. Big speed, the clock, a trip
//  computer (services/Odometer: distance, moving time, average and top speed since its reset),
//  altitude and heading; when Navigator is guiding, a strip with the arrow, the destination and the
//  distance (tap it for Navigate). Record starts / stops a trip (TripRecorder) without leaving the
//  screen. High contrast, no dimming while open. Product UI only.
// =============================================================================
#include "App.h"
#include "../shell/Shell.h"

#include <Arduino.h>
#include "../services/Backlight.h"
#include "../services/Geo.h"
#include "../services/Location.h"
#include "../services/Navigator.h"
#include "../services/Odometer.h"
#include "../services/TimeService.h"
#include "../services/TripRecorder.h"
#include "../ui/NavUi.h"
#include "../ui/UiText.h"

namespace {

constexpr uint32_t C_DIM = 0x9AA4B2, C_REC = 0xC62828, C_GO = 0x2E7D32, C_ARROW = 0x4FC3F7;

struct Ui {
  lv_obj_t *clock = nullptr, *gps = nullptr, *speed = nullptr, *unit = nullptr, *value[6] = {};
  lv_obj_t *strip = nullptr, *arrow = nullptr, *dest = nullptr, *destDist = nullptr;
  lv_obj_t *rec = nullptr, *recLbl = nullptr;
};
Ui s_ui;

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, text);
  return l;
}

const char *const CAPTION[6] = { "Distance", "Moving time", "Average", "Top speed", "Altitude", "Heading" };

void onReset(lv_event_t *) { Odometer::reset(); }
void onStrip(lv_event_t *) { Shell::switchApp("Navigate"); }
void onRec(lv_event_t *) {
  if (TripRecorder::recording()) TripRecorder::stop();
  else TripRecorder::start();
}

String hms(uint32_t s) {
  char b[16];
  snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
  return b;
}

void create(lv_obj_t *content) {
  s_ui = Ui();
  lv_obj_set_style_bg_color(content, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
  s_ui.clock = label(content, &lv_font_montserrat_48, 0xFFFFFF, "--:--");
  lv_obj_set_pos(s_ui.clock, 20, 0);
  s_ui.gps = label(content, &lv_font_montserrat_16, C_DIM);
  lv_obj_align(s_ui.gps, LV_ALIGN_TOP_RIGHT, -20, 22);

  s_ui.speed = label(content, &nav_digits_120, 0xFFFFFF, "--");
  lv_obj_align(s_ui.speed, LV_ALIGN_TOP_MID, 0, 74);
  s_ui.unit = label(content, &lv_font_montserrat_20, C_DIM, "km/h");
  lv_obj_align(s_ui.unit, LV_ALIGN_TOP_MID, 0, 176);

  for (int i = 0; i < 6; i++) {
    const int x = 24 + (i % 2) * 232, y = 222 + (i / 2) * 78;
    lv_obj_set_pos(label(content, &lv_font_montserrat_14, C_DIM, CAPTION[i]), x, y);
    s_ui.value[i] = label(content, &lv_font_montserrat_28, 0xFFFFFF, "--");
    lv_obj_set_pos(s_ui.value[i], x, y + 20);
  }

  s_ui.strip = lv_button_create(content);                  // navigation (hidden when not navigating)
  lv_obj_set_size(s_ui.strip, Apps::CONTENT_W - 24, 84);
  lv_obj_set_pos(s_ui.strip, 12, 460);
  lv_obj_set_style_bg_color(s_ui.strip, lv_color_hex(0x15202E), 0);
  lv_obj_add_event_cb(s_ui.strip, onStrip, LV_EVENT_CLICKED, nullptr);
  s_ui.arrow = NavUi::arrow(s_ui.strip, 60, C_ARROW);
  lv_obj_align(s_ui.arrow, LV_ALIGN_LEFT_MID, -4, 0);
  s_ui.dest = label(s_ui.strip, &nav_he_20, 0xDDDDDD);
  lv_obj_set_width(s_ui.dest, 220);
  lv_label_set_long_mode(s_ui.dest, LV_LABEL_LONG_DOT);
  lv_obj_align(s_ui.dest, LV_ALIGN_LEFT_MID, 70, 0);
  s_ui.destDist = label(s_ui.strip, &lv_font_montserrat_28, 0xFFFFFF);
  lv_obj_align(s_ui.destDist, LV_ALIGN_RIGHT_MID, 0, 0);

  const int y = Apps::CONTENT_H - 76, w = (Apps::CONTENT_W - 24 - 12) / 2;
  lv_obj_t *reset = lv_button_create(content);
  lv_obj_set_size(reset, w, 64);
  lv_obj_set_pos(reset, 12, y);
  lv_obj_set_style_bg_color(reset, lv_color_hex(0x37474F), 0);
  lv_obj_center(label(reset, &lv_font_montserrat_20, 0xFFFFFF, LV_SYMBOL_REFRESH "  Reset trip"));
  lv_obj_add_event_cb(reset, onReset, LV_EVENT_CLICKED, nullptr);
  s_ui.rec = lv_button_create(content);
  lv_obj_set_size(s_ui.rec, w, 64);
  lv_obj_set_pos(s_ui.rec, 24 + w, y);
  lv_obj_add_event_cb(s_ui.rec, onRec, LV_EVENT_CLICKED, nullptr);
  s_ui.recLbl = label(s_ui.rec, &lv_font_montserrat_20, 0xFFFFFF);
  lv_obj_center(s_ui.recLbl);
  Backlight::keepAwake(true);
}

void update() {
  char b[32];
  struct tm lt;
  if (TimeService::local(lt)) snprintf(b, sizeof(b), "%02d:%02d", lt.tm_hour, lt.tm_min);
  else snprintf(b, sizeof(b), "--:--");
  Apps::setText(s_ui.clock, b);

  const GpsData d = Location::snapshot();
  const bool fix = Location::quality(d) == Location::Quality::Fix;
  if (fix) snprintf(b, sizeof(b), LV_SYMBOL_GPS " %u sat", (unsigned)d.satsUsed);
  else snprintf(b, sizeof(b), LV_SYMBOL_GPS " no fix");
  Apps::setText(s_ui.gps, b);
  lv_obj_set_style_text_color(s_ui.gps, lv_color_hex(fix ? C_DIM : 0xFFB300), 0);
  if (fix && d.speedValid) snprintf(b, sizeof(b), "%.0f", d.speedKmh < 1.0 ? 0.0 : d.speedKmh);
  else snprintf(b, sizeof(b), "--");
  Apps::setText(s_ui.speed, b);
  lv_obj_set_style_text_opa(s_ui.speed, fix ? LV_OPA_COVER : LV_OPA_40, 0);

  snprintf(b, sizeof(b), "%.1f km", Odometer::distanceKm());
  Apps::setText(s_ui.value[0], b);
  Apps::setText(s_ui.value[1], hms(Odometer::movingS()).c_str());
  const float avg = Odometer::avgKmh();
  if (avg > 0) snprintf(b, sizeof(b), "%.0f km/h", avg); else snprintf(b, sizeof(b), "--");
  Apps::setText(s_ui.value[2], b);
  snprintf(b, sizeof(b), "%.0f km/h", Odometer::maxKmh());
  Apps::setText(s_ui.value[3], b);
  if (fix && d.altValid) snprintf(b, sizeof(b), "%.0f m", d.altM); else snprintf(b, sizeof(b), "--");
  Apps::setText(s_ui.value[4], b);
  float h;
  if (NavUi::heading(h)) snprintf(b, sizeof(b), "%s %03.0f\xC2\xB0", Geo::cardinal(h), h); else snprintf(b, sizeof(b), "--");
  Apps::setText(s_ui.value[5], b);

  const bool nav = Navigator::active();
  lv_obj_set_hidden(s_ui.strip, !nav);
  if (nav) {
    const Navigator::Status st = Navigator::status();
    UiText::setName(s_ui.dest, st.name);
    if (st.arrived) snprintf(b, sizeof(b), "Arrived");
    else Geo::formatDistance(st.distM, b, sizeof(b));
    Apps::setText(s_ui.destDist, b);
    float angle;
    NavUi::relative(st.bearingDeg, angle);
    NavUi::setArrow(s_ui.arrow, angle, st.fix && st.distM >= 0 && !st.arrived);
  }

  const bool rec = TripRecorder::recording();
  Apps::setText(s_ui.recLbl, rec ? LV_SYMBOL_STOP "  Stop recording" : LV_SYMBOL_PLAY "  Record trip");
  lv_obj_set_style_bg_color(s_ui.rec, lv_color_hex(rec ? C_REC : C_GO), 0);
}

void destroy() {
  Backlight::keepAwake(false);
  s_ui = Ui();
}

}  // namespace

extern const AppImpl DRIVE_APP = { "Drive", create, update, destroy };
