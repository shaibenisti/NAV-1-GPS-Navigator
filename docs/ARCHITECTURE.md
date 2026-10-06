# Architecture

NAV-1 is a small operating system for one device: a shell that hosts apps, a set of services the apps read from, and a display path tuned for this panel. Everything runs on the ESP32-S3 under FreeRTOS, built with **ESP-IDF 5.5.4** with arduino-esp32 used as a component (Arduino-style `setup()`/`loop()` and libraries, but our own system settings).

```
 ┌───────────────────────────── apps (firmware/src/apps) ─────────────────────────────┐
 │ GPS · Compass · Map · Trips · Phone · Files · Storage · Settings · Tools           │
 └───────────────▲──────────────────────────────▲──────────────────────────▲──────────┘
                 │ read state / call services    │ LVGL objects              │ lifecycle
 ┌───────────────┴───────────────┐   ┌──────────┴─────────┐   ┌─────────────┴─────────┐
 │ services (firmware/src/services)│  │ ui + display path  │   │ shell                 │
 │ Location · TripRecorder · Map*  │  │ LVGL 9, transitions│   │ launcher, status bar, │
 │ Wifi · Web · Ble · Ota · Time   │  │ RgbPanel, TouchPort│   │ app host, console     │
 │ Storage · Settings · GpsConfig  │  └──────────▲─────────┘   └───────────────────────┘
 └───────────────▲───────────────┘             │
                 │                      ESP-IDF esp_lcd RGB panel, GDMA, GT911
        GpsLink → GpsParser → SdLog      (hardware: see HARDWARE.md)
```

## Layers

**Shell** (`src/shell`) owns the UI loop on core 1: the launcher (two pages × six tiles, swipe between pages), the status bar (time, GPS state, recording / Wi-Fi / Bluetooth / SD icons), the app host and the serial console. An app is a small struct — `create(content)`, `update()` every 250 ms while open, `destroy()` — registered in one table (`Apps.cpp`) that also defines the tile order, icon and colour. Apps read services and call service functions; they never touch hardware.

**Services** (`src/services`) hold state and do the work that must keep running when no app is open:

| Service | Role |
|---|---|
| `GpsLink`, `GpsParser`, `Location` | UART → NMEA sentences with checksum and link statistics → `GpsData`; `Location::snapshot()` applies the fix rule and is what every app reads |
| `GpsConfig` | Configures the GPS module over UBX at every start (GPS + Galileo, NMEA 4.1), verified and reverted on failure |
| `TripRecorder` | Samples a trip once per second (a point when moved ≥ 3 m or every 10 s), writes GPX / CSV / JSON summary, survives a reset |
| `SdLog` | SD session logging (NMEA + CSV per boot) through a writer task; card access is serialized by one mutex; recovery of interrupted writes |
| `Storage` | Read access to the card for the apps: listing, usage, background delete and statistics, a second long-lived file handle for the map |
| `MapTiles`, `MapRender` | The offline map (below) |
| `WifiService`, `WebService` | Station and access point, mDNS, the web page and its JSON API, GPX/CSV download, static files and range requests for the phone map |
| `BleService` | NimBLE GATT service: status, location (1 Hz notify), command / response |
| `Ota` | Firmware update over Wi-Fi with an image check and automatic rollback |
| `Settings`, `TimeService`, `Backlight`, `Assets` | NVS settings mirrored to an editable `settings.json`; clock from GPS or NTP with time zones; PWM brightness and auto-dim; optional icons / wallpaper from the card |

## Display path

The LCD scans two 8-row **bounce buffers** in internal RAM; the esp_lcd driver refills each from the current PSRAM framebuffer by CPU copy, about 0.4 ms before it is needed. LVGL never draws into the buffer that is being scanned: it renders into the hidden framebuffer and asks the driver to switch; the driver takes the switch at the refill that wraps to row 0, so a frame is never shown half old, half new. The LCD DMA channel has the highest GDMA priority so memory copies and SPI cannot shift the picture. (Without bounce buffers the firmware can also flip pages itself by retargeting the DMA descriptor chain: `LCD_BOUNCE_LINES 0`.)

**Portrait.** The panel is 800 × 480, the UI is 480 × 800. LVGL renders 16-row strips in *partial* mode into a small internal-RAM buffer; the flush callback rotates each strip into the hidden panel framebuffer (16-row blocks so the source stays in the data cache and the destination is written in whole cache lines). LVGL's sync callback copies the areas the hidden buffer is missing from the shown one. Rotating inside the refill interrupt was measured and rejected — it needs more time than the refill has. Measured costs: an app opens in ≈ 0.4 s, a full-screen frame takes ≈ 0.2 s, and the refill timing stays clean (no refill later than 0.6 ms in repeated runs).

