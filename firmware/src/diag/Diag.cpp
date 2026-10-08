#include "Diag.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <soc/gdma_struct.h>
#include "Health.h"
#include "GpsReplay.h"
#include "../services/TripRecorder.h"
#include "../GpsLink.h"
#include "../GpsParser.h"
#include "../SdLog.h"
#include "../LvglPort.h"
#include "../RgbPanel.h"
#include "../TouchPort.h"
#include "../services/WifiService.h"
#include "../services/Settings.h"
#include "../services/BleService.h"
#include "../services/TimeService.h"
#include "../services/WebService.h"
#include "../services/Assets.h"
#include "../services/Ota.h"
#include "../services/Backlight.h"
#include "../services/GpsConfig.h"
#include "../services/Places.h"
#include "../services/Navigator.h"
#include "../services/Odometer.h"
#include <esp_app_desc.h>
#include "../../config.h"

namespace {

GpsLink *s_link = nullptr;
GpsParser *s_parser = nullptr;
SdLog *s_sd = nullptr;
bool s_parserSelfTestOk = true;
Diag::Pump s_pump = nullptr;
bool s_reports = false;
bool s_busy = false;
uint32_t s_bootMs = 0;
lv_obj_t *s_statusIcons = nullptr;

const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTERRUPT WDT";
    case ESP_RST_TASK_WDT: return "TASK WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

bool resetIsCrash(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
         r == ESP_RST_BROWNOUT;
}

GpsData gps() { return s_parser->snapshot(s_link->linkUp(GPS_LINK_TIMEOUT_MS)); }

void pumpFor(uint32_t ms) {
  const uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (s_pump) s_pump();
    else delay(5);
  }
}

// ---- sections ------------------------------------------------------------------------

void printSys(Print &o) {
  const uint32_t up = millis() / 1000;
  o.printf("[sys] %s v%s (%s)\n", APP_NAME, FW_VERSION, FW_GIT_DESCRIBE);
  o.printf("  uptime %luh%02lum%02lus, reset: %s, %s rev %d @ %u MHz\n", (unsigned long)(up / 3600),
           (unsigned long)(up / 60 % 60), (unsigned long)(up % 60), resetReasonName(esp_reset_reason()),
           ESP.getChipModel(), ESP.getChipRevision(), (unsigned)ESP.getCpuFreqMHz());
  o.printf("  flash %u MB, PSRAM %u MB, sketch %u KB (app partition 3072 KB)\n", (unsigned)(ESP.getFlashChipSize() >> 20),
           (unsigned)(ESP.getPsramSize() >> 20), (unsigned)(ESP.getSketchSize() >> 10));
}

