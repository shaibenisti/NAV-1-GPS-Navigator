#include "Ota.h"

#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_heap_caps.h>
#include "Backlight.h"

// Arduino marks a new OTA image valid right at start-up unless this returns true;
// NAV-1 confirms it itself after VERIFY_MS of healthy running (Ota::update).
extern "C" bool verifyRollbackLater() { return true; }

// Identifies a NAV-1 image: the app descriptor's project name is no proof ("arduino-lib-builder"
// for every Arduino sketch, "NAV1" in the ESP-IDF build), so the upload is searched for this
// marker (kept in flash rodata).
extern "C" __attribute__((used)) const char NAV1_IMAGE_MARKER[] = "NAV-1 firmware image / ESP32-S3 800x480 / v1";

namespace {

constexpr uint32_t VERIFY_MS = 20000;
constexpr size_t MARKER_LEN = sizeof(NAV1_IMAGE_MARKER) - 1;
uint8_t s_tail[MARKER_LEN];                  // last bytes of the previous chunk (marker across chunks)
size_t s_tailLen = 0;
bool s_markerFound = false;

void scanMarker(const uint8_t *data, size_t n) {
  if (s_markerFound) return;
  // bytes spanning the chunk boundary
  uint8_t join[2 * MARKER_LEN];
  const size_t head = min(n, MARKER_LEN - 1);
  memcpy(join, s_tail, s_tailLen);
  memcpy(join + s_tailLen, data, head);
  if (memmem(join, s_tailLen + head, NAV1_IMAGE_MARKER, MARKER_LEN) || memmem(data, n, NAV1_IMAGE_MARKER, MARKER_LEN)) {
    s_markerFound = true;
    return;
  }
  // keep the stream's last MARKER_LEN - 1 bytes for the next boundary
  constexpr size_t K = MARKER_LEN - 1;
  if (n >= K) {
    memcpy(s_tail, data + n - K, K);
    s_tailLen = K;
  } else {
    if (s_tailLen + n > K) {
      const size_t drop = s_tailLen + n - K;
      memmove(s_tail, s_tail + drop, s_tailLen - drop);
      s_tailLen -= drop;
    }
    memcpy(s_tail + s_tailLen, data, n);
    s_tailLen += n;
  }
}

volatile Ota::State s_state = Ota::State::Idle;
volatile uint8_t s_progress = 0;
uint32_t s_armedUntil = 0, s_rebootAt = 0;
size_t s_total = 0, s_written = 0;
String s_error;
bool s_pending = false;

void fail(const String &why) {
  s_error = why;
  s_state = Ota::State::Failed;
  Backlight::forceOff(false);
  Serial.printf("[OTA] failed: %s\n", why.c_str());
}

}  // namespace

void Ota::begin() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  s_pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  const esp_app_desc_t *d = esp_app_get_description();
  Serial.printf("[OTA] running %s (%s %s)%s\n", run ? run->label : "?", d->project_name, d->version,
                s_pending ? ", NEW firmware: verifying (rollback if it fails)" : "");
}

void Ota::update() {
  if (s_pending && millis() > VERIFY_MS) {
    s_pending = false;
    if (heap_caps_check_integrity_all(true)) {
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println("[OTA] new firmware verified (running 20 s, heap intact): kept");
    } else {
      Serial.println("[OTA] new firmware unhealthy (heap corrupt): rolling back");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }
  if (s_state == State::Armed && (int32_t)(millis() - s_armedUntil) > 0) { s_state = State::Idle; Serial.println("[OTA] disarmed (timeout)"); }
  if (s_state == State::Done && s_rebootAt && (int32_t)(millis() - s_rebootAt) > 0) {
    Serial.println("[OTA] rebooting into the new firmware");
    Serial.flush();
    ESP.restart();
  }
}

void Ota::arm(uint32_t ms) {
  if (s_state == State::Receiving || s_state == State::Done) return;
  s_armedUntil = millis() + ms;
  s_state = State::Armed;
  s_error = "";
  Serial.printf("[OTA] armed for %u s: POST the firmware to /api/update\n", (unsigned)(ms / 1000));
}

void Ota::disarm() { if (s_state == State::Armed || s_state == State::Failed) s_state = State::Idle; }
Ota::State Ota::state() { return s_state; }
uint8_t Ota::progress() { return s_progress; }
String Ota::lastError() { return s_error; }
bool Ota::pendingVerify() { return s_pending; }
String Ota::runningSlot() { const esp_partition_t *p = esp_ota_get_running_partition(); return p ? p->label : "?"; }

const char *Ota::stateName(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Armed: return "armed";
    case State::Receiving: return "receiving";
    case State::Done: return "installed, rebooting";
    case State::Failed: return "failed";
  }
  return "?";
}

