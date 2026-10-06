#include "TripRecorder.h"

#include <math.h>
#include <esp_heap_caps.h>
#include "../SdLog.h"
#include "Location.h"
#include "Settings.h"
#include "TimeService.h"
#include "../../config.h"

namespace {

constexpr float MOVING_KMH = 1.5f;       // below: standing (GPS speed noise)
constexpr double MIN_STEP_M = 3.0;       // new point when moved this far ...
constexpr uint32_t MAX_GAP_S = 10;       // ... or after this long

SdLog *s_sd = nullptr;
bool s_rec = false;
String s_base, s_name, s_title, s_startUtc;
int s_fGpx = -1, s_fCsv = -1;
uint32_t s_segStartMs = 0, s_offsetS = 0, s_lastTickMs = 0, s_lastSaveMs = 0, s_lastPointS = 0;
uint32_t s_movingS = 0, s_points = 0;
double s_distM = 0, s_lastLat = 0, s_lastLon = 0;
bool s_havePoint = false;
float s_maxKmh = 0, s_curKmh = 0;
bool s_noFix = true;

uint32_t elapsedS() { return s_offsetS + (millis() - s_segStartMs) / 1000; }

double distM(double la1, double lo1, double la2, double lo2) {
  const double R = 6371000.0, k = M_PI / 180.0;
  const double dLa = (la2 - la1) * k, dLo = (lo2 - lo1) * k;
  const double a = sin(dLa / 2) * sin(dLa / 2) + cos(la1 * k) * cos(la2 * k) * sin(dLo / 2) * sin(dLo / 2);
  return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

String gpsUtc(const GpsData &d) {        // point time from the GPS itself (also right during a replay)
  if (!d.dateValid || !d.timeValid) return "";
  char b[32];
  snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ", d.year, d.month, d.day, d.hour, d.minute, d.second);
  return b;
}

void saveStats() {
  char b[80];
  snprintf(b, sizeof(b), "%lu,%lu,%.1f,%.1f,%lu", (unsigned long)elapsedS(), (unsigned long)s_movingS, s_distM, s_maxKmh,
           (unsigned long)s_points);
  Settings::setTripStats(b);
}

bool openFiles(bool fresh) {
  s_fGpx = s_sd->auxOpen((s_base + ".gpx").c_str());
  s_fCsv = s_sd->auxOpen((s_base + ".csv").c_str());
  if (s_fGpx < 0 || s_fCsv < 0) return false;
  if (fresh) {
    s_sd->auxLine(s_fGpx, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    s_sd->auxLine(s_fGpx, (String("<gpx version=\"1.1\" creator=\"") + APP_NAME + " " + FW_VERSION +
                           "\" xmlns=\"http://www.topografix.com/GPX/1/1\">").c_str());
    s_sd->auxLine(s_fGpx, (String("<trk><name>") + s_title + "</name><trkseg>").c_str());
    s_sd->auxLine(s_fCsv, "t_s,utc,lat,lon,ele_m,speed_kmh,course_deg,sats,hdop");
  }
  return true;
}

void addPoint(const GpsData &d, uint32_t t) {
  const String utc = gpsUtc(d);
  // GPX 1.1 wptType element order: ele, time, ..., sat, hdop
  String gpx = "<trkpt lat=\"" + String(d.lat, 7) + "\" lon=\"" + String(d.lng, 7) + "\">";
  if (d.altValid) gpx += "<ele>" + String(d.altM, 1) + "</ele>";
  if (utc.length()) gpx += "<time>" + utc + "</time>";
  gpx += "<sat>" + String(d.satsUsed) + "</sat>";
  if (d.hdopValid && d.hdop < 50) gpx += "<hdop>" + String(d.hdop, 1) + "</hdop>";
  gpx += "</trkpt>";
  s_sd->auxLine(s_fGpx, gpx.c_str());
  char line[200];
  snprintf(line, sizeof(line), "%lu,%s,%.7f,%.7f,%s,%s,%s,%u,%s", (unsigned long)t, utc.c_str(), d.lat, d.lng,
           d.altValid ? String(d.altM, 1).c_str() : "", d.speedValid ? String(d.speedKmh, 1).c_str() : "",
           d.courseValid ? String(d.courseDeg, 0).c_str() : "", (unsigned)d.satsUsed,
           d.hdopValid && d.hdop < 50 ? String(d.hdop, 1).c_str() : "");
  s_sd->auxLine(s_fCsv, line);
  s_points++;
  s_lastPointS = t;
}

void tick() {
  const uint32_t t = elapsedS();
  const GpsData d = Location::snapshot();
  const bool fix = Location::quality(d) == Location::Quality::Fix;
  s_noFix = !fix;
  s_curKmh = fix && d.speedValid ? (float)d.speedKmh : 0;
  if (!fix) { s_havePoint = false; return; }
  if (s_curKmh >= MOVING_KMH) s_movingS++;
  if (s_curKmh > s_maxKmh) s_maxKmh = s_curKmh;
  const double step = s_havePoint ? distM(s_lastLat, s_lastLon, d.lat, d.lng) : 0;   // from the last track point
  if (!s_havePoint || step >= MIN_STEP_M || t - s_lastPointS >= MAX_GAP_S) {
    // Distance = the recorded track (same as the GPX). Counting the growing step on every
    // epoch between points added the same metres repeatedly: 1.09 km for a 0.58 km walk (2026-09-29).
    if (s_havePoint && (s_curKmh >= MOVING_KMH || step > 15)) s_distM += step;     // ignore standing-still jitter
    addPoint(d, t);
    s_lastLat = d.lat;
    s_lastLon = d.lng;
  }
  s_havePoint = true;
}

String jsonValue(const String &json, const char *key) {
  const int k = json.indexOf(String('"') + key + '"');
  if (k < 0) return "";
  int v = json.indexOf(':', k) + 1;
  while (v > 0 && (json[v] == ' ' || json[v] == '"')) v++;
  int e = v;
  while (e < (int)json.length() && json[e] != ',' && json[e] != '"' && json[e] != '}') e++;
  return json.substring(v, e);
}

}  // namespace

void TripRecorder::begin(SdLog &sd) {
  s_sd = &sd;
  const String base = Settings::tripPath();
  if (!base.length() || !sd.mounted() || !sd.writerRunning()) return;
  s_base = base;
  s_name = base.substring(base.lastIndexOf('/') + 1);
  s_title = s_name;
  if (!openFiles(false)) return;
  unsigned long dur = 0, mov = 0, pts = 0;
  float dist = 0, mx = 0;
  sscanf(Settings::tripStats().c_str(), "%lu,%lu,%f,%f,%lu", &dur, &mov, &dist, &mx, &pts);
  s_offsetS = dur + millis() / 1000;
  s_movingS = mov;
  s_distM = dist;
  s_maxKmh = mx;
  s_points = pts;
  s_segStartMs = s_lastTickMs = s_lastSaveMs = millis();
  s_havePoint = false;
  s_rec = true;
  Serial.printf("[TRIP] resumed %s after a reset\n", s_name.c_str());
}

void TripRecorder::update() {
  if (!s_rec) return;
  const uint32_t now = millis();
  if (now - s_lastTickMs < 1000) return;
  s_lastTickMs += 1000;
  if (now - s_lastTickMs > 3000) s_lastTickMs = now;
  tick();
  if (now - s_lastSaveMs >= 30000) { s_lastSaveMs = now; saveStats(); }
}

bool TripRecorder::start(String *error) {
  if (s_rec) return true;
  if (!s_sd || !s_sd->mounted() || !s_sd->writerRunning()) { if (error) *error = "no SD card"; return false; }
  struct tm lt, ut;
  String dir;
  if (TimeService::local(lt) && TimeService::utc(ut)) {
    char n[48], t[48], u[48];
    snprintf(n, sizeof(n), "%04d-%02d-%02d_%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    snprintf(t, sizeof(t), "%02d.%02d.%04d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900, lt.tm_hour, lt.tm_min);
    snprintf(u, sizeof(u), "%04d-%02d-%02dT%02d:%02d:%02dZ", ut.tm_year + 1900, ut.tm_mon + 1, ut.tm_mday, ut.tm_hour, ut.tm_min, ut.tm_sec);
    dir = "/data/trips/" + String(lt.tm_year + 1900);
    s_name = n;
    s_title = t;
    s_startUtc = u;
  } else {
    dir = "/data/trips/undated";
    for (int i = 1; i < 10000; i++) {
      char n[16];
      snprintf(n, sizeof(n), "TRIP-N%04d", i);
      if (!s_sd->exists((dir + "/" + n + ".gpx").c_str())) { s_name = n; break; }
    }
    s_title = s_name;
    s_startUtc = "";
  }
  const uint32_t t0 = millis();
  if (!s_sd->mkdirs(dir.c_str())) { if (error) *error = "cannot create " + dir; return false; }
  const uint32_t t1 = millis();
  s_base = dir + "/" + s_name;
  if (!openFiles(true)) { if (error) *error = "cannot create the trip files"; return false; }
  const uint32_t t2 = millis();
  s_offsetS = s_movingS = s_points = s_lastPointS = 0;
  s_distM = 0;
  s_maxKmh = s_curKmh = 0;
  s_havePoint = false;
  s_segStartMs = s_lastTickMs = s_lastSaveMs = millis();
  Settings::setTripPath(s_base);
  saveStats();
  s_rec = true;
  Serial.printf("[TRIP] recording %s (start %lu ms: folder %lu, files %lu, settings %lu)\n", s_base.c_str(),
                (unsigned long)(millis() - t0), (unsigned long)(t1 - t0), (unsigned long)(t2 - t1), (unsigned long)(millis() - t2));
  return true;
}

void TripRecorder::stop() {
  if (!s_rec) return;
  s_rec = false;
  s_sd->auxLine(s_fGpx, "</trkseg></trk></gpx>");
  s_sd->auxClose(s_fGpx);
  s_sd->auxClose(s_fCsv);
  s_fGpx = s_fCsv = -1;
  const uint32_t dur = elapsedS();
  // Started before the clock was set (undated/TRIP-Nnnnn)? File it under its real start time.
  struct tm lt, ut;
  if (s_name.startsWith("TRIP-N") && TimeService::valid()) {
    const time_t start = time(nullptr) - dur;
    localtime_r(&start, &lt);
    gmtime_r(&start, &ut);
    char n[48], t[48], u[48];
    snprintf(n, sizeof(n), "%04d-%02d-%02d_%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    snprintf(t, sizeof(t), "%02d.%02d.%04d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900, lt.tm_hour, lt.tm_min);
    snprintf(u, sizeof(u), "%04d-%02d-%02dT%02d:%02d:%02dZ", ut.tm_year + 1900, ut.tm_mon + 1, ut.tm_mday, ut.tm_hour, ut.tm_min, ut.tm_sec);
    const String dir = "/data/trips/" + String(lt.tm_year + 1900), base = dir + "/" + n;
    s_sd->auxFlushWait();                                  // closed by the writer task: finish before the rename
    if (s_sd->mkdirs(dir.c_str()) && s_sd->rename((s_base + ".gpx").c_str(), (base + ".gpx").c_str()) &&
        s_sd->rename((s_base + ".csv").c_str(), (base + ".csv").c_str())) {
      Serial.printf("[TRIP] %s filed as %s (clock set during the trip)\n", s_name.c_str(), base.c_str());
      s_base = base;
      s_name = n;
      s_title = t;
      s_startUtc = u;
    }
  }
  const float km = s_distM / 1000.0;
  const float avg = s_movingS >= 30 ? km / (s_movingS / 3600.0f) : 0;          // meaningless below 30 s of movement
  const int f = s_sd->auxOpen((s_base + ".json").c_str());
  if (f >= 0) {
    char b[300];
    snprintf(b, sizeof(b),
             "{\"name\":\"%s\",\"title\":\"%s\",\"start_utc\":\"%s\",\"duration_s\":%lu,\"moving_s\":%lu,\"distance_km\":%.3f,"
             "\"max_kmh\":%.1f,\"avg_kmh\":%.1f,\"points\":%lu,\"firmware\":\"%s %s\"}",
             s_name.c_str(), s_title.c_str(), s_startUtc.c_str(), (unsigned long)dur, (unsigned long)s_movingS, km, s_maxKmh,
             avg, (unsigned long)s_points, APP_NAME, FW_VERSION);
    s_sd->auxLine(f, b);
    s_sd->auxClose(f);
  }
  Settings::setTripPath("");
  Serial.printf("[TRIP] saved %s: %lu s, %.3f km, %lu points\n", s_base.c_str(), (unsigned long)dur, km, (unsigned long)s_points);
}

bool TripRecorder::recording() { return s_rec; }
bool TripRecorder::saving() { return s_sd && s_sd->auxPending(); }
bool TripRecorder::waitingForFix() { return s_rec && s_noFix; }
uint32_t TripRecorder::durationS() { return s_rec ? elapsedS() : 0; }
uint32_t TripRecorder::movingS() { return s_movingS; }
float TripRecorder::distanceKm() { return s_distM / 1000.0; }
float TripRecorder::maxKmh() { return s_maxKmh; }
float TripRecorder::currentKmh() { return s_curKmh; }
uint32_t TripRecorder::points() { return s_points; }
String TripRecorder::name() { return s_name; }

int TripRecorder::track(const String &base, float *lat, float *lon, int maxPts) {
  if (!s_sd || !s_sd->mounted() || maxPts < 2) return 0;
  const String path = base + ".csv";
  const int32_t size = s_sd->fileSize(path.c_str());
  if (size <= 0 || size > 4 * 1024 * 1024) return 0;
  char *buf = (char *)heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
  if (!buf) return 0;
  int32_t got = 0;
  while (got < size) {
    const int r = s_sd->readChunk(path.c_str(), got, (uint8_t *)buf + got, min<int32_t>(4096, size - got));
    if (r <= 0) break;
    got += r;
  }
  buf[got] = 0;
  // Lines: t_s,utc,lat,lon,... (first line = header). Count them, then take every k-th.
  int lines = 0;
  for (int32_t i = 0; i < got; i++) lines += buf[i] == '\n';
  const int step = max(1, (lines + maxPts - 1) / maxPts);
  int n = 0, line = 0;
  for (char *p = buf; p && *p && n < maxPts; line++) {
    char *next = strchr(p, '\n');
    if (next) *next++ = 0;
    if (line > 0 && (line - 1) % step == 0) {
      const char *f2 = strchr(p, ',');
      const char *f3 = f2 ? strchr(f2 + 1, ',') : nullptr;
      const char *f4 = f3 ? strchr(f3 + 1, ',') : nullptr;
      if (f3 && f4) { lat[n] = atof(f3 + 1); lon[n] = atof(f4 + 1); if (lat[n] != 0 || lon[n] != 0) n++; }
    }
    p = next;
  }
  heap_caps_free(buf);
  return n;
}

int TripRecorder::list(Summary *out, int max) {
  if (!s_sd || !s_sd->mounted()) return 0;
  String dirs[12];
  const int nd = s_sd->listNames("/data/trips", dirs, 12, true);
  String files[40];
  int nf = 0;
  for (int i = nd - 1; i >= 0 && nf < 40; i--) {                  // newest year first
    String names[40];
    const int n = s_sd->listNames(("/data/trips/" + dirs[i]).c_str(), names, 40);
    for (int k = n - 1; k >= 0 && nf < 40; k--)
      if (names[k].endsWith(".json")) files[nf++] = "/data/trips/" + dirs[i] + "/" + names[k];
  }
  int count = 0;
  for (int i = 0; i < nf && count < max; i++) {
    String json;
    if (!s_sd->readFile(files[i].c_str(), json, 600)) continue;
    Summary &s = out[count++];
    s.base = files[i].substring(0, files[i].length() - 5);          // without ".json"
    s.name = jsonValue(json, "name");
    s.title = jsonValue(json, "title");
    s.durationS = jsonValue(json, "duration_s").toInt();
    s.movingS = jsonValue(json, "moving_s").toInt();
    s.points = jsonValue(json, "points").toInt();
    s.distanceKm = jsonValue(json, "distance_km").toFloat();
    s.maxKmh = jsonValue(json, "max_kmh").toFloat();
    s.avgKmh = jsonValue(json, "avg_kmh").toFloat();
  }
  return count;
}