void printMem(Print &o) {
  o.printf("[mem] internal free %u KB (min %u KB, largest block %u KB), DMA-capable %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) >> 10),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) >> 10));
  o.printf("  PSRAM free %u KB (min %u KB, largest %u KB) of %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) >> 10),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >> 10),
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >> 10));
  // Unused stack per task (bytes never touched since the task started) - where internal RAM idles.
  const UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t *ts = (TaskStatus_t *)heap_caps_malloc((n + 4) * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
  if (!ts) return;
  const UBaseType_t got = uxTaskGetSystemState(ts, n + 4, nullptr);
  o.print("  stack never used (bytes):");
  for (UBaseType_t i = 0; i < got; i++) o.printf(" %s %u", ts[i].pcTaskName, (unsigned)ts[i].usStackHighWaterMark);
  o.println();
  heap_caps_free(ts);
}

void printGps(Print &o) {
  const GpsLink::Stats &s = s_link->stats();
  const GpsData d = gps();
  o.printf("[gps] link %s, bytes %u, sentences %u, bad crc %u, overflow %u, parser crc %u, self-test %s\n",
           d.linkUp ? "UP" : "DOWN", (unsigned)s.bytes, (unsigned)s.sentences, (unsigned)s.badChecksum,
           (unsigned)s.overflows, (unsigned)d.parserChecksumFail, s_parserSelfTestOk ? "ok" : "FAILED");
  o.printf("  receiver restarts %u (+%u ordered by NAV-1), truncated sentences %u\n", (unsigned)s.receiverRestarts,
           (unsigned)s.commandedRestarts, (unsigned)s.truncated);
  o.printf("  module profile %s: %s%s%s\n", GpsConfig::profile() ? "galileo" : "factory", GpsConfig::stateName(GpsConfig::state()),
           GpsConfig::moduleSummary().length() ? " - " : "", GpsConfig::moduleSummary().c_str());
  o.printf("  %s  rmc=%c q=%u  sats used %u, in view %u+%u+%u (GPS+GLO+GAL)", d.fix ? "FIX" : "NO FIX", d.rmcStatus,
           (unsigned)d.fixQuality, (unsigned)d.satsUsed, (unsigned)d.satsViewGps, (unsigned)d.satsViewGlonass, (unsigned)d.satsViewGalileo);
  if (d.hdopValid) o.printf(", hdop %.2f", d.hdop);
  if (d.ttffMs) o.printf(", ttff %us", (unsigned)(d.ttffMs / 1000));
  o.println();
  if (d.locValid) o.printf("  %.6f, %.6f  age %u ms", d.lat, d.lng, (unsigned)d.locAgeMs);
  else o.print("  position invalid");
  if (d.speedValid) o.printf(", %.1f km/h", d.speedKmh);
  if (d.altValid) o.printf(", alt %.1f m", d.altM);
  if (d.timeValid) o.printf(", %02u:%02u:%02u UTC", d.hour, d.minute, d.second);
  if (d.dateValid) o.printf(" %04u-%02u-%02u", d.year, d.month, d.day);
  o.println();
}

void printWifi(Print &o) {
  const WifiService::Stats &s = WifiService::stats();
  o.printf("[wifi] %s", WifiService::stateName(WifiService::state()));
  if (WifiService::ssid().length()) o.printf(" '%s'", WifiService::ssid().c_str());
  if (WifiService::state() == WifiService::State::Connected)
    o.printf(" ip %s rssi %d dBm ch %d, http://%s.local", WifiService::ip().c_str(), WifiService::rssi(),
             WifiService::channel(), WifiService::hostname().c_str());
  if (WifiService::lastError().length()) o.printf(", last error: %s", WifiService::lastError().c_str());
  o.println();
  if (WifiService::hotspotOn())
    o.printf("  hotspot '%s' ip %s, %d phone(s) connected\n", WifiService::hotspotSsid().c_str(), WifiService::hotspotIp().c_str(),
             WifiService::hotspotClients());
  if (WebService::running()) o.printf("  web: %s (%s), %lu requests, task stack unused %lu B\n", WebService::url().c_str(),
                                       WebService::localUrl().c_str(), (unsigned long)WebService::requests(),
                                       (unsigned long)WebService::stackFreeBytes());
  const String saved = Settings::wifiSsid();
  if (saved.length()) o.printf("  saved network '%s' (password %u chars) in NVS\n", saved.c_str(), (unsigned)Settings::wifiPass().length());
  else o.println("  saved network: none");
  o.printf("  scans %u (fail %u, last %u ms, %d networks), attempts %u, connects %u, disconnects %u (last reason %u),"
           " saves %u (failed %u)\n",
           (unsigned)s.scans, (unsigned)s.scanFails, (unsigned)s.lastScanMs, WifiService::networkCount(),
           (unsigned)s.connectAttempts, (unsigned)s.connects, (unsigned)s.disconnects, (unsigned)s.lastDisconnectReason,
           (unsigned)s.saves, (unsigned)s.saveFailures);
}

void printBle(Print &o) {
  const BleService::Stats &s = BleService::stats();
  o.printf("[ble] %s, stack %s, %s, name '%s', clients %d\n", BleService::enabled() ? "enabled" : "disabled",
           BleService::initialized() ? "up" : "down", BleService::advertising() ? "advertising" : "not advertising",
           BleService::name().c_str(), BleService::connectedCount());
  o.printf("  connects %u, disconnects %u, notifies %u, commands %u (last '%s'), mtu %u\n", (unsigned)s.connects,
           (unsigned)s.disconnects, (unsigned)s.notifies, (unsigned)s.commands, BleService::lastCommand().c_str(),
           (unsigned)s.mtu);
}

void printTouch(Print &o) {
  const TouchPort::Stats t = TouchPort::totals();
  const Health::Window &w = Health::last();
  o.printf("[touch] GT911 %s, since boot: polls %u, reports %u, i2c errors %u, overflows %u, stuck %u, poll max %.2f ms\n",
           TouchPort::present() ? "present" : "NOT FOUND", (unsigned)t.polls, (unsigned)t.reports, (unsigned)t.i2cErrors,
           (unsigned)t.overflows, (unsigned)t.stuckReleases, t.pollUsMax / 1000.0f);
  o.printf("  last 5 s: polls %u, reports %u, events to LVGL %u, batch max %u\n", (unsigned)w.touch.polls,
           (unsigned)w.touch.reports, (unsigned)w.frames.touchEvents, (unsigned)w.frames.touchBatchMax);
}

void printRefills(Print &o, const RgbPanel::RefillStats &r);

void printDisplay(Print &o) {
  const Health::Window &w = Health::last();
  const Health::Totals &t = Health::totals();
  const LvglPort::FrameStats &f = w.frames;
  o.printf("[display] %s, refresh %u Hz; last 5 s: frames %u, render avg %.1f ms max %.1f ms, write-back max %.2f ms\n",
           LvglPort::modeName(), (unsigned)w.vsyncHz, (unsigned)f.frames,
           f.frames ? f.renderUsSum / 1000.0f / f.frames : 0.0f, f.renderUsMax / 1000.0f, f.writebackUsMax / 1000.0f);
  o.printf("  buffer sync areas %u (GDMA %u) max %.1f ms; vsync races %u; loop max: LVGL %.1f ms, services %.1f ms\n",
           (unsigned)f.syncs, (unsigned)f.syncDmaAreas, f.syncUsMax / 1000.0f, (unsigned)w.vsyncRaces,
           w.loopLvglUsMax / 1000.0f, w.loopServicesUsMax / 1000.0f);
  o.printf("  since boot: frames %u, render max %.1f ms, loop max LVGL %.1f ms / services %.1f ms (slowest: %s %.1f ms), races %u\n",
           (unsigned)t.frames, t.renderUsMax / 1000.0f, t.loopLvglUsMax / 1000.0f, t.loopServicesUsMax / 1000.0f,
           Health::slowestPart()[0] ? Health::slowestPart() : "-", Health::slowestPartUs() / 1000.0f, (unsigned)t.vsyncRaces);
  printRefills(o, RgbPanel::refillStats());
}

// Screen refill timing (RgbPanel, bounce mode): frames whose refill came late against the typical frame.
// A smear shows once the delay exceeds the strip's margin (~0.4 ms per 8 rows of LCD_BOUNCE_LINES).
void printRefills(Print &o, const RgbPanel::RefillStats &r) {
  if (!r.frames) return;
  o.printf("  screen refills: %u frames, typical %.2f ms after vsync; late by >=0.3 ms %u, >=0.6 ms %u, >=1.0 ms %u, worst +%.2f ms\n",
           (unsigned)r.frames, r.typicalUs / 1000.0f, (unsigned)r.late300, (unsigned)r.late600, (unsigned)r.late1000,
           r.worstLateUs / 1000.0f);
}

void printTime(Print &o) {
  struct tm u, l;
  if (!TimeService::utc(u) || !TimeService::local(l)) {
    o.printf("[time] not set (needs GPS time or Wi-Fi/NTP), zone %s\n", TimeService::zoneName(TimeService::zone()));
    return;
  }
  o.printf("[time] %04d-%02d-%02d %02d:%02d:%02d UTC, local %02d:%02d:%02d (%s), source %s, last sync %lu s ago\n",
           u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec, l.tm_hour, l.tm_min, l.tm_sec,
           TimeService::zoneName(TimeService::zone()), TimeService::source(), (unsigned long)TimeService::lastSyncAgeS());
}

void printSd(Print &o) {
  SdLog &sd = *s_sd;
  o.printf("[sd] %s", sd.mounted() ? "mounted" : "NOT MOUNTED");
  if (sd.mounted()) o.printf(" %u MB card", (unsigned)sd.cardMB());
  o.printf(", %s", sd.logging() ? "logging" : "not logging");
  if (sd.logging()) o.printf(" /GPSLOG/%s", sd.sessionName());
  o.printf(", writer task %s\n", sd.writerRunning() ? "on" : "off");
  o.printf("  csv rows %u, nmea bytes %u, write errors %u, dropped %u, mount attempts %d%s\n", (unsigned)sd.csvLines(),
           (unsigned)sd.nmeaBytes(), (unsigned)sd.writeErrors(), (unsigned)sd.droppedBytes(), sd.mountAttempts(),
           sd.wakeUpUsed() ? " (card recovery used)" : "");
}

// ---- self-test -----------------------------------------------------------------------

struct Tally { int pass = 0, warn = 0, fail = 0; };

void check(Print &o, Tally &t, const char *area, int level, const char *fmt, ...) {
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  static const char *names[] = { "PASS", "WARN", "FAIL" };
  o.printf("[CHECK] %-8s %s %s\n", area, names[level], msg);
  (level == 0 ? t.pass : level == 1 ? t.warn : t.fail)++;
}
enum { CHK_PASS = 0, CHK_WARN = 1, CHK_FAIL = 2 };

}  // namespace

void Diag::begin(GpsLink &link, GpsParser &parser, SdLog &sd, bool parserSelfTestOk, Pump pump) {
  s_link = &link;
  s_parser = &parser;
  s_sd = &sd;
  s_parserSelfTestOk = parserSelfTestOk;
  s_pump = pump;
}

bool Diag::reportsOn() { return s_reports; }
void Diag::setBootMs(uint32_t ms) { s_bootMs = ms; }
bool Diag::busy() { return s_busy; }
void Diag::setStatusIconsLabel(lv_obj_t *label) { s_statusIcons = label; }

void Diag::print(Print &out, const char *section) {
  const bool all = !section || !*section || !strcmp(section, "all");
  if (all || !strcmp(section, "sys")) printSys(out);
  if (all || !strcmp(section, "mem")) printMem(out);
  if (all || !strcmp(section, "gps")) printGps(out);
  if (all || !strcmp(section, "wifi")) printWifi(out);
  if (all || !strcmp(section, "ble")) printBle(out);
  if (all || !strcmp(section, "touch")) printTouch(out);
  if (all || !strcmp(section, "display")) printDisplay(out);
  if (all || !strcmp(section, "time")) printTime(out);
  if (all || !strcmp(section, "sd")) printSd(out);
}

