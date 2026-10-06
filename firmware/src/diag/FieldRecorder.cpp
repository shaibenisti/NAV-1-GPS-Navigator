#include "FieldRecorder.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <math.h>
#include "Health.h"
#include "../SdLog.h"
#include "../TouchPort.h"
#include "../services/Location.h"
#include "../services/Settings.h"
#include "../services/TimeService.h"
#include "../services/WifiService.h"
#include "../services/BleService.h"
#include "../../config.h"

namespace {

const char *SAMPLE_HEADER =
    "t_s,utc,fix,state,lat,lon,alt_m,speed_kmh,course_deg,sats_used,sats_view,hdop,fix_age_ms,gps_sentences_per_s,"
    "gps_bad_crc,gps_overflow,wifi,wifi_rssi,ble_clients,touch_i2c_err,sd_err,sd_dropped,heap_int_kb,heap_int_min_kb,"
    "psram_kb,loop_ui_max_ms,loop_svc_max_ms,gps_restarts";
const char *EVENT_HEADER = "t_s,utc,event,detail";
const char *STATE_NAME[] = { "NO_SIGNAL", "ACQUIRING", "FIX", "FIX_LOST" };

SdLog *s_sd = nullptr;
bool s_running = false;
String s_id, s_dir, s_lastEvent;
int s_fSamples = -1, s_fEvents = -1;
uint32_t s_segStartMs = 0, s_offsetS = 0, s_lastSampleMs = 0, s_lastSaveMs = 0;
uint32_t s_resets = 0;
uint32_t s_targetS = 0;                 // 0 = open-ended
bool s_crashReset = false, s_completed = false;

struct Agg {
  uint32_t samples, fixSamples, linkDownSamples;
  int32_t firstFixS;                     // -1 = no fix yet
  bool fixAtStart;
  uint32_t fixLosses, longestLossS, lossStartS;
  bool inLoss;
  uint32_t satsMin, satsMax, satsSum;
  double hdopMin, hdopMax, hdopSum;
  uint32_t hdopN;
  double maxSpeed, distanceM, prevLat, prevLon;
  bool havePrev;
  uint32_t gpsRestarts0, gpsSentences0, gpsBad0, gpsOverflow0, touchErr0, sdErr0, sdDropped0;
  uint32_t intMinKb, psramMinKb, uiMaxMs, svcMaxMs, stalls, lowMem;
  uint32_t wifiChanges, bleChanges;
};
Agg s_a;

// change detection
Location::Quality s_prevQ = Location::Quality::NoSignal;
WifiService::State s_prevWifi = WifiService::State::Off;
int s_prevBle = 0;
uint32_t s_prevGpsRestarts = 0, s_prevGpsSentences = 0, s_prevTouchErr = 0, s_prevSdErr = 0, s_prevSdDropped = 0;
bool s_lowMem = false;
uint32_t s_prevStallUi = 0, s_prevStallSvc = 0;

uint32_t elapsedS() { return s_offsetS + (millis() - s_segStartMs) / 1000; }

String utcNow() {
  struct tm u;
  if (!TimeService::utc(u)) return "";
  char b[48];
  snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02dZ", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
  return b;
}

void event(const char *type, const char *fmt = "", ...) {
  char detail[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  for (char *p = detail; *p; p++) if (*p == ',' || *p == '\n') *p = ';';     // keep the CSV simple
  char line[240];
  snprintf(line, sizeof(line), "%lu,%s,%s,%s", (unsigned long)elapsedS(), utcNow().c_str(), type, detail);
  s_sd->auxLine(s_fEvents, line);
  s_lastEvent = String(type) + (detail[0] ? " " : "") + detail;
  Serial.printf("[FT] %s %s\n", type, detail);
}

const char *resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTERRUPT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

double haversineM(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0, k = M_PI / 180.0;
  const double dLat = (lat2 - lat1) * k, dLon = (lon2 - lon1) * k;
  const double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * k) * cos(lat2 * k) * sin(dLon / 2) * sin(dLon / 2);
  return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

bool openFiles(bool fresh) {
  s_dir = "/FIELD/" + s_id;
  if (!s_sd->mkdirs(s_dir.c_str())) return false;
  const bool newSamples = !s_sd->exists((s_dir + "/samples.csv").c_str());
  const bool newEvents = !s_sd->exists((s_dir + "/events.csv").c_str());
  s_fSamples = s_sd->auxOpen((s_dir + "/samples.csv").c_str());
  s_fEvents = s_sd->auxOpen((s_dir + "/events.csv").c_str());
  if (s_fSamples < 0 || s_fEvents < 0) return false;
  if (newSamples) s_sd->auxLine(s_fSamples, SAMPLE_HEADER);
  if (newEvents) s_sd->auxLine(s_fEvents, EVENT_HEADER);
  (void)fresh;
  return true;
}

void resetAgg() {
  s_a = Agg();
  s_a.firstFixS = -1;
  s_a.satsMin = UINT32_MAX;
  s_a.hdopMin = 1e9;
  const GpsLink::Stats &ls = Location::linkStats();
  s_a.gpsSentences0 = ls.sentences;
  s_a.gpsRestarts0 = s_prevGpsRestarts = ls.receiverRestarts;
  s_a.gpsBad0 = ls.badChecksum;
  s_a.gpsOverflow0 = ls.overflows;
  s_a.touchErr0 = TouchPort::totals().i2cErrors;
  s_a.sdErr0 = s_sd->writeErrors();
  s_a.sdDropped0 = s_sd->droppedBytes();
  s_a.intMinKb = UINT32_MAX;
  s_a.psramMinKb = UINT32_MAX;
  s_prevGpsSentences = ls.sentences;
  s_prevTouchErr = s_a.touchErr0;
  s_prevSdErr = s_a.sdErr0;
  s_prevSdDropped = s_a.sdDropped0;
  const GpsData d = Location::snapshot();
  s_prevQ = Location::quality(d);
  s_a.fixAtStart = s_prevQ == Location::Quality::Fix;
  if (s_a.fixAtStart) s_a.firstFixS = 0;
  s_prevWifi = WifiService::state();
  s_prevBle = BleService::connectedCount();
  s_lowMem = false;
  s_prevStallUi = s_prevStallSvc = 0;
}

void sample() {
  const uint32_t t = elapsedS();
  const GpsData d = Location::snapshot();
  const Location::Quality q = Location::quality(d);
  const GpsLink::Stats &ls = Location::linkStats();
  const TouchPort::Stats ts = TouchPort::totals();
  const Health::Window &hw = Health::last();
  const uint32_t intKb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10;
  const uint32_t intMinKb = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) >> 10;
  const uint32_t psKb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10;
  const uint32_t sentPerS = ls.sentences - s_prevGpsSentences;
  s_prevGpsSentences = ls.sentences;
  const bool fix = q == Location::Quality::Fix;

  char row[400];
  char pos[120] = ",,,,";
  if (d.locValid) {
    snprintf(pos, sizeof(pos), "%.7f,%.7f,%s,%s,%s", d.lat, d.lng, d.altValid ? String(d.altM, 1).c_str() : "",
             d.speedValid ? String(d.speedKmh, 2).c_str() : "", d.courseValid ? String(d.courseDeg, 1).c_str() : "");
  }
  snprintf(row, sizeof(row), "%lu,%s,%d,%s,%s,%u,%u,%s,%s,%u,%u,%u,%s,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u",
           (unsigned long)t, utcNow().c_str(), fix ? 1 : 0, STATE_NAME[(int)q], pos, (unsigned)d.satsUsed,
           (unsigned)d.satsInView(), d.hdopValid && d.hdop < 50 ? String(d.hdop, 2).c_str() : "",
           d.locValid && d.locAgeMs != UINT32_MAX ? String(d.locAgeMs).c_str() : "", (unsigned)sentPerS,
           (unsigned)ls.badChecksum, (unsigned)ls.overflows, WifiService::stateName(WifiService::state()),
           WifiService::rssi(), BleService::connectedCount(), (unsigned)ts.i2cErrors, (unsigned)s_sd->writeErrors(),
           (unsigned)s_sd->droppedBytes(), (unsigned)intKb, (unsigned)intMinKb, (unsigned)psKb,
           (unsigned)(hw.loopLvglUsMax / 1000), (unsigned)(hw.loopServicesUsMax / 1000), (unsigned)ls.receiverRestarts);
  s_sd->auxLine(s_fSamples, row);

  // --- aggregates ---
  Agg &a = s_a;
  a.samples++;
  if (!d.linkUp) a.linkDownSamples++;
  if (fix) {
    a.fixSamples++;
    a.satsMin = min(a.satsMin, (uint32_t)d.satsUsed);
    a.satsMax = max(a.satsMax, (uint32_t)d.satsUsed);
    a.satsSum += d.satsUsed;
    if (d.hdopValid && d.hdop < 50) {
      a.hdopMin = min(a.hdopMin, d.hdop);
      a.hdopMax = max(a.hdopMax, d.hdop);
      a.hdopSum += d.hdop;
      a.hdopN++;
    }
    if (d.speedValid) a.maxSpeed = max(a.maxSpeed, d.speedKmh);
    if (a.havePrev) {
      const double m = haversineM(a.prevLat, a.prevLon, d.lat, d.lng);
      if ((d.speedValid && d.speedKmh > 2.0) || m > 20) a.distanceM += m;   // ignore standing-still jitter
    }
    a.prevLat = d.lat;
    a.prevLon = d.lng;
    a.havePrev = true;
  } else {
    a.havePrev = false;
  }
  a.intMinKb = min(a.intMinKb, intKb);
  a.psramMinKb = min(a.psramMinKb, psKb);
  a.uiMaxMs = max(a.uiMaxMs, (uint32_t)(hw.loopLvglUsMax / 1000));
  a.svcMaxMs = max(a.svcMaxMs, (uint32_t)(hw.loopServicesUsMax / 1000));

  // --- events ---
  if (q != s_prevQ) {
    if (fix && a.firstFixS < 0) {
      a.firstFixS = t;
      event("FIRST_FIX", "after %lu s; %u satellites; hdop %.1f", (unsigned long)t, (unsigned)d.satsUsed, d.hdop);
    } else if (fix) {
      const uint32_t lost = a.inLoss ? t - a.lossStartS : 0;
      a.longestLossS = max(a.longestLossS, lost);
      a.inLoss = false;
      event("FIX_REGAINED", "after %lu s without fix; %u satellites", (unsigned long)lost, (unsigned)d.satsUsed);
    } else if (s_prevQ == Location::Quality::Fix) {
      a.fixLosses++;
      a.inLoss = true;
      a.lossStartS = t;
      event("FIX_LOST", "%s; %u satellites in view", STATE_NAME[(int)q], (unsigned)d.satsInView());
    } else if (q == Location::Quality::NoSignal) {
      event("GPS_LINK_LOST", "no data from the GPS receiver");
    } else if (s_prevQ == Location::Quality::NoSignal) {
      event("GPS_LINK_OK", "%s", STATE_NAME[(int)q]);
    }
    s_prevQ = q;
  }
  if (WifiService::state() != s_prevWifi) {
    s_prevWifi = WifiService::state();
    a.wifiChanges++;
    event("WIFI", "%s %s", WifiService::stateName(s_prevWifi), WifiService::ssid().c_str());
  }
  if (BleService::connectedCount() != s_prevBle) {
    s_prevBle = BleService::connectedCount();
    a.bleChanges++;
    event("BLE", "%d client(s)", s_prevBle);
  }
  if (ls.receiverRestarts != s_prevGpsRestarts) {
    s_prevGpsRestarts = ls.receiverRestarts;
    event("GPS_RESTART", "the GPS module rebooted (u-blox banner); fix lost until it reacquires - supply or wiring?");
  }
  if (ts.i2cErrors != s_prevTouchErr) {
    event("TOUCH_ERROR", "I2C errors %u (+%u)", (unsigned)ts.i2cErrors, (unsigned)(ts.i2cErrors - s_prevTouchErr));
    s_prevTouchErr = ts.i2cErrors;
  }
  if (s_sd->writeErrors() != s_prevSdErr || s_sd->droppedBytes() != s_prevSdDropped) {
    event("SD_ERROR", "write errors %u; dropped bytes %u", (unsigned)s_sd->writeErrors(), (unsigned)s_sd->droppedBytes());
    s_prevSdErr = s_sd->writeErrors();
    s_prevSdDropped = s_sd->droppedBytes();
  }
  if (!s_lowMem && intKb < 40) { s_lowMem = true; a.lowMem++; event("LOW_MEMORY", "internal free %u KB", (unsigned)intKb); }
  if (s_lowMem && intKb > 60) s_lowMem = false;
  if ((hw.loopServicesUsMax > 200000 && hw.loopServicesUsMax != s_prevStallSvc) ||
      (hw.loopLvglUsMax > 500000 && hw.loopLvglUsMax != s_prevStallUi)) {
    a.stalls++;
    event("LOOP_STALL", "UI %u ms; services %u ms", (unsigned)(hw.loopLvglUsMax / 1000), (unsigned)(hw.loopServicesUsMax / 1000));
    s_prevStallSvc = hw.loopServicesUsMax;
    s_prevStallUi = hw.loopLvglUsMax;
  }
}

void writeSummary() {
  const Agg &a = s_a;
  const uint32_t dur = elapsedS();
  const GpsLink::Stats &ls = Location::linkStats();
  const uint32_t touchErr = TouchPort::totals().i2cErrors - a.touchErr0;
  const uint32_t sdErr = s_sd->writeErrors() - a.sdErr0, sdDrop = s_sd->droppedBytes() - a.sdDropped0;
  const uint32_t afterFix = a.firstFixS >= 0 && a.samples > (uint32_t)a.firstFixS ? a.samples - a.firstFixS : 0;
  const double fixPct = afterFix ? 100.0 * a.fixSamples / afterFix : 0;

  // verdict
  String fails, warns;
  auto add = [](String &list, const char *msg) { if (list.length()) list += ", "; list += '"'; list += msg; list += '"'; };
  if (s_crashReset) add(fails, "device crashed and restarted (PANIC/WDT/brownout, see RESET events)");
  else if (s_resets) add(warns, "device restarted during the test (power or reset, see RESET events)");
  if (sdErr || sdDrop) add(fails, "SD write errors or dropped data");
  if (a.intMinKb < 30) add(fails, "internal RAM below 30 KB");
  if (a.samples && a.linkDownSamples * 10 > a.samples) add(fails, "GPS receiver silent in more than 10% of samples");
  if (ls.receiverRestarts != a.gpsRestarts0) add(warns, "the GPS module rebooted during the test (see GPS_RESTART events)");
  if (a.firstFixS < 0) add(warns, "no GPS fix during the test");
  if (a.fixLosses) add(warns, "fix lost at least once");
  if (afterFix && fixPct < 95) add(warns, "fix below 95% of the time after the first fix");
  if (!a.fixAtStart && a.firstFixS > 180) add(warns, "time to first fix above 3 minutes");
  if (a.hdopN && a.hdopSum / a.hdopN > 5) add(warns, "average HDOP above 5");
  if (touchErr) add(warns, "touch I2C errors");
  if (a.stalls) add(warns, "UI loop stalls");
  const char *verdict = fails.length() ? "FAIL" : (warns.length() ? "WARN" : "PASS");

  const int f = s_sd->auxOpen((s_dir + "/summary.json").c_str());
  if (f < 0) return;
  char b[200];
  auto line = [&](const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    s_sd->auxLine(f, b);
  };
  line("{");
  line("  \"id\": \"%s\", \"firmware\": \"%s %s (%s)\", \"end_utc\": \"%s\",", s_id.c_str(), APP_NAME, FW_VERSION,
       FW_GIT_DESCRIBE, utcNow().c_str());
  line("  \"duration_s\": %lu, \"samples\": %lu, \"resets\": %lu,", (unsigned long)dur, (unsigned long)a.samples,
       (unsigned long)s_resets);
  line("  \"gps\": { \"fix_at_start\": %s, \"time_to_first_fix_s\": %ld, \"fix_samples\": %lu, \"fix_pct_after_first_fix\": %.1f,",
       a.fixAtStart ? "true" : "false", (long)a.firstFixS, (unsigned long)a.fixSamples, fixPct);
  line("    \"fix_losses\": %lu, \"longest_loss_s\": %lu, \"link_down_samples\": %lu,", (unsigned long)a.fixLosses,
       (unsigned long)a.longestLossS, (unsigned long)a.linkDownSamples);
  line("    \"sats_min\": %lu, \"sats_max\": %lu, \"sats_avg\": %.1f,", a.fixSamples ? (unsigned long)a.satsMin : 0UL,
       (unsigned long)a.satsMax, a.fixSamples ? (double)a.satsSum / a.fixSamples : 0.0);
  line("    \"hdop_min\": %.2f, \"hdop_max\": %.2f, \"hdop_avg\": %.2f,", a.hdopN ? a.hdopMin : 0.0, a.hdopN ? a.hdopMax : 0.0,
       a.hdopN ? a.hdopSum / a.hdopN : 0.0);
  line("    \"max_speed_kmh\": %.1f, \"distance_km\": %.3f, \"sentences_per_s\": %.1f,", a.maxSpeed, a.distanceM / 1000.0,
       a.samples ? (double)(ls.sentences - a.gpsSentences0) / a.samples : 0.0);
  line("    \"bad_checksums\": %lu, \"overflows\": %lu, \"receiver_restarts\": %lu },", (unsigned long)(ls.badChecksum - a.gpsBad0),
       (unsigned long)(ls.overflows - a.gpsOverflow0), (unsigned long)(ls.receiverRestarts - a.gpsRestarts0));
  line("  \"errors\": { \"touch_i2c\": %lu, \"sd_write\": %lu, \"sd_dropped_bytes\": %lu, \"loop_stalls\": %lu, \"low_memory\": %lu },",
       (unsigned long)touchErr, (unsigned long)sdErr, (unsigned long)sdDrop, (unsigned long)a.stalls, (unsigned long)a.lowMem);
  line("  \"memory\": { \"internal_min_kb\": %lu, \"psram_min_kb\": %lu },", (unsigned long)a.intMinKb,
       (unsigned long)a.psramMinKb);
  line("  \"loop\": { \"ui_max_ms\": %lu, \"services_max_ms\": %lu },", (unsigned long)a.uiMaxMs, (unsigned long)a.svcMaxMs);
  line("  \"radio\": { \"wifi_changes\": %lu, \"ble_changes\": %lu },", (unsigned long)a.wifiChanges, (unsigned long)a.bleChanges);
  s_sd->auxLine(f, (String("  \"verdict\": \"") + verdict + "\", \"fail_reasons\": [" + fails + "], \"warn_reasons\": [" +
                    warns + "],").c_str());
  line("  \"note\": \"aggregates cover the samples since the last start/resume; tools/scripts/fieldtest.ps1 recomputes everything from samples.csv\"");
  line("}");
  s_sd->auxClose(f);
}

}  // namespace

