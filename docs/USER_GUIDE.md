# User guide

## First start

1. Format a microSD card as **FAT32** and insert it (the slot is on the right side of the case). NAV-1 works without a card, but then it cannot record trips or show the map.
2. Plug in USB-C power (a power bank is fine). The device is ready in about half a second.
3. Take it outside. The GPS module has no backup battery, so after every power-up it needs a few minutes with a clear view of the sky for its first fix. The status bar shows `no fix 2/9 sat` until then and `FIX 8/12 sat` afterwards.

The screen is used in portrait. Swipe left / right on the home screen for the second page of apps; **Home** at the top left of every app returns.

## Apps

| App | What it does |
|---|---|
| **GPS** | *Position*: coordinates, accuracy, altitude, speed, heading and the fix state (dimmed when the last position is old). *Satellites*: one bar per satellite, coloured by whether it is used in the fix. *Details*: time, date, fix type, time to first fix, GPS module status. |
| **Map** | The offline street map. Your position is the blue marker; the map follows you. **+ / −** zoom, **N / ↑** switch between north up and heading up, drag the map to look around and tap the GPS button to come back. The orange line is the trip being recorded. |
| **Trips** | *Record*: start / stop a trip, live distance, speed and time. *Trips*: saved trips; open one for its distance, duration, speed, a route preview and **Show on map**. *Totals*: all trips together. |
| **Compass** | Heading while you are moving (from the GPS course — there is no magnetic sensor, so it needs movement, about 3 km/h). The dial turns so that your direction of travel is always up. |
| **Phone** | A QR code that opens the NAV-1 web page on your phone, with the address under it. |
| **Files** | Browse the SD card, view text files, delete files and folders. |
| **Storage** | What uses the card (trips, GPS logs, system files) and a clean-up that keeps the newest GPS logs. |
| **Settings** | *Wi-Fi* (scan, pick a network, on-screen keyboard), *Bluetooth*, *Hotspot*, *Display* (brightness, dim after N seconds), *Date & Time* (time zone), *About*. |
| **Navigate** | The guidance screen: destination, a big arrow, distance, arrival time, speed and bearing; *Map*, *Reverse* (routes) and *Stop*. Not navigating: *Go to a saved place* or pick a recorded trip to follow. |
| **Places** | Saved points, nearest first, with distance and direction. *Save here* stores the current position; tap a place for *Go to*, *Show on map*, *Rename*, *Delete* (tap twice). |
| **Drive** | Dashboard: big speed, clock, distance / moving time / average / top speed since *Reset trip*, altitude, heading, the navigation strip (tap it for Navigate), *Record trip*. The screen does not dim while it is open. |
| **Tools** | *Health* (status of every subsystem at a glance), *Touch test*, *Update* (firmware update over Wi-Fi). |

## Recording a trip

Open **Trips → Record → Start trip**. Recording waits for the GPS fix, then adds a point whenever you have moved 3 m or every 10 s. **Stop trip** saves it. Every trip becomes three files in `/data/trips/<year>/`: `.gpx` (open it in any mapping tool), `.csv` and `.json` (summary). A recording survives a restart of the device.

Raw GPS data of every session is also logged to `/GPSLOG` (one NMEA and one CSV file per start).

## Places and navigation

**Saving places.** *Places → Save here* (needs a GPS fix) asks for a name — the on-screen keyboard has a Hebrew layout (the עב / ABC button next to the text switches), and the phone page takes names too. On the Map, a long press on any point offers *Save as a place* and *Go here*. Up to 50 places; they live in `/data/places.json` on the card (editable on a PC, read at the next start).

**Go to.** From a place (*Go to*), the map (*Go here*) or the phone. Navigate shows a big arrow and the distance in a straight line — there is no road routing on the device. While you move, the arrow is relative to your direction of travel (straight up = straight on); standing still the GPS has no heading, so the dial turns north up (the red N). The Map shows the destination as a red pin, a line from your position to it and a chip with the distance at the top (tap it for Navigate); the Compass marks the destination on its ring; the status bar shows the distance next to the clock. Within 25 m you get *Arrived*.