int Diag::healthRows(HealthRow *rows, int max) {
  int n = 0;
  auto add = [&](const char *name, Level level, const char *fmt, ...) {
    if (n >= max) return;
    HealthRow &r = rows[n++];
    r.name = name;
    r.level = level;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.text, sizeof(r.text), fmt, ap);
    va_end(ap);
  };

  const esp_reset_reason_t rr = esp_reset_reason();
  const bool crashed = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
                       rr == ESP_RST_BROWNOUT;
  const uint32_t up = millis() / 60000;
  char upText[24];
  if (up >= 60) snprintf(upText, sizeof(upText), "%lu h %lu min", (unsigned long)(up / 60), (unsigned long)(up % 60));
  else snprintf(upText, sizeof(upText), "%lu min", (unsigned long)up);
  add("System", crashed ? PROBLEM : OK, "v%s  -  up %s  -  last restart: %s", FW_VERSION, upText, resetReasonName(rr));

  const unsigned freeKb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10;
  add("Memory", freeKb < 40 ? ATTENTION : OK, "%u KB free (lowest %u KB)", freeKb,
      (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) >> 10));

  const GpsData d = gps();
  const GpsLink::Stats &gs = s_link->stats();
  if (!d.linkUp) add("GPS", PROBLEM, "No data from the GPS module");
  else if (d.fix) add("GPS", gs.receiverRestarts ? ATTENTION : OK, "Fix  -  %u satellites  -  accuracy %s%s", (unsigned)d.satsUsed,
                      !d.hdopValid ? "?" : d.hdop < 1.5 ? "very good" : d.hdop < 3 ? "good" : "fair",
                      gs.receiverRestarts ? "  -  module restarted" : "");
  else add("GPS", ATTENTION, "No fix  -  %u satellites in view%s", (unsigned)d.satsInView(),
           gs.receiverRestarts ? "  -  module restarted" : "");

  struct tm l;
  if (TimeService::local(l))
    add("Time", OK, "%02d:%02d %s, from %s", l.tm_hour, l.tm_min, TimeService::zoneName(TimeService::zone()),
        strcmp(TimeService::source(), "NTP") ? TimeService::source() : "the internet");
  else add("Time", ATTENTION, "Not set yet (needs GPS or Wi-Fi)");

  const WifiService::State ws = WifiService::state();
  if (ws == WifiService::State::Off) add("Wi-Fi", OFF, "Off");
  else if (ws == WifiService::State::Connected) {
    const int r = WifiService::rssi();
    add("Wi-Fi", OK, "Connected to '%s', signal %s", WifiService::ssid().c_str(),
        r >= -60 ? "strong" : r >= -70 ? "good" : r >= -80 ? "fair" : "weak");
  } else if (ws == WifiService::State::Connecting) add("Wi-Fi", ATTENTION, "Connecting to '%s'...", WifiService::ssid().c_str());
  else if (ws == WifiService::State::Idle) add("Wi-Fi", ATTENTION, "On, no network saved");
  else add("Wi-Fi", ATTENTION, "Not connected to '%s'", WifiService::ssid().c_str());

  if (!BleService::enabled()) add("Bluetooth", OFF, "Off");
  else add("Bluetooth", BleService::advertising() || BleService::connectedCount() ? OK : ATTENTION, "%s",
           BleService::connectedCount() ? "On, phone connected" : BleService::advertising() ? "On, visible" : "On, not visible");

  SdLog &sd = *s_sd;
  if (!sd.mounted()) add("SD card", PROBLEM, "No card (or it does not answer)");
  else if (sd.writeErrors() || sd.droppedBytes())
    add("SD card", ATTENTION, "%u errors while writing", (unsigned)(sd.writeErrors() + (sd.droppedBytes() ? 1 : 0)));
  else add("SD card", OK, "%s  -  no errors", sd.logging() ? "Recording" : "Ready");

  const TouchPort::Stats t = TouchPort::totals();
  if (!TouchPort::present()) add("Touch", PROBLEM, "Touch controller not found");
  else if (t.i2cErrors) add("Touch", ATTENTION, "%u read errors", (unsigned)t.i2cErrors);
  else add("Touch", OK, "OK");

  const Health::Window &w = Health::last();
  const uint32_t stallMs = Health::totals().loopServicesUsMax / 1000;
  add("Display", stallMs >= 150 ? ATTENTION : OK, "%u Hz  -  %s", (unsigned)w.vsyncHz,
      stallMs >= 150 ? "the screen froze briefly" : "smooth");
  return n;
}

