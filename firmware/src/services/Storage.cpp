#include "Storage.h"

#include "../SdLog.h"

namespace {
SdLog *s_sd = nullptr;
}

void Storage::begin(SdLog &sd) { s_sd = &sd; }
bool Storage::cardPresent() { return s_sd && s_sd->mounted(); }
uint32_t Storage::cardGB() { return cardPresent() ? (uint32_t)((s_sd->cardMB() + 512) / 1024) : 0; }
bool Storage::recording() { return s_sd && s_sd->logging(); }

String Storage::summary() {
  if (!cardPresent()) return "No SD card";
  uint64_t total = 0, freeB = 0;
  String s;
  if (usage(total, freeB) && total) s = String((double)total / 1073741824.0, 1) + " GB card";   // the real volume (the card reports a faked size)
  else s = String(cardGB()) + " GB card";
  s += recording() ? ", recording" : ", not recording";
  if (s_sd->writeErrors() || s_sd->droppedBytes()) s += " (errors)";
  return s;
}

int Storage::list(const char *dir, Entry *out, int max, int *total) { return cardPresent() ? s_sd->listDir(dir, out, max, total) : -1; }
bool Storage::usage(uint64_t &totalBytes, uint64_t &freeBytes) { return cardPresent() && s_sd->usage(totalBytes, freeBytes); }
bool Storage::inUse(const char *path) { return !cardPresent() || s_sd->inUse(path); }
bool Storage::removeStart(const char *path) { return cardPresent() && s_sd->removeStart(path); }
bool Storage::exists(const char *path) { return cardPresent() && s_sd->exists(path); }
bool Storage::removing() { return cardPresent() && s_sd->removing(); }
uint32_t Storage::removedCount() { return s_sd ? s_sd->removedCount() : 0; }
bool Storage::removeFailed() { return s_sd && s_sd->removeFailed(); }
int Storage::read(const char *path, uint32_t offset, uint8_t *buf, size_t n) { return cardPresent() ? s_sd->readChunk(path, offset, buf, n) : -1; }
int Storage::readMap(const char *path, uint32_t offset, uint8_t *buf, size_t n, uint32_t *size) { return cardPresent() ? s_sd->readAt(path, offset, buf, n, size, 1) : -1; }
void Storage::readMapClose() { if (s_sd) s_sd->readerClose(1); }

bool Storage::statStart(const char *const *paths, int n) { return cardPresent() && s_sd->statStart(paths, n); }
bool Storage::statBusy() { return cardPresent() && s_sd->statBusy(); }
Storage::TreeStat Storage::stat(int i) { return s_sd ? s_sd->stat(i) : TreeStat{0, 0}; }
bool Storage::removeOldLogsStart(int keepSessions) { return cardPresent() && s_sd->removeOldSessionsStart("/GPSLOG", keepSessions); }
