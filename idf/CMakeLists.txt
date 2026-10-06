# NAV-1 built with ESP-IDF 5.5.4 + arduino-esp32 3.3.10 as a component (instead of the precompiled
# Arduino core), so the system settings (sdkconfig) are ours. Sources stay in ..\firmware.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Same defines the Arduino IDE passes for this board (boards.txt esp32s3: USB mode hwcdc, CDC on boot off,
# OPI PSRAM), so the code compiles exactly as before.
idf_build_set_property(COMPILE_DEFINITIONS "ARDUINO_USB_MODE=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "ARDUINO_USB_CDC_ON_BOOT=0" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "ARDUINO_USB_MSC_ON_BOOT=0" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "ARDUINO_USB_DFU_ON_BOOT=0" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "BOARD_HAS_PSRAM" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CORE_DEBUG_LEVEL=0" APPEND)
# LVGL (lvgl/lvgl from the component registry) reads NAV-1's own lv_conf.h: firmware/config on every include path
idf_build_set_property(INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../firmware/config" APPEND)
project(NAV1)
