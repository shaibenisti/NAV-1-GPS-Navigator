# Hardware

All values below were measured or confirmed on the finished device unless marked *from the board documentation*.

## Bill of materials

Three bought parts make the electronics; everything else is printed or common.

| # | Part | What it is | Role |
|---|---|---|---|
| 1 | **ESP32-S3 4.3" touch-screen board** (Sunton ESP32-8048S043C-I) | ESP32-S3 microcontroller, 800 × 480 display, capacitive touch, Wi-Fi + Bluetooth LE, microSD slot, USB-C | The heart of the device: all code and the UI run here |
| 2 | **GPS module NEO-8M** (blue breakout board) with an external ceramic **patch antenna** | u-blox M8-series receiver | Position, speed and time |
| 3 | **4-pin cable**, SH 1.0 mm 4P, 200 mm, to DuPont female | Connects the GPS module to the board's 4-pin connector (P4) | Power and serial link between the GPS and the board |
| 4 | microSD card (genuine, FAT32, 8–128 GB) | | Trips, GPS logs, offline map |
| 5 | USB-C cable and a 5 V supply (a power bank works) | | Power, flashing, console |
| 6 | 3D-printed case | [hardware/case](../hardware/case) — the eight small pins are part of the print | Enclosure |

### Wiring the GPS module

The 4-pin cable goes from the board's **P4** connector to the GPS module:

| Board P4 | GPS module (NEO-8M) |
|---|---|
| IO17 | TXD (GPS → board; the NMEA stream) |
| IO18 | RXD (board → GPS; only used to configure the module) |
| 3V3 | VCC |
| GND | GND |

Check the pin order printed on your board before plugging the cable in; the module is 3.3 V logic.

## Components

| Part | Details |
|---|---|
| Main board | Sunton **ESP32-8048S043C-I** |
| MCU | ESP32-S3 rev 2, dual core, 240 MHz |
| Flash / PSRAM | 16 MB QIO flash, 8 MB **octal** PSRAM |
| Display | 4.3" IPS, 800 × 480, parallel RGB565 (16 data lines), backlight on GPIO 2 (PWM) |
| Touch | GT911 capacitive controller on I²C (400 kHz), polled — the INT line is not used |
| GPS | **NEO-8M** module (u-blox **M8030**, ROM 3.01, protocol 18) with an external ceramic patch antenna; GPS + Galileo, NMEA 4.1, 1 Hz, 9600 baud |
| Storage | microSD in the board's slot, SPI at 10 MHz, FAT32 |
| USB | USB-C → CH340 serial bridge on UART0 (flashing and console, 115200 baud) |
| Radio | Wi-Fi 2.4 GHz (station + access point) and Bluetooth LE, on-chip |

The GPS module is wired to connector **P4** (3V3, GND, IO17, IO18) with the 4-pin cable. The board has no battery; NAV-1 is meant to run from USB power (a power bank works). The GPS module has no backup battery, so every power-up is a cold start (usually 2–4 minutes in the open).

## GPIO map

| GPIO | Function |
|---|---|
| 0 | BOOT button (boot strap) |
| 1, 3, 8, 9, 46 | LCD blue B4, B1, B0, B3, B2 |
| 2 | LCD backlight (active high, PWM 5 kHz) |
| 4, 5, 6, 7, 15, 16 | LCD green G5, G0, G1, G2, G3, G4 |
| 10, 11, 12, 13 | SD card CS, MOSI, SCK, MISO |
| 14, 21, 45, 47, 48 | LCD red R4, R3, R0, R2, R1 |
| **17** | **GPS TX → ESP RX** (UART1, 9600 8N1) |
| **18** | **ESP → GPS RX** (connector P4). Used only to configure the module: driven open-drain with the weakest drive, one UBX message at a time, then released to input. |
| 19, 20 | Touch I²C SDA, SCL |
| 38 | Touch reset |
| 39, 40, 41, 42 | LCD HSYNC, DE, VSYNC, PCLK |
| 43, 44 | UART0 TX, RX (USB console) |
| 26–37 | Flash and octal PSRAM bus — never use |