void FieldRecorder::begin(SdLog &sd) {
  s_sd = &sd;
  const String id = Settings::fieldTestId();
  if (!id.length()) return;
  if (!sd.mounted() || !sd.writerRunning()) return;     // keeps the marker; resumes when the card is back
  s_id = id;
  // Continue the test clock: last recorded t_s + the time since the reset (= uptime now).
  String tail;
  sd.readTail(("/FIELD/" + id + "/samples.csv").c_str(), tail, 400);
  const int nl = tail.lastIndexOf('\n', tail.length() >= 2 ? tail.length() - 2 : 0);
  const long lastT = nl >= 0 ? tail.substring(nl + 1).toInt() : 0;
  if (!openFiles(false)) { Serial.printf("[FT] cannot resume %s\n", id.c_str()); return; }
  s_offsetS = max((uint32_t)(lastT > 0 ? lastT + 1 + millis() / 1000 : 0), Settings::fieldTestElapsedS());
  s_resets = Settings::fieldTestResets() + 1;
  s_targetS = Settings::fieldTestTargetS();
  Settings::setFieldTestResets(s_resets);
  s_segStartMs = millis();
  s_lastSampleMs = s_lastSaveMs = millis();
  resetAgg();
  s_running = true;
  const esp_reset_reason_t rr = esp_reset_reason();
  s_crashReset = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
                 rr == ESP_RST_BROWNOUT;
  event("RESET", "resumed after reset: %s (reset #%lu during this test)", resetName(rr), (unsigned long)s_resets);
}

