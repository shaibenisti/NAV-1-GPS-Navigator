#include "Places.h"

#include <esp_heap_caps.h>
#include "../SdLog.h"

namespace {

constexpr const char *FILE_PATH = "/data/places.json";
constexpr const char *BAD_PATH = "/data/places.bad";
constexpr uint32_t WRITE_DELAY_MS = 1000;
constexpr size_t FILE_MAX = 16384;

SdLog *s_sd = nullptr;
Places::Place *s_list = nullptr;       // PSRAM, CAPACITY entries
int s_n = 0;
uint32_t s_rev = 1;
bool s_dirty = false, s_pending = false;
uint32_t s_dirtyMs = 0;
String s_written;                      // content of the file as last written / read
String s_status = "no SD card";

void changed() { s_rev++; s_dirty = true; s_dirtyMs = millis(); }

// ---- JSON: the writer, and a reader for this file's shape (an array of flat objects) -------------
void appendString(String &o, const char *s) {
  o += '"';
  for (; *s; s++) {
    const unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  o += '"';
}

String build(bool pretty) {
  String o = pretty ? "{\"places\": [\n" : "[";
  for (int i = 0; i < s_n; i++) {
    const Places::Place &p = s_list[i];
    o += pretty ? "  {\"name\": " : "{\"name\":";
    appendString(o, p.name);
    o += pretty ? ", \"lat\": " : ",\"lat\":";
    o += String(p.lat, 7);
    o += pretty ? ", \"lon\": " : ",\"lon\":";
    o += String(p.lon, 7);
    o += '}';
    if (i + 1 < s_n) o += ',';
    if (pretty) o += '\n';
  }
  o += pretty ? "]}\n" : "]";
  return o;
}

struct Reader {
  const char *p, *e;
  void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; }
  bool eat(char c) { ws(); if (p < e && *p == c) { p++; return true; } return false; }
  static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
  bool u4(uint32_t &v) {
    if (e - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; i++) { const int h = hex(p[i]); if (h < 0) return false; v = v * 16 + h; }
    p += 4;
    return true;
  }
  // a string into out (UTF-8, cut at cap - 1 bytes on a character boundary)
  bool str(char *out, size_t cap) {
    ws();
    if (p >= e || *p != '"') return false;
    p++;
    size_t o = 0;
    auto put = [&](uint32_t v) {
      char b[4];
      int l;
      if (v < 0x80) { b[0] = (char)v; l = 1; }
      else if (v < 0x800) { b[0] = (char)(0xC0 | (v >> 6)); b[1] = (char)(0x80 | (v & 0x3F)); l = 2; }
      else if (v < 0x10000) { b[0] = (char)(0xE0 | (v >> 12)); b[1] = (char)(0x80 | ((v >> 6) & 0x3F)); b[2] = (char)(0x80 | (v & 0x3F)); l = 3; }
      else { b[0] = (char)(0xF0 | (v >> 18)); b[1] = (char)(0x80 | ((v >> 12) & 0x3F)); b[2] = (char)(0x80 | ((v >> 6) & 0x3F)); b[3] = (char)(0x80 | (v & 0x3F)); l = 4; }
      if (out && o + l < cap) { memcpy(out + o, b, l); o += l; }
    };
    while (p < e && *p != '"') {
      if (*p != '\\') {                                        // raw UTF-8 bytes: copy a whole character at once
        const unsigned char c = (unsigned char)*p;
        const int l = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (p + l > e) return false;
        if (out && o + l < cap) { memcpy(out + o, p, l); o += l; }
        p += l;
        continue;
      }
      if (++p >= e) return false;
      const char c = *p++;
      uint32_t v;
      switch (c) {
        case 'n': put('\n'); break;
        case 't': put('\t'); break;
        case 'r': put('\r'); break;
        case 'b': put('\b'); break;
        case 'f': put('\f'); break;
        case 'u':
          if (!u4(v)) return false;
          if (v >= 0xD800 && v < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {   // surrogate pair
            p += 2;
            uint32_t lo;
            if (!u4(lo)) return false;
            v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
          }
          put(v);
          break;
        default: put((unsigned char)c); break;              // \" \\ \/
      }
    }
    if (p >= e) return false;
    p++;
    if (out) out[o] = 0;
    return true;
  }
  bool skipValue() {
    ws();
    if (p < e && *p == '"') return str(nullptr, 0);
    int depth = 0;                                           // a number, literal, or a nested object / array
    while (p < e) {
      const char c = *p;
      if (c == '"') { if (!str(nullptr, 0)) return false; continue; }
      if (c == '{' || c == '[') depth++;
      else if (c == '}' || c == ']') { if (depth == 0) return true; depth--; }
      else if (c == ',' && depth == 0) return true;
      p++;
    }
    return false;
  }
  bool number(double &v) {
    ws();
    char *end = nullptr;
    char b[32];
    const size_t n = min<size_t>(sizeof(b) - 1, e - p);
    memcpy(b, p, n);
    b[n] = 0;
    v = strtod(b, &end);
    if (end == b) return false;
    p += end - b;
    return true;
  }
};

// -1 = not this file's format (kept as places.bad), else the number of places read
int parse(const String &j) {
  Reader r{ j.c_str(), j.c_str() + j.length() };
  const char *k = strstr(r.p, "\"places\"");
  if (!k) return -1;
  r.p = k + 8;
  if (!r.eat(':') || !r.eat('[')) return -1;
  int n = 0;
  if (r.eat(']')) return 0;
  for (;;) {
    if (!r.eat('{')) return -1;
    Places::Place pl = {};
    bool haveName = false, haveLat = false, haveLon = false;
    if (!r.eat('}')) {
      for (;;) {
        char key[16];
        if (!r.str(key, sizeof(key)) || !r.eat(':')) return -1;
        if (!strcmp(key, "name")) { if (!r.str(pl.name, sizeof(pl.name))) return -1; haveName = true; }
        else if (!strcmp(key, "lat")) { if (!r.number(pl.lat)) return -1; haveLat = true; }
        else if (!strcmp(key, "lon")) { if (!r.number(pl.lon)) return -1; haveLon = true; }
        else if (!r.skipValue()) return -1;
        if (r.eat(',')) continue;
        if (r.eat('}')) break;
        return -1;
      }
    }
    if (haveName && haveLat && haveLon && n < Places::CAPACITY && Places::validName(pl.name) &&
        fabs(pl.lat) <= 90 && fabs(pl.lon) <= 180) s_list[n++] = pl;
    if (r.eat(',')) continue;
    if (r.eat(']')) break;
    return -1;
  }
  s_n = n;
  return n;
}

}  // namespace

bool Places::validName(const char *name) {
  if (!name) return false;
  const size_t len = strlen(name);
  if (len < 1 || len > NAME_BYTES) return false;
  bool ink = false;
  for (size_t i = 0; i < len;) {
    const unsigned char c = (unsigned char)name[i];
    const int l = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (!l || i + l > len || c < 0x20 || c == 0x7F) return false;
    for (int k = 1; k < l; k++) if ((name[i + k] & 0xC0) != 0x80) return false;
    if (c != ' ') ink = true;
    i += l;
  }
  return ink;
}

void Places::begin(SdLog &sd) {
  if (!s_list) s_list = (Place *)heap_caps_calloc(CAPACITY, sizeof(Place), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_list || !sd.mounted()) return;
  s_sd = &sd;
  String j;
  if (sd.readFile(FILE_PATH, j, FILE_MAX) && j.length()) {
    const int n = parse(j);
    if (n < 0) {
      s_n = 0;
      sd.rename(FILE_PATH, BAD_PATH);
      s_status = String(FILE_PATH) + " unreadable (kept as places.bad)";
    } else {
      s_status = String(FILE_PATH) + ": " + n + (n == 1 ? " place" : " places");
      s_written = j;
      if (build(true) != j) changed();                      // normalise a hand-edited file
    }
  } else {
    s_status = String(FILE_PATH) + ": no places yet";
  }
  s_rev++;
  Serial.printf("[PLACES] %s\n", s_status.c_str());
}

void Places::update() {
  if (!s_sd) return;
  if (s_pending && !s_sd->writingAsync()) {
    s_pending = false;
    if (s_sd->asyncWriteFailed()) { s_status = "cannot write " + String(FILE_PATH); s_written = ""; s_dirty = true; s_dirtyMs = millis(); }
  }
  if (!s_dirty || millis() - s_dirtyMs < WRITE_DELAY_MS || s_sd->writingAsync()) return;
  const String j = build(true);
  if (j == s_written) { s_dirty = false; return; }
  if (s_sd->writeFileAsync(FILE_PATH, j)) {
    s_dirty = false;
    s_written = j;
    s_pending = true;
    s_status = String(FILE_PATH) + ": " + s_n + (s_n == 1 ? " place" : " places");
  }
}

int Places::count() { return s_n; }

bool Places::get(int i, Place &out) {
  if (i < 0 || i >= s_n) return false;
  out = s_list[i];
  return true;
}

int Places::add(const char *name, double lat, double lon) {
  if (!s_list || s_n >= CAPACITY || !validName(name) || fabs(lat) > 90 || fabs(lon) > 180 || (lat == 0 && lon == 0)) return -1;
  Place &p = s_list[s_n];
  strlcpy(p.name, name, sizeof(p.name));
  p.lat = lat;
  p.lon = lon;
  changed();
  return s_n++;
}

bool Places::rename(int i, const char *name) {
  if (i < 0 || i >= s_n || !validName(name)) return false;
  strlcpy(s_list[i].name, name, sizeof(s_list[i].name));
  changed();
  return true;
}

bool Places::remove(int i) {
  if (i < 0 || i >= s_n) return false;
  memmove(&s_list[i], &s_list[i + 1], sizeof(Place) * (s_n - i - 1));
  s_n--;
  changed();
  return true;
}

int Places::find(const char *name) {
  for (int i = 0; i < s_n; i++) if (!strcmp(s_list[i].name, name)) return i;
  return -1;
}

String Places::defaultName() {
  for (int k = 1;; k++) {
    char b[24];
    snprintf(b, sizeof(b), "Place %d", k);
    if (find(b) < 0) return b;
  }
}

uint32_t Places::revision() { return s_rev; }
String Places::json() { return build(false); }
String Places::status() { return s_status; }

bool Places::fileInSync() {
  if (!s_sd) return false;
  String j;
  if (s_dirty || s_sd->writingAsync()) return false;
  if (!s_sd->readFile(FILE_PATH, j, FILE_MAX)) return s_n == 0;   // no file: right only while there is nothing to save
  return j == build(true);
}
