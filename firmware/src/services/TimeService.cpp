#include "TimeService.h"

#include <sys/time.h>
#include <esp_sntp.h>
#include "Location.h"
#include "Settings.h"
#include "WifiService.h"

namespace {

struct Zone { const char *name; const char *posix; };
const Zone ZONES[] = {
  { "UTC",               "UTC0" },
  { "Israel",            "IST-2IDT,M3.4.4/26,M10.5.0" },
  { "UK / Portugal",     "GMT0BST,M3.5.0/1,M10.5.0" },
  { "Central Europe",    "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Eastern Europe",    "EET-2EEST,M3.5.0/3,M10.5.0/4" },
  { "US Eastern",        "EST5EDT,M3.2.0,M11.1.0" },
  { "US Central",        "CST6CDT,M3.2.0,M11.1.0" },
  { "US Mountain",       "MST7MDT,M3.2.0,M11.1.0" },
  { "US Pacific",        "PST8PDT,M3.2.0,M11.1.0" },
  { "India",             "IST-5:30" },
  { "Japan",             "JST-9" },
  { "Australia East",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
constexpr int ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);

int s_zone = 1;
const char *s_source = "none";
uint32_t s_lastSyncMs = 0;
bool s_synced = false;
bool s_ntpStarted = false;

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void applyZone() {
  setenv("TZ", ZONES[s_zone].posix, 1);
  tzset();
}

void setClock(time_t t, const char *source) {
  const timeval tv = { t, 0 };
  settimeofday(&tv, nullptr);
  s_source = source;
  s_synced = true;
  s_lastSyncMs = millis();
}

}  // namespace

void TimeService::begin() {
  s_zone = constrain(Settings::timeZone(), 0, ZONE_COUNT - 1);
  applyZone();
}

void TimeService::update() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();

  const GpsData d = Location::snapshot();
  if (d.linkUp && d.timeValid && d.dateValid && d.year >= 2024 && !Location::replaying()) {   // replay = old time
    const time_t gps = (time_t)(daysFromCivil(d.year, d.month, d.day) * 86400LL + d.hour * 3600 + d.minute * 60 + d.second);
    const time_t now = time(nullptr);
    if (!s_synced || strcmp(s_source, "GPS") != 0 || llabs((long long)(now - gps)) > 2) setClock(gps, "GPS");
    else s_lastSyncMs = millis();                 // GPS confirms the clock
    return;
  }
  // No GPS time: NTP once Wi-Fi is up (the SNTP client keeps it synced afterwards).
  if (!s_ntpStarted && WifiService::state() == WifiService::State::Connected) {
    s_ntpStarted = true;
    configTzTime(ZONES[s_zone].posix, "pool.ntp.org", "time.google.com");
  }
  if (s_ntpStarted && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && strcmp(s_source, "GPS") != 0) {
    s_source = "NTP";
    s_synced = true;
    s_lastSyncMs = millis();
  }
}

bool TimeService::valid() { return s_synced; }
const char *TimeService::source() { return s_source; }

bool TimeService::local(struct tm &out) {
  if (!s_synced) return false;
  const time_t now = time(nullptr);
  localtime_r(&now, &out);
  return true;
}

bool TimeService::utc(struct tm &out) {
  if (!s_synced) return false;
  const time_t now = time(nullptr);
  gmtime_r(&now, &out);
  return true;
}

uint32_t TimeService::lastSyncAgeS() { return s_synced ? (millis() - s_lastSyncMs) / 1000 : UINT32_MAX; }

int TimeService::zoneCount() { return ZONE_COUNT; }
const char *TimeService::zoneName(int i) { return ZONES[constrain(i, 0, ZONE_COUNT - 1)].name; }
int TimeService::zone() { return s_zone; }

void TimeService::setZone(int i) {
  s_zone = constrain(i, 0, ZONE_COUNT - 1);
  Settings::setTimeZone(s_zone);
  applyZone();
}
