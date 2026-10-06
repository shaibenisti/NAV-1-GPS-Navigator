#include "MapRender.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "MapTiles.h"

namespace {

using MapRender::W;
using MapRender::H;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

// Dark theme (same colours as tools/scripts/maprender.py)
constexpr uint16_t C_BG = rgb(18, 22, 32), C_LAND = rgb(26, 32, 46), C_PARK = rgb(28, 56, 44), C_FOREST = rgb(24, 62, 42),
                   C_FARM = rgb(30, 44, 40), C_IND = rgb(34, 38, 50), C_WATER = rgb(36, 78, 120), C_BUILD = rgb(44, 52, 70),
                   C_CASE = rgb(10, 12, 18);

enum Road : uint8_t { R_OTHER, R_PATH, R_MINOR, R_MEDIUM, R_MAJOR, R_HIGHWAY, R_RAIL, R_COUNT };
constexpr uint16_t ROAD_COL[R_COUNT] = { rgb(110, 118, 136), rgb(90, 100, 120), rgb(150, 158, 176), rgb(210, 214, 224),
                                         rgb(240, 200, 90), rgb(255, 170, 60), rgb(120, 120, 140) };
constexpr float ROAD_W[R_COUNT] = { 2.2f, 1.6f, 3.5f, 5.0f, 7.0f, 9.0f, 1.5f };
constexpr uint8_t ROAD_ORDER[R_COUNT] = { R_OTHER, R_RAIL, R_PATH, R_MINOR, R_MEDIUM, R_MAJOR, R_HIGHWAY };

enum Land : uint8_t { L_NONE, L_PARK, L_FOREST, L_FARM, L_IND };
constexpr uint16_t LAND_COL[] = { 0, C_PARK, C_FOREST, C_FARM, C_IND };

constexpr int MAX_TILES = 16;
constexpr int MAX_PTS = 24000;             // points of one feature
constexpr int MAX_RINGS = 2048;
constexpr int MAX_EDGES = 16000;
constexpr int MAX_TMP = 30000;             // features / values of one layer
constexpr int TZ_MAX = 15;                 // the file's maximum zoom
constexpr float ZMIN = 13.0f, ZMAX = 17.5f;
inline int tzFor(float zoom) { return zoom >= TZ_MAX ? TZ_MAX : (int)floorf(zoom); }   // tile zoom for a view zoom
bool s_wantBuildings = true;               // below zoom 15 buildings are not drawn (nor indexed)

struct Feat { uint32_t geom, glen; uint8_t type, kind; };
struct LayerIdx { Feat *f = nullptr; uint32_t n = 0; };
struct Tile {
  MapTiles::Buf buf;
  int tx = 0, ty = 0;
  uint32_t extent = 4096;
  LayerIdx earth, landuse, landcover, water, buildings, roads;
};
struct Edge { float y0, y1, x, k; };

// ---- state ------------------------------------------------------------------------------------------
uint16_t *s_img[2] = {};
int s_back = 0, s_readyIdx = -1;
MapRender::Req s_readyReq;
MapRender::Req s_want;
volatile bool s_haveWant = false, s_busy = false, s_run = false, s_taskDone = true, s_mapOk = false;
const char *s_err = "";
uint32_t s_lastMs = 0, s_lastTiles = 0, s_frames = 0, s_seq = 0;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t s_task = nullptr;
char s_path[48] = "";

Tile *s_tiles = nullptr;
float *s_sx = nullptr, *s_sy = nullptr;
int *s_ring = nullptr;
Edge *s_edges = nullptr;
int *s_act = nullptr;
float *s_xs = nullptr;
uint32_t *s_tmpFeat = nullptr, *s_tmpVal = nullptr;   // (offset, length) pairs
constexpr int MAX_TRACK = 600;
float *s_trkLat = nullptr, *s_trkLon = nullptr;       // PSRAM (begin)
int s_trkN = 0;
float *s_trkCopy = nullptr;

// Feature loops (indexing, decoding off-screen geometry) read lots of PSRAM without drawing: sleep a tick every 192.
int s_ticks = 0;
inline void featTick() { if (++s_ticks >= 96) { s_ticks = 0; vTaskDelay(1); } }

void *ps(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
// Big scratch arrays are not zeroed: a 1 MB memset of PSRAM in one go delays the screen refill
void *psRaw(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

// ---- protobuf bits ---------------------------------------------------------------------------------
inline uint64_t rdVar(const uint8_t *b, size_t &i, size_t end) {
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
inline int32_t zz(uint32_t n) { return (int32_t)(n >> 1) ^ -(int32_t)(n & 1); }

bool skipField(const uint8_t *b, size_t &i, size_t end, int wire) {
  switch (wire) {
    case 0: rdVar(b, i, end); return true;
    case 1: i += 8; return i <= end;
    case 2: { const size_t l = rdVar(b, i, end); i += l; return i <= end; }
    case 5: i += 4; return i <= end;
  }
  return false;
}

bool strEq(const uint8_t *s, size_t n, const char *lit) { return strlen(lit) == n && memcmp(s, lit, n) == 0; }

// ---- kind tables -----------------------------------------------------------------------------------
uint8_t roadKind(const uint8_t *s, size_t n) {
  if (strEq(s, n, "highway")) return R_HIGHWAY;
  if (strEq(s, n, "major_road")) return R_MAJOR;
  if (strEq(s, n, "medium_road")) return R_MEDIUM;
  if (strEq(s, n, "minor_road")) return R_MINOR;
  if (strEq(s, n, "path")) return R_PATH;
  if (strEq(s, n, "rail")) return R_RAIL;
  return R_OTHER;
}
uint8_t landKind(const uint8_t *s, size_t n) {
  static const char *const park[] = { "park", "garden", "grass", "meadow", "nature_reserve", "cemetery", "pitch", "playground",
                                      "golf_course", "recreation_ground", "village_green", "allotments", "zoo", "grassland", "scrub" };
  for (const char *p : park) if (strEq(s, n, p)) return L_PARK;
  if (strEq(s, n, "forest") || strEq(s, n, "wood")) return L_FOREST;
  if (strEq(s, n, "farmland") || strEq(s, n, "farmyard") || strEq(s, n, "orchard")) return L_FARM;
  if (strEq(s, n, "industrial") || strEq(s, n, "commercial") || strEq(s, n, "military") || strEq(s, n, "quarry")) return L_IND;
  return L_NONE;
}
uint8_t waterKind(const uint8_t *s, size_t n) { return strEq(s, n, "swimming_pool") ? 1 : 0; }

// ---- tile indexing ---------------------------------------------------------------------------------
// One layer message -> Feat array; `kindFn` turns the feature's "kind" string into Feat::kind.
void indexLayer(const uint8_t *b, size_t start, size_t end, LayerIdx &out, uint8_t (*kindFn)(const uint8_t *, size_t)) {
  size_t i = start;
  int nKeys = 0, kindKey = -1;
  uint32_t nFeat = 0, nVal = 0;
  while (i < end) {                                   // pass 1: keys, value + feature offsets
    const uint64_t k = rdVar(b, i, end);
    const int f = (int)(k >> 3), w = (int)(k & 7);
    if (w != 2) { if (!skipField(b, i, end, w)) return; continue; }
    const size_t l = rdVar(b, i, end);
    if (i + l > end) return;
    if (f == 3) { if (strEq(b + i, l, "kind")) kindKey = nKeys; nKeys++; }
    else if (f == 4) { if (nVal < (uint32_t)MAX_TMP) { s_tmpVal[2 * nVal] = (uint32_t)i; s_tmpVal[2 * nVal + 1] = (uint32_t)l; nVal++; } }
    else if (f == 2) { if (nFeat < (uint32_t)MAX_TMP) { s_tmpFeat[2 * nFeat] = (uint32_t)i; s_tmpFeat[2 * nFeat + 1] = (uint32_t)l; nFeat++; } }
    i += l;
  }
  if (!nFeat) return;
  out.f = (Feat *)ps(sizeof(Feat) * nFeat);
  if (!out.f) return;
  for (uint32_t q = 0; q < nFeat; q++) {              // pass 2: geometry + kind of each feature
    featTick();
    size_t j = s_tmpFeat[2 * q];
    const size_t fe = j + s_tmpFeat[2 * q + 1];
    Feat ft = {};
    size_t tagOff = 0, tagLen = 0;
    while (j < fe) {
      const uint64_t k = rdVar(b, j, fe);
      const int f = (int)(k >> 3), w = (int)(k & 7);
      if (w == 0) { const uint64_t v = rdVar(b, j, fe); if (f == 3) ft.type = (uint8_t)v; }
      else if (w == 2) {
        const size_t l = rdVar(b, j, fe);
        if (f == 4) { ft.geom = (uint32_t)j; ft.glen = (uint32_t)l; }
        else if (f == 2) { tagOff = j; tagLen = l; }
        j += l;
      } else if (!skipField(b, j, fe, w)) break;
    }
    if (kindFn && kindKey >= 0) {
      size_t t = tagOff;
      const size_t te = tagOff + tagLen;
      while (t < te) {
        const uint64_t kk = rdVar(b, t, te), vv = rdVar(b, t, te);
        if ((int)kk != kindKey || vv >= nVal) continue;
        size_t p = s_tmpVal[2 * vv];
        const size_t pe = p + s_tmpVal[2 * vv + 1];
        while (p < pe) {                              // Value.string_value = field 1
          const uint64_t vk = rdVar(b, p, pe);
          if ((vk >> 3) == 1 && (vk & 7) == 2) { const size_t sl = rdVar(b, p, pe); ft.kind = kindFn(b + p, sl); break; }
          if (!skipField(b, p, pe, (int)(vk & 7))) break;
        }
        break;
      }
    }
    if (ft.glen) out.f[out.n++] = ft;
  }
}

void freeTile(Tile &t) {
  for (LayerIdx *l : { &t.earth, &t.landuse, &t.landcover, &t.water, &t.buildings, &t.roads }) {
    if (l->f) heap_caps_free(l->f);
    *l = LayerIdx();
  }
  MapTiles::release(t.buf);
}

bool indexTile(Tile &t) {
  const uint8_t *b = t.buf.p;
  const size_t n = t.buf.len;
  size_t i = 0;
  while (i < n) {
    const uint64_t k = rdVar(b, i, n);
    const int f = (int)(k >> 3), w = (int)(k & 7);
    if (f != 3 || w != 2) { if (!skipField(b, i, n, w)) return false; continue; }
    const size_t l = rdVar(b, i, n);
    if (i + l > n) return false;
    // layer name = field 1
    size_t p = i;
    const size_t le = i + l;
    const uint8_t *name = nullptr;
    size_t nameLen = 0;
    while (p < le) {
      const uint64_t lk = rdVar(b, p, le);
      if ((lk >> 3) == 1 && (lk & 7) == 2) { nameLen = rdVar(b, p, le); name = b + p; break; }
      if (!skipField(b, p, le, (int)(lk & 7))) break;
    }
    if (name) {
      if (strEq(name, nameLen, "earth")) indexLayer(b, i, le, t.earth, nullptr);
      else if (strEq(name, nameLen, "landuse")) indexLayer(b, i, le, t.landuse, landKind);
      else if (strEq(name, nameLen, "landcover")) indexLayer(b, i, le, t.landcover, landKind);
      else if (strEq(name, nameLen, "water")) indexLayer(b, i, le, t.water, waterKind);
      else if (s_wantBuildings && strEq(name, nameLen, "buildings")) indexLayer(b, i, le, t.buildings, nullptr);
      else if (strEq(name, nameLen, "roads")) indexLayer(b, i, le, t.roads, roadKind);
    }
    i = le;
  }
  return true;
}

// ---- geometry -> screen points -----------------------------------------------------------------------
struct View {
  float ax, ay;                        // pixel of the tile's (0,0)
  float m00, m01, m10, m11;            // tile unit -> pixel: x = ax + gx*m00 + gy*m01, y = ay + gx*m10 + gy*m11 (scale + rotation)
};

// Decodes a feature's geometry into s_sx/s_sy (screen) + s_ring (ring/line start indices). Returns the point count
// (0 = too big / empty); nRings via reference. Also gives the bounding box.
int decodeGeom(const Tile &t, const Feat &f, const View &v, int &nRings, float &x0, float &y0, float &x1, float &y1) {
  const uint8_t *b = t.buf.p;
  size_t i = f.geom;
  const size_t e = f.geom + f.glen;
  int32_t cx = 0, cy = 0;
  int np = 0;
  nRings = 0;
  x0 = y0 = 1e9f; x1 = y1 = -1e9f;
  while (i < e) {
    const uint32_t ci = (uint32_t)rdVar(b, i, e);
    const int cmd = ci & 7;
    uint32_t cnt = ci >> 3;
    if (cmd == 7) {                                    // ClosePath: back to the ring's first point
      if (nRings && np < MAX_PTS) {
        const int s = s_ring[nRings - 1];
        s_sx[np] = s_sx[s]; s_sy[np] = s_sy[s];
        np++;
      }
      continue;
    }
    while (cnt--) {
      cx += zz((uint32_t)rdVar(b, i, e));
      cy += zz((uint32_t)rdVar(b, i, e));
      if (np >= MAX_PTS) return 0;
      if (cmd == 1) {
        if (nRings >= MAX_RINGS) return 0;
        s_ring[nRings++] = np;
      }
      const float x = v.ax + cx * v.m00 + cy * v.m01, y = v.ay + cx * v.m10 + cy * v.m11;
      s_sx[np] = x; s_sy[np] = y; np++;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
  }
  if (nRings < MAX_RINGS) s_ring[nRings] = np;
  return np;
}

// ---- rasteriser ----------------------------------------------------------------------------------------
uint16_t *s_cv = nullptr;                              // canvas being drawn

// Long runs of PSRAM writes from this core delay the screen refill (late refills = a one-frame smear):
// every WORK_SLICE pixels the task sleeps one tick.
constexpr int WORK_SLICE = 1500;
int s_work = 0;
inline void throttle(int px) {
  s_work += px;
  if (s_work >= WORK_SLICE) { s_work = 0; vTaskDelay(1); }
}

inline void span(int y, int xa, int xb, uint16_t c) {
  if (y < 0 || y >= H) return;
  if (xa < 0) xa = 0;
  if (xb > W - 1) xb = W - 1;
  uint16_t *p = s_cv + y * W + xa;
  const int n = xb - xa + 1;
  if (n <= 0) return;
  for (int k = n; k > 0; k--) *p++ = c;
  throttle(n + 8);
}

int cmpEdge(const void *a, const void *b) {
  const float ya = ((const Edge *)a)->y0, yb = ((const Edge *)b)->y0;
  return ya < yb ? -1 : ya > yb;
}

// Even-odd fill of all rings of a feature (rings are closed: last point = first)
void fillPoly(int nRings, int np, uint16_t c) {
  int ne = 0;
  for (int r = 0; r < nRings; r++) {
    const int a = s_ring[r], z = (r + 1 < nRings ? s_ring[r + 1] : np) - 1;
    for (int i = a; i < z; i++) {
      float xa = s_sx[i], ya = s_sy[i], xb = s_sx[i + 1], yb = s_sy[i + 1];
      if (ya == yb) continue;
      if (ya > yb) { const float tx = xa, ty = ya; xa = xb; ya = yb; xb = tx; yb = ty; }
      if (yb < 0 || ya >= H) continue;
      if (ne >= MAX_EDGES) return;
      s_edges[ne++] = { ya, yb, xa, (xb - xa) / (yb - ya) };
    }
  }
  if (ne < 2) return;
  qsort(s_edges, ne, sizeof(Edge), cmpEdge);
  int nAct = 0, next = 0;
  int y = (int)floorf(s_edges[0].y0 - 0.5f);
  if (y < 0) y = 0;
  const float lastY = s_edges[0].y1;
  (void)lastY;
  for (;; y++) {
    if (y >= H) break;
    const float yc = y + 0.5f;
    while (next < ne && s_edges[next].y0 <= yc) s_act[nAct++] = next++;
    if (nAct == 0 && next >= ne) break;
    int m = 0;
    for (int a = 0; a < nAct; a++) {                    // drop finished edges, compute crossings
      const Edge &e = s_edges[s_act[a]];
      if (e.y1 <= yc) continue;
      s_act[m] = s_act[a];
      s_xs[m] = e.x + (yc - e.y0) * e.k;
      m++;
    }
    nAct = m;
    for (int a = 1; a < nAct; a++) {                    // insertion sort of the crossings
      const float xv = s_xs[a];
      int b = a - 1;
      while (b >= 0 && s_xs[b] > xv) { s_xs[b + 1] = s_xs[b]; b--; }
      s_xs[b + 1] = xv;
    }
    for (int a = 0; a + 1 < nAct; a += 2) span(y, (int)lroundf(s_xs[a]), (int)lroundf(s_xs[a + 1]) - 1, c);
  }
}

void fillConvex(const float *px, const float *py, int n, uint16_t c) {
  float ymin = 1e9f, ymax = -1e9f;
  for (int i = 0; i < n; i++) { if (py[i] < ymin) ymin = py[i]; if (py[i] > ymax) ymax = py[i]; }
  int y0 = (int)floorf(ymin - 0.5f) , y1 = (int)ceilf(ymax - 0.5f);
  if (y0 < 0) y0 = 0;
  if (y1 > H - 1) y1 = H - 1;
  for (int y = y0; y <= y1; y++) {
    const float yc = y + 0.5f;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < n; i++) {
      const int j = (i + 1) % n;
      float ya = py[i], yb = py[j];
      if ((ya <= yc && yc < yb) || (yb <= yc && yc < ya)) {
        const float x = px[i] + (yc - ya) * (px[j] - px[i]) / (yb - ya);
        if (x < lo) lo = x;
        if (x > hi) hi = x;
      }
    }
    if (hi >= lo) span(y, (int)lroundf(lo), (int)lroundf(hi) - 1, c);
  }
}

void disc(float cx, float cy, float r, uint16_t c) {
  const int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
  for (int y = y0; y <= y1; y++) {
    const float dy = y + 0.5f - cy;
    const float d2 = r * r - dy * dy;
    if (d2 <= 0) continue;
    const float dx = sqrtf(d2);
    span(y, (int)lroundf(cx - dx), (int)lroundf(cx + dx) - 1, c);
  }
}

// Thick polyline: one quad per segment, round joints for wide lines
void thickLine(int a, int z, float wd, uint16_t c) {
  const float h = wd * 0.5f;
  for (int i = a; i < z - 1; i++) {
    const float x0 = s_sx[i], y0 = s_sy[i], x1 = s_sx[i + 1], y1 = s_sy[i + 1];
    if ((x0 < -h && x1 < -h) || (x0 > W + h && x1 > W + h) || (y0 < -h && y1 < -h) || (y0 > H + h && y1 > H + h)) continue;
    const float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.01f) continue;
    const float nx = -dy / l * h, ny = dx / l * h;
    const float qx[4] = { x0 + nx, x1 + nx, x1 - nx, x0 - nx }, qy[4] = { y0 + ny, y1 + ny, y1 - ny, y0 - ny };
    fillConvex(qx, qy, 4, c);
  }
  if (wd >= 3.0f)
    for (int i = a; i < z; i++) {
      if (s_sx[i] < -h || s_sx[i] > W + h || s_sy[i] < -h || s_sy[i] > H + h) continue;
      disc(s_sx[i], s_sy[i], h, c);
    }
}

// ---- one frame -----------------------------------------------------------------------------------------
void lonlatToTile(double lon, double lat, int z, double &x, double &y) {
  const double n = (double)(1 << z);
  x = (lon + 180.0) / 360.0 * n;
  const double s = sin(lat * M_PI / 180.0);
  y = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * n;
}

inline bool visible(float x0, float y0, float x1, float y1, float m) { return x1 >= -m && x0 <= W + m && y1 >= -m && y0 <= H + m; }

void drawPolys(const Tile &t, const LayerIdx &L, const View &v, int pass) {
  for (uint32_t q = 0; q < L.n; q++) {
    featTick();
    const Feat &f = L.f[q];
    if (f.type != 3) continue;
    uint16_t c;
    if (pass == 0) c = C_LAND;                                          // earth
    else if (pass == 1) { if (!f.kind) continue; c = LAND_COL[f.kind]; }  // landuse / landcover
    else if (pass == 2) c = C_WATER;                                    // water
    else c = C_BUILD;                                                   // buildings
    int nR;
    float x0, y0, x1, y1;
    const int np = decodeGeom(t, f, v, nR, x0, y0, x1, y1);
    if (np < 3 || !visible(x0, y0, x1, y1, 0)) continue;
    fillPoly(nR, np, c);
  }
}

void drawWaterLines(const Tile &t, const View &v, float sc) {
  for (uint32_t q = 0; q < t.water.n; q++) {
    const Feat &f = t.water.f[q];
    if (f.type != 2) continue;
    int nR;
    float x0, y0, x1, y1;
    const int np = decodeGeom(t, f, v, nR, x0, y0, x1, y1);
    if (np < 2 || !visible(x0, y0, x1, y1, 4)) continue;
    for (int r = 0; r < nR; r++) thickLine(s_ring[r], r + 1 < nR ? s_ring[r + 1] : np, 2.0f * sc, C_WATER);
  }
}

void drawRoads(const Tile &t, const View &v, int kind, bool casing, float sc) {
  float wd = ROAD_W[kind] * sc;
  if (casing) wd += 2.5f;
  const uint16_t c = casing ? C_CASE : ROAD_COL[kind];
  for (uint32_t q = 0; q < t.roads.n; q++) {
    featTick();
    const Feat &f = t.roads.f[q];
    if (f.type != 2 || f.kind != kind) continue;
    int nR;
    float x0, y0, x1, y1;
    const int np = decodeGeom(t, f, v, nR, x0, y0, x1, y1);
    if (np < 2 || !visible(x0, y0, x1, y1, wd)) continue;
    for (int r = 0; r < nR; r++) thickLine(s_ring[r], r + 1 < nR ? s_ring[r + 1] : np, wd, c);
  }
}

inline bool aborted() { return s_haveWant || !s_run; }

bool renderFrame(const MapRender::Req &rq) {
  const float zoom = constrain(rq.zoom, ZMIN, ZMAX);
  const int tz = tzFor(zoom);
  const float scale = 256.0f * powf(2.0f, zoom - tz);               // pixels per tile
  const float pw = W / 2.0f, ph = rq.headingUp ? H / 2.0f + 130.0f : H / 2.0f;   // where the requested position is drawn
  const float phi = rq.headingUp ? -rq.heading * (float)M_PI / 180.0f : 0.0f;   // map rotation (heading up)
  const float cs = cosf(phi), sn = sinf(phi);
  double fx, fy;
  lonlatToTile(rq.lon, rq.lat, tz, fx, fy);
  // tiles under the (possibly rotated) picture: bounding box of the 4 corners in tile space
  float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
  for (int c = 0; c < 4; c++) {
    const float dX = (c & 1 ? W : 0) - pw, dY = (c & 2 ? H : 0) - ph;
    const float wx = dX * cs + dY * sn, wy = -dX * sn + dY * cs;    // inverse rotation
    minX = fminf(minX, wx); maxX = fmaxf(maxX, wx); minY = fminf(minY, wy); maxY = fmaxf(maxY, wy);
  }
  const int tx0 = (int)floor(fx + minX / scale), tx1 = (int)floor(fx + maxX / scale);
  const int ty0 = (int)floor(fy + minY / scale), ty1 = (int)floor(fy + maxY / scale);
  if ((tx1 - tx0 + 1) * (ty1 - ty0 + 1) > MAX_TILES) { s_err = "too many tiles"; return false; }
  s_wantBuildings = zoom >= 15.0f;

  s_cv = s_img[s_back];
  for (int y = 0; y < H; y++) { uint16_t *row = s_cv + y * W; for (int x = 0; x < W; x++) row[x] = C_BG; throttle(W); }
  int nt = 0;
  for (int ty = ty0; ty <= ty1; ty++)
    for (int tx = tx0; tx <= tx1; tx++) {
      Tile &t = s_tiles[nt];
      t.tx = tx; t.ty = ty;
      if (!MapTiles::get(tz, tx, ty, t.buf)) { s_err = MapTiles::lastError(); }
      else if (t.buf.len) {
        if (indexTile(t)) nt++;
        else s_err = "bad tile";
      }
      if (aborted()) { for (int i = 0; i <= nt && i < MAX_TILES; i++) freeTile(s_tiles[i]); return false; }   // superseded / closing
      vTaskDelay(1);
    }
  const float sc = fmaxf(0.6f, powf(2.0f, zoom - 15.5f));
  auto view = [&](const Tile &t) {
    const float k = scale / 4096.0f;
    const float wx0 = (float)((t.tx - fx) * scale), wy0 = (float)((t.ty - fy) * scale);
    View v;
    v.ax = pw + wx0 * cs - wy0 * sn;
    v.ay = ph + wx0 * sn + wy0 * cs;
    v.m00 = k * cs; v.m01 = -k * sn; v.m10 = k * sn; v.m11 = k * cs;
    return v;
  };

  for (int i = 0; i < nt; i++) drawPolys(s_tiles[i], s_tiles[i].earth, view(s_tiles[i]), 0);
  for (int i = 0; i < nt; i++) { drawPolys(s_tiles[i], s_tiles[i].landcover, view(s_tiles[i]), 1); drawPolys(s_tiles[i], s_tiles[i].landuse, view(s_tiles[i]), 1); }
  for (int i = 0; i < nt; i++) { drawPolys(s_tiles[i], s_tiles[i].water, view(s_tiles[i]), 2); drawWaterLines(s_tiles[i], view(s_tiles[i]), sc); }
  vTaskDelay(1);
  if (aborted()) { for (int i = 0; i < nt; i++) freeTile(s_tiles[i]); return false; }
  if (zoom >= 15.0f) for (int i = 0; i < nt; i++) drawPolys(s_tiles[i], s_tiles[i].buildings, view(s_tiles[i]), 3);
  vTaskDelay(1);
  for (int ki = 0; ki < R_COUNT; ki++) {
    const int kind = ROAD_ORDER[ki];
    for (int casing = 1; casing >= 0; casing--)
      for (int i = 0; i < nt; i++) drawRoads(s_tiles[i], view(s_tiles[i]), kind, casing, sc);
    vTaskDelay(1);
    if (aborted()) { for (int i = 0; i < nt; i++) freeTile(s_tiles[i]); return false; }
  }
  if (s_trkN >= 2) {                                                 // the recording trip
    float *lat = s_trkLat, *lon = s_trkLon;
    int n;
    portENTER_CRITICAL(&s_mux);
    n = s_trkN;
    memcpy(s_trkCopy, s_trkLat, sizeof(float) * n);
    memcpy(s_trkCopy + MAX_TRACK, s_trkLon, sizeof(float) * n);
    portEXIT_CRITICAL(&s_mux);
    lat = s_trkCopy;
    lon = s_trkCopy + MAX_TRACK;
    for (int i = 0; i < n; i++) {
      double px, py;
      lonlatToTile(lon[i], lat[i], tz, px, py);
      const float wx = (float)((px - fx) * scale), wy = (float)((py - fy) * scale);
      s_sx[i] = pw + wx * cs - wy * sn;
      s_sy[i] = ph + wx * sn + wy * cs;
    }
    thickLine(0, n, 9.0f * sc, rgb(10, 12, 18));
    thickLine(0, n, 6.0f * sc, rgb(255, 87, 34));
  }
  s_lastTiles = nt;
  for (int i = 0; i < nt; i++) freeTile(s_tiles[i]);
  return true;
}

void task(void *) {
  // MapTiles::open() and the inflate in MapTiles::get() need a big stack (the ROM decompressor keeps ~11 KB on it)
  s_mapOk = MapTiles::open(s_path);
  if (!s_mapOk) s_err = MapTiles::lastError();
  while (s_run && s_mapOk) {
    if (!s_haveWant) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    MapRender::Req rq;
    portENTER_CRITICAL(&s_mux);
    rq = s_want;
    s_haveWant = false;
    portEXIT_CRITICAL(&s_mux);
    s_busy = true;
    s_err = "";
    const uint32_t t0 = millis();
    if (renderFrame(rq)) {
      s_lastMs = millis() - t0;
      portENTER_CRITICAL(&s_mux);
      s_readyIdx = s_back;
      s_readyReq = rq;
      s_back ^= 1;
      s_frames++;
      portEXIT_CRITICAL(&s_mux);
    }
    s_busy = false;
  }
  s_taskDone = true;
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));          // end() deletes this task (its stack is PSRAM: vTaskDeleteWithCaps)
}

}  // namespace

