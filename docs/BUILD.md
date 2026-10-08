# Build, flash and develop

The tooling is written for **Windows + PowerShell 7** (the board shows up as a COM port). The firmware itself is portable ESP-IDF code.

## Prerequisites

| | |
|---|---|
| ESP-IDF **5.5.4** | Install to `C:\esp\esp-idf-v5.5.4` (or set `IDF_PATH`). `idf-build.ps1` runs its `export.ps1`. |
| Python 3 | Used by ESP-IDF and by the helper scripts (`tools/scripts/*.py`). |
| USB driver | CH340 serial driver (the board's USB-C port). |
| Internet on the first build | The IDF component manager downloads arduino-esp32 3.3.10, LVGL 9.6 and a few more into `idf/managed_components` (git-ignored). |

The display, touch and GPS libraries (GFX Library for Arduino, TAMC_GT911, TinyGPSPlus) are vendored in `idf/components`.

## Quick flash (ready-made firmware)

No ESP-IDF needed: `docs/firmware/NAV1-v<version>-full.bin` is the complete image (bootloader, partition table, application) for flash offset 0.

```powershell
.\flash.ps1                  # Windows: finds the board, installs esptool if needed, flashes
.\flash.ps1 -Port COM5       # choose the port
```
```bash
./flash.sh [/dev/ttyUSB0]    # macOS / Linux
```

or the browser flasher (Chrome / Edge) on the project's GitHub Pages site, or `python -m esptool --chip esp32s3 -p <port> write_flash 0x0 NAV1-v<version>-full.bin`. `docs/firmware/NAV1-v<version>-app.bin` is the application only, for updates over Wi-Fi (`tools/scripts/ota.ps1 -Bin ...`). `SHA256SUMS.txt` lists the checksums.

## Build and flash

```powershell
.\idf-build.ps1                      # compile → idf\build\NAV1.bin
.\idf-build.ps1 -Upload              # compile + flash (COM3; -Port COM5 to change)
.\idf-build.ps1 -Upload -OutDir archive\v0.7.2   # also keep the binaries and sdkconfig
.\idf-build.ps1 -Menuconfig          # browse system settings (copy changes into idf\sdkconfig.defaults)
```

The script stamps `git describe` into `firmware/build_info.h` (shown at boot and in `/api/status`) and prints the image size and the number of compiler warnings in project code (the build treats warnings in third-party code as non-fatal, in ours it aims for zero). Adding a new `.cpp` file requires touching `idf/main/CMakeLists.txt` once so CMake re-globs the sources.

All pins and tunables are in `firmware/config.h`; the version is in `firmware/version.h`.

## Console

The device has a serial console (115200 baud on the board's USB port):

```powershell
.\tools\scripts\nav.ps1 help                      # list of commands
.\tools\scripts\nav.ps1 "diag mem" "diag gps"    # one or more commands, prints the replies
.\tools\scripts\nav.ps1 "open Map" "map zoom 2"  # UI hooks: open <app>, page <App> <n>, home, swipe
```

Useful commands: `diag [sys|mem|gps|time|wifi|ble|touch|display|sd]`, `selftest`, `gps profile|ubx|replay <file> [speed]|live`, `trip start|stop|list`, `place list|add|rename|rm|go`, `nav [status]|goto|follow <trip> [back]|reverse|stop`, `wifi on|off|scan|connect`, `ble on|off`, `sd ls|cat|df|rm|put`, `backlight`, `map`, `refills` (screen refill timing). Opening the COM port resets the board (CH340 DTR/RTS); the firmware is written for that.

## One-command validation

```powershell
.\tools\scripts\validate.ps1                  # build + flash + boot + UI smoke + self-test + drift check (~3 min)
.\tools\scripts\validate.ps1 -NoBuild        # the same on the firmware already on the device
.\tools\scripts\validate.ps1 -Only gps,wifi  # quick focused check (areas: system memory gps time touch display sd wifi ble ui web trips nav sdfiles)
.\tools\scripts\validate.ps1 -Archive        # keep binaries + log + JSON in archive\<build id>\ (for a release)
.\tools\scripts\validate.ps1 -UpdateBaseline # accept this run's measurements as the new expected values
```

It opens every app several times and checks for memory leaks, runs the on-device self-test, serves the web page and API to the PC (including adding and deleting a place from the "phone"), adds a test place and navigates to it over the console, edits `settings.json` and images on the card, and compares boot time, app-open time, frame time, RAM and firmware size with `tools/scripts/baseline.json`. Exit code 1 on any failure. Some checks (`trips`, which also follows the replayed trip back with the navigator) replay a recorded GPS session from `/system/replay/` on the card; indoors, GPS "no fix" warnings are normal.

## Other tools

| Script | |
|---|---|
| `nav.ps1`, `NavSerial.ps1` | Console client |
| `shot.ps1`, `shotdocs.ps1` | Screenshot of the real display over the console (`-Open "page Settings 2;open Settings"` first) / the set in `docs/screenshots` |
| `ota.ps1` | Build + install over Wi-Fi with rollback |
| `mapfont.py` | Builds the map label font (`firmware/src/services/MapFontData.cpp`) from a TrueType font with ImageMagick |
| `uifont.py` | Builds the UI fonts for names in Hebrew and the Drive speed (`firmware/src/ui/Font*.c`) with `lv_font_conv` (npm) |
| `shiftnmea.py` | Move a recorded NMEA session to another place (for demos and screenshots without real locations) |
| `shotmap.ps1`, `shotframe.ps1`, `stlrender.py` | The README overview picture (framed screenshots, ImageMagick); a dependency-free STL renderer for the case previews |
| `maps.ps1`, `mapload.ps1`, `maps_sat_fetch.py` | Build the offline maps and put them on the card; load-test the map server |
| `mvtlib.py`, `maprender.py` | Readable Python version of the map renderer (`python maprender.py <lon> <lat> out.png`, needs a `.pmtiles`; set `NAV1_PMTILES`) |
| `tripmap.ps1`, `tripmap-standalone.ps1` | Draw a trip on OpenStreetMap, locally or as one self-contained HTML page |
| `imgconv.ps1`, `sdput.ps1` | Convert images for `/assets`; copy a small file to the card over the console |
| `cardcheck.py`, `cardfill.py`, `cardverify.py` | Detect counterfeit / failing SD cards |
| `stress.ps1` | UI stress and reboot soak |

`tools/bringup/*` are stand-alone Arduino sketches that were used to verify each part of the hardware (GPS stream, SD, touch accuracy, display, Wi-Fi/BLE). They build with the Arduino IDE / `arduino-cli` for the `esp32:esp32:esp32s3` board with `PSRAM=opi`, `FlashSize=16M`, `PartitionScheme=app3M_fat9M_16MB`, `CDCOnBoot=default`.

## Preparing the SD card

```powershell
format G: /FS:FAT32 /Q /V:NAV1                # 32 KB clusters for big cards
python tools\scripts\cardcheck.py G:           # non-destructive capacity check (run as administrator)
.\tools\scripts\maps.ps1 -Card G:             # offline map + phone page
```

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Flash fails at "Connecting" | Another program has the COM port, or the wrong port — `-Port COMx`. Hold BOOT, tap RST, release BOOT as a fallback. |
| Device restarts when you open the console | Normal for the CH340 bridge. |
| "No map on the card" | `/maps/israel.pmtiles` is missing, or the card was not read — check **Files** and `nav.ps1 map`. |
| Bluetooth will not start while Wi-Fi and the hotspot are on | It needs 80 KB of free internal RAM; turn the hotspot off first (`diag mem` shows what is free). |
| First fix takes minutes | Normal after power-up (no backup battery in the GPS module); stand in the open. |
