// =============================================================================
//  BleService  -  Bluetooth LE foundation for the future iPhone app.
// -----------------------------------------------------------------------------
//  NimBLE (arduino-esp32 BLE library). One GATT service, JSON text payloads:
//    SERVICE   4750a000-5344-4556-8000-000000000001
//    STATUS    ...a001  read + notify (every 5 s): fw, uptime, fix, sats, wifi, sd
//    LOCATION  ...a002  read + notify (1 Hz):      lat, lon, speed, course, alt, hdop, utc
//    COMMAND   ...a003  write:                    "ping", "status", "version"
//    RESPONSE  ...a004  read + notify:            reply to the last command
//  No pairing/bonding yet (plain GATT, like any BLE sensor); security comes with
//  Wi-Fi provisioning over BLE (M7). Payloads fit a 185-byte ATT MTU (iOS default).
//  Callbacks from the NimBLE task only set flags; update() does the work.
//  Never draws.
// =============================================================================
#pragma once

#include <Arduino.h>

class GpsLink;
class GpsParser;

namespace BleService {

  struct Stats {
    uint32_t connects, disconnects;
    uint32_t notifies;
    uint32_t commands;
    uint16_t mtu;                       // negotiated MTU of the last connection
  };

  void begin(GpsLink &link, GpsParser &parser);   // starts if enabled in Settings
  void update();                        // call every loop

  void setEnabled(bool on);             // persisted; off = advertising stopped, clients dropped
  bool enabled();
  bool initialized();                   // BLE stack running
  bool advertising();
  int connectedCount();
  String name();                        // advertised device name
  String lastCommand();
  String refusal();                     // why the last "on" was refused (not enough memory), "" = none
  const Stats &stats();
}
