// =============================================================================
//  SD_CHECK.ino  -  microSD health check (pins from docs/HARDWARE.md)
// -----------------------------------------------------------------------------
//  1. Mount at 1 MHz (the verified clock), print card type / size / usage.
//  2. Write, append, read back and compare, delete a test file.
//  3. Re-mount at 4 / 10 / 20 MHz and measure write + read throughput with a
//     64 KB file, so loggers can pick a clock that is proven on this card.
//  Prints one PASS/FAIL line per step; never formats or deletes user files.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#define SD_CS   10
#define SD_MOSI 11
#define SD_SCK  12
#define SD_MISO 13

static const char *TEST_FILE = "/sd_check.tmp";

static const char *cardTypeName(uint8_t t) {
  switch (t) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    default:        return "NONE";
  }
}

static bool mountAt(uint32_t hz) {
  SD.end();
  return SD.begin(SD_CS, SPI, hz);
}

static bool rwTest() {
  File f = SD.open(TEST_FILE, FILE_WRITE);
  if (!f) { Serial.println("  [FAIL] open for write"); return false; }
  f.println("line1 written");
  f.close();

  f = SD.open(TEST_FILE, FILE_APPEND);
  if (!f) { Serial.println("  [FAIL] open for append"); return false; }
  f.println("line2 appended");
  f.close();

  f = SD.open(TEST_FILE, FILE_READ);
  if (!f) { Serial.println("  [FAIL] open for read"); return false; }
  String content = f.readString();
  f.close();
  const bool match = content == "line1 written\r\nline2 appended\r\n";
  Serial.printf("  [%s] write + append + read-back (%u bytes)\n", match ? "PASS" : "FAIL", content.length());

  const bool removed = SD.remove(TEST_FILE);
  Serial.printf("  [%s] delete test file\n", removed ? "PASS" : "FAIL");
  return match && removed;
}

static void speedTest(uint32_t hz) {
  if (!mountAt(hz)) { Serial.printf("  %2u MHz: mount FAILED\n", (unsigned)(hz / 1000000)); return; }

  static uint8_t buf[4096];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)i;
  const size_t total = 64 * 1024;

  File f = SD.open(TEST_FILE, FILE_WRITE);
  uint32_t t0 = millis();
  size_t written = 0;
  while (f && written < total) written += f.write(buf, sizeof(buf));
  if (f) f.close();
  const uint32_t tw = millis() - t0;

  f = SD.open(TEST_FILE, FILE_READ);
  t0 = millis();
  size_t readBytes = 0, bad = 0;
  while (f && f.available()) {
    const size_t n = f.read(buf, sizeof(buf));
    for (size_t i = 0; i < n; i++) if (buf[i] != (uint8_t)i) bad++;
    readBytes += n;
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)i;
  }
  if (f) f.close();
  const uint32_t tr = millis() - t0;
  SD.remove(TEST_FILE);

  const bool ok = written == total && readBytes == total && bad == 0;
  Serial.printf("  %2u MHz: [%s] write %u KB/s, read %u KB/s, verify errors=%u\n",
                (unsigned)(hz / 1000000), ok ? "PASS" : "FAIL",
                (unsigned)(tw ? total / tw : 0), (unsigned)(tr ? total / tr : 0), (unsigned)bad);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SD_CHECK ===");
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!mountAt(1000000)) {
    Serial.println("  [FAIL] SD.begin at 1 MHz: no card, unformatted/exFAT card, or wiring");
    Serial.println("=== SD_CHECK: FAIL ===");
    return;
  }
  const uint8_t type = SD.cardType();
  Serial.printf("  [PASS] mount at 1 MHz: type=%s size=%llu MB fs_total=%llu MB fs_used=%llu MB\n",
                cardTypeName(type), SD.cardSize() / (1024ULL * 1024ULL),
                SD.totalBytes() / (1024ULL * 1024ULL), SD.usedBytes() / (1024ULL * 1024ULL));

  const bool rw = rwTest();

  Serial.println("  Speed test (64 KB):");
  speedTest(4000000);
  speedTest(10000000);
  speedTest(20000000);

  Serial.println("  Root directory:");
  mountAt(1000000);
  File root = SD.open("/");
  int shown = 0;
  for (File e = root.openNextFile(); e && shown < 20; e = root.openNextFile(), shown++) {
    Serial.printf("    %s%s  %u\n", e.name(), e.isDirectory() ? "/" : "", (unsigned)e.size());
  }
  Serial.printf("=== SD_CHECK: %s ===\n", rw ? "PASS" : "FAIL");
}

void loop() {}
