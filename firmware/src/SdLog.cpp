#include "SdLog.h"

#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>
#include <dirent.h>
#include <ff.h>
#include <diskio.h>
#include <algorithm>

// ---- mount ------------------------------------------------------------------

bool SdLog::begin(const Pins &pins, uint32_t spiHz, int maxAttempts) {
  _pins = pins;
  for (_attempts = 1; _attempts <= maxAttempts; _attempts++) {
    SPI.begin(_pins.sck, _pins.miso, _pins.mosi, _pins.cs);
    // 10 open files: session CSV+NMEA, field test (2), trip (2), summaries, replay, list/dump.
    // The default 5 ran out with a trip + replay + the Trips list (2026-09-28).
    if (SD.begin(_pins.cs, SPI, spiHz, "/sd", 10) && SD.cardType() != CARD_NONE) {
      _mounted = true;
      _cardMB = SD.cardSize() / (1024ULL * 1024ULL);
      return true;
    }
    SD.end();
    SPI.end();
    if (_attempts < maxAttempts) {       // recover, then try again (never wake without a retry)
      _wakeR1 = wakeUpCard();
      _wakeUsed = true;
    }
  }
  _attempts = maxAttempts;
  return false;
}

// SD recovery, bit-banged at ~25 kHz (no SPI peripheral involved):
//  1. with CS low, clock until the card releases MISO (0xFF): finishes a read or
//     write that an ESP reset interrupted - the card keeps power across resets;
//  2. >= 74 clocks with CS high;
//  3. CMD0 (GO_IDLE_STATE) until the card answers R1 = 0x01, up to 8 tries.
// Returns the last R1 byte seen (0x01 = card is idle and ready for SD.begin()).
uint8_t SdLog::wakeUpCard() {
  pinMode(_pins.cs, OUTPUT);   digitalWrite(_pins.cs, HIGH);
  pinMode(_pins.mosi, OUTPUT); digitalWrite(_pins.mosi, HIGH);
  pinMode(_pins.sck, OUTPUT);  digitalWrite(_pins.sck, LOW);
  pinMode(_pins.miso, INPUT_PULLUP);

  auto xfer = [this](uint8_t out) -> uint8_t {
    uint8_t in = 0;
    for (int b = 7; b >= 0; b--) {
      digitalWrite(_pins.mosi, (out >> b) & 1);
      delayMicroseconds(20);
      digitalWrite(_pins.sck, HIGH);
      in = (in << 1) | digitalRead(_pins.miso);
      delayMicroseconds(20);
      digitalWrite(_pins.sck, LOW);
    }
    return in;
  };

  digitalWrite(_pins.cs, LOW);
  for (int i = 0; i < 2000 && xfer(0xFF) != 0xFF; i++) {}   // drain busy / pending data (<= ~0.7 s)
  // A reset in the middle of a multi-block WRITE leaves the card receiving data: it takes
  // CMD0 as block bytes and never answers (R1 0xFF after many rapid resets, 2026-09-29).
  // Finish that: stop-tran token, then enough 0xFF to complete a 512-byte block + CRC, wait
  // while it programs (busy = 0x00), then CMD12 for an interrupted multi-block READ.
  xfer(0xFD);
  for (int i = 0; i < 520; i++) xfer(0xFF);
  for (int i = 0; i < 4000 && xfer(0xFF) == 0x00; i++) {}  // busy (<= ~1.3 s)
  const uint8_t cmd12[] = { 0x4C, 0, 0, 0, 0, 0x61 };
  for (uint8_t b : cmd12) xfer(b);
  for (int i = 0; i < 16; i++) xfer(0xFF);
  for (int i = 0; i < 4000 && xfer(0xFF) == 0x00; i++) {}
  digitalWrite(_pins.cs, HIGH);

  uint8_t r1 = 0xFF;
  for (int attempt = 0; attempt < 8 && r1 != 0x01; attempt++) {
    for (int i = 0; i < 10; i++) xfer(0xFF);                 // 80 clocks, CS high
    digitalWrite(_pins.cs, LOW);
    const uint8_t cmd0[] = { 0x40, 0, 0, 0, 0, 0x95 };
    for (uint8_t b : cmd0) xfer(b);
    for (int i = 0; i < 16; i++) {                           // R1 arrives within 8 bytes
      r1 = xfer(0xFF);
      if (r1 != 0xFF) break;
    }
    digitalWrite(_pins.cs, HIGH);
    xfer(0xFF);
  }
  delay(10);
  return r1;
}
// ---- session files ----------------------------------------------------------