**Memory strategy.** Internal RAM (≈ 130 KB free after boot) is the scarce resource and is reserved for the radios, DMA buffers and task stacks. LVGL's heap, framebuffers, tiles and images live in PSRAM. The ESP-IDF configuration (`idf/sdkconfig.defaults`) uses a 32 KB instruction cache, Wi-Fi and lwIP code in IRAM and the NimBLE host in PSRAM, which keeps the screen refill and the Wi-Fi throughput both healthy; Bluetooth refuses to start below 80 KB of free internal RAM instead of crashing.

**Tasks.** Core 1: Arduino `loop()` (shell, LVGL, services). Core 0: touch sampling (events queued for LVGL), the SD writer, Wi-Fi / BLE stacks, the web server, and the map renderer while the Map app is open.

## The offline map

`MapTiles` reads a **PMTiles v3** archive (Protomaps basemap, Mapbox Vector Tiles, gzip) from the SD card with random reads: header and root directory once, leaf directories and tiles on demand. Tiles are inflated in 8 KB slices with the ROM inflater.

`MapRender` runs in a low-priority task with a 32 KB PSRAM stack. For a requested centre, zoom and heading it picks the tiles under the (possibly rotated) picture, indexes the layers it needs (earth, land use, land cover, water, buildings, roads) without building feature lists, and draws into a 480 × 688 RGB565 picture: polygon fill with an active-edge scanline, thick polylines as quads with round joints, roads in class order with casings. Rendering is double-buffered — the app swaps the finished picture in and only repaints the marker in between. The work is throttled (a tick of sleep per 1500 drawn pixels / 96 features / 8 KB inflated) so the PSRAM traffic of the renderer never delays the panel refill.

Typical cost: 4 tiles (zoom 15.5) ≈ 0.8–1 s, 12 tiles (zoom 13) ≈ 1.8 s, ≈ 2 MB of PSRAM while the Map app is open, nothing when it is closed. A readable Python prototype of the same algorithm is in `tools/scripts/mvtlib.py` and `maprender.py`.

The Map app keeps the view following the GPS position; in a car the picture is centred a little ahead of the position so it lasts longer before it is rendered again. Heading-up mode renders the picture rotated; dragging the map frees the view.

## Data on the SD card

```
/GPSLOG/Snnnn.NMEA|.CSV       raw GPS of every boot (session files)
/data/trips/<year>/<name>.gpx|.csv|.json   recorded trips
/maps/<country>.pmtiles       street map (and optional satellite archive for the phone page)
/www/map/                     the offline map page served to phones
/assets/icons, /assets/wallpapers   optional LVGL images (tools/scripts/imgconv.ps1)
/system/settings.json         editable settings; /system/replay/ for GPS replay files
```

## Software updates

Two 3 MB application slots. `/api/update` accepts an upload only while armed (from the Tools screen or the USB console). The image is checked (NAV-1 marker + ESP-IDF image validation) before the slot switch; the new firmware marks itself valid after 20 s of running normally, otherwise the boot loader rolls back. `tools/scripts/ota.ps1` automates it.

## Quality tooling

- `tools/scripts/validate.ps1` — build, flash, boot, UI smoke test (opens every app several times, leak check), on-device self-test (GPS, touch, display, SD, Wi-Fi, BLE, memory), web / API checks, and a drift check against `baseline.json` (boot time, app-open time, frame time, RAM, firmware size). Exit code 1 on any failure.
- The console (`tools/scripts/nav.ps1`, `help` on the device) offers diagnostics, GPS replay of a recorded session (`gps replay`), screenshots of the real display (`shot.ps1`), and test hooks like `open <app>`, `map zoom`, `map trip`.
- The GPS parser has a self-test that runs at every boot.

## Source map

```
firmware/NAV1.ino             wiring only
firmware/config.h             pins and tunables
firmware/config/lv_conf.h     LVGL configuration
firmware/src/Rgb*, Lvgl*, Touch*   display path, LVGL port, touch task
firmware/src/Gps*, SdLog.*    GPS link / parser / CSV, SD logging
firmware/src/shell            launcher, status bar, console
firmware/src/apps             the apps
firmware/src/services         services (table above)
firmware/src/diag             health, self-test, replay, console diagnostics
firmware/src/ui               transitions
```
