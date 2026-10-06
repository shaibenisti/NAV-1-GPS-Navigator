// =============================================================================
//  Files app: SD card usage, browse folders, file details,
//  view text files, delete files and folders. Product UI only.
// -----------------------------------------------------------------------------
//  - Folders open on tap; files show their details on the right (View, Delete).
//  - Newest first (folders by name first), in pages of ROWS: the row objects are
//    created once and only relabelled (100 new rows took ~0.6 s for /GPSLOG).
//  - Delete asks first and runs in the background (Storage); files being written
//    (GPS session, recording trip, field test) cannot be deleted.
// =============================================================================
#include "../Display.h"
#include "App.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include "../services/Storage.h"

namespace {

constexpr int MAX_ENTRIES = 600;         // read per folder (PSRAM)
constexpr int ROWS = 20;                 // rows per page (created once)
constexpr size_t VIEW_BYTES = 3072;

struct Ui {
  lv_obj_t *up = nullptr, *path = nullptr, *list = nullptr, *empty = nullptr;
  lv_obj_t *prev = nullptr, *next = nullptr, *page = nullptr;
  lv_obj_t *row[ROWS] = {}, *rowName[ROWS] = {}, *rowMeta[ROWS] = {};
  lv_obj_t *info = nullptr, *viewBtn = nullptr, *delBtn = nullptr, *delLbl = nullptr;
  lv_obj_t *overlay = nullptr;
};
Ui s_ui;

Storage::Entry *s_ents = nullptr;
int s_count = 0, s_total = 0;
String s_cwd = "/";
int s_sel = -1;                          // selected file (index into s_ents), -1 = the folder itself
int s_page = 0;
String s_usage;                          // "250.0 GB free of 250.0 GB"
bool s_deleting = false;
String s_delPath;

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  return l;
}

lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, lv_event_cb_t cb, void *user = nullptr,
                 lv_obj_t **lbl = nullptr) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, 44);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  if (lbl) *lbl = l;
  return b;
}

String humanSize(uint64_t b) {
  char s[24];
  if (b < 1024) snprintf(s, sizeof(s), "%u B", (unsigned)b);
  else if (b < 1024 * 1024) snprintf(s, sizeof(s), "%.1f KB", b / 1024.0);
  else if (b < 1024ULL * 1024 * 1024) snprintf(s, sizeof(s), "%.1f MB", b / 1048576.0);
  else snprintf(s, sizeof(s), "%.1f GB", b / 1073741824.0);
  return s;
}

String dateText(const Storage::Entry &e) {
  const int year = 1980 + (e.fdate >> 9);
  if (year < 2020) return "";                        // written before the clock was set
  char s[20];
  snprintf(s, sizeof(s), "%04d-%02u-%02u %02u:%02u", year, (e.fdate >> 5) & 15, e.fdate & 31, e.ftime >> 11, (e.ftime >> 5) & 63);
  return s;
}

String childPath(const char *name) { return (s_cwd == "/" ? String("/") : s_cwd + "/") + name; }
String targetPath() { return s_sel >= 0 ? childPath(s_ents[s_sel].name) : s_cwd; }

bool newerFirst(const Storage::Entry &a, const Storage::Entry &b) {   // a before b?
  if (a.dir != b.dir) return a.dir;
  if (a.dir) return strcasecmp(a.name, b.name) < 0;
  const uint32_t ta = ((uint32_t)a.fdate << 16) | a.ftime, tb = ((uint32_t)b.fdate << 16) | b.ftime;
  if (ta != tb) return ta > tb;
  return strcasecmp(a.name, b.name) > 0;
}

void showDetails();
void load(const String &dir);

// ---- overlays: confirm delete, text viewer ---------------------------------------------
void closeOverlay() {
  if (s_ui.overlay) { lv_obj_delete_async(s_ui.overlay); s_ui.overlay = nullptr; }
}
void onClose(lv_event_t *) { closeOverlay(); }