bool MapRender::begin(const char *path) {
  if (s_run || !s_taskDone) return true;
  strlcpy(s_path, path, sizeof(s_path));
  s_err = "";
  s_mapOk = false;
  s_img[0] = (uint16_t *)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_img[1] = (uint16_t *)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_tiles = (Tile *)ps(sizeof(Tile) * MAX_TILES);
  s_sx = (float *)psRaw(sizeof(float) * MAX_PTS); s_sy = (float *)psRaw(sizeof(float) * MAX_PTS);
  s_ring = (int *)ps(sizeof(int) * (MAX_RINGS + 1));
  s_edges = (Edge *)psRaw(sizeof(Edge) * MAX_EDGES);
  s_act = (int *)psRaw(sizeof(int) * MAX_EDGES);
  s_xs = (float *)psRaw(sizeof(float) * MAX_EDGES);
  s_tmpFeat = (uint32_t *)psRaw(sizeof(uint32_t) * 2 * MAX_TMP);
  s_tmpVal = (uint32_t *)psRaw(sizeof(uint32_t) * 2 * MAX_TMP);
  s_trkLat = (float *)ps(sizeof(float) * MAX_TRACK * 4);
  s_trkLon = s_trkLat + MAX_TRACK;
  s_trkCopy = s_trkLat + 2 * MAX_TRACK;
  s_trkN = 0;
  if (!s_img[0] || !s_img[1] || !s_tiles || !s_sx || !s_sy || !s_ring || !s_edges || !s_act || !s_xs || !s_tmpFeat || !s_tmpVal || !s_trkLat) {
    s_err = "out of memory";
    end();
    return false;
  }
  s_back = 0; s_readyIdx = -1; s_haveWant = false; s_frames = 0;
  s_run = true;
  s_taskDone = false;
  if (xTaskCreatePinnedToCoreWithCaps(task, "map", 32768, nullptr, 1, &s_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    s_run = false; s_taskDone = true;
    s_err = "no task";
    end();
    return false;
  }
  return true;
}

void MapRender::end() {
  s_run = false;
  for (int i = 0; i < 200 && !s_taskDone; i++) vTaskDelay(pdMS_TO_TICKS(50));   // up to 10 s: a frame may be running
  if (s_task) { vTaskDeleteWithCaps(s_task); s_task = nullptr; }
  if (s_tiles) { for (int i = 0; i < MAX_TILES; i++) freeTile(s_tiles[i]); }
  void **all[] = { (void **)&s_img[0], (void **)&s_img[1], (void **)&s_tiles, (void **)&s_sx, (void **)&s_sy, (void **)&s_ring,
                   (void **)&s_edges, (void **)&s_act, (void **)&s_xs, (void **)&s_tmpFeat, (void **)&s_tmpVal, (void **)&s_trkLat };
  for (void **p : all) { if (*p) heap_caps_free(*p); *p = nullptr; }
  MapTiles::close();
  s_mapOk = false;
  s_readyIdx = -1;
}

bool MapRender::running() { return s_run && !s_taskDone; }

void MapRender::request(const Req &r) {
  portENTER_CRITICAL(&s_mux);
  s_want = r;
  s_want.seq = ++s_seq;
  s_haveWant = true;
  portEXIT_CRITICAL(&s_mux);
}

bool MapRender::poll(const uint16_t **frame, Req *rendered) {
  bool got = false;
  portENTER_CRITICAL(&s_mux);
  if (s_readyIdx >= 0) {
    *frame = s_img[s_readyIdx];
    *rendered = s_readyReq;
    s_readyIdx = -1;
    got = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return got;
}

MapRender::Status MapRender::status() {
  return { s_busy || s_haveWant, s_mapOk, s_err, s_lastMs, s_lastTiles, s_frames,
           s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0 };
}

void MapRender::setTrack(const float *lat, const float *lon, int n) {
  n = constrain(n, 0, MAX_TRACK);
  if (!s_trkLat) return;
  portENTER_CRITICAL(&s_mux);
  if (n) { memcpy(s_trkLat, lat, sizeof(float) * n); memcpy(s_trkLon, lon, sizeof(float) * n); }
  s_trkN = n;
  portEXIT_CRITICAL(&s_mux);
}

void MapRender::positionAt(const Req &c, int dx, int dy, double &lon, double &lat) {   // north-up pictures: dx, dy from the picture centre
  const float zoom = constrain(c.zoom, ZMIN, ZMAX);
  const int tz = tzFor(zoom);
  const double scale = 256.0 * pow(2.0, zoom - tz);
  double cx, cy;
  lonlatToTile(c.lon, c.lat, tz, cx, cy);
  const double n = (double)(1 << tz), x = cx + dx / scale, y = cy + dy / scale;
  lon = x / n * 360.0 - 180.0;
  lat = atan(sinh(M_PI * (1.0 - 2.0 * y / n))) * 180.0 / M_PI;
}

double MapRender::metersPerPixel(double lat, float zoom) {
  return 156543.03392 * cos(lat * M_PI / 180.0) / pow(2.0, (double)zoom);
}

void MapRender::pixelOf(const Req &c, double lon, double lat, int &x, int &y) {
  const float zoom = constrain(c.zoom, ZMIN, ZMAX);
  const int tz = tzFor(zoom);
  const double scale = 256.0 * pow(2.0, zoom - tz);
  double cx, cy, px, py;
  lonlatToTile(c.lon, c.lat, tz, cx, cy);
  lonlatToTile(lon, lat, tz, px, py);
  const float pw = W / 2.0f, ph = c.headingUp ? H / 2.0f + 130.0f : H / 2.0f;
  const float phi = c.headingUp ? -c.heading * (float)M_PI / 180.0f : 0.0f;
  const double wx = (px - cx) * scale, wy = (py - cy) * scale;
  x = (int)lround(pw + wx * cos(phi) - wy * sin(phi));
  y = (int)lround(ph + wx * sin(phi) + wy * cos(phi));
}
