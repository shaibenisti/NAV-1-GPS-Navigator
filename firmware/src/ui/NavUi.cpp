#include "NavUi.h"

#include <Arduino.h>
#include <math.h>
#include "../Display.h"
#include "../services/Location.h"
#include "UiText.h"

namespace {

constexpr float START_KMH = 3.0f, HOLD_KMH = 1.5f;      // as the Compass app
bool s_moving = false;
float s_heading = 0;

struct ArrowData { float angle; uint32_t color; bool live; };

void onDrawArrow(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target_obj(e);
  const ArrowData *a = (const ArrowData *)lv_obj_get_user_data(o);
  if (!a) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t c;
  lv_obj_get_coords(o, &c);
  const float cx = (c.x1 + c.x2) / 2.0f, cy = (c.y1 + c.y2) / 2.0f, r = (c.x2 - c.x1) / 2.0f;
  const float th = a->angle * (float)M_PI / 180.0f, cs = cosf(th), sn = sinf(th);
  // arrow in unit coordinates (up = -y): a head and a shaft, so the direction reads at a glance
  const float P[7][2] = { { 0, -0.95f }, { 0.55f, -0.15f }, { -0.55f, -0.15f },        // head
                          { 0.2f, -0.2f }, { -0.2f, -0.2f }, { 0.2f, 0.9f }, { -0.2f, 0.9f } };   // shaft
  auto pt = [&](int i, lv_point_precise_t &p) {
    const float x = P[i][0] * r, y = P[i][1] * r;
    p.x = (lv_value_precise_t)(cx + x * cs - y * sn);
    p.y = (lv_value_precise_t)(cy + x * sn + y * cs);
  };
  lv_draw_triangle_dsc_t td;
  lv_draw_triangle_dsc_init(&td);
  td.color = lv_color_hex(a->color);
  td.opa = a->live ? LV_OPA_COVER : LV_OPA_40;
  static const uint8_t T[3][3] = { { 0, 1, 2 }, { 3, 4, 5 }, { 4, 6, 5 } };
  for (const auto &t : T) {
    pt(t[0], td.p[0]); pt(t[1], td.p[1]); pt(t[2], td.p[2]);
    lv_draw_triangle(layer, &td);
  }
}

void onDeleteArrow(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target_obj(e);
  free(lv_obj_get_user_data(o));
  lv_obj_set_user_data(o, nullptr);
}

// ---- keyboard dialog ----
// Latin (LVGL's own maps) or Hebrew (the map below, standard Israeli layout). The text area only holds the
// text in typed (logical) order; what is shown is a label above it in visual order (UiText), so Hebrew reads
// right to left while it is typed. The length limit is in bytes (a Hebrew letter is 2).
lv_obj_t *s_kbOverlay = nullptr, *s_kbTa = nullptr, *s_kbShow = nullptr, *s_kbKb = nullptr, *s_kbLangLbl = nullptr;
void (*s_kbDone)(const char *) = nullptr;
int s_kbMaxBytes = 0;
bool s_kbHebrew = false;

const char *const KB_HE[] = {
  "\xd7\xa7", "\xd7\xa8", "\xd7\x90", "\xd7\x98", "\xd7\x95", "\xd7\x9f", "\xd7\x9d", "\xd7\xa4", LV_SYMBOL_BACKSPACE, "\n",   // ק ר א ט ו ן ם פ
  "\xd7\xa9", "\xd7\x93", "\xd7\x92", "\xd7\x9b", "\xd7\xa2", "\xd7\x99", "\xd7\x97", "\xd7\x9c", "\xd7\x9a", "\xd7\xa3", "\n",   // ש ד ג כ ע י ח ל ך ף
  "\xd7\x96", "\xd7\xa1", "\xd7\x91", "\xd7\x94", "\xd7\xa0", "\xd7\x9e", "\xd7\xa6", "\xd7\xaa", "\xd7\xa5", "-", "\n",        // ז ס ב ה נ מ צ ת ץ
  LV_SYMBOL_KEYBOARD, "1#", "'", " ", ".", LV_SYMBOL_OK, "" };
constexpr lv_buttonmatrix_ctrl_t C(int v) { return (lv_buttonmatrix_ctrl_t)v; }   // (a C enum in C++)
constexpr int KB_BTN = LV_BUTTONMATRIX_CTRL_POPOVER | 4;
constexpr int KB_FN = LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED;
const lv_buttonmatrix_ctrl_t KB_HE_CTRL[] = {
  C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(LV_BUTTONMATRIX_CTRL_CHECKED | 7),
  C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN),
  C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN), C(KB_BTN),
  C(KB_FN | 3), C(KB_FN | 3), C(2), C(9), C(2), C(KB_FN | 3) };

