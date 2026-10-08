// =============================================================================
//  Storage  -  read access to the SD card state for apps (the start of the
//  StorageService: SdLog stays the implementation).
// =============================================================================
#pragma once

#include <Arduino.h>

#include "../SdLog.h"

namespace Storage {
  void begin(SdLog &sd);
  bool cardPresent();
  uint32_t cardGB();
  bool recording();                      // GPS session being written
  String summary();                      // short user text, e.g. "256 GB card, recording"

  // Files app (file manager); all false/-1 without a card
  using Entry = SdLog::DirEntry;
  int list(const char *dir, Entry *out, int max, int *total = nullptr);   // unsorted, -1 = no folder
  bool usage(uint64_t &totalBytes, uint64_t &freeBytes);
  bool inUse(const char *path);          // being written (GPS session, trip)
  bool exists(const char *path);
  bool removeStart(const char *path);    // background delete (file or folder tree)
  bool removing();
  uint32_t removedCount();
  bool removeFailed();
  int read(const char *path, uint32_t offset, uint8_t *buf, size_t n);   // bytes, -1 = no file
  int readMap(const char *path, uint32_t offset, uint8_t *buf, size_t n, uint32_t *size = nullptr);   // Map app: big file kept open (own slot)
  void readMapClose();

  // Storage app
  using TreeStat = SdLog::TreeStat;
  bool statStart(const char *const *paths, int n);   // folder sizes in the background
  bool statBusy();
  TreeStat stat(int i);
  bool removeOldLogsStart(int keepSessions);          // /GPSLOG: keep the newest N sessions
}