bool SdLog::openSession(const char *dir) {
  if (!_mounted) return false;
  if (!SD.exists(dir) && !SD.mkdir(dir)) { _errors++; return false; }

  // Next free session number = highest existing Snnnn + 1, from ONE pass over the directory.
  // (Probing S0001, S0002, ... with SD.exists() took ~15 ms per existing session: 2.9 s of the
  // boot at 178 sessions, growing with every boot - found 2026-09-28.)
  // POSIX readdir() only reads directory entries (File::openNextFile() opens every file: slower).
  int maxN = 0;
  if (DIR *d = opendir((String("/sd") + dir).c_str())) {
    while (struct dirent *e = readdir(d)) {
      if (e->d_name[0] == 'S' && isdigit((unsigned char)e->d_name[1])) maxN = max(maxN, atoi(e->d_name + 1));
    }
    closedir(d);
  }

  char csvPath[32], nmeaPath[32];
  for (int n = maxN + 1; n <= 9999; n++) {
    snprintf(csvPath, sizeof(csvPath), "%s/S%04d.CSV", dir, n);
    if (SD.exists(csvPath)) continue;                  // (normally the first try)
    snprintf(nmeaPath, sizeof(nmeaPath), "%s/S%04d.NMEA", dir, n);
    snprintf(_session, sizeof(_session), "S%04d", n);
    _csv  = SD.open(csvPath, FILE_WRITE);
    _nmea = SD.open(nmeaPath, FILE_WRITE);
    if (!_csv || !_nmea) { _errors++; return false; }
    _lastFlush = millis();
    return true;
  }
  _errors++;
  return false;
}

void SdLog::line(const char *text) {
  if (_writer) {                               // same bytes as println(): text + CR LF
    const size_t n = strlen(text);
    if (xStreamBufferSpacesAvailable(_csvStream) < n + 2) { _dropped += n + 2; return; }
    xStreamBufferSend(_csvStream, text, n, 0);
    xStreamBufferSend(_csvStream, "\r\n", 2, 0);
    _csvLines++;
    return;
  }
  if (!_csv) return;
  if (_csv.println(text) == 0) { _errors++; return; }
  _csvLines++;
}

size_t SdLog::NmeaSink::write(uint8_t c) {
  if (!_owner._nmea) return 0;
  _buf[_len++] = c;
  if (_len == sizeof(_buf)) drain();
  return 1;
}

void SdLog::NmeaSink::drain() {
  if (_len == 0) return;
  if (_owner._writer) {                        // hand over to the writer task, never block
    const size_t sent = xStreamBufferSend(_owner._nmeaStream, _buf, _len, 0);
    if (sent < _len) _owner._dropped += _len - sent;
    _len = 0;
    return;
  }
  if (!_owner._nmea) return;
  _owner.writeNmea(_buf, _len);
  _len = 0;
}

void SdLog::writeNmea(const uint8_t *data, size_t n) {
  if (_nmea.write(data, n) != n) _errors++;
  else _nmeaBytes += n;
}

void SdLog::writeCsv(const uint8_t *data, size_t n) {
  if (_csv.write(data, n) != n) _errors++;
}

void SdLog::flushAll() {
  _nmeaSink.drain();
  lockCard();
  if (_csv) _csv.flush();
  if (_nmea) _nmea.flush();
  for (File &f : _aux) if (f) f.flush();
  unlockCard();
}

// ---- auxiliary files -------------------------------------------------------------
// Queue record: [slot][len lo][len hi][len bytes]; len 0xFFFF = close the slot.

bool SdLog::mkdirs(const char *dir) {
  if (!_mounted) return false;
  char path[96];
  strlcpy(path, dir, sizeof(path));
  bool ok = true;
  lockCard();
  for (char *p = path + 1; ; p++) {
    if (*p == '/' || *p == 0) {
      const char c = *p;
      *p = 0;
      if (!SD.exists(path) && !SD.mkdir(path)) ok = false;
      *p = c;
      if (!c) break;
    }
  }
  unlockCard();
  return ok;
}

bool SdLog::exists(const char *path) {
  if (!_mounted) return false;
  lockCard();
  const bool e = SD.exists(path);
  unlockCard();
  return e;
}

bool SdLog::rename(const char *from, const char *to) {
  if (!_mounted) return false;
  lockCard();
  const bool ok = SD.rename(from, to);
  unlockCard();
  return ok;
}

bool SdLog::writeFile(const char *path, const char *text) { return writeFile(path, (const uint8_t *)text, strlen(text)); }

bool SdLog::writeFile(const char *path, const uint8_t *data, size_t n) {
  if (!_mounted) return false;
  lockCard();
  File f = SD.open(path, FILE_WRITE);
  const bool ok = f && f.write(data, n) == n;
  if (f) f.close();
  unlockCard();
  return ok;
}

bool SdLog::readFile(const char *path, String &out, size_t maxBytes) {
  out = "";
  if (!_mounted) return false;
  lockCard();
  File f = SD.open(path, FILE_READ);
  const bool ok = (bool)f;
  while (f && f.available() && out.length() < maxBytes) out += (char)f.read();
  if (f) f.close();
  unlockCard();
  return ok;
}

