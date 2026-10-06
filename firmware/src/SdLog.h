// =============================================================================
//  SdLog  -  microSD session logging (generic; knows nothing about GPS)
// -----------------------------------------------------------------------------
//  - Mounts the card; if SD.begin fails, runs a bit-banged recovery (drain busy,
//    74+ clocks, CMD0 until R1=0x01) and retries. Needed because ESP resets don't
//    power-cycle the card, which can be left mid-transaction (seen 2026-09-27).
//  - Each boot opens a new session: <dir>/Snnnn.CSV (text lines) and
//    <dir>/Snnnn.NMEA (raw byte stream via nmeaSink()).
//  - Data is flushed every flushMs, so a sudden power-off loses at most that.
//  - Never blocks for long and never throws; failures are counted and reported.
//  - Optional background writer (startWriter): card writes and flushes then run in
//    their own task; line() and nmeaSink() only copy into RAM stream buffers. A
//    flush can take ~90 ms on the 256 GB card, which stalled the UI loop (2026-09-28).
// =============================================================================
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>

class SdLog {
public:
  struct Pins { int cs, mosi, sck, miso; };

  bool begin(const Pins &pins, uint32_t spiHz, int maxAttempts = 4);
  bool openSession(const char *dir);          // after begin(); creates dir if needed

  void line(const char *text);                // one line to the .CSV (newline added)
  Print &nmeaSink() { return _nmeaSink; }     // raw bytes to the .NMEA file
  void update(uint32_t flushMs);              // call from loop(): periodic flush (writer: hand-off only)

  // After openSession(): move all card access into a task (flush every flushMs).
  // line()/nmeaSink() never block afterwards; data that doesn't fit the RAM buffers
  // (card stalled for many seconds) is dropped and counted.
  bool startWriter(uint32_t flushMs, int core, int priority);

  // --- auxiliary files (e.g. field-test recordings), up to AUX_FILES open ---
  // Writer mode: open, lines and close are all queued and done by the writer task, in order,
  // so none of them blocks the caller (creating a file takes ~300 ms on the 256 GB card);
  // otherwise done directly. auxFailed() reports an open that failed in the writer.
  static constexpr int AUX_FILES = 4;
  bool mkdirs(const char *dir);                          // creates each missing level
  int auxOpen(const char *path);                         // append mode; slot or -1
  bool auxLine(int slot, const char *text);              // text + CR LF; false = dropped
  void auxClose(int slot);
  bool auxFailed(int slot) const { return slot >= 0 && slot < AUX_FILES && _auxFailed[slot]; }
  bool auxFlushWait(uint32_t ms = 3000);                 // blocks until queued aux work is done (before a rename)
  bool auxPending() const { return _writer && xStreamBufferBytesAvailable(_auxStream) > 0; }   // never blocks
  bool exists(const char *path);
  bool rename(const char *from, const char *to);         // file or directory (must not be open)
  bool readFile(const char *path, String &out, size_t maxBytes = 4096);   // small files only
  bool writeFile(const char *path, const char *text);                      // replaces; small files only
  bool writeFile(const char *path, const uint8_t *data, size_t n);         // replaces (blocks while writing)
  // Same in a background task (creates the parent folders). A small file rewrite takes
  // ~0.5 s on the 256 GB card - too long for the UI loop. false = busy / no card.
  bool writeFileAsync(const char *path, const String &text);
  bool writingAsync() const { return _wBusy; }
  bool asyncWriteFailed() const { return _wFailed; }
  bool readTail(const char *path, String &out, size_t bytes = 512);       // last bytes of a file
  int readChunk(const char *path, uint32_t offset, uint8_t *buf, size_t n);   // bytes read, -1 = no file
  int32_t fileSize(const char *path);                                         // -1 = no file
  // Random access to one big file kept open between calls (offline map, web task only): opening and
  // seeking walk the FAT cluster chain from the start (no fast seek in this build), a forward seek on
  // the open file only walks the new part. -1 = no file. readerClose(): after idle / before deletes.
  int readAt(const char *path, uint32_t offset, uint8_t *buf, size_t n, uint32_t *size = nullptr, int slot = 0);   // slot 1: Map app
  void readerClose(int slot = 0);
  void fsInfo(const char *path, Print &o);    // read-only: volume layout, allocation hints, a file's clusters
  void rawSector(uint32_t sector, Print &o);  // read-only: one raw card sector (non-zero count + first bytes)
  int listNames(const char *dir, String *names, int max, bool dirs = false);  // entries, sorted by name