void FieldRecorder::update() {
  if (!s_running) return;
  const uint32_t now = millis();
  if (now - s_lastSampleMs < 1000) return;
  s_lastSampleMs += 1000;
  if (now - s_lastSampleMs > 3000) s_lastSampleMs = now;           // after a long stall: no burst of samples
  sample();
  if (now - s_lastSaveMs >= 30000) { s_lastSaveMs = now; Settings::setFieldTestElapsedS(elapsedS()); }
  if (s_targetS && elapsedS() >= s_targetS) {
    event("TARGET_REACHED", "planned duration %lu min completed", (unsigned long)(s_targetS / 60));
    stop();
    s_completed = true;
  }
}

bool FieldRecorder::start(uint32_t targetS, String *error) {
  if (s_running) return true;
  s_completed = false;
  if (!s_sd || !s_sd->mounted() || !s_sd->writerRunning()) { if (error) *error = "no SD card"; return false; }
  struct tm lt;
  char id[48];
  if (TimeService::local(lt)) {
    snprintf(id, sizeof(id), "FT-%04d%02d%02d-%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour,
             lt.tm_min, lt.tm_sec);
  } else {
    for (int n = 1; n < 10000; n++) {
      snprintf(id, sizeof(id), "FT-N%04d", n);
      if (!s_sd->exists((String("/FIELD/") + id).c_str())) break;
    }
  }
  s_id = id;
  if (!openFiles(true)) { if (error) *error = "cannot create files"; return false; }
  Settings::setFieldTestId(s_id);
  Settings::setFieldTestElapsedS(0);
  Settings::setFieldTestResets(0);
  Settings::setFieldTestTargetS(targetS);
  s_targetS = targetS;
  s_crashReset = false;
  s_offsetS = 0;
  s_resets = 0;
  s_segStartMs = millis();
  s_lastSampleMs = s_lastSaveMs = millis();
  resetAgg();
  s_running = true;
  const GpsData d = Location::snapshot();
  event("START", "%s %s (%s); GPS %s; %u satellites; Wi-Fi %s; BLE %s", APP_NAME, FW_VERSION, FW_GIT_DESCRIBE,
        STATE_NAME[(int)Location::quality(d)], (unsigned)d.satsUsed, WifiService::stateName(WifiService::state()),
        BleService::enabled() ? "on" : "off");
  Health::skipLoop();                  // creating the folder/files is a deliberate one-off wait
  return true;
}