bool SdLog::readTail(const char *path, String &out, size_t bytes) {
  out = "";
  if (!_mounted) return false;
  lockCard();
  File f = SD.open(path, FILE_READ);
  const bool ok = (bool)f;
  if (f) {
    const size_t size = f.size();
    f.seek(size > bytes ? size - bytes : 0);
    while (f.available()) out += (char)f.read();
    f.close();
  }
  unlockCard();
  return ok;
}

int SdLog::readChunk(const char *path, uint32_t offset, uint8_t *buf, size_t n) {
  if (!_mounted) return -1;
  lockCard();
  File f = SD.open(path, FILE_READ);
  int got = -1;
  if (f) {
    got = 0;
    if (f.seek(offset)) got = f.read(buf, n);
    f.close();
  }
  unlockCard();
  return got;
}

int SdLog::readAt(const char *path, uint32_t offset, uint8_t *buf, size_t n, uint32_t *size, int slot) {
  if (!_mounted) return -1;
  slot &= 1;
  File &rd = _reader[slot];
  lockCard();
  if (!rd || _readerPath[slot] != path) {
    if (rd) rd.close();
    rd = SD.open(path, FILE_READ);
    _readerPath[slot] = rd ? path : "";
  }
  int got = -1;
  if (rd && !rd.isDirectory()) {
    if (size) *size = rd.size();
    got = n && (rd.position() == offset || rd.seek(offset)) ? rd.read(buf, n) : 0;
  }
  unlockCard();
  return got;
}

void SdLog::readerClose(int slot) {
  slot &= 1;
  if (!_reader[slot]) return;
  lockCard();
  _reader[slot].close();
  _readerPath[slot] = "";
  unlockCard();
}

int32_t SdLog::fileSize(const char *path) {
  if (!_mounted) return -1;
  lockCard();
  File f = SD.open(path, FILE_READ);
  const int32_t size = f && !f.isDirectory() ? (int32_t)f.size() : -1;
  if (f) f.close();
  unlockCard();
  return size;
}

int SdLog::listNames(const char *dir, String *names, int max, bool dirs) {
  if (!_mounted) return 0;
  int n = 0;
  lockCard();
  File d = SD.open(dir);
  if (d && d.isDirectory()) {
    for (File f = d.openNextFile(); f && n < max; f = d.openNextFile()) {
      if (f.isDirectory() == dirs) {
        String name = f.name();
        const int slash = name.lastIndexOf('/');
        names[n++] = slash >= 0 ? name.substring(slash + 1) : name;
      }
    }
  }
  unlockCard();
  for (int a = 1; a < n; a++)                    // insertion sort by name
    for (int b = a; b > 0 && names[b] < names[b - 1]; b--) { String t = names[b]; names[b] = names[b - 1]; names[b - 1] = t; }
  return n;
}

int SdLog::auxOpen(const char *path) {
  if (!_mounted) return -1;
  if (_rmBusy) {                                 // never create a file inside a folder being deleted
    const size_t n = strlen(_rmPath);
    if (!strncmp(path, _rmPath, n) && (path[n] == 0 || path[n] == '/')) return -1;
  }
  const size_t plen = strlen(path);
  if (plen >= sizeof(_auxPath[0])) return -1;
  int slot = -1;
  for (int i = 0; i < AUX_FILES && slot < 0; i++) if (!_auxUsed[i]) slot = i;
  if (slot < 0) return -1;
  if (_writer) {                                 // queued: the writer task opens it (in order)
    uint8_t rec[4 + sizeof(_auxPath[0])];
    rec[0] = (uint8_t)slot; rec[1] = 0xFE; rec[2] = 0xFF; rec[3] = (uint8_t)plen;
    memcpy(rec + 4, path, plen);
    if (xStreamBufferSpacesAvailable(_auxStream) < plen + 4) { _errors++; return -1; }
    _auxUsed[slot] = true;
    _auxFailed[slot] = false;
    strlcpy(_auxPath[slot], path, sizeof(_auxPath[slot]));
    xStreamBufferSend(_auxStream, rec, plen + 4, 0);
    return slot;
  }
  lockCard();
  _aux[slot] = SD.open(path, FILE_APPEND);
  const bool ok = (bool)_aux[slot];
  unlockCard();
  if (!ok) { _errors++; return -1; }
  _auxUsed[slot] = true;
  strlcpy(_auxPath[slot], path, sizeof(_auxPath[slot]));
  return slot;
}

