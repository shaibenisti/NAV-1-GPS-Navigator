#include "Settings.h"

#include <Preferences.h>
#include <esp_mac.h>
#include "TimeService.h"
#include "../SdLog.h"
#include "../../config.h"

namespace {
Preferences s_prefs;
bool s_open = false;

// ---- /system/settings.json --------------------------------------------
constexpr const char *FILE_PATH = "/system/settings.json";
constexpr uint32_t WRITE_DELAY_MS = 1000;      // several quick changes -> one write
SdLog *s_sd = nullptr;
bool s_dirty = false, s_pending = false;   // pending: background write running
uint32_t s_dirtyMs = 0;
String s_written;                              // last content written / read (no rewrite when equal)
String s_status = "no SD card";

void markDirty() { s_dirty = true; s_dirtyMs = millis(); }

// Minimal reader for the flat object written below: "key": "string" | true | false | number.
bool jsonValue(const String &j, const char *key, String &out) {
  const int k = j.indexOf(String("\"") + key + "\"");
  if (k < 0) return false;
  int i = j.indexOf(':', k);
  if (i < 0) return false;
  i++;
  while (i < (int)j.length() && isspace((unsigned char)j[i])) i++;
  if (i >= (int)j.length()) return false;
  if (j[i] == '"') {
    const int e = j.indexOf('"', i + 1);
    if (e < 0) return false;
    out = j.substring(i + 1, e);
  } else {
    int e = i;
    while (e < (int)j.length() && j[e] != ',' && j[e] != '}' && !isspace((unsigned char)j[e])) e++;
    out = j.substring(i, e);
  }
  return true;
}

bool validName(const String &n) {
  if (n.length() < 1 || n.length() > 24) return false;
  for (char c : n) if (!isalnum((unsigned char)c) && c != '-') return false;
  return true;
}

String jsonBool(bool b) { return b ? "true" : "false"; }

String buildFile() {
  return String("{\n") +
         "  \"_about\": \"NAV-1 settings. Edit on a PC, applied at the next start. Wi-Fi passwords are never stored here.\",\n" +
         "  \"device_name\": \"" + Settings::deviceName() + "\",\n" +
         "  \"time_zone\": \"" + TimeService::zoneName(Settings::timeZone()) + "\",\n" +
         "  \"wifi\": " + jsonBool(Settings::wifiEnabled()) + ",\n" +
         "  \"wifi_hotspot\": " + jsonBool(Settings::hotspotEnabled()) + ",\n" +
         "  \"bluetooth\": " + jsonBool(Settings::bleEnabled()) + ",\n" +
         "  \"brightness\": " + String(Settings::brightness()) + ",\n" +
         "  \"dim_after_s\": " + String(Settings::dimAfterS()) + "\n" +
         "}\n";
}

// Values from the file win over NVS (the file was edited on a PC or comes from another card).
int applyFile(const String &j, int &ignored) {
  int applied = 0;
  ignored = 0;
  String v;
  if (jsonValue(j, "device_name", v)) {
    if (!validName(v)) ignored++;
    else if (v != Settings::deviceName()) { s_prefs.putString("dev_name", v); applied++; }
  }
  if (jsonValue(j, "time_zone", v)) {
    int z = -1;
    for (int i = 0; i < TimeService::zoneCount(); i++) if (v.equalsIgnoreCase(TimeService::zoneName(i))) z = i;
    if (z < 0) ignored++;
    else if (z != Settings::timeZone()) { s_prefs.putInt("tz", z); applied++; }
  }
  struct { const char *key, *nvs; bool def; } bools[] = {
    { "wifi", "wifi_on", true }, { "wifi_hotspot", "ap_on", false }, { "bluetooth", "ble_on", true } };
  for (auto &b : bools) {
    if (!jsonValue(j, b.key, v)) continue;
    if (v != "true" && v != "false") { ignored++; continue; }
    const bool on = v == "true";
    if (on != s_prefs.getBool(b.nvs, b.def)) { s_prefs.putBool(b.nvs, on); applied++; }
  }
  if (jsonValue(j, "brightness", v)) {
    const int pct = v.toInt();
    if (pct < 10 || pct > 100) ignored++;
    else if (pct != Settings::brightness()) { s_prefs.putInt("bl_pct", pct); applied++; }
  }
  if (jsonValue(j, "dim_after_s", v)) {
    const long s = v.toInt();
    if (s < 0 || s > 3600 || (s == 0 && v != "0")) ignored++;
    else if ((uint32_t)s != Settings::dimAfterS()) { s_prefs.putUInt("bl_dim_s", (uint32_t)s); applied++; }
  }
  return applied;
}
}  // namespace

void Settings::attachSd(SdLog &sd) {
  if (!sd.mounted()) return;
  s_sd = &sd;
  String j;
  if (sd.readFile(FILE_PATH, j, 4096) && j.length()) {
    int ignored = 0;
    const int applied = j.indexOf('{') >= 0 ? applyFile(j, ignored) : -1;
    if (applied < 0) {                              // not JSON: keep it for the user, write a fresh one
      sd.rename(FILE_PATH, "/system/settings.bad");
      s_status = "settings.json unreadable (kept as settings.bad), rewritten";
    } else {
      s_status = String(FILE_PATH) + ": " + applied + " value(s) applied" + (ignored ? ", " + String(ignored) + " invalid ignored" : "");
      s_written = j;
    }
  } else {
    s_status = String(FILE_PATH) + " created";
  }
  s_dirty = true;                                   // (re)written by update() if it differs,
  s_dirtyMs = millis() - WRITE_DELAY_MS;            // in the background (~0.5 s on this card)
  Serial.printf("[SET] %s\n", s_status.c_str());
}

