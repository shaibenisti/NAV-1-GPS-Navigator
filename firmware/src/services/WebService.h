// =============================================================================
//  WebService  -  the first iPhone link.
// -----------------------------------------------------------------------------
//  HTTP on port 80 while Wi-Fi is connected: http://<device-name>.local/ or the IP,
//  and on the NAV-1 hotspot: http://192.168.4.1/.
//    /                      NAV-1 web page: live location, status, trips with GPX download
//    /api/status            JSON: device, Wi-Fi, BLE, GPS, storage, recording trip
//    /api/location          JSON: fix, position, speed, course, altitude, accuracy, UTC
//    /api/trips             JSON: recorded trips (newest first) with file links
//    /trips/<dir>/<name>.gpx|.csv   trip files from the SD card
//    /api/places            GET: saved places [{name, lat, lon}] (index = position in the list)
//                           POST name, lat, lon: add one;  POST /api/places/delete i, name: delete one
//    /api/nav               GET: the guidance (Navigator);  POST /api/goto i | lat, lon, name: go to a place
//                           or a point;  POST /api/nav/stop
//  The server runs in its own task (core 0): downloads never block the UI loop.
//  Thread safety: the UI loop prepares the status/location/places/nav JSON every 0.5 s
//  (update()); the web task only copies it. Commands (POST) are handed to the UI loop through
//  one slot and answered when it has run them (<= 0.5 s). SD access goes through SdLog's lock.
//  Places and navigation can be changed by anyone on the same network or the hotspot (like the
//  device's own screen); firmware updates need arming on the device (Ota).
// =============================================================================
#pragma once

#include <Arduino.h>

class SdLog;

namespace WebService {
  void begin(SdLog &sd);
  void update();                         // call every loop (UI task)
  bool running();
  String url();                          // "http://192.168.1.23/" (station first, else hotspot) or "" when not reachable
  String hotspotUrl();                   // "http://192.168.4.1/" or "" when the hotspot is off
  String localUrl();                     // "http://nav-1-xxxx.local/"
  uint32_t requests();
  uint32_t stackFreeBytes();              // web task stack never used so far (diagnostics)
}
