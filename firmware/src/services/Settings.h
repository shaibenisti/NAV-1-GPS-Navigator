// =============================================================================
//  Settings  -  persistent device settings in NVS (Preferences, namespace "gpsdev").
// -----------------------------------------------------------------------------
//  Small, critical keys only.
//  Services read these at begin(); UI changes go through the services, which
//  write back here. Never called from ISRs or other tasks.
// =============================================================================
#pragma once

#include <Arduino.h>

class SdLog;

namespace Settings {
  void begin();

  // /system/settings.json on the SD card: user settings (name, time zone, Wi-Fi/hotspot/BLE on)
  // for backup and editing on a PC. Read after the card is mounted - values in the file are
  // applied to NVS (file wins) - and rewritten ~1 s after every change. NVS stays the store the
  // services read; Wi-Fi/hotspot passwords never go to the card. Works unchanged without a card.
  void attachSd(SdLog &sd);            // before the services' begin()
  void update();                       // call every loop (debounced file write)
  String fileStatus();                 // e.g. "/system/settings.json: 0 value(s) applied"
  bool fileInSync();                   // file == current settings (self-test)

  bool wifiEnabled();                  // default true
  void setWifiEnabled(bool on);
  String wifiSsid();                   // "" = no saved network
  String wifiPass();
  void setWifiNetwork(const String &ssid, const String &pass);   // "" clears
  int wifiChannel();                   // channel of the saved network (0 = unknown)
  void setWifiChannel(int ch);

  bool hotspotEnabled();               // NAV-1's own Wi-Fi access point (direct iPhone link), default false
  void setHotspotEnabled(bool on);
  String hotspotPass();                // WPA2 password, generated once (8 characters)

  bool bleEnabled();                   // default true
  void setBleEnabled(bool on);

  String deviceName();                 // default "NAV-1-xxxx" (last MAC bytes)

  String fieldTestId();                // running field test ("" = none); survives resets
  void setFieldTestId(const String &id);
  uint32_t fieldTestElapsedS();        // saved every 30 s so a resumed test keeps its duration
  void setFieldTestElapsedS(uint32_t s);
  uint32_t fieldTestTargetS();         // planned field-test duration (0 = open-ended)
  void setFieldTestTargetS(uint32_t s);
  uint32_t fieldTestResets();
  void setFieldTestResets(uint32_t n);

  String tripPath();                   // recording trip: base path without extension ("" = none)
  void setTripPath(const String &path);
  String tripStats();                  // "dur,moving,dist_m,max_kmh,points" saved every 30 s
  void setTripStats(const String &s);

  bool lastPos(double &lat, double &lon);   // last GPS position seen by the Map app (default view without a fix)
  void setLastPos(double lat, double lon);

  bool nvsRoundTrip();                 // self-test: write, read back and erase a scratch key

  int brightness();                    // display 10..100 %, default 100
  void setBrightness(int pct);
  uint32_t dimAfterS();                // dim the display after this long without touch, 0 = never (default)
  void setDimAfterS(uint32_t s);

  int timeZone();                      // TimeService zone index, default 1 (Israel)
  void setTimeZone(int index);

  int gpsProfile();                    // GpsConfig profile: 0 = factory (module untouched), 1 = GPS + Galileo;
  void setGpsProfile(int p);           //   default GPS_PROFILE_DEFAULT (config.h). Developer setting, NVS only
}