void FieldRecorder::stop() {
  if (!s_running) return;
  event("STOP", "duration %lu s; %lu samples", (unsigned long)elapsedS(), (unsigned long)s_a.samples);
  s_sd->auxClose(s_fSamples);
  s_sd->auxClose(s_fEvents);
  s_fSamples = s_fEvents = -1;
  // Started before the clock was set (FT-Nnnnn)? Name it by its real start time now.
  struct tm lt;
  if (s_id.startsWith("FT-N") && TimeService::valid()) {
    const time_t start = time(nullptr) - elapsedS();
    localtime_r(&start, &lt);
    char id[48];
    snprintf(id, sizeof(id), "FT-%04d%02d%02d-%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour,
             lt.tm_min, lt.tm_sec);
    s_sd->auxFlushWait();              // files are closed by the writer task: finish before the rename
    if (s_sd->rename(s_dir.c_str(), (String("/FIELD/") + id).c_str())) {
      Serial.printf("[FT] renamed %s -> %s (clock set during the test)\n", s_id.c_str(), id);
      s_id = id;
      s_dir = String("/FIELD/") + id;
    }
  }
  writeSummary();
  Settings::setFieldTestId("");
  s_running = false;
  Health::skipLoop();                  // writing the summary and closing files: deliberate wait
}

bool FieldRecorder::running() { return s_running; }
uint32_t FieldRecorder::targetS() { return s_targetS; }
bool FieldRecorder::completed() { return s_completed; }
String FieldRecorder::id() { return s_id; }
uint32_t FieldRecorder::durationS() { return s_running ? elapsedS() : 0; }
uint32_t FieldRecorder::samples() { return s_a.samples; }
String FieldRecorder::lastEvent() { return s_lastEvent; }

void FieldRecorder::status(Print &out) {
  if (!s_running) { out.printf("[FT] not running%s%s\n", s_id.length() ? ", last test " : "", s_id.c_str()); return; }
  const uint32_t d = elapsedS();
  out.printf("[FT] running %s: %02lu:%02lu:%02lu, %lu samples, %lu fix samples, %lu fix losses, dir %s, last event: %s\n",
             s_id.c_str(), (unsigned long)(d / 3600), (unsigned long)(d / 60 % 60), (unsigned long)(d % 60),
             (unsigned long)s_a.samples, (unsigned long)s_a.fixSamples, (unsigned long)s_a.fixLosses, s_dir.c_str(),
             s_lastEvent.c_str());
}
