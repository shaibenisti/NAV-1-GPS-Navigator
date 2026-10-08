#include "MapTiles.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <miniz.h>
#include <string.h>
#include "Storage.h"

namespace {

struct Entry { uint32_t id, run, len; uint32_t off; };      // PMTiles directory entry (offset relative to its section)

struct Dir {
  Entry *e = nullptr;
  uint32_t n = 0, cap = 0;
  uint32_t srcOff = 0;               // which leaf this is (UINT32_MAX = none)
};

char s_path[48] = "";
bool s_open = false;
uint64_t s_rootOff = 0, s_rootLen = 0, s_leafOff = 0, s_dataOff = 0;
uint8_t s_tileComp = 2, s_intComp = 2;
int s_maxZoom = 0;
Dir s_root, s_leaf;
const char *s_err = "";
MapTiles::Stats s_stats = {};
MapTiles::Buf s_raw;                 // compressed bytes of the last read

void *psAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

bool grow(MapTiles::Buf &b, size_t need) {
  if (b.cap >= need) return true;
  size_t cap = need + need / 4 + 4096;
  uint8_t *p = (uint8_t *)psAlloc(cap);
  if (!p) { s_err = "out of memory"; return false; }
  if (b.p) heap_caps_free(b.p);
  b.p = p;
  b.cap = cap;
  return true;
}

bool readAt(uint64_t off, uint8_t *dst, size_t n) {
  uint32_t got = 0;
  while (got < n) {
    const int r = Storage::readMap(s_path, (uint32_t)(off + got), dst + got, n - got);
    if (r <= 0) { s_err = "SD read failed"; return false; }
    got += r;
  }
  s_stats.bytesRead += n;
  return true;
}

uint64_t varint(const uint8_t *b, size_t &i, size_t end) {
  uint64_t r = 0;
  int s = 0;
  while (i < end) {
    const uint8_t c = b[i++];
    r |= (uint64_t)(c & 0x7F) << s;
    if (c < 0x80) break;
    s += 7;
  }
  return r;
}

// gzip (RFC 1952) -> out; sizes from the trailer
bool gunzip(const uint8_t *src, size_t n, MapTiles::Buf &out) {
  if (n < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 8) { s_err = "not gzip"; return false; }
  const uint8_t flg = src[3];
  size_t i = 10;
  if (flg & 4) { const size_t xl = src[i] | (src[i + 1] << 8); i += 2 + xl; }
  if (flg & 8) { while (i < n && src[i]) i++; i++; }
  if (flg & 16) { while (i < n && src[i]) i++; i++; }
  if (flg & 2) i += 2;
  if (i + 8 > n) { s_err = "bad gzip"; return false; }
  const uint32_t isize = src[n - 4] | (src[n - 3] << 8) | (src[n - 2] << 16) | ((uint32_t)src[n - 1] << 24);
  if (isize > 4u * 1024 * 1024) { s_err = "tile too big"; return false; }
  if (!grow(out, isize + 16)) return false;
  // Inflate in slices of 8 KB with a one-tick sleep between: one long burst of PSRAM writes would delay the
  // screen refill (a one-frame smear). tinfl_decompressor is ~11 KB of stack.
  tinfl_decompressor dec;
  tinfl_init(&dec);
  size_t inPos = i, outPos = 0;
  const size_t inEnd = n - 8;
  for (;;) {
    size_t inSz = inEnd - inPos, outSz = isize - outPos < 8192 ? isize - outPos : 8192;
    const tinfl_status st = tinfl_decompress(&dec, src + inPos, &inSz, out.p, out.p + outPos, &outSz, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    inPos += inSz;
    outPos += outSz;
    if (st == TINFL_STATUS_DONE) break;
    if (st < 0) { s_err = "inflate failed"; return false; }
    vTaskDelay(1);
  }
  if (outPos != isize) { s_err = "inflate size"; return false; }
  out.len = isize;
  return true;
}

bool parseDir(const uint8_t *b, size_t len, Dir &d) {
  size_t i = 0;
  const uint32_t n = (uint32_t)varint(b, i, len);
  if (n > 400000) { s_err = "directory too big"; return false; }
  if (d.cap < n) {
    if (d.e) heap_caps_free(d.e);
    d.e = (Entry *)psAlloc(sizeof(Entry) * (n + 16));
    if (!d.e) { d.cap = 0; s_err = "out of memory"; return false; }
    d.cap = n + 16;
  }
  d.n = n;
  uint32_t last = 0;
  for (uint32_t k = 0; k < n; k++) { last += (uint32_t)varint(b, i, len); d.e[k].id = last; }
  for (uint32_t k = 0; k < n; k++) d.e[k].run = (uint32_t)varint(b, i, len);
  for (uint32_t k = 0; k < n; k++) d.e[k].len = (uint32_t)varint(b, i, len);
  for (uint32_t k = 0; k < n; k++) {
    const uint64_t v = varint(b, i, len);
    d.e[k].off = (v == 0 && k > 0) ? d.e[k - 1].off + d.e[k - 1].len : (uint32_t)(v - 1);
  }
  return i <= len;
}

bool loadDir(uint64_t off, uint64_t len, Dir &d) {
  if (len > 2u * 1024 * 1024) { s_err = "directory too big"; return false; }
  if (!grow(s_raw, len)) return false;
  if (!readAt(off, s_raw.p, len)) return false;
  s_stats.dirReads++;
  if (s_intComp == 2) {
    static MapTiles::Buf dec;
    if (!gunzip(s_raw.p, len, dec)) return false;
    return parseDir(dec.p, dec.len, d);
  }
  return parseDir(s_raw.p, len, d);
}

// Hilbert tile id (PMTiles spec)
uint64_t tileId(int z, uint32_t x, uint32_t y) {
  uint64_t acc = 0;
  for (int t = 0; t < z; t++) acc += (uint64_t)1 << (2 * t);
  const uint32_t n = 1u << z;
  uint64_t d = 0;
  for (uint32_t s = n / 2; s > 0; s /= 2) {
    const uint32_t rx = (x & s) ? 1 : 0, ry = (y & s) ? 1 : 0;
    d += (uint64_t)s * s * ((3 * rx) ^ ry);
    if (ry == 0) {
      if (rx == 1) { x = n - 1 - x; y = n - 1 - y; }
      const uint32_t t = x; x = y; y = t;
    }
  }
  return acc + d;
}

// Last entry with id <= tid
int findEntry(const Dir &d, uint64_t tid) {
  int lo = 0, hi = (int)d.n - 1, hit = -1;
  while (lo <= hi) {
    const int m = (lo + hi) / 2;
    if (d.e[m].id <= tid) { hit = m; lo = m + 1; }
    else hi = m - 1;
  }
  return hit;
}

}  // namespace

bool MapTiles::open(const char *path) {
  close();
  s_err = "";
  strlcpy(s_path, path, sizeof(s_path));
  uint8_t h[127];
  uint32_t size = 0;
  if (Storage::readMap(s_path, 0, h, sizeof(h), &size) != (int)sizeof(h)) { s_err = "map file not found"; return false; }
  if (memcmp(h, "PMTiles", 7) != 0 || h[7] != 3) { s_err = "not a PMTiles v3 file"; return false; }
  auto u64 = [&](int o) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | h[o + i]; return v; };
  s_rootOff = u64(8); s_rootLen = u64(16);
  s_leafOff = u64(40);
  s_dataOff = u64(56);
  s_intComp = h[97]; s_tileComp = h[98];
  s_maxZoom = h[101];
  if (h[99] != 1) { s_err = "not a vector tile file"; return false; }
  if (s_tileComp != 2 || s_intComp > 2) { s_err = "unsupported compression"; return false; }
  s_stats = {};
  if (!loadDir(s_rootOff, s_rootLen, s_root)) return false;
  s_leaf.srcOff = UINT32_MAX;
  s_stats.maxZoom = s_maxZoom;
  s_open = true;
  return true;
}