bool SdLog::auxLine(int slot, const char *text) {
  if (slot < 0 || slot >= AUX_FILES) return false;
  const size_t n = strlen(text);
  if (n > 500) { _dropped += n; return false; }
  if (!_writer) {                                // direct mode (field-test firmware mode)
    lockCard();
    const bool ok = _aux[slot] && _aux[slot].print(text) == n && _aux[slot].print("\r\n") == 2;
    unlockCard();
    if (!ok) _errors++;
    return ok;
  }
  uint8_t rec[3 + 502];
  rec[0] = (uint8_t)slot;
  rec[1] = (uint8_t)((n + 2) & 0xFF);
  rec[2] = (uint8_t)((n + 2) >> 8);
  memcpy(rec + 3, text, n);
  rec[3 + n] = '\r';
  rec[4 + n] = '\n';
  if (xStreamBufferSpacesAvailable(_auxStream) < n + 5) { _dropped += n + 2; return false; }
  xStreamBufferSend(_auxStream, rec, n + 5, 0);  // one send: the writer sees the whole record
  return true;
}

void SdLog::auxClose(int slot) {
  if (slot < 0 || slot >= AUX_FILES) return;
  if (!_auxUsed[slot]) return;
  _auxUsed[slot] = false;                        // reusable at once: its close is queued before any new open
  _auxPath[slot][0] = 0;
  if (_writer) {                                 // queued: closed after its lines, without waiting here
    const uint8_t rec[3] = { (uint8_t)slot, 0xFF, 0xFF };
    while (xStreamBufferSpacesAvailable(_auxStream) < 3) delay(1);
    xStreamBufferSend(_auxStream, rec, 3, 0);
    return;
  }
  lockCard();
  if (_aux[slot]) { _aux[slot].flush(); _aux[slot].close(); }
  unlockCard();
}

bool SdLog::auxFlushWait(uint32_t ms) {
  if (!_writer) return true;
  const uint32_t t0 = millis();
  while (xStreamBufferBytesAvailable(_auxStream) && millis() - t0 < ms) delay(5);
  lockCard();                                    // the writer holds the lock while it handles a record
  unlockCard();
  return xStreamBufferBytesAvailable(_auxStream) == 0;
}

void SdLog::writeAuxRecords() {
  static uint8_t data[512];
  uint8_t hdr[3];
  while (xStreamBufferReceive(_auxStream, hdr, 3, 0) == 3) {
    const int slot = hdr[0] < AUX_FILES ? hdr[0] : 0;
    const uint16_t len = hdr[1] | (hdr[2] << 8);
    if (len == 0xFFFF) {                         // close
      if (_aux[slot]) { _aux[slot].flush(); _aux[slot].close(); }
      continue;
    }
    if (len == 0xFFFE) {                         // open: 1 byte length + path
      uint8_t plen = 0;
      char path[sizeof(_auxPath[0])];
      xStreamBufferReceive(_auxStream, &plen, 1, pdMS_TO_TICKS(10));
      size_t got = 0;
      for (int tries = 0; got < plen && tries < 20; tries++)
        got += xStreamBufferReceive(_auxStream, (uint8_t *)path + got, plen - got, pdMS_TO_TICKS(10));
      path[min<size_t>(got, sizeof(path) - 1)] = 0;
      if (_aux[slot]) { _aux[slot].flush(); _aux[slot].close(); }
      _aux[slot] = SD.open(path, FILE_APPEND);
      if (!_aux[slot]) { _errors++; _auxFailed[slot] = true; }
      continue;
    }
    size_t got = 0;
    for (int tries = 0; got < len && got < sizeof(data) && tries < 20; tries++)
      got += xStreamBufferReceive(_auxStream, data + got, min<size_t>(len, sizeof(data)) - got, pdMS_TO_TICKS(10));
    if (_aux[slot] && _aux[slot].write(data, got) != got) _errors++;
  }
}

bool SdLog::lockCard() {
  if (_cardMutex) xSemaphoreTake(_cardMutex, portMAX_DELAY);
  return true;
}

void SdLog::unlockCard() {
  if (_cardMutex) xSemaphoreGive(_cardMutex);
}

// ---- background writer ---------------------------------------------------------