lv_obj_t *openOverlay(const char *title) {
  lv_obj_t *ov = lv_obj_create(lv_obj_get_screen(s_ui.list));
  lv_obj_remove_style_all(ov);
  lv_obj_set_size(ov, Display::UI_W, Display::UI_H);
  lv_obj_set_style_bg_color(ov, lv_color_hex(0x10141C), 0);
  lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);           // opaque: cheap to draw
  lv_obj_set_clickable(ov, true);                          // swallows taps
  lv_obj_t *t = label(ov, &lv_font_montserrat_20, lv_color_white(), title);
  lv_obj_set_pos(t, 24, 52);
  lv_obj_set_width(t, 432);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  s_ui.overlay = ov;
  return ov;
}

void onDeleteConfirmed(lv_event_t *) {
  closeOverlay();
  s_delPath = targetPath();
  if (!Storage::removeStart(s_delPath.c_str())) { lv_label_set_text(s_ui.info, "Could not delete\n(in use or card busy)."); return; }
  s_deleting = true;
  lv_label_set_text(s_ui.info, "Deleting...");
  lv_obj_add_state(s_ui.delBtn, LV_STATE_DISABLED);
  lv_obj_add_state(s_ui.viewBtn, LV_STATE_DISABLED);
}

void onDelete(lv_event_t *) {
  const String p = targetPath();
  const String q = "Delete " + p + " ?";
  lv_obj_t *ov = openOverlay(q.c_str());
  const String what = s_sel >= 0 ? humanSize(s_ents[s_sel].size) + ". This cannot be undone."
                                 : "The folder and everything in it (" + String(s_total) + " items at the top level).\nThis cannot be undone.";
  lv_obj_t *w = label(ov, &lv_font_montserrat_20, lv_color_hex(0xDDDDDD), what.c_str());
  lv_obj_set_pos(w, 24, 100);
  lv_obj_set_width(w, 432);
  button(ov, "Cancel", 24, 240, 200, onClose);
  lv_obj_t *d = button(ov, LV_SYMBOL_TRASH "  Delete", 256, 240, 200, onDeleteConfirmed);
  lv_obj_set_style_bg_color(d, lv_color_hex(0xC62828), 0);
}

void onView(lv_event_t *) {
  if (s_sel < 0) return;
  const String p = targetPath();
  char *buf = (char *)heap_caps_malloc(VIEW_BYTES + 1, MALLOC_CAP_SPIRAM);
  if (!buf) return;
  const int n = Storage::read(p.c_str(), 0, (uint8_t *)buf, VIEW_BYTES);
  for (int i = 0; i < n; i++)                               // keep the label printable
    if (buf[i] != '\n' && (buf[i] < 32 || buf[i] > 126)) buf[i] = buf[i] == '\r' ? ' ' : '.';
  buf[n > 0 ? n : 0] = 0;
  const String title = p + (s_ents[s_sel].size > VIEW_BYTES ? "   (first 3 KB)" : "");
  lv_obj_t *ov = openOverlay(title.c_str());
  button(ov, LV_SYMBOL_CLOSE "  Close", 296, 84, 160, onClose);
  lv_obj_t *box = lv_obj_create(ov);
  lv_obj_remove_style_all(box);
  lv_obj_set_pos(box, 24, 150);
  lv_obj_set_size(box, 432, 620);
  lv_obj_set_scroll_dir(box, LV_DIR_VER);
  lv_obj_t *txt = label(box, &lv_font_montserrat_14, lv_color_hex(0xDDDDDD), n > 0 ? buf : "(empty)");
  lv_obj_set_width(txt, 420);
  heap_caps_free(buf);
}

