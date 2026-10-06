// =============================================================================
//  App  -  minimal app interface for the launcher (first step towards the M2
//  AppRegistry / AppManager).
// -----------------------------------------------------------------------------
//  The launcher builds the app screen (colour, Back button, title) and hands the
//  app an empty content area below the header (800 x CONTENT_H). Lifecycle:
//    create(content) -> update() every UPDATE_MS while open -> destroy()
//  destroy() runs before the screen is deleted; the app must drop every pointer
//  to its LVGL objects there. Apps read service state and call service
//  functions; they never touch hardware.
// =============================================================================
#pragma once

#include <lvgl.h>

struct AppImpl {
  const char *name;                      // = the launcher tile name
  void (*create)(lv_obj_t *content);
  void (*update)();
  void (*destroy)();
};

namespace Apps {
  constexpr int CONTENT_W = 480;
  constexpr int CONTENT_H = 688;         // 800 - status bar 36 - header 76
  constexpr uint32_t UPDATE_MS = 250;

  // App registry = the launcher (Apps.cpp): one entry per home tile, in tile order.
  constexpr int COUNT = 12;
  struct Entry { const char *name; const char *icon; uint32_t color; const AppImpl *impl; };
  const Entry &entry(int i);
  int indexOf(const char *name);         // case-insensitive, -1 = none
  const AppImpl *find(const char *name); // nullptr = placeholder app

  // Shared helpers
  void setText(lv_obj_t *label, const char *text);   // only redraws when the text changed
  lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h);

  // Menu + pages layout (Settings, Tools, GPS, Trips): a tab strip on top (MENU_COLS per row), one page card
  // per entry below it, full width; only the selected page is visible and its tab is highlighted.
  constexpr int MENU_COLS = 3, MENU_BTN_H = 48;
  constexpr int PAGE_X = 12, PAGE_W = CONTENT_W - 24, PAGE_H = CONTENT_H - 20 - 2 * MENU_BTN_H - 8 - 8;   // worst case: 2 tab rows
  constexpr int PAGE_INNER_W = PAGE_W - 20;          // usable width inside a page card
  void buildMenu(lv_obj_t *content, const char *const *names, int n, lv_obj_t **menu, lv_obj_t **pages, lv_event_cb_t onEntry);
  void showMenuPage(lv_obj_t *const *menu, lv_obj_t *const *pages, int n, int page);
  lv_obj_t *pageTitle(lv_obj_t *page, const char *text);   // 28 px title at the top of a page
}

// Files app: the folder it opens next (console "files <folder>", tests).
void filesAppSetFolder(const char *path);
// Settings app: the page it opens on next (console "settings <0..4>", screenshots).
void settingsAppSetPage(int page);
// Apps with a menu: the page they open on next (console "page <App> <n>", screenshots).
void toolsAppSetPage(int page);
void gpsAppSetPage(int page);
void tripsAppSetPage(int page);
// Map app: show this finished trip (its base path, as TripRecorder::Summary::base) the next time it opens.
void mapAppShowTrip(const char *base);
// Map app, console tests: "up" / "north" (heading-up or not), "zoom <0..7>".
void mapAppCommand(const char *arg);