int Diag::selfTest(Print &o, const char *only) {
  if (s_busy) return 0;
  s_busy = true;
  Tally t;
  // Area filter: "" = all, else a comma/space separated list, e.g. "gps,wifi".
  auto want = [only](const char *area) {
    if (!only || !*only) return true;
    const size_t n = strlen(area);
    for (const char *p = strstr(only, area); p; p = strstr(p + 1, area))
      if ((p == only || p[-1] == ',' || p[-1] == ' ') && (p[n] == 0 || p[n] == ',' || p[n] == ' ')) return true;
    return false;
  };
  String metrics;                      // "[METRICS] key=value ..." for validate.ps1 baselines
  auto metric = [&metrics](const char *key, uint32_t value) { metrics += String(' ') + key + '=' + String(value); };
  o.printf("[SELFTEST] start %s\n", only && *only ? only : "all");
  metric("boot_ms", s_bootMs);

  if (want("system")) {
    const esp_reset_reason_t rr = esp_reset_reason();
    check(o, t, "system", resetIsCrash(rr) ? CHK_FAIL : CHK_PASS, "reset reason %s, uptime %lu s", resetReasonName(rr),
          (unsigned long)(millis() / 1000));
    const bool board = ESP.getFlashChipSize() == 16UL * 1024 * 1024 && ESP.getPsramSize() >= 8UL * 1024 * 1024 - 65536;
    check(o, t, "system", board ? CHK_PASS : CHK_FAIL, "flash %u MB, PSRAM %u MB", (unsigned)(ESP.getFlashChipSize() >> 20),
          (unsigned)(ESP.getPsramSize() >> 20));
  }

  if (want("memory")) {
    const bool heapOk = heap_caps_check_integrity_all(true);
    check(o, t, "memory", heapOk ? CHK_PASS : CHK_FAIL, "heap integrity %s", heapOk ? "ok" : "CORRUPT");
    const uint32_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10;
    const uint32_t minInt = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) >> 10;
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10;
    // Marks lowered on purpose: the display's bounce buffers take 51 KB for good since v0.2.4 (were
    // free >= 60 / largest >= 32 KB before them). Measured with Wi-Fi + BLE + web: ~35 KB free, largest 21 KB.
    check(o, t, "memory", freeInt >= 28 && largest >= 16 ? CHK_PASS : (freeInt >= 20 ? CHK_WARN : CHK_FAIL),
          "internal free %u KB (min since boot %u KB, largest block %u KB)", (unsigned)freeInt, (unsigned)minInt,
          (unsigned)largest);
    const uint32_t freePs = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10;
    check(o, t, "memory", freePs >= 1024 ? CHK_PASS : (freePs >= 256 ? CHK_WARN : CHK_FAIL), "PSRAM free %u KB", (unsigned)freePs);
    metric("internal_free_kb", freeInt);
    metric("internal_min_kb", minInt);
    metric("internal_largest_kb", largest);
    metric("psram_free_kb", freePs);
  }

  const GpsData d = gps();
  if (want("gps")) {
    check(o, t, "gps", s_parserSelfTestOk ? CHK_PASS : CHK_FAIL, "parser self-test at boot %s", s_parserSelfTestOk ? "ok" : "FAILED");
    const GpsLink::Stats &ls = s_link->stats();
    check(o, t, "gps", d.linkUp ? CHK_PASS : CHK_FAIL, "UART link %s (%u bytes, %u sentences)", d.linkUp ? "up" : "DOWN",
          (unsigned)ls.bytes, (unsigned)ls.sentences);
    const uint32_t total = ls.sentences + ls.badChecksum;
    const float badPct = total ? 100.0f * ls.badChecksum / total : 0;
    check(o, t, "gps", badPct < 1 ? CHK_PASS : (badPct < 5 ? CHK_WARN : CHK_FAIL), "bad checksums %u (%.2f %%)",
          (unsigned)ls.badChecksum, badPct);
    check(o, t, "gps", ls.overflows == 0 ? CHK_PASS : CHK_WARN, "line overflows %u", (unsigned)ls.overflows);
    if (ls.receiverRestarts)
      check(o, t, "gps", CHK_WARN, "GPS module restarted %u time(s) since boot (last %lu s ago) - supply or wiring?",
            (unsigned)ls.receiverRestarts, (unsigned long)((millis() - ls.lastRestartMs) / 1000));
    else check(o, t, "gps", CHK_PASS, "GPS module has not restarted since boot (%u truncated sentences, %u restarts ordered by NAV-1)",
               (unsigned)ls.truncated, (unsigned)ls.commandedRestarts);
    {
      const GpsConfig::State cs = GpsConfig::state();
      const bool verified = cs == GpsConfig::State::Applied || cs == GpsConfig::State::Factory;
      const bool expected = GpsConfig::profile() ? cs == GpsConfig::State::Applied : cs == GpsConfig::State::Factory;
      check(o, t, "gps", expected ? CHK_PASS : (cs == GpsConfig::State::Failed || verified ? CHK_FAIL : CHK_WARN),
            "module profile %s: %s%s%s", GpsConfig::profile() ? "galileo" : "factory", GpsConfig::stateName(cs),
            GpsConfig::lastError().length() ? " - " : (GpsConfig::moduleSummary().length() ? " - " : ""),
            GpsConfig::lastError().length() ? GpsConfig::lastError().c_str() : GpsConfig::moduleSummary().c_str());
    }
    if (d.fix) check(o, t, "gps", CHK_PASS, "fix, %u satellites used, hdop %.2f", (unsigned)d.satsUsed, d.hdopValid ? d.hdop : 0.0);
    else check(o, t, "gps", CHK_WARN, "no fix (%u used, %u in view) - normal indoors", (unsigned)d.satsUsed,
               (unsigned)d.satsInView());
  }

  if (want("time")) {
    struct tm l;
    if (TimeService::local(l))
      check(o, t, "time", CHK_PASS, "clock set from %s, local %02d:%02d (%s)", TimeService::source(), l.tm_hour, l.tm_min,
            TimeService::zoneName(TimeService::zone()));
    else check(o, t, "time", CHK_WARN, "clock not set yet (needs GPS time or Wi-Fi) - normal indoors without Wi-Fi");
  }

  if (want("touch")) {
    check(o, t, "touch", TouchPort::present() ? CHK_PASS : CHK_FAIL, "GT911 %s at 0x5D", TouchPort::present() ? "found" : "NOT FOUND");
    const TouchPort::Stats t0 = TouchPort::totals();
    pumpFor(300);
    const TouchPort::Stats t1 = TouchPort::totals();
    const uint32_t polls = t1.polls - t0.polls;
    check(o, t, "touch", polls >= 30 ? CHK_PASS : CHK_FAIL, "touch task alive: %u polls in 300 ms", (unsigned)polls);
    const float errPct = t1.polls ? 100.0f * t1.i2cErrors / t1.polls : 0;
    check(o, t, "touch", t1.i2cErrors == 0 ? CHK_PASS : (errPct < 1 ? CHK_WARN : CHK_FAIL),
          "I2C errors %u since boot (%.2f %%), stuck releases %u", (unsigned)t1.i2cErrors, errPct, (unsigned)t1.stuckReleases);
    metric("touch_i2c_errors", t1.i2cErrors);
  }

  if (want("display")) {
    const uint32_t v0 = RgbPanel::vsyncCount();
    pumpFor(1000);
    const uint32_t hz = RgbPanel::vsyncCount() - v0;
    check(o, t, "display", hz >= 30 && hz <= 50 ? CHK_PASS : CHK_FAIL, "panel refresh %u Hz", (unsigned)hz);
    lv_obj_invalidate(lv_screen_active());
    const uint32_t r0 = micros();
    lv_refr_now(nullptr);
    const uint32_t fullMs = (micros() - r0) / 1000;
    check(o, t, "display", fullMs < 200 ? CHK_PASS : (fullMs < 350 ? CHK_WARN : CHK_FAIL), "full-screen frame %u ms (render + show)",
          (unsigned)fullMs);
    const Health::Totals &ht = Health::totals();
    check(o, t, "display", ht.loopServicesUsMax < 50000 ? CHK_PASS : (ht.loopServicesUsMax < 150000 ? CHK_WARN : CHK_FAIL),
          "longest non-UI loop stall %.1f ms since boot (slowest part: %s %.1f ms)", ht.loopServicesUsMax / 1000.0f,
          Health::slowestPart()[0] ? Health::slowestPart() : "-", Health::slowestPartUs() / 1000.0f);
    check(o, t, "display", Backlight::pwmOk() ? CHK_PASS : CHK_FAIL, "backlight PWM %s, brightness %d %%, dim after %s",
          Backlight::pwmOk() ? "on" : "NOT attached", Backlight::brightness(),
          Backlight::dimAfter() ? (String(Backlight::dimAfter()) + " s").c_str() : "never");
    metric("refresh_hz", hz);
    metric("full_frame_ms", fullMs);
    metric("loop_stall_ms", ht.loopServicesUsMax / 1000);
  }

  if (want("sd")) {
    SdLog &sd = *s_sd;
    if (!sd.mounted()) {
      check(o, t, "sd", CHK_WARN, "no card mounted (device works without SD)");
    } else {
      check(o, t, "sd", sd.logging() && sd.writerRunning() ? CHK_PASS : CHK_FAIL, "%u MB card, session %s, writer task %s",
            (unsigned)sd.cardMB(), sd.sessionName(), sd.writerRunning() ? "on" : "off");
      const uint32_t n0 = sd.nmeaBytes();
      pumpFor(1500);
      const uint32_t n1 = sd.nmeaBytes();
      const bool flowing = n1 > n0 || !d.linkUp;
      check(o, t, "sd", flowing ? CHK_PASS : CHK_FAIL, "data reaching the card: +%u NMEA bytes in 1.5 s", (unsigned)(n1 - n0));
      check(o, t, "sd", sd.writeErrors() == 0 && sd.droppedBytes() == 0 ? CHK_PASS : CHK_FAIL, "write errors %u, dropped bytes %u",
            (unsigned)sd.writeErrors(), (unsigned)sd.droppedBytes());
      // Files app layer: listing, free space, delete in the background, in-use protection.
      static SdLog::DirEntry e[4];
      int total = 0;
      uint32_t t0 = millis();
      const int listed = sd.listDir("/GPSLOG", e, 4, &total);
      const uint32_t listMs = millis() - t0;
      uint64_t cap = 0, free = 0;
      const bool df = sd.usage(cap, free);
      check(o, t, "sd", listed > 0 && df ? CHK_PASS : CHK_FAIL, "file manager: /GPSLOG %d entries listed in %u ms, %llu of %llu MB free",
            total, (unsigned)listMs, free >> 20, cap >> 20);
      const bool refused = !sd.removeStart("/GPSLOG");
      const bool made = sd.mkdirs("/system/selftest/a") && sd.writeFile("/system/selftest/a/x.txt", "x") &&
                        sd.writeFile("/system/selftest/y.txt", "y");
      bool deleted = false;
      t0 = millis();
      if (made && sd.removeStart("/system/selftest")) {
        while (sd.removing() && millis() - t0 < 10000) pumpFor(20);
        deleted = !sd.removing() && !sd.removeFailed() && sd.removedCount() == 4 && !sd.exists("/system/selftest");
      }
      check(o, t, "sd", refused && deleted ? CHK_PASS : CHK_FAIL, "delete: test folder (4 items) removed in %u ms, folder in use %s",
            (unsigned)(millis() - t0), refused ? "refused" : "NOT refused");
      pumpFor(1200);                     // a pending debounced settings write lands first
      check(o, t, "sd", Settings::fileInSync() ? CHK_PASS : CHK_FAIL, "settings file matches the device settings (%s)",
            Settings::fileStatus().c_str());
      check(o, t, "sd", Assets::rejectedCount() ? CHK_WARN : CHK_PASS, "SD images: %d loaded, %d ignored (see [ASSET] boot lines)",
            Assets::loadedCount(), Assets::rejectedCount());
    }
  }

  if (want("wifi")) {
    const bool nvs = Settings::nvsRoundTrip();
    check(o, t, "wifi", nvs ? CHK_PASS : CHK_FAIL, "settings storage (NVS) write/read-back %s", nvs ? "ok" : "FAILED");
    if (!WifiService::enabled()) {
      check(o, t, "wifi", CHK_WARN, "disabled in Settings");
    } else {
      uint32_t w0 = millis();
      while (WifiService::state() == WifiService::State::Connecting && millis() - w0 < 16000) pumpFor(100);
      const uint32_t serial = WifiService::scanSerial();
      bool started = WifiService::startScan();
      w0 = millis();
      while (started && WifiService::scanSerial() == serial && millis() - w0 < 12000) pumpFor(100);
      const int n = WifiService::networkCount();
      if (!started || WifiService::scanSerial() == serial) check(o, t, "wifi", CHK_FAIL, "scan did not complete");
      else check(o, t, "wifi", n > 0 ? CHK_PASS : CHK_WARN, "scan: %d networks in %u ms%s%s", n,
                 (unsigned)WifiService::stats().lastScanMs, n ? ", strongest " : "", n ? WifiService::network(0).ssid : "");
      metric("wifi_scan_ms", WifiService::stats().lastScanMs);
      if (!WifiService::hasSavedNetwork()) {
        check(o, t, "wifi", CHK_WARN, "no network configured (Settings > Wi-Fi)");
      } else if (WifiService::state() == WifiService::State::Connected) {
        check(o, t, "wifi", CHK_PASS, "connected to '%s', ip %s, rssi %d dBm", WifiService::ssid().c_str(),
              WifiService::ip().c_str(), WifiService::rssi());
        pumpFor(600);                                    // status bar refreshes every 500 ms
        const bool icon = s_statusIcons && strstr(lv_label_get_text(s_statusIcons), LV_SYMBOL_WIFI);
        check(o, t, "wifi", icon ? CHK_PASS : CHK_FAIL, "status bar %s the Wi-Fi icon", icon ? "shows" : "does NOT show");
        const bool saved = Settings::wifiSsid() == WifiService::ssid() && Settings::wifiPass().length() > 0;
        check(o, t, "wifi", saved ? CHK_PASS : CHK_FAIL, "connected network %s in NVS (auto-reconnect after reboot)",
              saved ? "is saved" : "is NOT saved");
      } else {
        check(o, t, "wifi", CHK_FAIL, "saved network '%s' not connected (%s)", WifiService::ssid().c_str(),
              WifiService::stateName(WifiService::state()));
      }
    }
  }

  if (want("ble")) {
    if (!BleService::enabled()) {
      check(o, t, "ble", CHK_WARN, "disabled in Settings");
    } else {
      const bool ok = BleService::initialized() && (BleService::advertising() || BleService::connectedCount() > 0);
      check(o, t, "ble", ok ? CHK_PASS : CHK_FAIL, "'%s' %s, %d client(s)", BleService::name().c_str(),
            BleService::advertising() ? "advertising" : "not advertising", BleService::connectedCount());
    }
  }

  o.printf("[METRICS]%s\n", metrics.c_str());
  o.printf("[SELFTEST] done pass=%d warn=%d fail=%d\n", t.pass, t.warn, t.fail);
  s_busy = false;
  Health::skipLoop();                  // this loop iteration waited on purpose: not a stall
  return t.fail;
}

