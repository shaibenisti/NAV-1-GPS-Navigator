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
//  The server runs in its own task (core 0): downloads never block the UI loop.
//  Thread safety: the UI loop prepares the status/location JSON every 0.5 s
//  (update()); the web task only copies it. SD access goes through SdLog's lock.
//  Read-only for now; settings/commands over HTTP come later (with security).
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
