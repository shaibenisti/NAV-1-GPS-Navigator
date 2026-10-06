#include "App.h"

#include <string.h>

extern const AppImpl GPS_APP;
extern const AppImpl SETTINGS_APP;
extern const AppImpl TOOLS_APP;
extern const AppImpl TRIPS_APP;
extern const AppImpl PHONE_APP;
extern const AppImpl FILES_APP;
extern const AppImpl STORAGE_APP;
extern const AppImpl COMPASS_APP;
extern const AppImpl MAP_APP;

namespace {
// The launcher: home tile order (6 per page), symbol, tile colour, implementation
// (nullptr = placeholder app, not built yet).
const Apps::Entry ENTRIES[] = {
  {"Phone",    LV_SYMBOL_CALL,     0x8E24AA, &PHONE_APP},    {"Tools",    LV_SYMBOL_LIST,      0xE53935, &TOOLS_APP},
  {"Trips",    LV_SYMBOL_LOOP,     0x43A047, &TRIPS_APP},    {"Settings", LV_SYMBOL_SETTINGS,  0x546E7A, &SETTINGS_APP},
  {"GPS",      LV_SYMBOL_GPS,      0x1E88E5, &GPS_APP},      {"Files",    LV_SYMBOL_DIRECTORY, 0xFB8C00, &FILES_APP},
  {"Map",      LV_SYMBOL_IMAGE,    0x00897B, &MAP_APP},       {"Compass",  LV_SYMBOL_EYE_OPEN,  0x3949AB, &COMPASS_APP},
  {"Alerts",   LV_SYMBOL_BELL,     0xC0CA33, nullptr},       {"Notes",    LV_SYMBOL_EDIT,      0x6D4C41, nullptr},
  {"Messages", LV_SYMBOL_ENVELOPE, 0xD81B60, nullptr},       {"Storage",  LV_SYMBOL_DRIVE,     0x00ACC1, &STORAGE_APP},
};
static_assert(sizeof(ENTRIES) / sizeof(ENTRIES[0]) == Apps::COUNT, "launcher has COUNT tiles");
}

const Apps::Entry &Apps::entry(int i) { return ENTRIES[i < 0 || i >= COUNT ? 0 : i]; }

int Apps::indexOf(const char *name) {
  for (int i = 0; i < COUNT; i++)
    if (!strcasecmp(ENTRIES[i].name, name)) return i;
  return -1;
}

const AppImpl *Apps::find(const char *name) {
  const int i = indexOf(name);
  return i < 0 ? nullptr : ENTRIES[i].impl;
}

void Apps::setText(lv_obj_t *label, const char *text) {
  if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void Apps::buildMenu(lv_obj_t *content, const char *const *names, int n, lv_obj_t **menu, lv_obj_t **pages,
                     lv_event_cb_t onEntry) {
  // Portrait: a tab strip (3 per row) on top, the selected page full width below it.
  const int rows = (n + MENU_COLS - 1) / MENU_COLS;
  const int mh = 20 + rows * MENU_BTN_H + (rows - 1) * 8;
  lv_obj_t *m = card(content, PAGE_X, 0, PAGE_W, mh);
  lv_obj_set_flex_flow(m, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(m, 8, 0);
  lv_obj_set_style_pad_column(m, 8, 0);
  const int bw = (PAGE_W - 20 - (MENU_COLS - 1) * 8) / MENU_COLS;
  for (int i = 0; i < n; i++) {
    lv_obj_t *b = lv_button_create(m);
    lv_obj_set_size(b, bw, MENU_BTN_H);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, names[i]);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(b, onEntry, LV_EVENT_CLICKED, (void *)(intptr_t)i);   // user data = page index
    menu[i] = b;
    pages[i] = card(content, PAGE_X, mh + 8, PAGE_W, CONTENT_H - mh - 8);
  }
}

void Apps::showMenuPage(lv_obj_t *const *menu, lv_obj_t *const *pages, int n, int page) {
  for (int i = 0; i < n; i++) {
    lv_obj_set_hidden(pages[i], i != page);
    lv_obj_set_state(menu[i], LV_STATE_CHECKED, i == page);   // selected entry: theme accent colour
  }
}

lv_obj_t *Apps::pageTitle(lv_obj_t *page, const char *text) {
  lv_obj_t *t = lv_label_create(page);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_label_set_text(t, text);
  lv_obj_set_pos(t, 4, 4);
  return t;
}

lv_obj_t *Apps::card(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_pos(c, x, y);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_30, 0);
  lv_obj_set_style_radius(c, 14, 0);
  lv_obj_set_style_pad_all(c, 10, 0);
  lv_obj_set_scrollable(c, false);
  return c;
}