**Follow a trip / back-track.** *Trips → open a trip → Follow*, or *Navigate → pick a trip*. NAV-1 picks the direction from where you stand: at the trip's end it leads you back to its start (*Reverse* turns it around). The arrow points along the route a little ahead of you, the distance is what is left of the route, a bar shows the progress, and a message appears when you are more than 45 m off the route (and again when you are back on it). The route is drawn in cyan on the Map.

The guidance continues when you leave the app and after a restart; *Stop* ends it.

## Phone link

Switch Wi-Fi on in **Settings → Wi-Fi** and join your network, or switch on **Settings → Hotspot** (the hotspot carries the device name; its password is generated on first use). Then open the **Phone** app and scan the QR code — with the hotspot on, the code joins the hotspot first and then opens the page. The page offers:

- live location and status (`/api/location`, `/api/status`),
- your trips with GPX / CSV download,
- your places (*Go* starts the guidance on NAV-1, × deletes) and *Save NAV-1's position* with a name (Hebrew works),
- the navigation in progress, with *Stop*,
- the offline map with your position, the trips and the places on it (needs the map files on the card). A long press on the map offers *Save place* and *Navigate here*; the destination, the line to it and a route being followed are drawn too. After updating NAV-1 copy the new page to the card once: `.\tools\scripts\maps.ps1 -Card G: -PageOnly`.

Anyone on the same network or the hotspot can change places and the navigation from this page, as on the device itself. Firmware updates stay locked until you allow them on the device.

The same address works as `http://nav-1-xxxx.local/` where `xxxx` is the end of the device's MAC address.

Bluetooth LE publishes a GATT service with status and location notifications (any BLE app that can read characteristics can use it); there is no phone app yet.

## Offline map

The map is a single file, `/maps/<name>.pmtiles`, made on a PC:

```powershell
.\tools\scripts\maps.ps1 -Card G:                                   # street map of the default region + the phone page
.\tools\scripts\maps.ps1 -Card G: -Bbox "34.2,29.45,35.95,33.35"    # west,south,east,north of your own region
```

The street map comes from a Protomaps basemap build (OpenStreetMap data, zoom 0–15; Israel is ≈ 185 MB, a small country a few tens of MB). The Map app on the device reads `/maps/israel.pmtiles` — to use another region, name the file accordingly or change `MAP_FILE` in `firmware/src/apps/MapApp.cpp`. Without the file the app says "No map on the card". The map shows roads, water, parks, buildings and land use, with **street and place names** (Hebrew, and Latin where there is no Hebrew name; names in other scripts are left out). Street names follow the road, place names are horizontal; zoomed out you see only the bigger roads' names.

## Customising from the SD card

`/system/settings.json` — created by the device, edit it on a PC; applied at the next start:

```json
{ "device_name": "NAV-1-xxxx", "time_zone": "Israel", "wifi": true, "wifi_hotspot": false,
  "bluetooth": true, "brightness": 100, "dim_after_s": 0 }
```

(Wi-Fi passwords are never stored in this file.) `/data/places.json` holds the saved places (`{"places": [{"name": "Home", "lat": 32.1, "lon": 34.8}, ...]}`) and can be edited the same way; a file NAV-1 cannot read is kept as `places.bad`. Optional images: `/assets/icons/<App name>.bin` replaces a home-screen icon and `/assets/wallpapers/home.bin` the background (up to 480 × 800, portrait) — convert PNG/JPG with `tools/scripts/imgconv.ps1`. Without them the built-in look is used.

## Updating the firmware

From a PC with the device on the same network: `.\tools\scripts\ota.ps1`. It builds, arms the device over the USB console, uploads the image and watches the restart. The new firmware has to run correctly for 20 s; otherwise the previous version is restored automatically. Without a cable, switch on **Tools → Update → Allow upload** on the device and run `ota.ps1 -NoArm -Ip <address>`.

## Good to know

- First fix after power-up: a few minutes. Fixes inside buildings and cars are weak; a window seat or the dashboard works better than a bag.
- The screen can dim after a set time (Settings → Display); the touch that wakes it is not passed to the app.
- Use a genuine SD card. Fake "high capacity" cards lose data silently — see [HARDWARE.md](HARDWARE.md).
