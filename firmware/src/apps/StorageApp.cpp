// =============================================================================
//  Storage app: what fills the SD card, and a clean-up for old GPS logs.
//    - Capacity bar coloured by category + used / free
//    - Categories: Trips, GPS logs, System (settings, images), Other
//    - Clean up: keep only the newest GPS log sessions (NAV-1 starts one at every
//      start-up); asks first, deletes in the background
//  Folder sizes are measured in the background (Storage::statStart) after the
//  screen is shown. Product UI only.
// =============================================================================
#include "../Display.h"
#include "App.h"

#include <Arduino.h>
#include "../services/Storage.h"

namespace {

enum Cat { C_TRIPS, C_LOGS, C_SYSTEM, C_OTHER, C_COUNT };
const char *const CAT_NAME[C_COUNT] = { "Trips", "GPS logs", "System", "Other" };
const uint32_t CAT_COLOR[C_COUNT] = { 0x43A047, 0x1E88E5, 0xFB8C00, 0x8A96A6 };
const char *const PATHS[] = { "/data/trips", "/GPSLOG", "/system", "/assets" };   // system = /system + /assets
constexpr int KEEP_SESSIONS = 20;
constexpr int BAR_W = 436;

struct Ui {
  lv_obj_t *summary = nullptr, *freeText = nullptr, *bar = nullptr;
  lv_obj_t *seg[C_COUNT] = {}, *size[C_COUNT] = {}, *files[C_COUNT] = {};
  lv_obj_t *cleanInfo = nullptr, *cleanBtn = nullptr, *overlay = nullptr;
};
Ui s_ui;
bool s_statPending = false, s_cleaning = false;
uint64_t s_bytes[C_COUNT] = {};
uint32_t s_files[C_COUNT] = {};

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  return l;
}

String humanSize(uint64_t b) {
  char s[24];
  if (b < 1024) snprintf(s, sizeof(s), "%u B", (unsigned)b);
  else if (b < 1024 * 1024) snprintf(s, sizeof(s), "%.1f KB", b / 1024.0);
  else if (b < 1024ULL * 1024 * 1024) snprintf(s, sizeof(s), "%.1f MB", b / 1048576.0);
  else snprintf(s, sizeof(s), "%.1f GB", b / 1073741824.0);
  return s;
}

void measure() {
  s_statPending = Storage::statStart(PATHS, sizeof(PATHS) / sizeof(PATHS[0]));
  if (s_statPending) lv_label_set_text(s_ui.summary, "Measuring...");
}

// Everything that depends on the measured sizes.
void showSizes() {
  uint64_t total = 0, freeB = 0;
  const bool card = Storage::usage(total, freeB);
  for (int i = 0; i < 2; i++) { s_bytes[i] = Storage::stat(i).bytes; s_files[i] = Storage::stat(i).files; }
  s_bytes[C_SYSTEM] = Storage::stat(2).bytes + Storage::stat(3).bytes;
  s_files[C_SYSTEM] = Storage::stat(2).files + Storage::stat(3).files;
  const uint64_t used = card ? total - freeB : 0;
  uint64_t known = 0;
  for (int i = 0; i < C_OTHER; i++) known += s_bytes[i];
  s_bytes[C_OTHER] = used > known ? used - known : 0;           // FAT overhead, other folders
  s_files[C_OTHER] = 0;

  if (!card) {
    lv_label_set_text(s_ui.summary, "No SD card");
    lv_label_set_text(s_ui.freeText, "Insert a card to record trips and GPS logs.");
  } else {
    lv_label_set_text_fmt(s_ui.summary, "%s used of %s", humanSize(used).c_str(), humanSize(total).c_str());
    lv_label_set_text_fmt(s_ui.freeText, "%s free", humanSize(freeB).c_str());
  }
  // Bar: proportional, but every non-empty category at least 8 px so it can be seen
  int x = 0;
  for (int i = 0; i < C_COUNT; i++) {
    int w = total ? (int)((double)s_bytes[i] / total * BAR_W) : 0;
    if (s_bytes[i] && w < 8) w = 8;
    w = min(w, BAR_W - x);
    lv_obj_set_hidden(s_ui.seg[i], w <= 0);
    lv_obj_set_pos(s_ui.seg[i], x, 0);
    lv_obj_set_size(s_ui.seg[i], max(w, 1), 28);
    x += w;
  }
  for (int i = 0; i < C_COUNT; i++) {
    lv_label_set_text(s_ui.size[i], humanSize(s_bytes[i]).c_str());
    if (i == C_OTHER) lv_label_set_text(s_ui.files[i], "overhead, other");
    else lv_label_set_text_fmt(s_ui.files[i], "%lu %s", (unsigned long)s_files[i], s_files[i] == 1 ? "file" : "files");
  }
  const unsigned sessions = (s_files[C_LOGS] + 1) / 2;           // Snnnn.CSV + Snnnn.NMEA
  lv_label_set_text_fmt(s_ui.cleanInfo, "%u GPS log sessions, %s.\nNAV-1 starts a new one at every start-up.", sessions,
                        humanSize(s_bytes[C_LOGS]).c_str());
  lv_obj_set_state(s_ui.cleanBtn, LV_STATE_DISABLED, !card || sessions <= KEEP_SESSIONS || s_cleaning);
}

