// =============================================================================
//  LvglMem  -  LVGL heap in PSRAM (lv_conf.h: LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM).
// -----------------------------------------------------------------------------
//  With the C-library allocator every LVGL object (all < 4 KB) came from internal
//  RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL = 4096), which Wi-Fi and BLE need:
//  opening three apps cost ~40 KB of the ~108 KB left after the radios (2026-09-28).
//  LVGL's allocations now go to PSRAM; internal RAM only if PSRAM is exhausted.
// =============================================================================
#include <lvgl.h>
#include <esp_heap_caps.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

namespace {
constexpr uint32_t PSRAM = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr uint32_t ANY = MALLOC_CAP_8BIT;
}

extern "C" {

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *, size_t) { return nullptr; }
void lv_mem_remove_pool(lv_mem_pool_t) {}

void *lv_malloc_core(size_t size) {
  void *p = heap_caps_malloc(size, PSRAM);
  return p ? p : heap_caps_malloc(size, ANY);
}

void *lv_realloc_core(void *p, size_t new_size) {
  void *r = heap_caps_realloc(p, new_size, PSRAM);
  return r ? r : heap_caps_realloc(p, new_size, ANY);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon) {
  if (!mon) return;
  lv_memzero(mon, sizeof(*mon));
  mon->total_size = heap_caps_get_total_size(PSRAM);
  mon->free_size = heap_caps_get_free_size(PSRAM);
  mon->free_biggest_size = heap_caps_get_largest_free_block(PSRAM);
  mon->used_pct = mon->total_size ? 100 - (uint8_t)(100ULL * mon->free_size / mon->total_size) : 0;
}

lv_result_t lv_mem_test_core(void) { return heap_caps_check_integrity(PSRAM, false) ? LV_RESULT_OK : LV_RESULT_INVALID; }

}  // extern "C"

#endif
