#include "Assets.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include "../Display.h"
#include "../SdLog.h"

// imgconv.ps1 writes these values: keep both sides in sync.
static_assert(sizeof(lv_image_header_t) == 12, "LVGL image header layout changed");
static_assert(LV_IMAGE_HEADER_MAGIC == 0x19, "LVGL image magic changed");
static_assert(LV_COLOR_FORMAT_RGB565 == 0x12 && LV_COLOR_FORMAT_RGB565A8 == 0x14 && LV_COLOR_FORMAT_ARGB8888 == 0x10,
              "LVGL colour format ids changed");

namespace {

constexpr int MAX_APPS = 12;
constexpr int ICON_MAX = 128;

lv_image_dsc_t s_icons[MAX_APPS];
bool s_hasIcon[MAX_APPS] = {};
lv_image_dsc_t s_wall;
bool s_hasWall = false;
int s_loaded = 0, s_rejected = 0;

// Pixel bytes an image of this header must carry (RGB565A8: colour plane + alpha plane).
uint32_t dataSize(const lv_image_header_t &h) {
  switch (h.cf) {
    case LV_COLOR_FORMAT_RGB565: return (uint32_t)h.stride * h.h;
    case LV_COLOR_FORMAT_RGB565A8: return (uint32_t)h.stride * h.h + (uint32_t)h.w * h.h;
    case LV_COLOR_FORMAT_ARGB8888: return (uint32_t)h.stride * h.h;
    default: return 0;
  }
}

bool load(SdLog &sd, const char *path, int maxW, int maxH, lv_image_dsc_t &out) {
  const int32_t size = sd.fileSize(path);
  if (size < 0) return false;                                  // not there: built-in look
  lv_image_header_t h = {};
  const char *why = nullptr;
  if (size < (int32_t)sizeof(h) || sd.readChunk(path, 0, (uint8_t *)&h, sizeof(h)) != (int)sizeof(h)) why = "unreadable";
  else if (h.magic != LV_IMAGE_HEADER_MAGIC || !dataSize(h)) why = "not an LVGL RGB565/RGB565A8/ARGB8888 image";
  else if (h.w > maxW || h.h > maxH || h.w == 0 || h.h == 0) why = "too large";
  else if (h.stride < h.w * (h.cf == LV_COLOR_FORMAT_ARGB8888 ? 4 : 2)) why = "bad stride";
  else if ((uint32_t)size != sizeof(h) + dataSize(h)) why = "size does not match the header";
  uint8_t *px = nullptr;
  if (!why) {
    px = (uint8_t *)heap_caps_malloc(dataSize(h), MALLOC_CAP_SPIRAM);
    if (!px) why = "no memory";
    else if (sd.readChunk(path, sizeof(h), px, dataSize(h)) != (int)dataSize(h)) why = "read error";
  }
  if (why) {
    heap_caps_free(px);
    s_rejected++;
    Serial.printf("[ASSET] %s ignored: %s\n", path, why);
    return false;
  }
  out = {};
  out.header = h;
  out.header.flags = 0;
  out.data_size = dataSize(h);
  out.data = px;
  s_loaded++;
  return true;
}

}  // namespace

void Assets::begin(SdLog &sd, const char *const *appNames, int appCount) {
  if (!sd.mounted()) return;
  const uint32_t t0 = millis();
  // One directory read instead of probing a path per app (each missing-file open costs ~10 ms).
  SdLog::DirEntry *e = (SdLog::DirEntry *)heap_caps_malloc(sizeof(SdLog::DirEntry) * 24, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const int n = e ? sd.listDir("/assets/icons", e, 24) : 0;
  for (int k = 0; k < n; k++) {
    for (int i = 0; i < appCount && i < MAX_APPS; i++) {
      char name[40];
      snprintf(name, sizeof(name), "%s.bin", appNames[i]);
      if (e[k].dir || strcasecmp(e[k].name, name)) continue;
      char path[64];
      snprintf(path, sizeof(path), "/assets/icons/%s", name);   // == the file name (FAT: case-insensitive)
      s_hasIcon[i] = load(sd, path, ICON_MAX, ICON_MAX, s_icons[i]);
    }
  }
  if (e && sd.listDir("/assets/wallpapers", e, 24) > 0) s_hasWall = load(sd, "/assets/wallpapers/home.bin", Display::UI_W, Display::UI_H, s_wall);
  heap_caps_free(e);
  if (s_loaded || s_rejected)
    Serial.printf("[ASSET] %d loaded, %d ignored (%u ms)\n", s_loaded, s_rejected, (unsigned)(millis() - t0));
}

const lv_image_dsc_t *Assets::icon(int app) { return app >= 0 && app < MAX_APPS && s_hasIcon[app] ? &s_icons[app] : nullptr; }
const lv_image_dsc_t *Assets::wallpaper() { return s_hasWall ? &s_wall : nullptr; }
int Assets::loadedCount() { return s_loaded; }
int Assets::rejectedCount() { return s_rejected; }