// ---- console -------------------------------------------------------------------------

namespace {
// Next argument; "double quotes" allow spaces. Returns "" at the end.
String nextArg(const char *&p) {
  while (*p == ' ') p++;
  String a;
  if (*p == '"') {
    p++;
    while (*p && *p != '"') a += *p++;
    if (*p == '"') p++;
  } else {
    while (*p && *p != ' ') a += *p++;
  }
  return a;
}

// ---- "sd put <path> <size>": file upload over the console (tools/scripts/sdput.ps1) ------
// Base64 lines follow, "." ends. The PC waits for "[PUT] <bytes>" after every 32 lines, so the
// 4 KB console RX buffer never overflows. Data is collected in PSRAM, written in one go.
uint8_t *s_putBuf = nullptr;
size_t s_putSize = 0, s_putLen = 0;
uint32_t s_putLines = 0;
String s_putPath;

void putEnd() {
  heap_caps_free(s_putBuf);
  s_putBuf = nullptr;
}

void putLine(const char *line, Print &o) {
  if (!strcmp(line, ".")) {
    bool ok = s_putLen == s_putSize;
    const int slash = s_putPath.lastIndexOf('/');
    if (ok && slash > 0) ok = s_sd->mkdirs(s_putPath.substring(0, slash).c_str());
    ok = ok && s_sd->writeFile(s_putPath.c_str(), s_putBuf, s_putLen);
    o.printf("[PUT] %s %s %u bytes\n", ok ? "ok" : "FAILED", s_putPath.c_str(), (unsigned)s_putLen);
    putEnd();
    return;
  }
  size_t n = 0;
  if (mbedtls_base64_decode(s_putBuf + s_putLen, s_putSize - s_putLen, &n, (const unsigned char *)line, strlen(line)) != 0) {
    o.printf("[PUT] FAILED decode at line %u\n", (unsigned)s_putLines);
    putEnd();
    return;
  }
  s_putLen += n;
  if (++s_putLines % 32 == 0) o.printf("[PUT] %u\n", (unsigned)s_putLen);
}
}  // namespace

