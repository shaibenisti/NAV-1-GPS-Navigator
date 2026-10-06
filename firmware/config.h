// =============================================================================
//  config.h  -  All pins and tunables for firmware
//  Every value here comes from docs/HARDWARE.md. Change pins here only.
// =============================================================================
#pragma once

#include <Arduino.h>

#include "version.h"                   // FW_VERSION, FW_GIT_DESCRIBE

#define APP_NAME            "NAV-1"              // product name (UI, BLE/mDNS name, logs); firmware folder stays firmware

// ---- Firmware mode ------------------------------------------------------------
#define APP_MODE_FIELD_TEST 0           // stage 2B diagnostic screen + SD log (src/fieldtest/)
#define APP_MODE_SHELL      1           // NAV-1 shell: launcher + apps + services (src/shell/)
#define APP_MODE            APP_MODE_SHELL
#define FIELD_TEST          (APP_MODE == APP_MODE_FIELD_TEST)

// ---- Serial console (UART0 -> CH340) : VERIFIED -----------------------------
#define CONSOLE_BAUD        115200
#define CONSOLE_TX_BUFFER   4096        // Serial = UART0: without a TX buffer every print blocks the loop (~11.5 B/ms)

// ---- GPS (NEO-8M) : VERIFIED 2026-09-27 -------------------------------------
#define GPS_UART            Serial1
#define GPS_RX_PIN          17          // GPS TX -> ESP GPIO17
#define GPS_TX_PIN          -1          // not used; GPIO18 must never be driven
#define GPS_TX_TEST_PIN     18          // ESP -> GPS RX, used open-drain per UBX message (GpsLink::sendUbxOnce), released after.
                                        // P4 = IO18/IO17/3V3/GND; GT911 INT reaches IO18 only via R17 (unfitted), R5 pulls it up
#define GPS_PROFILE_DEFAULT 1           // GpsConfig: 0 = factory (NAV-1 sends nothing), 1 = GPS + Galileo package
                                        // (console "gps profile factory|galileo" changes it, saved in NVS)
#define GPS_BAUD            9600
#define GPS_RX_BUFFER       1024        // UART RX ring buffer (bytes)
#define GPS_LINK_TIMEOUT_MS 2000        // no byte for this long -> link DOWN
#define GPS_FIX_MAX_AGE_MS  2000        // position older than this -> NO FIX (normal age 0.7-0.8 s)

// ---- Display (RGB 800x480) : VERIFIED June 2026 ----------------------------
//  Control pins + clock here; the 16 RGB data pins are fixed in src/Display.cpp.
#define LCD_BL_PIN          2           // backlight, active HIGH
#define LCD_DE_PIN          40
#define LCD_VSYNC_PIN       41
#define LCD_HSYNC_PIN       39
#define LCD_PCLK_PIN        42
#define LCD_PCLK_HZ         16000000    // 16 MHz verified; 14/18 MHz are NOT
// RGB565 data lines (Arduino_GFX argument order R0..R4, G0..G5, B0..B4), docs/HARDWARE.md
#define LCD_R0_PIN 45
#define LCD_R1_PIN 48
#define LCD_R2_PIN 47
#define LCD_R3_PIN 21
#define LCD_R4_PIN 14
#define LCD_G0_PIN 5
#define LCD_G1_PIN 6
#define LCD_G2_PIN 7
#define LCD_G3_PIN 15
#define LCD_G4_PIN 16
#define LCD_G5_PIN 4
#define LCD_B0_PIN 8
#define LCD_B1_PIN 3
#define LCD_B2_PIN 46
#define LCD_B3_PIN 9
#define LCD_B4_PIN 1
// Timing (all VERIFIED): HSYNC pol 0 / fp 8 / pw 4 / bp 8, VSYNC pol 0 / fp 8 / pw 4 / bp 8,
// PCLK active-neg 1, DE idle high 0, PCLK idle high 0.

// ---- microSD (SPI) : VERIFIED June 2026 + 2026-09-27 in enclosure ----------
#define SD_CS_PIN           10
#define SD_MOSI_PIN         11
#define SD_SCK_PIN          12
#define SD_MISO_PIN         13
#define SD_SPI_HZ           10000000    // 4/10/20 MHz all passed SD_CHECK on the 256 GB card
#define SD_LOG_DIR          "/GPSLOG"
#define SD_FLUSH_MS         2000        // max data lost on sudden power-off
#define SD_WRITER_CORE      0           // M1: SdLog background writer (card writes off the UI loop)
#define SD_WRITER_PRIO      2           // below the touch task

// ---- Touch (GT911, I2C) : VERIFIED June 2026 ------------------------------
#define TOUCH_SDA_PIN       19
#define TOUCH_SCL_PIN       20
#define TOUCH_RST_PIN       38
#define TOUCH_INT_PIN       -1          // polling; GPIO18 is never used
#define TOUCH_I2C_HZ        400000
#define TOUCH_RAW_W         480         // raw X range, inverted vs screen
#define TOUCH_RAW_H         272         // raw Y range, inverted vs screen
#define TOUCH_POLL_MS       5           // TouchPort task poll period (GT911 reports every 10 ms)
#define TOUCH_TASK_CORE     0           // UI loop + LVGL run on core 1
#define TOUCH_TASK_PRIO     5           // above loopTask (1), below the Wi-Fi/lwIP tasks
#define TOUCH_STUCK_POLLS   40          // pressed + no GT911 report for 40 polls (~200 ms) -> release

// ---- LVGL port (M1) ------------------------------------------------------------
#define LVGL_RENDER_PARTIAL 0           // Arduino_GFX panel; LVGL renders into internal RAM chunks, copied to the framebuffer
#define LVGL_RENDER_DIRECT  1           // Arduino_GFX panel; LVGL renders into the (single, visible) framebuffer
#define LVGL_RENDER_DOUBLE  2           // RgbPanel, 2 PSRAM framebuffers, swap at vsync
#define LVGL_RENDER_MODE    LVGL_RENDER_DOUBLE
#define LCD_BOUNCE_LINES    8           // >0: esp_lcd bounce buffers of N rows x2 in internal RAM (driver does the flip); 0 = off
#define LCD_PORTRAIT        1           // 1: UI is 480 x 800 portrait (top = the panel's x = 799 edge, next to the GPS antenna). LVGL renders
                                        //    PARTIAL portrait strips; the flush rotates them into the panel's own 800 x 480 framebuffers
#define LCD_OWN_FLIP        1           // page flip by retargeting fb0's DMA descriptors (esp_lcd's fb1 switch is broken, 2026-09-29); 0 = esp_lcd
#define LVGL_PARTIAL_LINES  16          // partial buffer height (800 x 40 x 2 B = 64 KB)
#define HOME_BG_GRADIENT    1           // home background: vertical gradient (1) or solid (0) (M1 cost probe)
#define HOME_TILE_STYLE     2           // home tiles: 0 flat square, 1 rounded, 2 rounded + gradient (M1 cost probe)

// ---- Stage 2B field test (see src/fieldtest/) -------------------------------
#define FIELD_SCREEN_MS     500         // screen refresh period
#define FIELD_LOG_MS        1000        // one CSV row per second

// ---- Console diagnostics ----------------------------------------------------
#define GPS_PARSER_SELFTEST 1           // 1 = run GpsParser self-test at boot (~5 ms)
#define GPS_ECHO_RAW        0           // 1 = copy every GPS byte to the console
#define STATUS_INTERVAL_MS  5000        // link + parsed status line period