bool SdLog::startWriter(uint32_t flushMs, int core, int priority) {
  if (!logging() || _writer) return false;
  constexpr size_t CSV_BUF = 8 * 1024, NMEA_BUF = 16 * 1024;   // NMEA ~1 KB/s: 16 s of card stall
  constexpr size_t AUX_BUF = 16 * 1024;                         // field test ~0.3 KB/s
  const size_t bytes = (CSV_BUF + 1) + (NMEA_BUF + 1) + (AUX_BUF + 1);   // static stream buffers need size + 1
  uint8_t *mem = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) mem = (uint8_t *)malloc(bytes);
  if (!mem) return false;
  _csvStream = xStreamBufferCreateStatic(CSV_BUF, 1, mem, &_csvStreamCtl);
  _nmeaStream = xStreamBufferCreateStatic(NMEA_BUF, 1, mem + CSV_BUF + 1, &_nmeaStreamCtl);
  _auxStream = xStreamBufferCreateStatic(AUX_BUF, 1, mem + CSV_BUF + 1 + NMEA_BUF + 1, &_auxStreamCtl);
  _cardMutex = xSemaphoreCreateMutex();
  if (!_csvStream || !_nmeaStream || !_auxStream || !_cardMutex) return false;
  _flushMs = flushMs;
  flushAll();                                  // everything so far is on the card
  _writer = true;                              // from now on only the task touches the files
  if (xTaskCreatePinnedToCore(writerTask, "sdlog", 4096, this, priority, nullptr, core) != pdPASS) {
    _writer = false;
    return false;
  }
  return true;
}

void SdLog::writerTask(void *self) { static_cast<SdLog *>(self)->writerLoop(); }

void SdLog::writerLoop() {
  static uint8_t chunk[1024];
  uint32_t lastFlush = millis();
  for (;;) {
    size_t n = xStreamBufferReceive(_nmeaStream, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
    lockCard();
    while (n) {
      writeNmea(chunk, n);
      n = xStreamBufferReceive(_nmeaStream, chunk, sizeof(chunk), 0);
    }
    while ((n = xStreamBufferReceive(_csvStream, chunk, sizeof(chunk), 0)) > 0) writeCsv(chunk, n);
    writeAuxRecords();
    if (millis() - lastFlush >= _flushMs) {
      lastFlush = millis();
      _csv.flush();
      _nmea.flush();
      for (File &f : _aux) if (f) f.flush();
    }
    unlockCard();
  }
}

void SdLog::list(const char *dir, Print &out) {
  if (!_mounted) { out.println("SD not mounted"); return; }
  flushAll();
  lockCard();
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) {
    out.printf("no directory %s\n", dir);
  } else {
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      out.printf("  %-12s %10u\n", f.name(), (unsigned)f.size());
    }
  }
  unlockCard();
}

bool SdLog::dump(const char *path, Print &out) {
  if (!_mounted) { out.println("SD not mounted"); return false; }
  flushAll();                                  // include everything written so far
  lockCard();
  File f = SD.open(path, FILE_READ);
  if (!f) { unlockCard(); out.printf("cannot open %s\n", path); return false; }
  out.printf("<<<BEGIN %s %u>>>\n", path, (unsigned)f.size());
  uint8_t buf[256];
  while (f.available()) {
    const size_t n = f.read(buf, sizeof(buf));
    out.write(buf, n);
  }
  f.close();
  unlockCard();
  out.printf("\n<<<END %s>>>\n", path);
  return true;
}

void SdLog::update(uint32_t flushMs) {
  if (!logging()) return;
  if (_writer) {                               // the task writes + flushes; hand over NMEA bytes
    if (millis() - _lastFlush >= 200) { _lastFlush = millis(); _nmeaSink.drain(); }
    return;
  }
  if (millis() - _lastFlush >= flushMs) {
    _lastFlush = millis();
    flushAll();
  }
}

// ---- file manager (Files app) ----------------------------------------------------

bool SdLog::fatPath(const char *path, char *out, size_t n) {
  if (!_mounted || !path || path[0] != '/') return false;
  if (_pdrv < 0) {                               // the card is the only mounted FatFs volume
    for (int d = 0; d < FF_VOLUMES && _pdrv < 0; d++) {
      char root[4] = { (char)('0' + d), ':', '/', 0 };
      FF_DIR dir;
      lockCard();
      if (f_opendir(&dir, root) == FR_OK) { f_closedir(&dir); _pdrv = d; }
      unlockCard();
    }
    if (_pdrv < 0) return false;
  }
  return snprintf(out, n, "%d:%s", _pdrv, path) < (int)n;
}

