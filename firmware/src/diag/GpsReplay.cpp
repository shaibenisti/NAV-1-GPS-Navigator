#include "GpsReplay.h"

#include "../SdLog.h"
#include "../GpsLink.h"
#include "../GpsParser.h"
#include "../services/Location.h"

namespace {

SdLog *s_sd = nullptr;
GpsLink *s_link = nullptr;
GpsParser *s_parser = nullptr;
bool s_active = false;
String s_path;
int s_speed = 1;
uint32_t s_offset = 0, s_waitUntil = 0;
uint8_t s_buf[1024];
size_t s_bufLen = 0, s_bufPos = 0;
char s_line[120];
size_t s_lineLen = 0;
uint32_t s_sentences = 0, s_bad = 0, s_epochs = 0, s_fixEpochs = 0, s_truncated = 0, s_restarts = 0;
char s_lastTime[12] = "?";

bool checksumOk(const char *s) {
  if (s[0] != '$') return false;
  const char *star = strchr(s, '*');
  if (!star || strlen(star) < 3) return false;
  uint8_t x = 0;
  for (const char *p = s + 1; p < star; p++) x ^= (uint8_t)*p;
  return x == (uint8_t)strtol(star + 1, nullptr, 16);
}

// Next complete line into s_line; false at end of file.
bool nextLine() {
  for (;;) {
    if (s_bufPos >= s_bufLen) {
      const int got = s_sd->readChunk(s_path.c_str(), s_offset, s_buf, sizeof(s_buf));
      if (got <= 0) return false;
      s_offset += got;
      s_bufLen = got;
      s_bufPos = 0;
    }
    const char c = (char)s_buf[s_bufPos++];
    if (c == '\r') continue;
    if (c == '$' && s_lineLen > 0) { s_truncated++; s_lineLen = 0; }   // cut-off sentence: resync like GpsLink
    if (c == '\n') {
      s_line[s_lineLen] = 0;
      const bool any = s_lineLen > 0;
      s_lineLen = 0;
      if (any) return true;
      continue;
    }
    if (s_lineLen < sizeof(s_line) - 1) s_line[s_lineLen++] = c;
    else s_lineLen = 0;                                   // overlong: drop it
  }
}

}  // namespace

void GpsReplay::begin(SdLog &sd, GpsLink &link, GpsParser &parser) {
  s_sd = &sd;
  s_link = &link;
  s_parser = &parser;
}

bool GpsReplay::start(const char *path, int speed, String *error) {
  if (!s_sd || !s_sd->exists(path)) { if (error) *error = "file not found"; return false; }
  stop();
  s_path = path;
  s_speed = constrain(speed, 1, 20);
  s_offset = s_bufLen = s_bufPos = s_lineLen = 0;
  s_waitUntil = 0;
  s_sentences = s_bad = s_epochs = s_fixEpochs = s_truncated = s_restarts = 0;
  s_link->setSentenceHandler(nullptr, nullptr);          // live receiver detached
  Location::setReplay(true);
  s_active = true;
  Serial.printf("[REPLAY] %s at %dx\n", path, s_speed);
  return true;
}

void GpsReplay::stop() {
  if (!s_active) return;
  s_active = false;
  s_parser->attach(*s_link);                             // live receiver back
  Location::setReplay(false);
  Serial.printf("[REPLAY] stopped: %lu epochs (%lu with fix), %lu sentences, %lu bad, %lu truncated, %lu GPS module restarts\n",
                (unsigned long)s_epochs, (unsigned long)s_fixEpochs, (unsigned long)s_sentences, (unsigned long)s_bad,
                (unsigned long)s_truncated, (unsigned long)s_restarts);
}

void GpsReplay::update() {
  if (!s_active || (s_waitUntil && (int32_t)(millis() - s_waitUntil) < 0)) return;
  s_waitUntil = 0;
  for (int n = 0; n < 60; n++) {                         // bounded work per loop
    if (!nextLine()) {
      Serial.println("[REPLAY] end of file");
      stop();
      return;
    }
    if (!checksumOk(s_line)) {
      if (++s_bad <= 3) Serial.printf("[REPLAY] bad line at offset ~%lu: %.80s\n", (unsigned long)s_offset, s_line);
      continue;
    }
    s_parser->feed(s_line);
    s_sentences++;
    if (!strncmp(s_line + 3, "TXT", 3) && strstr(s_line, "u-blox AG") && s_sentences > 20) {
      s_restarts++;
      Serial.printf("[REPLAY] GPS module restart in the recording after %s UTC (last fix epoch count %lu)\n", s_lastTime,
                    (unsigned long)s_fixEpochs);
    }
    if (strlen(s_line) > 6 && !strncmp(s_line + 3, "RMC", 3)) {      // one navigation epoch
      s_epochs++;
      const char *tf = strchr(s_line, ',');                          // hhmmss.ss of this epoch
      if (tf && strlen(tf) > 7 && isdigit((unsigned char)tf[1])) snprintf(s_lastTime, sizeof(s_lastTime), "%.2s:%.2s:%.2s", tf + 1, tf + 3, tf + 5);
      const char *f = s_line;
      for (int i = 0; i < 2 && f; i++) { f = strchr(f, ','); if (f) f++; }
      if (f && *f == 'A') s_fixEpochs++;
      s_waitUntil = millis() + 1000 / s_speed;
      return;
    }
  }
}

bool GpsReplay::active() { return s_active; }

void GpsReplay::status(Print &out) {
  if (!s_active) { out.println("[REPLAY] off (live receiver)"); return; }
  out.printf("[REPLAY] %s at %dx: offset %lu, %lu epochs (%lu with fix), %lu sentences, %lu bad\n", s_path.c_str(), s_speed,
             (unsigned long)s_offset, (unsigned long)s_epochs, (unsigned long)s_fixEpochs, (unsigned long)s_sentences,
             (unsigned long)s_bad);
}