void kbShowText() {
  if (!s_kbShow || !s_kbTa) return;
  const char *t = lv_textarea_get_text(s_kbTa);
  char v[160];
  UiText::visual(t, v, sizeof(v) - 4);
  bool rtl = false;                                      // the cursor sits where the next letter appears
  for (const unsigned char *p = (const unsigned char *)t; *p; p++) {
    if (*p == 0xD7) { rtl = true; break; }               // Hebrew letters are 0xD7 0x90..0xAA in UTF-8
    if (isalnum(*p)) break;
  }
  char s[168];
  snprintf(s, sizeof(s), rtl ? "|%s" : "%s|", v);
  lv_label_set_text(s_kbShow, s);
  lv_obj_set_style_text_align(s_kbShow, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);   // Hebrew: from the right
}

void kbSetHebrew(bool he) {
  s_kbHebrew = he;
  if (he) {
    lv_keyboard_set_map(s_kbKb, LV_KEYBOARD_MODE_USER_1, KB_HE, KB_HE_CTRL);
    lv_keyboard_set_mode(s_kbKb, LV_KEYBOARD_MODE_USER_1);
  } else {
    lv_keyboard_set_mode(s_kbKb, LV_KEYBOARD_MODE_TEXT_LOWER);
  }
  lv_label_set_text(s_kbLangLbl, he ? "ABC" : "\xd7\xa2\xd7\x91");   // the language a tap switches to: "עב"
}

void onKbLang(lv_event_t *) { kbSetHebrew(!s_kbHebrew); }

void onKbText(lv_event_t *) {                             // typed: byte limit, then the visual copy
  const char *t = lv_textarea_get_text(s_kbTa);
  while (s_kbMaxBytes > 0 && (int)strlen(t) > s_kbMaxBytes) { lv_textarea_delete_char(s_kbTa); t = lv_textarea_get_text(s_kbTa); }
  kbShowText();
}

void onKb(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;
  void (*done)(const char *) = s_kbDone;
  char text[64];
  strlcpy(text, s_kbTa ? lv_textarea_get_text(s_kbTa) : "", sizeof(text));
  NavUi::keyboardClose();
  if (done) done(code == LV_EVENT_READY ? text : nullptr);
}

}  // namespace

lv_obj_t *NavUi::arrow(lv_obj_t *parent, int size, uint32_t color) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, size, size);
  lv_obj_set_clickable(o, false);
  ArrowData *a = (ArrowData *)malloc(sizeof(ArrowData));
  if (a) { a->angle = 0; a->color = color; a->live = true; }
  lv_obj_set_user_data(o, a);
  lv_obj_add_event_cb(o, onDrawArrow, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(o, onDeleteArrow, LV_EVENT_DELETE, nullptr);
  return o;
}

void NavUi::setArrow(lv_obj_t *o, float angleDeg, bool live) {
  ArrowData *a = o ? (ArrowData *)lv_obj_get_user_data(o) : nullptr;
  if (!a) return;
  while (angleDeg < 0) angleDeg += 360;
  while (angleDeg >= 360) angleDeg -= 360;
  if (fabsf(a->angle - angleDeg) < 1.0f && a->live == live) return;
  a->angle = angleDeg;
  a->live = live;
  lv_obj_invalidate(o);
}

bool NavUi::heading(float &deg) {
  const GpsData d = Location::snapshot();
  const bool fix = Location::quality(d) == Location::Quality::Fix;
  const float kmh = d.speedValid && fix ? (float)d.speedKmh : -1.0f;
  s_moving = fix && d.courseValid && kmh >= (s_moving ? HOLD_KMH : START_KMH);
  if (s_moving) s_heading = (float)d.courseDeg;
  deg = s_heading;
  return s_moving;
}

