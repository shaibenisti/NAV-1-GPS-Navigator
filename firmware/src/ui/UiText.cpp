#include "UiText.h"

#include <string.h>

namespace {

enum Cls : uint8_t { C_NEUTRAL, C_R, C_L };

Cls classify(uint32_t cp) {
  if ((cp >= 0x0590 && cp <= 0x05FF) || (cp >= 0xFB1D && cp <= 0xFB4F)) return C_R;
  if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7))
    return C_L;
  return C_NEUTRAL;
}

uint32_t mirror(uint32_t cp) {
  switch (cp) {
    case '(': return ')'; case ')': return '(';
    case '[': return ']'; case ']': return '[';
    case '{': return '}'; case '}': return '{';
    case '<': return '>'; case '>': return '<';
    default: return cp;
  }
}

int encode(uint32_t v, char *o) {
  if (v < 0x80) { o[0] = (char)v; return 1; }
  if (v < 0x800) { o[0] = (char)(0xC0 | (v >> 6)); o[1] = (char)(0x80 | (v & 0x3F)); return 2; }
  if (v < 0x10000) { o[0] = (char)(0xE0 | (v >> 12)); o[1] = (char)(0x80 | ((v >> 6) & 0x3F)); o[2] = (char)(0x80 | (v & 0x3F)); return 3; }
  o[0] = (char)(0xF0 | (v >> 18)); o[1] = (char)(0x80 | ((v >> 12) & 0x3F)); o[2] = (char)(0x80 | ((v >> 6) & 0x3F)); o[3] = (char)(0x80 | (v & 0x3F));
  return 4;
}

}  // namespace

bool UiText::isAscii(const char *s) {
  for (; s && *s; s++) if ((unsigned char)*s >= 0x80) return false;
  return true;
}

void UiText::visual(const char *in, char *out, size_t cap) {
  if (!cap) return;
  out[0] = 0;
  if (!in) return;
  if (isAscii(in)) { strlcpy(out, in, cap); return; }
  constexpr int MAX = 96;
  uint32_t cp[MAX];
  Cls cl[MAX];
  int n = 0;
  const uint8_t *s = (const uint8_t *)in;
  const size_t len = strlen(in);
  for (size_t i = 0; i < len && n < MAX;) {                 // UTF-8 -> code points (bad bytes are skipped)
    const uint8_t c = s[i];
    uint32_t v;
    int l;
    if (c < 0x80) { v = c; l = 1; }
    else if ((c & 0xE0) == 0xC0) { v = c & 0x1F; l = 2; }
    else if ((c & 0xF0) == 0xE0) { v = c & 0x0F; l = 3; }
    else if ((c & 0xF8) == 0xF0) { v = c & 0x07; l = 4; }
    else { i++; continue; }
    if (i + l > len) break;
    for (int k = 1; k < l; k++) v = (v << 6) | (s[i + k] & 0x3F);
    cl[n] = classify(v);
    cp[n++] = v;
    i += l;
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
    const Cls r = left == right ? left : para;
    for (int k = i; k < b; k++) if (cl[k] == C_NEUTRAL) cl[k] = r;
  }
  struct Run { int s, e; Cls c; } runs[MAX];
  int nr = 0;
  for (int i = 0; i < n;) {
    int j = i;
    while (j < n && cl[j] == cl[i]) j++;
    runs[nr++] = { i, j, cl[i] };
    i = j;
  }
  size_t o = 0;
  auto put = [&](uint32_t v) {
    char b[4];
    const int l = encode(v, b);
    if (o + l >= cap) return;
    memcpy(out + o, b, l);
    o += l;
  };
  auto emit = [&](const Run &r) {
    if (r.c == C_R) for (int k = r.e - 1; k >= r.s; k--) put(mirror(cp[k]));
    else for (int k = r.s; k < r.e; k++) put(cp[k]);
  };
  if (para == C_R) for (int r = nr - 1; r >= 0; r--) emit(runs[r]);
  else for (int r = 0; r < nr; r++) emit(runs[r]);
  out[o] = 0;
}

void UiText::setName(lv_obj_t *label, const char *utf8) {
  char v[192];
  visual(utf8, v, sizeof(v));
  if (strcmp(lv_label_get_text(label), v) != 0) lv_label_set_text(label, v);
}