// Read-only file-system inspection (2026-09-30: new data read back as zeros). Volume layout and
// allocation hints, then for `path`: its first cluster, that cluster's FAT entry and the raw sector.
void SdLog::fsInfo(const char *path, Print &o) {
  char p[128];
  if (!fatPath("/", p, sizeof(p))) { o.println("[FS] no volume"); return; }
  p[2] = 0;
  FATFS *fs = nullptr;
  DWORD fre = 0;
  lockCard();
  const FRESULT gr = f_getfree(p, &fre, &fs);
  unlockCard();
  if (gr != FR_OK || !fs) { o.printf("[FS] f_getfree %s: %d\n", p, (int)gr); return; }
  o.printf("[FS] drive %s type FAT%d, clusters %lu (x %u sectors), fats %u, fatbase %lu, database %lu, fsize %lu\n", p,
           fs->fs_type == FS_FAT32 ? 32 : fs->fs_type == FS_FAT16 ? 16 : 12, (unsigned long)(fs->n_fatent - 2),
           (unsigned)fs->csize, (unsigned)fs->n_fats, (unsigned long)fs->fatbase, (unsigned long)fs->database,
           (unsigned long)fs->fsize);
  o.printf("[FS] next-free hint %lu, free clusters %lu (%lu MB), fsi_flag %u\n", (unsigned long)fs->last_clst,
           (unsigned long)fs->free_clst, (unsigned long)((uint64_t)fs->free_clst * fs->csize / 2048), (unsigned)fs->fsi_flag);
  if (!path || !*path || !fatPath(path, p, sizeof(p))) return;
  FIL f;
  lockCard();
  const FRESULT r = f_open(&f, p, FA_READ);
  DWORD scl = 0;
  if (r == FR_OK) { scl = f.obj.sclust; f_close(&f); }
  unlockCard();
  if (r != FR_OK) { o.printf("[FS] %s: f_open %d\n", path, (int)r); return; }
  static uint8_t sec[4096];
  const LBA_t fatSec = fs->fatbase + scl / (512 / 4), dataSec = fs->database + (LBA_t)(scl - 2) * fs->csize;
  lockCard();
  const bool fatOk = disk_read(fs->pdrv, sec, fatSec, 1) == RES_OK;
  const uint32_t next = fatOk ? ((uint32_t *)sec)[scl % 128] & 0x0FFFFFFF : 0;
  const bool dataOk = disk_read(fs->pdrv, sec, dataSec, 1) == RES_OK;
  unlockCard();
  o.printf("[FS] %s: first cluster %lu -> FAT entry %s0x%08lx, data sector %lu read %s: ", path, (unsigned long)scl,
           fatOk ? "" : "(read FAILED) ", (unsigned long)next, (unsigned long)dataSec, dataOk ? "ok" : "FAILED");
  for (int i = 0; i < 24; i++) o.write(isprint(sec[i]) ? sec[i] : '.');
  o.println();
}

void SdLog::rawSector(uint32_t sector, Print &o) {
  char p[8];
  if (!fatPath("/", p, sizeof(p))) { o.println("[FS] no volume"); return; }
  static uint8_t sec[4096];
  lockCard();
  const DRESULT r = disk_read(p[0] - '0', sec, sector, 1);
  unlockCard();
  uint32_t nz = 0;
  for (int i = 0; i < 512; i++) nz += sec[i] != 0;
  o.printf("[FS] sector %lu (%lu MB): read %s, %lu of 512 bytes non-zero: ", (unsigned long)sector,
           (unsigned long)(sector / 2048), r == RES_OK ? "ok" : "FAILED", (unsigned long)nz);
  for (int i = 0; i < 16; i++) o.printf("%02x", sec[i]);
  o.println();
}

int SdLog::listDir(const char *dir, DirEntry *out, int max, int *total) {
  char p[160];
  if (total) *total = 0;
  if (!fatPath(dir, p, sizeof(p))) return -1;
  FF_DIR d;
  FILINFO fi;
  int n = 0, all = 0;
  lockCard();
  if (f_opendir(&d, p) != FR_OK) { unlockCard(); return -1; }
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
    if (fi.fattrib & (AM_HID | AM_SYS)) continue;   // "System Volume Information" etc.
    all++;
    if (n >= max) continue;
    DirEntry &e = out[n++];
    strlcpy(e.name, fi.fname, sizeof(e.name));
    e.size = (uint32_t)fi.fsize;
    e.fdate = fi.fdate;
    e.ftime = fi.ftime;
    e.dir = fi.fattrib & AM_DIR;
  }
  f_closedir(&d);
  unlockCard();
  if (total) *total = all;
  return n;
}

bool SdLog::usage(uint64_t &totalBytes, uint64_t &freeBytes) {
  char p[8];
  if (!fatPath("/", p, sizeof(p))) return false;
  p[2] = 0;                                      // "0:"
  FATFS *fs = nullptr;
  DWORD freeClusters = 0;
  lockCard();
  const FRESULT r = f_getfree(p, &freeClusters, &fs);   // FSINFO free count: fast
  unlockCard();
  if (r != FR_OK || !fs) return false;
#if FF_MAX_SS != FF_MIN_SS
  const uint64_t cluster = (uint64_t)fs->csize * fs->ssize;
#else
  const uint64_t cluster = (uint64_t)fs->csize * FF_MAX_SS;
#endif
  totalBytes = (uint64_t)(fs->n_fatent - 2) * cluster;
  freeBytes = (uint64_t)freeClusters * cluster;
  return true;
}

