set(NAV "${CMAKE_CURRENT_LIST_DIR}/../../firmware")
file(GLOB_RECURSE NAV_SRCS "${NAV}/src/*.cpp" "${NAV}/src/*.c")
idf_component_register(SRCS "main.cpp" ${NAV_SRCS}
                       INCLUDE_DIRS "${NAV}" "${NAV}/src"
                       REQUIRES arduino-esp32 lvgl__lvgl Arduino_GFX TAMC_GT911 TinyGPSPlus
                                esp_lcd esp_mm fatfs esp_timer esp_psram app_update bt mbedtls)
# BLE start-up runs its IPC call on a bigger stack (BleService.cpp, __wrap_esp_ipc_call_blocking)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ipc_call_blocking")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
# lv_conf.h: LV_STDLIB_CUSTOM - LVGL's allocator (lv_mem_init, lv_malloc_core, ...) is NAV-1's LvglMem.cpp
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${lvgl_lib} PUBLIC ${COMPONENT_LIB})