void Settings::update() {
  if (!s_sd) return;
  if (s_pending && !s_sd->writingAsync()) {
    s_pending = false;
    if (s_sd->asyncWriteFailed()) { s_status = "cannot write " + String(FILE_PATH); s_written = ""; }
  }
  if (!s_dirty || millis() - s_dirtyMs < WRITE_DELAY_MS || s_sd->writingAsync()) return;
  s_dirty = false;
  const String j = buildFile();
  if (j != s_written && s_sd->writeFileAsync(FILE_PATH, j)) { s_written = j; s_pending = true; }
}

String Settings::fileStatus() { return s_status; }
bool Settings::fileInSync() {
  String j;
  return s_sd && !s_dirty && !s_sd->writingAsync() && s_sd->readFile(FILE_PATH, j, 4096) && j == buildFile();
}

void Settings::begin() {
  if (!s_open) s_open = s_prefs.begin("gpsdev", false);
}

bool Settings::wifiEnabled() { return s_prefs.getBool("wifi_on", true); }
void Settings::setWifiEnabled(bool on) { s_prefs.putBool("wifi_on", on); markDirty(); }
String Settings::wifiSsid() { return s_prefs.getString("wifi_ssid", ""); }
String Settings::wifiPass() { return s_prefs.getString("wifi_pass", ""); }

void Settings::setWifiNetwork(const String &ssid, const String &pass) {
  s_prefs.putString("wifi_ssid", ssid);
  s_prefs.putString("wifi_pass", pass);
}

int Settings::wifiChannel() { return s_prefs.getInt("wifi_ch", 0); }
void Settings::setWifiChannel(int ch) { s_prefs.putInt("wifi_ch", ch); }

bool Settings::hotspotEnabled() { return s_prefs.getBool("ap_on", false); }
void Settings::setHotspotEnabled(bool on) { s_prefs.putBool("ap_on", on); markDirty(); }

String Settings::hotspotPass() {
  String p = s_prefs.getString("ap_pass", "");
  if (p.length() < 8) {                  // first use: random, easy to type (no 0/O/1/l)
    static const char CH[] = "abcdefghijkmnpqrstuvwxyz23456789";
    p = "";
    for (int i = 0; i < 8; i++) p += CH[esp_random() % (sizeof(CH) - 1)];
    s_prefs.putString("ap_pass", p);
  }
  return p;
}

bool Settings::bleEnabled() { return s_prefs.getBool("ble_on", true); }
void Settings::setBleEnabled(bool on) { s_prefs.putBool("ble_on", on); markDirty(); }

String Settings::fieldTestId() { return s_prefs.getString("ft_id", ""); }
void Settings::setFieldTestId(const String &id) { s_prefs.putString("ft_id", id); }
uint32_t Settings::fieldTestElapsedS() { return s_prefs.getUInt("ft_elapsed", 0); }
void Settings::setFieldTestElapsedS(uint32_t s) { s_prefs.putUInt("ft_elapsed", s); }
uint32_t Settings::fieldTestTargetS() { return s_prefs.getUInt("ft_target", 0); }
void Settings::setFieldTestTargetS(uint32_t s) { s_prefs.putUInt("ft_target", s); }
uint32_t Settings::fieldTestResets() { return s_prefs.getUInt("ft_resets", 0); }
void Settings::setFieldTestResets(uint32_t n) { s_prefs.putUInt("ft_resets", n); }

bool Settings::lastPos(double &lat, double &lon) {
  if (!s_prefs.isKey("pos_lat")) return false;
  lat = s_prefs.getDouble("pos_lat", 0);
  lon = s_prefs.getDouble("pos_lon", 0);
  return true;
}
void Settings::setLastPos(double lat, double lon) { s_prefs.putDouble("pos_lat", lat); s_prefs.putDouble("pos_lon", lon); }
String Settings::tripPath() { return s_prefs.getString("trip_path", ""); }
void Settings::setTripPath(const String &path) { s_prefs.putString("trip_path", path); }
String Settings::tripStats() { return s_prefs.getString("trip_stats", ""); }
void Settings::setTripStats(const String &s) { s_prefs.putString("trip_stats", s); }

bool Settings::nvsRoundTrip() {
  const String v = String("probe-") + String(esp_random(), HEX);
  const bool ok = s_prefs.putString("t_probe", v) == v.length() && s_prefs.getString("t_probe", "") == v;
  s_prefs.remove("t_probe");
  return ok && !s_prefs.isKey("t_probe");
}

int Settings::brightness() { return s_prefs.getInt("bl_pct", 100); }
void Settings::setBrightness(int pct) { s_prefs.putInt("bl_pct", pct); markDirty(); }
uint32_t Settings::dimAfterS() { return s_prefs.getUInt("bl_dim_s", 0); }
void Settings::setDimAfterS(uint32_t s) { s_prefs.putUInt("bl_dim_s", s); markDirty(); }

int Settings::timeZone() { return s_prefs.getInt("tz", 1); }
void Settings::setTimeZone(int index) { s_prefs.putInt("tz", index); markDirty(); }

int Settings::gpsProfile() { return s_prefs.getInt("gps_prof", GPS_PROFILE_DEFAULT); }
void Settings::setGpsProfile(int p) { s_prefs.putInt("gps_prof", p); }       // not in settings.json

String Settings::deviceName() {
  String name = s_prefs.getString("dev_name", "");
  if (name.length()) return name;
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[16];
  snprintf(buf, sizeof(buf), "NAV-1-%02X%02X", mac[4], mac[5]);
  return String(buf);
}
