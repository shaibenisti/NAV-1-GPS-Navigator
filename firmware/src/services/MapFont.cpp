#include "MapFont.h"

namespace {

enum Cls : uint8_t { C_NEUTRAL, C_R, C_L };

Cls classify(uint32_t cp) {
  if ((cp >= 0x05D0 && cp <= 0x05EA) || cp == 0x05BE || cp == 0x05F3 || cp == 0x05F4) return C_R;
  if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return C_L;
  return C_NEUTRAL;
}

const MapGlyph *find(const MapFont &f, uint32_t cp) {
  for (int i = 0; i < f.count; i++)
    if (f.glyphs[i].cp == cp) return &f.glyphs[i];
  return nullptr;
}

}  // namespace

bool MapText::layout(const MapFont &f, const uint8_t *s, int len, Run &out) {
  constexpr int MAX = 47;
  uint32_t cp[MAX + 1];
  int n = 0;
  for (int i = 0; i < len;) {                               // UTF-8 -> code points
    const uint8_t c = s[i];
    uint32_t v;
    int l;
    if (c < 0x80) { v = c; l = 1; }
    else if ((c & 0xE0) == 0xC0) { v = c & 0x1F; l = 2; }
    else if ((c & 0xF0) == 0xE0) { v = c & 0x0F; l = 3; }
    else return false;                                      // 4-byte sequences: not in the font
    if (i + l > len) return false;
    for (int k = 1; k < l; k++) v = (v << 6) | (s[i + k] & 0x3F);
    if (n >= MAX) return false;
    cp[n++] = v;
    i += l;
  }
  if (n == 0) return false;

  const MapGlyph *gl[MAX];
  Cls cl[MAX];
  for (int i = 0; i < n; i++) {
    gl[i] = find(f, cp[i]);
    if (!gl[i]) return false;
    cl[i] = classify(cp[i]);
  }
  // neutrals take the type of their neighbours when both agree, else the paragraph direction
  Cls para = C_L;
  for (int i = 0; i < n; i++) if (cl[i] != C_NEUTRAL) { para = cl[i]; break; }
  for (int i = 0; i < n; i++) {
    if (cl[i] != C_NEUTRAL) continue;
    int a = i - 1, b = i + 1;
    while (a >= 0 && cl[a] == C_NEUTRAL) a--;
    while (b < n && cl[b] == C_NEUTRAL) b++;
    const Cls left = a >= 0 ? cl[a] : para, right = b < n ? cl[b] : para;
    // (assign into a copy so that a run of neutrals is resolved consistently)
    Cls r = left == right ? left : para;
    for (int k = i; k < b; k++) if (cl[k] == C_NEUTRAL) cl[k] = r;
  }
  // runs of equal type, in logical order
  struct R { int s, e; Cls c; } runs[MAX];
  int nr = 0;
  for (int i = 0; i < n;) {
    int j = i;
    while (j < n && cl[j] == cl[i]) j++;
    runs[nr++] = { i, j, cl[i] };
    i = j;
  }
  out.n = 0;
  out.width = 0;
  auto emit = [&](const R &r) {
    if (r.c == C_R) for (int k = r.e - 1; k >= r.s; k--) { out.g[out.n++] = gl[k]; out.width += gl[k]->adv; }
    else for (int k = r.s; k < r.e; k++) { out.g[out.n++] = gl[k]; out.width += gl[k]->adv; }
  };
  if (para == C_R) for (int r = nr - 1; r >= 0; r--) emit(runs[r]);
  else for (int r = 0; r < nr; r++) emit(runs[r]);
  return out.n > 0;
}
