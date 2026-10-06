// =============================================================================
//  GpsConfig  -  configures the u-blox M8 GPS module (UBX over GPIO18, GpsLink).
// -----------------------------------------------------------------------------
//  Profiles (Settings::gpsProfile, console "gps profile factory|galileo"):
//    0 factory : module on its own defaults (GPS + GLONASS + SBAS + QZSS, NMEA 4.0,
//                GGA GLL GSA GSV RMC VTG). If the module still carries the package in its
//                battery-backed RAM (NAV-1 restarted, module kept power), it is reverted.
//    1 galileo : GPS + Galileo (+ QZSS, as u-blox recommends with GPS), GLONASS and SBAS off,
//                NMEA 4.1, GLL and VTG off. 9600 baud, 1 Hz, navigation filters unchanged.
//
//  Every change is a transaction (u-blox M8 protocol 18, UBX-13003221):
//    check   poll CFG-GNSS, CFG-NMEA, CFG-MSG GLL/VTG; nothing to do if they match
//    apply   CFG-GNSS, CFG-NMEA, CFG-MSG x2 (read-modify-write), each must be ACKed;
//            CFG-CFG save to BBR, CFG-RST hardware reset (required when Galileo is enabled)
//    verify  poll again after the module restarted
//  A NAK, a missing reply or a failed verification reverts: CFG-CFG clear + load defaults
//  (BBR), hardware reset, verify factory. After a failure nothing is retried until
//  the next NAV-1 start or a console "gps profile ...".
//  Runs again after every module start-up banner (module rebooted: it keeps the saved package
//  while powered; after a power loss it starts on defaults and is configured again).
//  Never draws. Non-blocking, except each UBX send (<= ~75 ms at 9600 baud).
// =============================================================================
#pragma once

#include <Arduino.h>

class GpsLink;

namespace GpsConfig {
  enum class State : uint8_t {
    Waiting,      // for the module (boot) or a scheduled check
    Checking,     // polling the module's settings
    Applying,     // sending the package
    Reverting,    // restoring the module's defaults
    Applied,      // verified: module runs the GPS + Galileo package
    Factory,      // verified: module runs its defaults
    Failed,       // see lastError(); the module was left on (or reverted to) its defaults when possible
  };

  void begin(GpsLink &link);
  void update();                        // UI loop

  int profile();                        // 0 factory, 1 galileo
  void setProfile(int p);               // saved; clears a failure and checks the module now
  State state();
  const char *stateName(State s);
  String lastError();                   // "" = none
  String moduleSummary();               // last polled module settings, e.g. "GNSS GPS+GAL+QZSS, NMEA 4.1, GLL 0, VTG 0"
  void status(Print &o);                // console
}