// ---- clean up -----------------------------------------------------------------------------
void closeOverlay() {
  if (s_ui.overlay) { lv_obj_delete_async(s_ui.overlay); s_ui.overlay = nullptr; }
}
void onCancel(lv_event_t *) { closeOverlay(); }

void onConfirm(lv_event_t *) {
  closeOverlay();
  if (!Storage::removeOldLogsStart(KEEP_SESSIONS)) { lv_label_set_text(s_ui.cleanInfo, "Could not start (card busy)."); return; }
  s_cleaning = true;
  lv_obj_add_state(s_ui.cleanBtn, LV_STATE_DISABLED);
  lv_label_set_text(s_ui.cleanInfo, "Deleting old GPS logs...");
}

void onClean(lv_event_t *e) {
  const unsigned sessions = (s_files[C_LOGS] + 1) / 2;
  lv_obj_t *ov = lv_obj_create(lv_obj_get_screen((lv_obj_t *)lv_event_get_target(e)));
  lv_obj_remove_style_all(ov);
  lv_obj_set_size(ov, Display::UI_W, Display::UI_H);
  lv_obj_set_style_bg_color(ov, lv_color_hex(0x10141C), 0);
  lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
  lv_obj_set_clickable(ov, true);
  lv_obj_t *t = label(ov, &lv_font_montserrat_28, lv_color_white(), "Delete old GPS logs?");
  lv_obj_set_pos(t, 24, 52);
  lv_obj_t *d = label(ov, &lv_font_montserrat_20, lv_color_hex(0xDDDDDD), "");
  lv_label_set_text_fmt(d, "Keeps the newest %d sessions and deletes the other %u.\nTrips are not touched. "
                           "This cannot be undone.", KEEP_SESSIONS, sessions > KEEP_SESSIONS ? sessions - KEEP_SESSIONS : 0);
  lv_obj_set_pos(d, 24, 104);
  lv_obj_set_width(d, 432);
  lv_obj_t *c = lv_button_create(ov);
  lv_obj_set_pos(c, 24, 280);
  lv_obj_set_size(c, 200, 48);
  lv_obj_center(label(c, &lv_font_montserrat_20, lv_color_white(), "Cancel"));
  lv_obj_add_event_cb(c, onCancel, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *ok = lv_button_create(ov);
  lv_obj_set_pos(ok, 256, 280);
  lv_obj_set_size(ok, 200, 48);
  lv_obj_set_style_bg_color(ok, lv_color_hex(0xC62828), 0);
  lv_obj_center(label(ok, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_TRASH "  Delete"));
  lv_obj_add_event_cb(ok, onConfirm, LV_EVENT_CLICKED, nullptr);
  s_ui.overlay = ov;
}

// ---- lifecycle -------------------------------------------------------------------------
void create(lv_obj_t *content) {
  s_ui = Ui();
  s_cleaning = Storage::removing();

  lv_obj_t *top = Apps::card(content, 12, 0, 456, 110);
  lv_obj_t *t = label(top, &lv_font_montserrat_28, lv_color_white(), LV_SYMBOL_SD_CARD "  SD card");
  lv_obj_set_pos(t, 4, 2);
  s_ui.summary = label(top, &lv_font_montserrat_20, lv_color_white());
  lv_obj_align(s_ui.summary, LV_ALIGN_TOP_RIGHT, -4, 8);
  s_ui.bar = lv_obj_create(top);                                 // free space = the bar's background
  lv_obj_remove_style_all(s_ui.bar);
  lv_obj_set_pos(s_ui.bar, 4, 44);
  lv_obj_set_size(s_ui.bar, BAR_W, 28);
  lv_obj_set_style_bg_color(s_ui.bar, lv_color_hex(0x2A3440), 0);
  lv_obj_set_style_bg_opa(s_ui.bar, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_ui.bar, 8, 0);
  lv_obj_set_style_clip_corner(s_ui.bar, true, 0);
  for (int i = 0; i < C_COUNT; i++) {
    s_ui.seg[i] = lv_obj_create(s_ui.bar);
    lv_obj_remove_style_all(s_ui.seg[i]);
    lv_obj_set_style_bg_color(s_ui.seg[i], lv_color_hex(CAT_COLOR[i]), 0);
    lv_obj_set_style_bg_opa(s_ui.seg[i], LV_OPA_COVER, 0);
    lv_obj_set_hidden(s_ui.seg[i], true);
  }
  s_ui.freeText = label(top, &lv_font_montserrat_14, lv_color_hex(0xBBBBBB));
  lv_obj_set_pos(s_ui.freeText, 4, 78);

  lv_obj_t *list = Apps::card(content, 12, 122, 456, 200);
  for (int i = 0; i < C_COUNT; i++) {
    const int y = 6 + i * 44;
    lv_obj_t *dot = lv_obj_create(list);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(CAT_COLOR[i]), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot, 4, y + 5);
    lv_obj_t *n = label(list, &lv_font_montserrat_20, lv_color_white(), CAT_NAME[i]);
    lv_obj_set_pos(n, 28, y);
    s_ui.files[i] = label(list, &lv_font_montserrat_14, lv_color_hex(0xBBBBBB));
    lv_obj_set_pos(s_ui.files[i], 160, y + 4);
    s_ui.size[i] = label(list, &lv_font_montserrat_20, lv_color_white(), "-");
    lv_obj_align(s_ui.size[i], LV_ALIGN_TOP_RIGHT, -4, y);
  }

  lv_obj_t *cl = Apps::card(content, 12, 334, 456, 190);
  lv_obj_t *ct = label(cl, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_TRASH "  Clean up");
  lv_obj_set_pos(ct, 4, 4);
  s_ui.cleanInfo = label(cl, &lv_font_montserrat_14, lv_color_hex(0xDDDDDD));
  lv_obj_set_pos(s_ui.cleanInfo, 4, 40);
  lv_obj_set_width(s_ui.cleanInfo, 430);
  s_ui.cleanBtn = lv_button_create(cl);
  lv_obj_set_size(s_ui.cleanBtn, 430, 48);
  lv_obj_align(s_ui.cleanBtn, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_t *bl = label(s_ui.cleanBtn, &lv_font_montserrat_14, lv_color_white(), "");
  lv_label_set_text_fmt(bl, "Keep the newest %d sessions", KEEP_SESSIONS);
  lv_obj_center(bl);
  lv_obj_add_event_cb(s_ui.cleanBtn, onClean, LV_EVENT_CLICKED, nullptr);

  showSizes();                                                   // card size now, folders when measured
  if (!s_cleaning) measure();
}

void update() {
  if (s_cleaning && !Storage::removing()) {                      // clean-up finished: measure again
    s_cleaning = false;
    const uint32_t n = Storage::removedCount();
    measure();
    showSizes();
    lv_label_set_text_fmt(s_ui.cleanInfo, "%s %lu old GPS log files.", Storage::removeFailed() ? "Stopped after" : "Deleted",
                          (unsigned long)n);
    return;
  }
  if (s_statPending && !Storage::statBusy()) {
    s_statPending = false;
    showSizes();
  }
}

void destroy() { s_ui = Ui(); }

}  // namespace

extern const AppImpl STORAGE_APP = { "Storage", create, update, destroy };