  // --- file manager (Files app) ---
  // listDir reads the directory with FatFs directly: name, size and date in one pass
  // (Arduino's openNextFile opens every entry - seconds for /GPSLOG's ~400 files).
  struct DirEntry { char name[64]; uint32_t size; uint16_t fdate, ftime; bool dir; };   // FAT date/time
  int listDir(const char *dir, DirEntry *out, int max, int *total = nullptr);   // unsorted; -1 = no dir
  bool usage(uint64_t &totalBytes, uint64_t &freeBytes);
  bool inUse(const char *path);                          // path is (or contains) a file being written
  // Delete a file or a whole folder in a background task (card lock per file, so the
  // UI and the writer keep running). Refuses paths in use. One deletion at a time.
  bool removeStart(const char *path);
  bool removing() const { return _rmBusy; }
  uint32_t removedCount() const { return _rmCount; }      // files + folders deleted by the last/current run
  bool removeFailed() const { return _rmFailed; }
  // Delete the older GPS log sessions (Snnnn.*) in a folder, keeping the newest `keep` and
  // anything in use. Background task; progress via removing() / removedCount().
  bool removeOldSessionsStart(const char *dir, int keep);

  // Size of folder trees (Storage app) in a background task: up to STAT_MAX paths.
  static constexpr int STAT_MAX = 6;
  struct TreeStat { uint64_t bytes; uint32_t files; };
  bool statStart(const char *const *paths, int n);
  bool statBusy() const { return _statBusy; }
  TreeStat stat(int i) const { return i >= 0 && i < STAT_MAX ? _stat[i] : TreeStat{0, 0}; }

  // --- inspection over serial (so the card never has to be removed) ---
  void list(const char *dir, Print &out);                // name + size per file
  bool dump(const char *path, Print &out);               // framed: <<<BEGIN ...>>> ... <<<END ...>>>

  // --- status (for screens / logs) ---
  bool mounted() const { return _mounted; }
  bool logging() const { return _csv && _nmea; }
  const char *sessionName() const { return _session; }   // "S0003" or ""
  int mountAttempts() const { return _attempts; }
  bool wakeUpUsed() const { return _wakeUsed; }
  uint8_t wakeUpR1() const { return _wakeR1; }             // last CMD0 reply (0x01 = ok, 0xFF = silent)
  uint32_t csvLines() const { return _csvLines; }
  uint32_t nmeaBytes() const { return _nmeaBytes; }
  uint32_t writeErrors() const { return _errors; }
  uint32_t droppedBytes() const { return _dropped; }     // writer mode: RAM buffer full
  bool writerRunning() const { return _writer; }
  uint64_t cardMB() const { return _cardMB; }

private:
  class NmeaSink : public Print {
  public:
    explicit NmeaSink(SdLog &owner) : _owner(owner) {}
    size_t write(uint8_t c) override;
    void drain();
  private:
    SdLog &_owner;
    uint8_t _buf[512];
    size_t _len = 0;
  };

  uint8_t wakeUpCard();
  void flushAll();
  void writeCsv(const uint8_t *data, size_t n);
  void writeNmea(const uint8_t *data, size_t n);
  static void writerTask(void *self);
  void writerLoop();
  bool lockCard();
  void unlockCard();

  Pins _pins = {};
  bool _mounted = false;
  bool _wakeUsed = false;
  uint8_t _wakeR1 = 0;
  int _attempts = 0;
  uint64_t _cardMB = 0;
  char _session[16] = "";
  File _csv, _nmea;
  File _reader[2];                              // readAt(): open map file (slot 0: web page, slot 1: Map app)
  String _readerPath[2];
  NmeaSink _nmeaSink{*this};
  uint32_t _csvLines = 0, _nmeaBytes = 0, _errors = 0, _dropped = 0;
  uint32_t _lastFlush = 0;

  // writer mode (one producer: the loop; one consumer: the writer task)
  bool _writer = false;
  uint32_t _flushMs = 0;
  StreamBufferHandle_t _csvStream = nullptr, _nmeaStream = nullptr, _auxStream = nullptr;
  StaticStreamBuffer_t _csvStreamCtl, _nmeaStreamCtl, _auxStreamCtl;
  File _aux[AUX_FILES];                         // writer mode: opened/closed by the writer task
  bool _auxUsed[AUX_FILES] = {};                // slot handed out (auxOpen .. auxClose), UI side
  char _auxPath[AUX_FILES][96] = {};            // path of a handed-out slot (inUse before the writer opened it)
  volatile bool _auxFailed[AUX_FILES] = {};
  void writeAuxRecords();                       // writer task: drain _auxStream
  SemaphoreHandle_t _cardMutex = nullptr;       // list()/dump() vs the writer task
  // file manager
  int _pdrv = -1;                               // FatFs drive of the card ("0:"), found at first use
  bool fatPath(const char *path, char *out, size_t n);
  volatile bool _rmBusy = false, _rmFailed = false;
  volatile uint32_t _rmCount = 0;
  char _rmPath[128] = "";
  static void removeTask(void *self);
  int _rmKeep = -1;                             // >= 0: removeTask deletes old sessions instead of a tree
  void removeOldSessions();
  volatile bool _statBusy = false;
  char _statPath[STAT_MAX][32] = {};
  int _statN = 0;
  TreeStat _stat[STAT_MAX] = {};
  static void statTask(void *self);
  void treeStat(char *path, size_t cap, TreeStat &out);
  volatile bool _wBusy = false, _wFailed = false;
  char _wPath[128] = "";
  String _wData;
  static void writeTask(void *self);
  bool removeTree(char *path, size_t cap);       // FatFs path buffer, extended in place while recursing
};