void MapTiles::close() {
  s_open = false;
  Storage::readMapClose();
  if (s_root.e) { heap_caps_free(s_root.e); s_root = Dir(); }
  if (s_leaf.e) { heap_caps_free(s_leaf.e); s_leaf = Dir(); }
  if (s_raw.p) { heap_caps_free(s_raw.p); s_raw = Buf(); }
}

bool MapTiles::isOpen() { return s_open; }

void MapTiles::release(Buf &b) {
  if (b.p) heap_caps_free(b.p);
  b = Buf();
}

bool MapTiles::get(int z, int x, int y, Buf &out) {
  out.len = 0;
  if (!s_open) { s_err = "map not open"; return false; }
  if (x < 0 || y < 0 || x >= (1 << z) || y >= (1 << z)) return true;
  const uint32_t t0 = millis();
  const uint64_t tid = tileId(z, (uint32_t)x, (uint32_t)y);
  const Dir *d = &s_root;
  for (int depth = 0; depth < 3; depth++) {
    const int k = findEntry(*d, tid);
    if (k < 0) return true;
    const Entry &e = d->e[k];
    if (e.run > 0) {                                   // a tile (or a run of the same tile)
      if (tid >= (uint64_t)e.id + e.run) return true;
      if (!grow(s_raw, e.len)) return false;
      const uint32_t r0 = micros();
      if (!readAt(s_dataOff + e.off, s_raw.p, e.len)) return false;
      const uint32_t r1 = micros();
      if (!gunzip(s_raw.p, e.len, out)) return false;
      s_stats.readUsSum += r1 - r0;
      s_stats.inflateUsSum += micros() - r1;
      s_stats.tiles++;
      s_stats.tileMsSum += millis() - t0;
      return true;
    }
    if (s_leaf.srcOff != e.off) {                      // leaf directory pointer
      s_leaf.srcOff = UINT32_MAX;
      const uint32_t d0 = micros();
      if (!loadDir(s_leafOff + e.off, e.len, s_leaf)) return false;
      s_stats.dirUsSum += micros() - d0;
      s_leaf.srcOff = e.off;
    }
    d = &s_leaf;
  }
  s_err = "directory nesting";
  return false;
}

MapTiles::Stats MapTiles::stats() { return s_stats; }
const char *MapTiles::lastError() { return s_err; }