bool Diag::command(const char *line, Print &o) {
  if (s_putBuf) { putLine(line, o); return true; }
  const char *p = line;
  const String cmd = nextArg(p);
  if (cmd == "help") {
    o.println("[HELP] diag [sys|mem|gps|time|wifi|ble|touch|display|sd]   report on|off");
    o.println("[HELP] selftest [system,memory,gps,time,touch,display,sd,wifi,ble]   (no argument = all)");
    o.println("[HELP] wifi scan | wifi list | wifi connect \"ssid\" [pass] | wifi forget | wifi on|off | wifi hotspot on|off");
    o.println("[HELP] ble on|off   |  UI: open <app>  page <App> <n>  files <folder>  home  |  probes: flash  swipe  bench  races  pulse 0|1");
    o.println("[HELP] backlight [10..100] | backlight dim <seconds, 0 = never> | backlight wake");
    o.println("[HELP] trip start|stop|status|list   gps replay <file.NMEA> [speed] | gps live (replay a recorded session) | gps profile [factory|galileo] | gps ubxtest | gps ubx");
    o.println("[HELP] sd ls [dir] | sd cat <path> | sd dir [dir] | sd df | sd rm <path> | sd put <path> <size> (sdput.ps1)");
    o.println("[HELP] ota arm|disarm|status|reject   (firmware update over Wi-Fi: tools/scripts/ota.ps1)");
    o.println("[HELP] place list | place add <lat> <lon> \"name\" | place rename <i> \"name\" | place rm <i> | place go <i>");
    o.println("[HELP] nav [status] | nav goto <lat> <lon> [\"name\"] | nav follow <trip name> [back|forward] | nav reverse | nav stop");
    return true;
  }
  if (cmd == "diag") { print(o, nextArg(p).c_str()); return true; }
  if (cmd == "mem") { print(o, "mem"); return true; }
  if (cmd == "selftest") { selfTest(o, p); return true; }   // rest of the line = area filter
  if (cmd == "report") {
    s_reports = nextArg(p) != "off";
    o.printf("[DIAG] periodic reports %s\n", s_reports ? "on" : "off");
    return true;
  }
  if (cmd == "wifi") {
    const String sub = nextArg(p);
    if (sub == "scan") o.printf("[WIFI] scan %s\n", WifiService::startScan() ? "started" : "not started (off/busy)");
    else if (sub == "list") {
      for (int i = 0; i < WifiService::networkCount(); i++) {
        const WifiService::Network &n = WifiService::network(i);
        o.printf("[WIFI] %2d %4d dBm ch%2u %s %s\n", i, n.rssi, n.channel, n.open ? "open  " : "secure", n.ssid);
      }
    } else if (sub == "connect") {
      const String ssid = nextArg(p), pass = nextArg(p);
      if (!ssid.length()) { o.println("[WIFI] usage: wifi connect \"ssid\" [pass]"); return true; }
      WifiService::connect(ssid.c_str(), pass.c_str());
      o.printf("[WIFI] connecting to '%s'\n", ssid.c_str());
    } else if (sub == "forget") { WifiService::forget(); o.println("[WIFI] saved network forgotten"); }
    else if (sub == "on" || sub == "off") { WifiService::setEnabled(sub == "on"); o.printf("[WIFI] %s\n", sub.c_str()); }
    else if (sub == "hotspot") {
      const String v = nextArg(p);
      if (v == "on" || v == "off") WifiService::setHotspot(v == "on");
      o.printf("[WIFI] hotspot %s\n", Settings::hotspotEnabled() ? "on" : "off");
    }
    else print(o, "wifi");
    return true;
  }
  if (cmd == "place") {
    const String sub = nextArg(p);
    if (sub == "add") {
      const double lat = nextArg(p).toDouble(), lon = nextArg(p).toDouble();
      String name = nextArg(p);
      if (!name.length()) name = Places::defaultName();
      const int i = Places::add(name.c_str(), lat, lon);
      if (i >= 0) o.printf("[PLACE] added %d '%s' %.7f %.7f\n", i, name.c_str(), lat, lon);
      else o.println("[PLACE] cannot add (list full, bad name or position)");
    } else if (sub == "rename") {
      const int i = nextArg(p).toInt();
      const String name = nextArg(p);
      o.printf("[PLACE] rename %d: %s\n", i, Places::rename(i, name.c_str()) ? "ok" : "FAILED");
    } else if (sub == "rm") {
      const int i = nextArg(p).toInt();
      o.printf("[PLACE] rm %d: %s\n", i, Places::remove(i) ? "ok" : "FAILED");
    } else if (sub == "go") {
      Places::Place pl;
      if (Places::get(nextArg(p).toInt(), pl) && Navigator::goTo(pl.name, pl.lat, pl.lon)) o.printf("[NAV] going to '%s'\n", pl.name);
      else o.println("[NAV] no such place");
    } else if (sub == "file") {
      o.printf("[PLACE] %s, in sync %s\n", Places::status().c_str(), Places::fileInSync() ? "yes" : "no");
    } else {
      for (int i = 0; i < Places::count(); i++) {
        Places::Place pl;
        Places::get(i, pl);
        o.printf("[PLACE] %2d %.7f %.7f %s\n", i, pl.lat, pl.lon, pl.name);
      }
      o.printf("[PLACE] list end (%d) - %s\n", Places::count(), Places::status().c_str());
    }
    return true;
  }
  if (cmd == "nav") {
    const String sub = nextArg(p);
    if (sub == "goto") {
      const double lat = nextArg(p).toDouble(), lon = nextArg(p).toDouble();
      const String name = nextArg(p);
      o.printf("[NAV] goto: %s\n", Navigator::goTo(name.length() ? name.c_str() : "Destination", lat, lon) ? "ok" : "bad position");
    } else if (sub == "follow") {
      String base = nextArg(p), title = base;
      const String dir = nextArg(p);
      if (!base.startsWith("/")) {                     // a trip name (trip list), else a base path
        TripRecorder::Summary *trips = new TripRecorder::Summary[20];
        const int n = TripRecorder::list(trips, 20);
        for (int i = 0; i < n; i++) if (trips[i].name == base) { base = trips[i].base; title = trips[i].title; }
        delete[] trips;
      }
      const bool ok = Navigator::follow(base, title.c_str(), dir == "back", dir != "forward" && dir != "back");
      o.printf("[NAV] follow %s: %s\n", base.c_str(), ok ? "ok" : "no track (a name from 'trip list' or a base path)");
    } else if (sub == "reverse") {
      Navigator::reverse();
    } else if (sub == "stop") {
      Navigator::stop();
      o.println("[NAV] stopped");
    }
    const Navigator::Status st = Navigator::status();
    if (!Navigator::active()) o.println("[NAV] off");
    else o.printf("[NAV] %s '%s' fix=%d dist=%.0f m bearing=%.0f off=%.0f m progress=%.2f eta=%lu s%s%s\n", Navigator::modeName(), st.name, st.fix,
                  st.distM, st.bearingDeg, st.offM, st.progress, (unsigned long)st.etaS, st.arrived ? " ARRIVED" : "", st.offRoute ? " OFF-ROUTE" : "");
    o.printf("[ODO] %.3f km, moving %lu s, max %.1f km/h, avg %.1f km/h\n", Odometer::distanceKm(), (unsigned long)Odometer::movingS(),
             Odometer::maxKmh(), Odometer::avgKmh());
    return true;
  }
  if (cmd == "trip") {
    const String sub = nextArg(p);
    if (sub == "start") {
      String err;
      if (TripRecorder::start(&err)) o.printf("[TRIP] started %s\n", TripRecorder::name().c_str());
      else o.printf("[TRIP] cannot start: %s\n", err.c_str());
    } else if (sub == "stop") {
      TripRecorder::stop();
    } else if (sub == "list") {
      static TripRecorder::Summary trips[10];
      const int n = TripRecorder::list(trips, 10);
      for (int i = 0; i < n; i++)
        o.printf("[TRIP] %s  %s  %.3f km  %lu s  max %.1f km/h  %lu points\n", trips[i].name.c_str(), trips[i].title.c_str(),
                 trips[i].distanceKm, (unsigned long)trips[i].durationS, trips[i].maxKmh, (unsigned long)trips[i].points);
      o.printf("[TRIP] list end (%d)\n", n);
    } else {
      if (TripRecorder::recording())
        o.printf("[TRIP] recording %s: %lu s, %.3f km, %lu points, max %.1f km/h%s\n", TripRecorder::name().c_str(),
                 (unsigned long)TripRecorder::durationS(), TripRecorder::distanceKm(), (unsigned long)TripRecorder::points(),
                 TripRecorder::maxKmh(), TripRecorder::waitingForFix() ? ", waiting for fix" : "");
      else o.println("[TRIP] not recording");
    }
    return true;
  }
  if (cmd == "gps") {                  // developer GPS replay
    const String sub = nextArg(p);
    if (sub == "replay") {
      const String path = nextArg(p);
      const int speed = max(1L, nextArg(p).toInt());
      String err;
      if (!GpsReplay::start(path.c_str(), speed, &err)) o.printf("[REPLAY] cannot start: %s\n", err.c_str());
    } else if (sub == "live") {
      GpsReplay::stop();
    } else if (sub == "replaystatus") {
      GpsReplay::status(o);
    } else if (sub == "profile") {     // GPS module settings: factory defaults or the GPS + Galileo package
      const String v = nextArg(p);
      if (v == "factory") GpsConfig::setProfile(0);
      else if (v == "galileo") GpsConfig::setProfile(1);
      else if (v.length()) o.println("[GPSCFG] usage: gps profile [factory|galileo]");
      GpsConfig::status(o);
    } else if (sub == "simloss") {     // test: module back on defaults + restarted, as after a power loss (GpsConfig not told)
      const uint8_t clr[13] = { 0x1A, 0, 0, 0, 0, 0, 0, 0, 0x1A, 0, 0, 0, 0x01 };   // CFG-CFG clear + load MSG/NAV/RXM, BBR
      const uint8_t rst[4] = { 0, 0, 0x00, 0 };                                       // CFG-RST hot, hardware reset
      const char *e1 = s_link->sendUbxOnce(GPS_TX_TEST_PIN, 0x06, 0x09, clr, sizeof(clr));
      delay(200);
      s_link->expectRestart(6000);
      const char *e2 = *e1 ? e1 : s_link->sendUbxOnce(GPS_TX_TEST_PIN, 0x06, 0x04, rst, sizeof(rst));
      o.printf("[UBX] simulated power loss: %s\n", *e2 ? e2 : "module defaults loaded, module restarting");
    } else if (sub == "ubxtest") {     // one read-only poll: can NAV-1 talk to the module? (no configuration)
      if (!s_link->linkUp(GPS_LINK_TIMEOUT_MS)) { o.println("[UBX] GPS link down - nothing sent"); return true; }
      const uint32_t seq = s_link->lastUbx().seq;
      const char *err = s_link->sendUbxOnce(GPS_TX_TEST_PIN, 0x0A, 0x04, nullptr, 0);   // UBX-MON-VER poll
      if (*err) o.printf("[UBX] not sent: %s\n", err);
      else o.printf("[UBX] MON-VER poll sent on GPIO%d (open-drain, pin released); reply frames so far %u - check with: gps ubx\n",
                    GPS_TX_TEST_PIN, (unsigned)seq);
    } else if (sub == "ubx") {         // last UBX frame received from the module
      const GpsLink::UbxFrame &f = s_link->lastUbx();
      const GpsLink::Stats &ls = s_link->stats();
      o.printf("[UBX] frames ok %u, bad %u\n", (unsigned)ls.ubxFrames, (unsigned)ls.ubxBad);
      if (!f.seq) { o.println("[UBX] no frame received"); return true; }
      o.printf("[UBX] last: class 0x%02X id 0x%02X, %u bytes, %lu ms ago\n", f.cls, f.id, f.len, (unsigned long)(millis() - f.atMs));
      if (f.cls == 0x0A && f.id == 0x04 && f.len >= 40) {   // MON-VER: sw[30] hw[10] extension[30] x n
        char s[31];
        memcpy(s, f.payload, 30); s[30] = 0; o.printf("[UBX] MON-VER sw: %s\n", s);
        memcpy(s, f.payload + 30, 10); s[10] = 0; o.printf("[UBX] MON-VER hw: %s\n", s);
        for (uint16_t off = 40; off + 30 <= f.len; off += 30) { memcpy(s, f.payload + off, 30); s[30] = 0; o.printf("[UBX] MON-VER ext: %s\n", s); }
      }
    } else {
      print(o, "gps");
      GpsReplay::status(o);
    }
    return true;
  }
  if (cmd == "sd") {                   // SD inspection for the PC scripts
    const String sub = nextArg(p), path = nextArg(p);
    if (sub == "ls") { s_sd->list(path.length() ? path.c_str() : "/", o); o.println("[SD] list end"); }
    else if (sub == "cat" && path.length()) s_sd->dump(path.c_str(), o);
    else if (sub == "dir") {             // FatFs listing (Files app path): count + timing
      static SdLog::DirEntry e[8];
      int total = 0;
      const uint32_t t0 = millis();
      const int n = s_sd->listDir(path.length() ? path.c_str() : "/", e, 8, &total);
      o.printf("[SD] dir %s: %d entries in %u ms\n", path.length() ? path.c_str() : "/", total, (unsigned)(millis() - t0));
      for (int i = 0; i < n; i++)
        o.printf("  %s%-24s %10u  %04u-%02u-%02u %02u:%02u\n", e[i].dir ? "/" : " ", e[i].name, (unsigned)e[i].size,
                 1980 + (e[i].fdate >> 9), (e[i].fdate >> 5) & 15, e[i].fdate & 31, e[i].ftime >> 11, (e[i].ftime >> 5) & 63);
    } else if (sub == "fs") {             // sd fs [path]: read-only file-system inspection
      s_sd->fsInfo(path.c_str(), o);
    } else if (sub == "sector") {         // sd sector <n>: raw read of one card sector (read-only)
      s_sd->rawSector(strtoul(path.c_str(), nullptr, 10), o);
    } else if (sub == "df") {
      uint64_t total = 0, free = 0;
      const uint32_t t0 = millis();
      if (s_sd->usage(total, free))
        o.printf("[SD] %llu MB total, %llu MB free (%u ms)\n", total >> 20, free >> 20, (unsigned)(millis() - t0));
      else o.println("[SD] usage unavailable");
    } else if (sub == "put" && path.length()) {
      const long size = nextArg(p).toInt();
      if (size <= 0 || size > 4 * 1024 * 1024 || !s_sd->mounted() || s_sd->inUse(path.c_str())) {
        o.println("[PUT] FAILED (size 1..4 MB, card mounted, file not in use)");
        return true;
      }
      s_putBuf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
      if (!s_putBuf) { o.println("[PUT] FAILED no memory"); return true; }
      s_putSize = size;
      s_putLen = s_putLines = 0;
      s_putPath = path;
      o.printf("[PUT] ready %s %ld bytes\n", path.c_str(), size);
    } else if (sub == "rm" && path.length()) {
      if (!s_sd->removeStart(path.c_str())) { o.printf("[SD] cannot delete %s (in use, busy or missing)\n", path.c_str()); return true; }
      const uint32_t t0 = millis();
      while (s_sd->removing() && millis() - t0 < 60000) pumpFor(50);
      o.printf("[SD] rm %s: %s, %u item(s)\n", path.c_str(), s_sd->removeFailed() ? "FAILED" : "ok", (unsigned)s_sd->removedCount());
    }
    else print(o, "sd");
    return true;
  }
  if (cmd == "dmapri") {                // GDMA channels: peripheral, priority, weight (display investigation)
    static const char *const PERI[] = { "SPI2", "SPI3", "UHCI0", "I2S0", "I2S1", "LCD_CAM", "AES", "SHA", "ADC", "RMT" };
    for (int i = 0; i < 5; i++) {
      const uint32_t in = GDMA.channel[i].in.peri_sel.sel, out = GDMA.channel[i].out.peri_sel.sel;
      o.printf("[DMA] ch%d  tx: %-7s pri %u weight %u   rx: %-7s pri %u\n", i, out < 10 ? PERI[out] : (out == 63 ? "-" : "M2M?"),
               (unsigned)GDMA.channel[i].out.pri.tx_pri, (unsigned)GDMA.channel[i].out.weight.tx_weight,
               in < 10 ? PERI[in] : (in == 63 ? "-" : "M2M?"), (unsigned)GDMA.channel[i].in.pri.rx_pri);
    }
    o.printf("[DMA] priority arbitration %s\n", GDMA.misc_conf.arb_pri_dis ? "DISABLED" : "on");
    return true;
  }
  if (cmd == "refills") {               // screen refill timing since the last call ("refills" = read + restart)
    printRefills(o, RgbPanel::refillStats(true));
    return true;
  }
  if (cmd == "fbdiff") {                // are both framebuffers the same picture? (idle: they must be)
    const uint16_t *a = RgbPanel::framebuffer(0), *b = RgbPanel::framebuffer(1);
    uint32_t n = 0;
    int x0 = 800, y0 = 480, x1 = -1, y1 = -1;
    for (int y = 0; y < 480; y++)
      for (int x = 0; x < 800; x++)
        if (a[y * 800 + x] != b[y * 800 + x]) {
          n++;
          x0 = min(x0, x); x1 = max(x1, x); y0 = min(y0, y); y1 = max(y1, y);
        }
    if (n) o.printf("[FBDIFF] %u pixels differ, area x%d..%d y%d..%d\n", (unsigned)n, x0, x1, y0, y1);
    else o.println("[FBDIFF] both framebuffers identical");
    Health::skipLoop();
    return true;
  }
  if (cmd == "shot") {                  // screenshot of the shown framebuffer (tools/scripts/shot.ps1)
    // RLE (u16 count, u16 RGB565) -> base64 lines between <<<SHOT w h bytes>>> and <<<END SHOT>>>
    const uint16_t *fb = LvglPort::shownFramebuffer();
    if (!fb) { o.println("[SHOT] no framebuffer"); return true; }
    const int n = 800 * 480;
    uint8_t *rle = (uint8_t *)heap_caps_malloc(n * 4, MALLOC_CAP_SPIRAM);
    if (!rle) { o.println("[SHOT] no memory"); return true; }
    size_t len = 0;
    for (int i = 0; i < n;) {
      const uint16_t v = fb[i];
      int run = 1;
      while (i + run < n && run < 65535 && fb[i + run] == v) run++;
      rle[len++] = run & 0xFF; rle[len++] = run >> 8; rle[len++] = v & 0xFF; rle[len++] = v >> 8;
      i += run;
    }
    o.printf("<<<SHOT 800 480 %u>>>\n", (unsigned)len);
    char line[100];
    for (size_t off = 0; off < len; off += 72) {
      size_t w = 0;
      mbedtls_base64_encode((unsigned char *)line, sizeof(line), &w, rle + off, min<size_t>(72, len - off));
      line[w] = 0;
      o.println(line);
    }
    o.println("<<<END SHOT>>>");
    Health::skipLoop();                  // sending the picture over USB is a deliberate wait, not a UI stall
    heap_caps_free(rle);
    return true;
  }
  if (cmd == "backlight") {             // backlight [<10..100>] | backlight dim <seconds, 0 = never> | backlight wake
    const String a = nextArg(p);
    if (a == "dim") Backlight::setDimAfter(nextArg(p).toInt());
    else if (a == "wake") Backlight::wake();
    else if (a.length()) Backlight::setBrightness(a.toInt());
    o.printf("[BL] brightness %d %%, dim after %u s (0 = never), %s\n", Backlight::brightness(), (unsigned)Backlight::dimAfter(),
             Backlight::dimmed() ? "DIMMED" : "on");
    return true;
  }
  if (cmd == "ota") {                   // firmware update over Wi-Fi (tools/scripts/ota.ps1)
    const String sub = nextArg(p);
    if (sub == "arm") Ota::arm();
    else if (sub == "disarm") { Ota::disarm(); o.println("[OTA] disarmed"); }
    else if (sub == "reject") Ota::rejectForTest();
    else {
      const esp_app_desc_t *d = esp_app_get_description();
      o.printf("[OTA] %s, slot %s, %s %s built %s %s%s%s\n", Ota::stateName(Ota::state()), Ota::runningSlot().c_str(), d->project_name,
               FW_GIT_DESCRIBE, d->date, d->time, Ota::pendingVerify() ? ", NOT verified yet" : "",
               Ota::lastError().length() ? (", last error: " + Ota::lastError()).c_str() : "");
    }
    return true;
  }
  if (cmd == "ble") {
    const String sub = nextArg(p);
    if (sub == "on" || sub == "off") { BleService::setEnabled(sub == "on"); o.printf("[BLE] %s\n", sub.c_str()); }
    else print(o, "ble");
    return true;
  }
  return false;
}
