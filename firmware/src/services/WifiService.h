// =============================================================================
//  WifiService  -  Wi-Fi station: scan, connect, status, auto-reconnect.
// -----------------------------------------------------------------------------
//  - Station (iPhone hotspot / home Wi-Fi) + optional NAV-1 hotspot (access point
//    "<device name>", WPA2, password generated once) for a direct iPhone link where
//    there is no shared Wi-Fi. With the hotspot on, the station keeps its saved
//    network (AP+STA); reconnect attempts wait while a phone is on the hotspot.
//  - Credentials are stored in NVS (Settings) only after a successful connect
//    (verified by read-back), loaded at boot and used to reconnect automatically
//    (retry 5 -> 10 -> 20 -> 30 s). A new network gets 30 s and up to 3 tries;
//    two authentication failures = "wrong password?", previous network kept.
//  - Never blocks: scan and connect are asynchronous, update() advances the state
//    machine from the UI loop. Wi-Fi events (Wi-Fi task) only set flags.
//  - mDNS "<device-name>.local" while connected.
//  Never draws (services know nothing about LVGL).
// =============================================================================
#pragma once

#include <Arduino.h>

namespace WifiService {

  enum class State : uint8_t {
    Off,          // radio off (disabled in Settings)
    Idle,         // on, no saved network
    Connecting,
    Connected,
    Failed,       // last attempt failed; retried every RETRY_MS
  };

  struct Network {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool open;
  };

  struct Stats {
    uint32_t scans, scanFails;
    uint32_t connectAttempts, connects, disconnects;
    uint8_t lastDisconnectReason;       // wifi_err_reason_t of the last disconnect
    uint32_t lastScanMs;                // duration of the last scan
    uint32_t saves, saveFailures;       // credentials written to NVS (verified by read-back)
  };

  void begin();                         // reads Settings; connects if a network is saved
  void update();                        // call every loop

  void setEnabled(bool on);             // persisted
  bool enabled();

  bool startScan();                     // async; false if off or already scanning
  bool scanning();
  int networkCount();                   // results of the last completed scan (strongest first, unique SSIDs)
  const Network &network(int i);
  uint32_t scanSerial();                // increments when new results are available

  void connect(const char *ssid, const char *pass, int channel = 0);   // saved to NVS once connected;
                                        // channel 0 = take it from the last scan / scan all
  void forget();                        // clears the saved network and disconnects

  void setHotspot(bool on);             // persisted; switches Wi-Fi on if needed
  bool hotspotOn();                     // access point running
  String hotspotSsid();
  String hotspotPass();
  String hotspotIp();                   // "192.168.4.1", "" when off
  int hotspotClients();

  State state();
  const char *stateName(State s);
  String ssid();                        // current / target network
  String ip();                          // "" if not connected
  int rssi();                           // dBm, 0 if not connected
  int channel();
  String hostname();                    // mDNS name without ".local"
  bool hasSavedNetwork();
  String lastError();                   // why the last attempt failed ("" = none), for the UI
  const Stats &stats();
}