bool SdLog::inUse(const char *path) {
  const size_t n = strlen(path);
  if (n <= 1) return true;                       // the root always holds the session
  auto under = [&](const File &f) {
    if (!f) return false;
    const char *fp = f.path();
    return fp && !strncmp(fp, path, n) && (fp[n] == 0 || fp[n] == '/');
  };
  auto underPath = [&](const char *fp) { return fp[0] && !strncmp(fp, path, n) && (fp[n] == 0 || fp[n] == '/'); };
  lockCard();                                    // the writer task opens/closes the aux files
  bool used = under(_csv) || under(_nmea);
  for (int i = 0; i < AUX_FILES; i++) used = used || under(_aux[i]) || (_auxUsed[i] && underPath(_auxPath[i]));
  unlockCard();
  return used;
}

bool SdLog::removeStart(const char *path) {
  if (_rmBusy || !_mounted || inUse(path)) return false;
  char p[sizeof(_rmPath)];
  if (!fatPath(path, p, sizeof(p))) return false;
  strlcpy(_rmPath, path, sizeof(_rmPath));
  _rmKeep = -1;
  _rmCount = 0;
  _rmFailed = false;
  _rmBusy = true;
  if (xTaskCreatePinnedToCore(removeTask, "sd_rm", 6144, this, 1, nullptr, 0) != pdPASS) { _rmBusy = false; return false; }
  return true;
}

bool SdLog::removeOldSessionsStart(const char *dir, int keep) {
  if (_rmBusy || !_mounted || keep < 1) return false;
  char p[sizeof(_rmPath)];
  if (!fatPath(dir, p, sizeof(p))) return false;
  strlcpy(_rmPath, dir, sizeof(_rmPath));
  _rmKeep = keep;
  _rmCount = 0;
  _rmFailed = false;
  _rmBusy = true;
  if (xTaskCreatePinnedToCore(removeTask, "sd_rm", 6144, this, 1, nullptr, 0) != pdPASS) { _rmBusy = false; return false; }
  return true;
}

// Session files are "Snnnn.<ext>": keep the `_rmKeep` highest numbers (and anything in use).
void SdLog::removeOldSessions() {
  char p[400];
  if (!fatPath(_rmPath, p, sizeof(p))) { _rmFailed = true; return; }
  constexpr int MAX = 4000;
  uint16_t *nums = (uint16_t *)heap_caps_malloc(MAX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!nums) { _rmFailed = true; return; }
  auto sessionNo = [](const char *name) -> int {
    if (name[0] != 'S' || strlen(name) < 6 || name[5] != '.') return -1;
    for (int i = 1; i < 5; i++) if (!isdigit((unsigned char)name[i])) return -1;
    return atoi(name + 1);
  };
  FF_DIR d;
  FILINFO fi;
  int n = 0;
  lockCard();
  if (f_opendir(&d, p) == FR_OK) {
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] && n < MAX) {
      const int s = sessionNo(fi.fname);
      if (s < 0) continue;
      bool dup = false;
      for (int i = 0; i < n && !dup; i++) dup = nums[i] == s;
      if (!dup) nums[n++] = (uint16_t)s;
    }
    f_closedir(&d);
  }
  unlockCard();
  std::sort(nums, nums + n, [](uint16_t a, uint16_t b) { return a > b; });
  const int threshold = n > _rmKeep ? nums[_rmKeep - 1] : 0;   // keep numbers >= threshold
  heap_caps_free(nums);
  if (!threshold) return;                                      // nothing older than the newest `keep`
  lockCard();
  const bool open = f_opendir(&d, p) == FR_OK;
  unlockCard();
  if (!open) { _rmFailed = true; return; }
  const size_t len = strlen(p);
  for (;;) {
    lockCard();
    const FRESULT r = f_readdir(&d, &fi);
    unlockCard();
    if (r != FR_OK || !fi.fname[0]) break;
    const int s = sessionNo(fi.fname);
    if (s < 0 || s >= threshold) continue;
    char user[400];
    snprintf(user, sizeof(user), "%s/%s", _rmPath, fi.fname);
    if (inUse(user)) continue;
    snprintf(p + len, sizeof(p) - len, "/%s", fi.fname);
    lockCard();
    const bool gone = f_unlink(p) == FR_OK;
    unlockCard();
    p[len] = 0;
    if (gone) _rmCount = _rmCount + 1;
    else _rmFailed = true;
  }
  lockCard();
  f_closedir(&d);
  unlockCard();
}

// ---- folder sizes (Storage app) --------------------------------------------------------

bool SdLog::statStart(const char *const *paths, int n) {
  if (_statBusy || !_mounted || n < 1 || n > STAT_MAX) return false;
  _statN = n;
  for (int i = 0; i < n; i++) { strlcpy(_statPath[i], paths[i], sizeof(_statPath[i])); _stat[i] = {0, 0}; }
  _statBusy = true;
  if (xTaskCreatePinnedToCore(statTask, "sd_stat", 6144, this, 1, nullptr, 0) != pdPASS) { _statBusy = false; return false; }
  return true;
}