// ---- list ----------------------------------------------------------------------------
void showPage() {
  const int pages = max(1, (s_count + ROWS - 1) / ROWS);
  s_page = constrain(s_page, 0, pages - 1);
  for (int r = 0; r < ROWS; r++) {
    const int i = s_page * ROWS + r;
    lv_obj_set_hidden(s_ui.row[r], i >= s_count);
    if (i >= s_count) continue;
    const Storage::Entry &e = s_ents[i];
    lv_label_set_text_fmt(s_ui.rowName[r], "%s  %s", e.dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, e.name);
    const String d = e.dir ? String() : dateText(e);
    const String meta = e.dir ? String() : humanSize(e.size) + (d.length() ? "   " + d.substring(5) : "");
    lv_label_set_text(s_ui.rowMeta[r], meta.c_str());
    lv_obj_set_state(s_ui.row[r], LV_STATE_CHECKED, i == s_sel);
  }
  const bool card = Storage::cardPresent();
  lv_label_set_text(s_ui.empty, !card ? "No SD card" : "Empty folder");
  lv_obj_set_hidden(s_ui.empty, card && s_count > 0);
  const bool paged = s_count > ROWS;
  lv_obj_set_hidden(s_ui.prev, !paged);
  lv_obj_set_hidden(s_ui.next, !paged);
  lv_obj_set_state(s_ui.prev, LV_STATE_DISABLED, s_page == 0);
  lv_obj_set_state(s_ui.next, LV_STATE_DISABLED, s_page >= pages - 1);
  if (paged) lv_label_set_text_fmt(s_ui.page, "%d-%d of %d", s_page * ROWS + 1, min(s_count, (s_page + 1) * ROWS), s_total);
  else lv_label_set_text(s_ui.page, "");
  lv_obj_scroll_to_y(s_ui.list, 0, LV_ANIM_OFF);
}

void onPage(lv_event_t *e) {
  s_page += (int)(intptr_t)lv_event_get_user_data(e);
  showPage();
}

void onUp(lv_event_t *) {
  if (s_cwd == "/") return;
  const int slash = s_cwd.lastIndexOf('/');
  load(slash <= 0 ? String("/") : s_cwd.substring(0, slash));
}

void onRow(lv_event_t *e) {
  const int i = s_page * ROWS + (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_count || s_deleting) return;
  if (s_ents[i].dir) { load(childPath(s_ents[i].name)); return; }
  s_sel = (s_sel == i) ? -1 : i;                            // tap again: back to the folder
  for (int r = 0; r < ROWS; r++) lv_obj_set_state(s_ui.row[r], LV_STATE_CHECKED, s_page * ROWS + r == s_sel);
  showDetails();
}

void showDetails() {
  const String p = targetPath();
  String t;
  if (s_sel >= 0) {
    const Storage::Entry &e = s_ents[s_sel];
    const String d = dateText(e);
    t = String(e.name) + "\n" + humanSize(e.size) + (d.length() ? "\n" + d : "");
  } else {
    uint64_t bytes = 0;
    int files = 0;
    for (int i = 0; i < s_count; i++) if (!s_ents[i].dir) { bytes += s_ents[i].size; files++; }
    t = s_cwd == "/" ? "SD card\n" + s_usage : s_cwd;
    t += "\n" + String(s_total) + " items";
    if (files) t += "\n" + String(files) + " files, " + humanSize(bytes);
    if (s_total > s_count) t += "\n(first " + String(s_count) + " read)";
    t += "\n\nTap a file for details.";
  }
  const bool inUse = Storage::inUse(p.c_str());
  if (inUse && p != "/") t += "\n\nIn use (being recorded):\ncannot be deleted now.";
  lv_label_set_text(s_ui.info, t.c_str());
  lv_obj_set_hidden(s_ui.viewBtn, s_sel < 0);
  lv_obj_set_state(s_ui.viewBtn, LV_STATE_DISABLED, s_deleting);
  lv_obj_set_state(s_ui.delBtn, LV_STATE_DISABLED, inUse || s_deleting);
  lv_obj_set_hidden(s_ui.delBtn, p == "/");
  lv_label_set_text(s_ui.delLbl, s_sel >= 0 ? LV_SYMBOL_TRASH "  Delete file" : LV_SYMBOL_TRASH "  Delete folder");
}

void load(const String &dir) {
  const uint32_t t0 = millis();
  s_cwd = dir;
  s_sel = -1;
  s_page = 0;
  s_count = s_ents ? Storage::list(dir.c_str(), s_ents, MAX_ENTRIES, &s_total) : -1;
  if (s_count < 0) { s_count = 0; s_total = 0; }
  std::sort(s_ents, s_ents + s_count, newerFirst);
  lv_label_set_text(s_ui.path, dir.c_str());
  lv_obj_set_state(s_ui.up, LV_STATE_DISABLED, dir == "/");
  showPage();
  showDetails();
  Serial.printf("[FILES] %s: %d entries, %u ms\n", dir.c_str(), s_total, (unsigned)(millis() - t0));
}