No GPIO is free on this board; the enclosure is closed, so no extra wiring is planned.

## Display

The panel is driven by the ESP-IDF `esp_lcd` RGB driver with two PSRAM framebuffers (800 × 480 × 2 B each) and **bounce buffers**: the LCD DMA reads two 8-row buffers in internal RAM which the driver refills from the current framebuffer, so the scan-out never competes with the CPU for the PSRAM at the wrong moment.

| Setting | Value |
|---|---|
| Pixel clock | 16 MHz (≈ 39 Hz refresh) |
| HSYNC polarity / front porch / pulse / back porch | 0 / 8 / 4 / 8 |
| VSYNC polarity / front porch / pulse / back porch | 0 / 8 / 4 / 8 |
| PCLK | active on the falling edge, idle low; DE idle low |
| LCD DMA channel priority | 9 (highest) |

The device is used **in portrait**: the top of the UI is the panel edge next to the GPS antenna window. The firmware renders the 480 × 800 UI and rotates it into the panel's native 800 × 480 framebuffers (see [ARCHITECTURE.md](ARCHITECTURE.md)). `LCD_PORTRAIT 0` in `firmware/config.h` switches back to the landscape layout.

Memory bandwidth is the limit of this design: the panel scan-out alone reads ≈ 31 MB/s from the PSRAM, and a full-screen PSRAM→PSRAM copy takes ≈ 48 ms with the CPU. A full-screen redraw therefore costs about 0.1–0.2 s; partial updates are unaffected.

## Touch (GT911)

| Item | Value |
|---|---|
| I²C address | 0x5D |
| Raw range | 480 × 272, inverted relative to the screen |
| Report rate | 100 Hz, up to 5 points |
| Mapping to the 800 × 480 panel | `x = raw_x · 799 / 480`, `y = raw_y · 479 / 272` (then rotated for portrait) |
| Accuracy (measured) | mean offset < 6 px, worst ≈ 21 px (2.5 mm) in the top-left corner |

The controller is read with a small own driver (no configuration write). A 256-byte `Wire` buffer is required by the library; the touch task samples on its own core and queues events for the UI.

## GPS module

| Item | Value |
|---|---|
| Output | NMEA 4.1: RMC, GGA, GSA, GSV (GLL and VTG switched off), 1 Hz, ≈ 500 B/s |
| Configuration | NAV-1 sends a UBX configuration at every start and whenever the module restarts: GPS + Galileo + QZSS enabled, GLONASS and SBAS off. It is verified message by message and reverted to factory defaults if anything fails. Console: `gps profile`, `gps profile factory`. |
| Fix rule | RMC status `A` and a position younger than 2 s |
| Time to first fix | ≈ 2–4 min cold in the open; handheld fixes hold with HDOP 1.1–3 and 5–9 satellites |

Raw sentences of every session are logged to the SD card, so a drive can be replayed later (`gps replay`).

## microSD

FAT32, SPI at 10 MHz (writes are limited by the card, ≈ 115 KB/s; reads ≈ 0.9 MB/s). Logging writes through a background task with a 2 s flush so the UI never waits for the card, and interrupted writes are recovered at the next mount.

> **Check your card.** Counterfeit cards that report a larger size than they have are common: they accept writes beyond the real capacity and return zeros. `tools/scripts/cardcheck.py` (sampled, non-destructive) and `cardfill.py` / `cardverify.py` (full write test) detect them.

## Power and thermal

USB-C 5 V. Typical use needs well under 1 A; the backlight is the main consumer (brightness and auto-dim are in Settings). Opening the console COM port resets the board (CH340 DTR/RTS) — firmware does nothing automatic at boot that depends on that.

## Flash layout

16 MB: `nvs` 20 KB, `otadata` 8 KB, two OTA application slots of 3 MB each, a 9.9 MB FAT partition (unused), 64 KB core dump. See `idf/partitions.csv`.