bool NavUi::relative(float bearingDeg, float &angleDeg) {
  float h;
  const bool moving = heading(h);
  angleDeg = moving ? bearingDeg - h : bearingDeg;
  return moving;
}

void NavUi::formatEta(uint32_t s, char *out, size_t n) {
  if (!s) { snprintf(out, n, "--"); return; }
  const uint32_t m = (s + 30) / 60;
  if (m < 60) snprintf(out, n, "%lu min", (unsigned long)max<uint32_t>(1, m));
  else snprintf(out, n, "%lu h %02lu", (unsigned long)(m / 60), (unsigned long)(m % 60));
}

void NavUi::keyboard(lv_obj_t *anyChild, const char *title, const char *initial, int maxBytes, void (*done)(const char *)) {
  keyboardClose();
  lv_obj_t *ov = lv_obj_create(lv_obj_get_screen(anyChild));
  lv_obj_remove_style_all(ov);
  lv_obj_set_size(ov, Display::UI_W, Display::UI_H);
  lv_obj_set_style_bg_color(ov, lv_color_hex(0x10141C), 0);
  lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
  lv_obj_set_clickable(ov, true);                   // swallows taps
  lv_obj_t *t = lv_label_create(ov);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_obj_set_width(t, Display::UI_W - 48);
  lv_label_set_text_fmt(t, "%s\n" LV_SYMBOL_OK " save   " LV_SYMBOL_KEYBOARD " cancel", title);
  lv_obj_set_pos(t, 24, 60);
  lv_obj_t *ta = lv_textarea_create(ov);              // holds the text; its own drawing is hidden (logical order)
  lv_textarea_set_one_line(ta, true);
  lv_obj_set_size(ta, Display::UI_W - 48 - 96, 56);
  lv_obj_set_pos(ta, 24, 136);
  lv_obj_set_style_text_opa(ta, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
  lv_textarea_set_text(ta, initial ? initial : "");
  s_kbShow = lv_label_create(ta);                      // the text as it reads (Hebrew right to left)
  lv_obj_set_style_text_font(s_kbShow, &nav_he_20, 0);
  lv_obj_set_style_text_color(s_kbShow, lv_color_white(), 0);
  lv_obj_set_style_text_opa(s_kbShow, LV_OPA_COVER, 0);   // (the text area's transparent text is inherited)
  lv_obj_set_width(s_kbShow, lv_pct(100));
  lv_label_set_long_mode(s_kbShow, LV_LABEL_LONG_CLIP);
  lv_obj_align(s_kbShow, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *lang = lv_button_create(ov);              // Latin <-> Hebrew
  lv_obj_set_size(lang, 84, 56);
  lv_obj_set_pos(lang, Display::UI_W - 24 - 84, 136);
  lv_obj_add_event_cb(lang, onKbLang, LV_EVENT_CLICKED, nullptr);
  s_kbLangLbl = lv_label_create(lang);
  lv_obj_set_style_text_font(s_kbLangLbl, &nav_he_20, 0);
  lv_obj_center(s_kbLangLbl);
  lv_obj_t *kb = lv_keyboard_create(ov);
  lv_obj_set_size(kb, Display::UI_W, 320);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(kb, &nav_he_20, LV_PART_ITEMS);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_add_event_cb(kb, onKb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(kb, onKb, LV_EVENT_CANCEL, nullptr);
  lv_obj_add_event_cb(ta, onKbText, LV_EVENT_VALUE_CHANGED, nullptr);
  s_kbOverlay = ov;
  s_kbTa = ta;
  s_kbKb = kb;
  s_kbDone = done;
  s_kbMaxBytes = maxBytes;
  bool he = false;                                     // start in the language of the name being edited
  for (const unsigned char *p = (const unsigned char *)(initial ? initial : ""); *p; p++) if (*p == 0xD7) { he = true; break; }
  kbSetHebrew(he);
  kbShowText();
}

bool NavUi::keyboardOpen() { return s_kbOverlay != nullptr; }

void NavUi::keyboardClose() {
  if (s_kbOverlay) lv_obj_delete_async(s_kbOverlay);
  s_kbOverlay = s_kbTa = s_kbShow = s_kbKb = s_kbLangLbl = nullptr;
  s_kbDone = nullptr;
}