void updateUsage() {
  uint64_t total = 0, free = 0;
  s_usage = Storage::usage(total, free) ? humanSize(free) + " free of " + humanSize(total) : String();
}

// ---- lifecycle -------------------------------------------------------------------------
void create(lv_obj_t *content) {
  s_ui = Ui();
  s_deleting = Storage::removing();                        // reopened during a delete
  if (!s_ents) s_ents = (Storage::Entry *)heap_caps_malloc(sizeof(Storage::Entry) * MAX_ENTRIES, MALLOC_CAP_SPIRAM);
  s_ui.up = button(content, LV_SYMBOL_UP, 12, 0, 64, onUp);
  s_ui.path = label(content, &lv_font_montserrat_20, lv_color_white());
  lv_obj_set_pos(s_ui.path, 88, 10);
  lv_obj_set_width(s_ui.path, 236);
  lv_label_set_long_mode(s_ui.path, LV_LABEL_LONG_DOT);
  s_ui.prev = button(content, LV_SYMBOL_LEFT, 332, 0, 64, onPage, (void *)(intptr_t)-1);
  s_ui.next = button(content, LV_SYMBOL_RIGHT, 404, 0, 64, onPage, (void *)(intptr_t)1);
  s_ui.page = label(content, &lv_font_montserrat_14, lv_color_hex(0xDDDDDD));
  lv_obj_set_width(s_ui.page, 236);
  lv_obj_set_pos(s_ui.page, 88, 38);

  s_ui.list = lv_obj_create(content);                      // scrollable column of ROWS reusable rows
  lv_obj_remove_style_all(s_ui.list);
  lv_obj_set_pos(s_ui.list, 12, 64);
  lv_obj_set_size(s_ui.list, 456, 300);
  lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_ui.list, 6, 0);
  lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
  s_ui.empty = label(s_ui.list, &lv_font_montserrat_20, lv_color_white());
  for (int r = 0; r < ROWS; r++) {
    lv_obj_t *b = lv_button_create(s_ui.list);
    lv_obj_set_size(b, lv_pct(100), 44);
    lv_obj_add_event_cb(b, onRow, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    s_ui.rowName[r] = lv_label_create(b);
    lv_obj_align(s_ui.rowName[r], LV_ALIGN_LEFT_MID, 0, 0);
    s_ui.rowMeta[r] = lv_label_create(b);
    lv_obj_align(s_ui.rowMeta[r], LV_ALIGN_RIGHT_MID, 0, 0);
    s_ui.row[r] = b;
  }

  lv_obj_t *r = Apps::card(content, 12, 376, 456, 312);
  s_ui.info = label(r, &lv_font_montserrat_20, lv_color_hex(0xDDDDDD));
  lv_obj_set_pos(s_ui.info, 4, 4);
  lv_obj_set_width(s_ui.info, 430);
  s_ui.viewBtn = button(r, LV_SYMBOL_EYE_OPEN "  View", 0, 240, 210, onView);
  s_ui.delBtn = button(r, "", 220, 240, 210, onDelete, nullptr, &s_ui.delLbl);
  updateUsage();
  load(s_cwd);                                             // reopens the last folder
}

void update() {
  if (!s_deleting || Storage::removing()) return;
  s_deleting = false;                                     // background delete finished
  const bool failed = Storage::removeFailed();
  const uint32_t n = Storage::removedCount();
  if (s_delPath == s_cwd) {                               // deleted the open folder: go up
    const int slash = s_cwd.lastIndexOf('/');
    s_cwd = slash <= 0 ? String("/") : s_cwd.substring(0, slash);
  }
  updateUsage();
  load(s_cwd);
  const String t = String(failed ? "Delete FAILED after " : "Deleted ") + n + (n == 1 ? " item." : " items.");
  lv_label_set_text(s_ui.info, t.c_str());
}

void destroy() {
  s_ui = Ui();                           // s_ents is kept (PSRAM) for the next open; a delete keeps running
}

}  // namespace

extern const AppImpl FILES_APP = { "Files", create, update, destroy };

// Console / tests: the folder the app opens next ("files /GPSLOG").
void filesAppSetFolder(const char *path) { s_cwd = path && path[0] == '/' ? path : "/"; }