void Ota::rejectForTest() {
  if (!s_pending) { Serial.println("[OTA] nothing to reject (firmware already verified)"); return; }
  Serial.println("[OTA] rejecting the new firmware: rollback");
  Serial.flush();
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

// ---- web task side ------------------------------------------------------------------------
String Ota::uploadBegin(const uint8_t *first, size_t n, size_t total) {
  if (s_state != State::Armed) return "not armed (console: ota arm, or the device's update button)";
  constexpr size_t DESC_AT = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  if (n < DESC_AT + sizeof(esp_app_desc_t)) return "first chunk too small";
  const esp_image_header_t *h = (const esp_image_header_t *)first;
  const esp_app_desc_t *d = (const esp_app_desc_t *)(first + DESC_AT);
  if (h->magic != ESP_IMAGE_HEADER_MAGIC || h->chip_id != ESP_CHIP_ID_ESP32S3) return "not an ESP32-S3 firmware image";
  if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) return "not an application image";
  if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) return String("cannot start: ") + Update.errorString();
  s_tailLen = 0;
  s_markerFound = false;
  s_total = total;
  s_written = 0;
  s_progress = 0;
  s_error = "";
  s_state = State::Receiving;
  Serial.printf("[OTA] receiving %s %s (%u bytes) into the inactive slot\n", d->project_name, d->version, (unsigned)total);
  // Flash writes block the CPU cache and with it the display's refill interrupt: the panel would
  // show garbage for the whole install. First let the UI show the "screen goes dark" message (this runs
  // in the web task), then switch the backlight off until the flash writes are over.
  vTaskDelay(pdMS_TO_TICKS(1500));
  Backlight::forceOff(true);
  return uploadWrite(first, n) ? "" : s_error;
}

bool Ota::uploadWrite(const uint8_t *data, size_t n) {
  if (s_state != State::Receiving) return false;
  if (Update.write((uint8_t *)data, n) != n) { fail(String("write: ") + Update.errorString()); Update.abort(); return false; }
  scanMarker(data, n);
  s_written += n;
  if (s_total) s_progress = (uint8_t)min<size_t>(99, s_written * 100 / s_total);
  return true;
}

String Ota::uploadEnd() {
  if (s_state != State::Receiving) return s_error.length() ? s_error : "no upload";
  if (!s_markerFound) {                          // before end(): the boot slot is never switched
    Update.abort();
    fail("not NAV-1 firmware (marker missing)");
    return s_error;
  }
  if (!Update.end(true)) { fail(String("image check failed: ") + Update.errorString()); return s_error; }
  s_progress = 100;
  s_state = State::Done;
  Backlight::forceOff(false);                        // flash writes are over: "installed - restarting" is readable
  s_rebootAt = millis() + 1500;                      // let the HTTP reply go out
  Serial.printf("[OTA] %u bytes verified and installed, boot slot switched\n", (unsigned)s_written);
  return "";
}

void Ota::uploadAbort() {
  if (s_state != State::Receiving) return;
  Update.abort();
  fail("upload aborted");
}