void SdLog::statTask(void *self) {
  SdLog &s = *(SdLog *)self;
  const uint32_t t0 = millis();
  for (int i = 0; i < s._statN; i++) {
    char p[256];
    TreeStat t = {0, 0};
    if (s.fatPath(s._statPath[i], p, sizeof(p))) s.treeStat(p, sizeof(p), t);
    s._stat[i] = t;
  }
  Serial.printf("[SD] folder sizes: %d folder(s) in %u ms\n", s._statN, (unsigned)(millis() - t0));
  s._statBusy = false;
  vTaskDelete(nullptr);
}

void SdLog::treeStat(char *path, size_t cap, TreeStat &out) {
  FF_DIR d;
  FILINFO fi;
  lockCard();
  FRESULT r = f_opendir(&d, path);
  unlockCard();
  if (r != FR_OK) return;
  const size_t len = strlen(path);
  for (;;) {
    lockCard();
    r = f_readdir(&d, &fi);
    unlockCard();
    if (r != FR_OK || !fi.fname[0]) break;
    if (fi.fattrib & AM_DIR) {
      if (len + 1 + strlen(fi.fname) >= cap) continue;
      path[len] = '/';
      strcpy(path + len + 1, fi.fname);
      treeStat(path, cap, out);
      path[len] = 0;
    } else {
      out.bytes += fi.fsize;
      out.files++;
    }
  }
  lockCard();
  f_closedir(&d);
  unlockCard();
}

void SdLog::removeTask(void *self) {
  SdLog &s = *(SdLog *)self;
  char p[256];
  const uint32_t t0 = millis();
  bool ok;
  if (s._rmKeep >= 0) { s.removeOldSessions(); ok = !s._rmFailed; }
  else ok = s.fatPath(s._rmPath, p, sizeof(p)) && s.removeTree(p, sizeof(p));
  s._rmFailed = !ok;
  Serial.printf("[SD] delete %s: %s, %u item(s) in %u ms\n", s._rmPath, ok ? "ok" : "FAILED", (unsigned)s._rmCount,
                (unsigned)(millis() - t0));
  s._rmBusy = false;
  vTaskDelete(nullptr);
}

// Depth-first; the card lock is held per FatFs call only (ChaN's delete_node pattern:
// entries may be unlinked while their directory is being read).
bool SdLog::removeTree(char *path, size_t cap) {
  FILINFO fi;
  lockCard();
  FRESULT r = f_stat(path, &fi);
  unlockCard();
  if (r != FR_OK) return false;
  bool ok = true;
  if (fi.fattrib & AM_DIR) {
    FF_DIR d;
    lockCard();
    r = f_opendir(&d, path);
    unlockCard();
    if (r != FR_OK) return false;
    const size_t len = strlen(path);
    for (;;) {
      lockCard();
      r = f_readdir(&d, &fi);
      unlockCard();
      if (r != FR_OK) { ok = false; break; }
      if (!fi.fname[0]) break;
      if (len + 1 + strlen(fi.fname) >= cap) { ok = false; continue; }
      path[len] = '/';
      strcpy(path + len + 1, fi.fname);
      if (fi.fattrib & AM_DIR) ok = removeTree(path, cap) && ok;
      else {
        lockCard();
        const bool gone = f_unlink(path) == FR_OK;
        unlockCard();
        if (gone) _rmCount = _rmCount + 1;
        else ok = false;
      }
      path[len] = 0;
    }
    lockCard();
    f_closedir(&d);
    unlockCard();
    if (!ok) return false;
  }
  lockCard();
  r = f_unlink(path);
  unlockCard();
  if (r == FR_OK) _rmCount = _rmCount + 1;
  return r == FR_OK;
}

// ---- background small-file write ------------------------------------------------------

bool SdLog::writeFileAsync(const char *path, const String &text) {
  if (_wBusy || !_mounted || strlen(path) >= sizeof(_wPath)) return false;
  strlcpy(_wPath, path, sizeof(_wPath));
  _wData = text;
  _wFailed = false;
  _wBusy = true;
  if (xTaskCreatePinnedToCore(writeTask, "sd_wr", 4096, this, 1, nullptr, 0) != pdPASS) { _wBusy = false; return false; }
  return true;
}

void SdLog::writeTask(void *self) {
  SdLog &s = *(SdLog *)self;
  String dir = s._wPath;
  dir = dir.substring(0, max(dir.lastIndexOf('/'), 0));
  const bool ok = (!dir.length() || s.mkdirs(dir.c_str())) && s.writeFile(s._wPath, s._wData.c_str());
  s._wFailed = !ok;
  s._wBusy = false;
  vTaskDelete(nullptr);
}
