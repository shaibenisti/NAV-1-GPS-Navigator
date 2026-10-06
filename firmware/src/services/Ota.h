// =============================================================================
//  Ota  -  firmware update over Wi-Fi with automatic rollback.
// -----------------------------------------------------------------------------
//  - Upload: HTTP POST /api/update (multipart file, WebService) into the inactive
//    app slot (ota_0/ota_1, 3 MB each). tools/scripts/ota.ps1 builds and sends it.
//  - Only while ARMED (console "ota arm" = USB access, or the device's own button),
//    for 10 minutes: nobody else on the network can push firmware.
//  - The image must be NAV-1 firmware for this chip (app descriptor: project name,
//    magic) - checked on the first chunk, before anything is written. The ESP-IDF
//    image check (SHA-256) runs at the end; only then is the boot slot switched.
//  - New firmware boots "pending verify": it marks itself valid after running
//    VERIFY_MS without a crash and with an intact heap. A crash / reset before that
//    -> the bootloader boots the previous firmware again (rollback).
//  - While installing, a full-screen system overlay shows the progress.
//  Services never draw: the shell draws the overlay from state().
// =============================================================================
#pragma once

#include <Arduino.h>

namespace Ota {
  enum class State : uint8_t { Idle, Armed, Receiving, Done, Failed };

  void begin();                          // after boot: detect "pending verify"
  void update();                         // UI loop: verify timer, reboot after Done, arm timeout

  void arm(uint32_t ms = 10 * 60 * 1000);
  void disarm();
  State state();
  const char *stateName(State s);
  uint8_t progress();                    // 0..100 while Receiving
  String lastError();
  bool pendingVerify();                  // running firmware not confirmed yet
  String runningSlot();                  // "app0" / "app1"
  void rejectForTest();                  // console: mark the pending firmware invalid -> rollback

  // WebService upload hooks (web task). begin returns "" = ok, else the refusal reason.
  String uploadBegin(const uint8_t *first, size_t n, size_t total);
  bool uploadWrite(const uint8_t *data, size_t n);
  String uploadEnd();                    // "" = installed (reboot follows), else error
  void uploadAbort();
}
