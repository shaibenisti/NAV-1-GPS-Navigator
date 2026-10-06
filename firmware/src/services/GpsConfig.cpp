#include "GpsConfig.h"

#include "Settings.h"
#include "../GpsLink.h"
#include "../../config.h"

namespace {

// ---- UBX identifiers (u-blox M8, protocol 18) ------------------------------------------------
constexpr uint8_t CLS_ACK = 0x05, ACK_NAK = 0x00, ACK_ACK = 0x01;
constexpr uint8_t CLS_CFG = 0x06, CFG_MSG = 0x01, CFG_RST = 0x04, CFG_CFG = 0x09, CFG_NMEA = 0x17, CFG_GNSS = 0x3E;
constexpr uint8_t NMEA_CLS = 0xF0, NMEA_GLL = 0x01, NMEA_VTG = 0x05;
constexpr uint8_t GNSS_ID_GPS = 0, GNSS_ID_SBAS = 1, GNSS_ID_GAL = 2, GNSS_ID_GLO = 6, GNSS_ID_QZSS = 5;
constexpr uint8_t NMEA_V41 = 0x41;
// CFG-CFG sections touched by the package: MSG (bit 1), NAV incl. NMEA (bit 3), RXM incl. GNSS (bit 4)
constexpr uint32_t CFG_SECTIONS = 0x02 | 0x08 | 0x10;
constexpr uint8_t DEV_BBR = 0x01;

constexpr uint32_t REPLY_MS = 1200, RESET_MS = 6000;   // wait for a reply / for the start-up banner
constexpr uint32_t POLL_SETTLE_MS = 250;               // a CFG poll is followed by its own ACK: let it pass
constexpr uint32_t GNSS_SETTLE_MS = 500;               // manual: wait 0.5 s after CFG-GNSS is acknowledged
constexpr uint32_t BOOT_SETTLE_MS = 1500;              // module ready after its banner
constexpr uint32_t FIRST_CHECK_MS = 3000;              // after NAV-1 start

GpsLink *s_link = nullptr;
GpsConfig::State s_state = GpsConfig::State::Waiting;
String s_error, s_summary;
uint32_t s_checkAt = 0;          // millis() of the next scheduled check (0 = none)
uint32_t s_bannersSeen = 0;
bool s_failedLatch = false;      // a transaction failed: only report until setProfile() / next boot

// ---- what the module sent (UBX handler, called from GpsLink::update in the UI loop) ----------
struct Rx {
  uint32_t ackSeq; uint8_t ackCls, ackId; bool ackOk;
  uint32_t gnssSeq; uint8_t gnss[4 + 8 * 8]; uint16_t gnssLen;
  uint32_t nmeaSeq; uint8_t nmea[20]; uint16_t nmeaLen;
  uint32_t msgSeq; uint8_t msgCls, msgId, msgRate;       // CFG-MSG poll reply: rate on UART1
} s_rx = {};
uint8_t s_gllRate = 0xFF, s_vtgRate = 0xFF;              // from the last check (0xFF = unknown)

void onUbx(uint8_t cls, uint8_t id, const uint8_t *p, uint16_t len, void *) {
  if (cls == CLS_ACK && len >= 2) {
    s_rx.ackCls = p[0]; s_rx.ackId = p[1]; s_rx.ackOk = id == ACK_ACK; s_rx.ackSeq++;
  } else if (cls == CLS_CFG && id == CFG_GNSS && len >= 4 && len <= sizeof(s_rx.gnss) && (len - 4) % 8 == 0) {
    memcpy(s_rx.gnss, p, len); s_rx.gnssLen = len; s_rx.gnssSeq++;
  } else if (cls == CLS_CFG && id == CFG_NMEA && len >= 4 && len <= sizeof(s_rx.nmea)) {
    memcpy(s_rx.nmea, p, len); s_rx.nmeaLen = len; s_rx.nmeaSeq++;
  } else if (cls == CLS_CFG && id == CFG_MSG && len >= 8) {
    s_rx.msgCls = p[0]; s_rx.msgId = p[1]; s_rx.msgRate = p[3]; s_rx.msgSeq++;   // rate[1] = UART1
  }
}

// ---- one UBX operation at a time -----------------------------------------------------------
enum class Kind : uint8_t { Poll, Set, Reset };
struct Op { Kind kind; uint8_t cls, id; uint16_t len; uint8_t data[4 + 8 * 8]; uint32_t settleMs; const char *what; };
enum class Result : uint8_t { Pending, Ok, Failed };

constexpr int MAX_OPS = 8;
Op s_ops[MAX_OPS];
int s_nOps = 0, s_step = 0, s_tries = 0;
bool s_sent = false;
uint32_t s_sentMs = 0, s_settleUntil = 0, s_seqAtSend = 0, s_bannersAtSend = 0;
String s_opError;

enum class Phase : uint8_t { None, Check, Apply, Revert } s_phase = Phase::None;
// Why the current check runs: a plain check, or verification after our own apply / revert.
enum class Verify : uint8_t { None, AfterApply, AfterFailRevert, AfterFactoryRevert } s_verify = Verify::None;

Op poll(uint8_t cls, uint8_t id, const uint8_t *d, uint16_t len, const char *what) {
  Op o = {}; o.kind = Kind::Poll; o.cls = cls; o.id = id; o.len = len; if (len) memcpy(o.data, d, len);
  o.settleMs = POLL_SETTLE_MS; o.what = what; return o;
}
Op set(uint8_t cls, uint8_t id, const uint8_t *d, uint16_t len, uint32_t settleMs, const char *what) {
  Op o = {}; o.kind = Kind::Set; o.cls = cls; o.id = id; o.len = len; memcpy(o.data, d, len);
  o.settleMs = settleMs; o.what = what; return o;
}

uint32_t respSeq(const Op &o) {
  if (o.kind == Kind::Set) return s_rx.ackSeq;
  if (o.id == CFG_GNSS) return s_rx.gnssSeq;
  if (o.id == CFG_NMEA) return s_rx.nmeaSeq;
  return s_rx.msgSeq;
}

void start(Phase ph) { s_phase = ph; s_step = 0; s_tries = 0; s_sent = false; s_settleUntil = 0; }

// Advances the current operation. Ok / Failed once it is finished (s_opError says why).
Result runOp(uint32_t now) {
  const Op &o = s_ops[s_step];
  if (!s_sent) {
    s_seqAtSend = respSeq(o);
    s_bannersAtSend = s_link->stats().banners;
    if (o.kind == Kind::Reset) s_link->expectRestart(RESET_MS);
    const char *err = s_link->sendUbxOnce(GPS_TX_TEST_PIN, o.cls, o.id, o.data, o.len);
    if (*err) { s_opError = String(o.what) + ": not sent (" + err + ")"; return Result::Failed; }
    s_sent = true;
    s_sentMs = now;
    return Result::Pending;
  }
  bool done = false;
  if (o.kind == Kind::Reset) {
    done = s_link->stats().banners != s_bannersAtSend;
  } else if (o.kind == Kind::Poll) {
    done = respSeq(o) != s_seqAtSend && (o.id != CFG_MSG || (s_rx.msgCls == o.data[0] && s_rx.msgId == o.data[1]));
  } else if (s_rx.ackSeq != s_seqAtSend) {                 // Set: an ACK/NAK arrived
    if (s_rx.ackCls == o.cls && s_rx.ackId == o.id) {
      if (!s_rx.ackOk) { s_opError = String(o.what) + ": rejected by the module (NAK)"; return Result::Failed; }
      done = true;
    } else {
      s_seqAtSend = s_rx.ackSeq;                          // an ACK for something else: keep waiting
    }
  }
  if (done) return Result::Ok;
  if (now - s_sentMs < (o.kind == Kind::Reset ? RESET_MS : REPLY_MS)) return Result::Pending;
  // timeout: polls may be repeated (read-only), a set once more, a reset never
  const int maxTries = o.kind == Kind::Poll ? 3 : o.kind == Kind::Set ? 2 : 1;
  if (++s_tries < maxTries) { s_sent = false; return Result::Pending; }
  s_opError = String(o.what) + (o.kind == Kind::Reset ? ": module did not restart" : ": no reply from the module");
  return Result::Failed;
}

// ---- reading the polled settings ------------------------------------------------------------
bool gnssOn(uint8_t id) {
  for (uint16_t off = 4; off + 8 <= s_rx.gnssLen; off += 8)
    if (s_rx.gnss[off] == id) return s_rx.gnss[off + 4] & 0x01;
  return false;
}
uint8_t nmeaVersion() { return s_rx.nmeaLen >= 2 ? s_rx.nmea[1] : 0; }

String summarize() {
  String g;
  const struct { uint8_t id; const char *n; } sys[] = { {GNSS_ID_GPS, "GPS"}, {GNSS_ID_GLO, "GLO"}, {GNSS_ID_GAL, "GAL"},
                                                          {3, "BDS"}, {GNSS_ID_SBAS, "SBAS"}, {GNSS_ID_QZSS, "QZSS"} };
  for (const auto &s : sys) if (gnssOn(s.id)) { if (g.length()) g += '+'; g += s.n; }
  char b[96];
  snprintf(b, sizeof(b), "GNSS %s, NMEA %X.%X, GLL %u, VTG %u", g.c_str(), nmeaVersion() >> 4, nmeaVersion() & 0x0F,
           (unsigned)s_gllRate, (unsigned)s_vtgRate);
  return b;
}

bool isPackage() {
  return gnssOn(GNSS_ID_GPS) && gnssOn(GNSS_ID_GAL) && !gnssOn(GNSS_ID_GLO) && !gnssOn(GNSS_ID_SBAS) &&
         nmeaVersion() == NMEA_V41 && s_gllRate == 0 && s_vtgRate == 0;
}
// The module's own defaults (UBX-CFG-CFG load): GLONASS + SBAS on, Galileo off, GLL/VTG on, not NMEA 4.1.
bool isFactory() {
  return gnssOn(GNSS_ID_GPS) && gnssOn(GNSS_ID_GLO) && gnssOn(GNSS_ID_SBAS) && !gnssOn(GNSS_ID_GAL) &&
         nmeaVersion() != NMEA_V41 && s_gllRate > 0 && s_vtgRate > 0;
}

// ---- building the transactions --------------------------------------------------------------
void queueCheck() {
  const uint8_t gll[2] = { NMEA_CLS, NMEA_GLL }, vtg[2] = { NMEA_CLS, NMEA_VTG };
  s_nOps = 0;
  s_ops[s_nOps++] = poll(CLS_CFG, CFG_GNSS, nullptr, 0, "poll CFG-GNSS");
  s_ops[s_nOps++] = poll(CLS_CFG, CFG_NMEA, nullptr, 0, "poll CFG-NMEA");
  s_ops[s_nOps++] = poll(CLS_CFG, CFG_MSG, gll, 2, "poll CFG-MSG GLL");
  s_ops[s_nOps++] = poll(CLS_CFG, CFG_MSG, vtg, 2, "poll CFG-MSG VTG");
}

void addCfgCfg(uint32_t clearM, uint32_t saveM, uint32_t loadM, const char *what) {
  uint8_t d[13];
  for (int i = 0; i < 4; i++) { d[i] = clearM >> (8 * i); d[4 + i] = saveM >> (8 * i); d[8 + i] = loadM >> (8 * i); }
  d[12] = DEV_BBR;
  s_ops[s_nOps++] = set(CLS_CFG, CFG_CFG, d, sizeof(d), 0, what);
}

void addReset() {
  Op o = {};
  o.kind = Kind::Reset; o.cls = CLS_CFG; o.id = CFG_RST; o.len = 4;
  o.data[0] = 0x00; o.data[1] = 0x00;              // navBbrMask 0: hot start (keep orbit data)
  o.data[2] = 0x00;                                // resetMode 0: hardware reset (watchdog) immediately
  o.settleMs = BOOT_SETTLE_MS; o.what = "CFG-RST hardware reset";
  s_ops[s_nOps++] = o;
}

// Package from the polled settings (read-modify-write). false = cannot be built (s_opError says why).
bool queueApply() {
  if (s_rx.gnssLen < 4 || s_rx.gnss[0] != 0 || s_rx.nmeaLen < 4) { s_opError = "unexpected CFG-GNSS/CFG-NMEA reply"; return false; }
  uint8_t g[sizeof(s_rx.gnss)];
  memcpy(g, s_rx.gnss, s_rx.gnssLen);
  bool haveGal = false;
  unsigned reserved = 0;
  for (uint16_t off = 4; off + 8 <= s_rx.gnssLen; off += 8) {
    const uint8_t id = g[off];
    uint32_t flags = g[off + 4] | (g[off + 5] << 8) | ((uint32_t)g[off + 6] << 16) | ((uint32_t)g[off + 7] << 24);
    if (id == GNSS_ID_GPS) flags |= 1;
    if (id == GNSS_ID_SBAS || id == GNSS_ID_GLO) flags &= ~1u;
    if (id == GNSS_ID_GAL) {
      haveGal = true;
      flags |= 1;
      if (!((flags >> 16) & 0xFF)) flags |= 0x01u << 16;   // signal E1
      if (g[off + 2] < 4 || g[off + 2] > 8) g[off + 2] = 8;  // maxTrkCh: >= 4 (manual), <= 8 recommended
      if (g[off + 1] > g[off + 2]) g[off + 1] = g[off + 2];  // resTrkCh <= maxTrkCh
    }
    for (int i = 0; i < 4; i++) g[off + 4 + i] = flags >> (8 * i);
    reserved += g[off + 1];
  }
  if (!haveGal) { s_opError = "module offers no Galileo block in CFG-GNSS"; return false; }
  if (reserved > g[2]) { s_opError = "reserved tracking channels exceed channels in use"; return false; }
  uint8_t n[sizeof(s_rx.nmea)];
  memcpy(n, s_rx.nmea, s_rx.nmeaLen);
  n[1] = NMEA_V41;
  const uint8_t gll[3] = { NMEA_CLS, NMEA_GLL, 0 }, vtg[3] = { NMEA_CLS, NMEA_VTG, 0 };
  s_nOps = 0;
  s_ops[s_nOps++] = set(CLS_CFG, CFG_GNSS, g, s_rx.gnssLen, GNSS_SETTLE_MS, "CFG-GNSS GPS+Galileo");
  s_ops[s_nOps++] = set(CLS_CFG, CFG_NMEA, n, s_rx.nmeaLen, 0, "CFG-NMEA 4.1");
  s_ops[s_nOps++] = set(CLS_CFG, CFG_MSG, gll, 3, 0, "CFG-MSG GLL off");
  s_ops[s_nOps++] = set(CLS_CFG, CFG_MSG, vtg, 3, 0, "CFG-MSG VTG off");
  addCfgCfg(0, CFG_SECTIONS, 0, "CFG-CFG save to BBR");
  addReset();
  return true;
}

void queueRevert() {
  s_nOps = 0;
  addCfgCfg(CFG_SECTIONS, 0, CFG_SECTIONS, "CFG-CFG clear + load defaults");
  addReset();
}

void fail(const String &why) {
  s_error = why;
  s_failedLatch = true;
  Serial.printf("[GPSCFG] FAILED: %s\n", why.c_str());
}

void beginRevert(const String &why) {
  fail(why);
  Serial.println("[GPSCFG] reverting the module to its defaults");
  queueRevert();
  s_verify = Verify::AfterFailRevert;
  s_state = GpsConfig::State::Reverting;
  start(Phase::Revert);
}

const char *REVERT_NOT_VERIFIED = "revert NOT verified: switch NAV-1 off and on to restore the module's defaults";

// After a check: decide what to do. Every path ends in a verified state, one transaction, or Failed.
void evaluate() {
  s_summary = summarize();
  const int prof = Settings::gpsProfile();
  Serial.printf("[GPSCFG] module: %s (profile %s)\n", s_summary.c_str(), prof ? "galileo" : "factory");
  const Verify v = s_verify;
  s_verify = Verify::None;
  s_phase = Phase::None;
  if (v == Verify::AfterFailRevert) {                    // end of a failure: report, never retry
    s_state = GpsConfig::State::Failed;
    s_error += isFactory() ? " - module back on its defaults (verified)" : String(" - ") + REVERT_NOT_VERIFIED;
    Serial.printf("[GPSCFG] %s\n", s_error.c_str());
    return;
  }
  if (v == Verify::AfterFactoryRevert) {                 // a requested rollback
    if (isFactory()) { s_state = GpsConfig::State::Factory; s_error = ""; Serial.println("[GPSCFG] defaults restored (verified)"); }
    else { s_state = GpsConfig::State::Failed; fail(REVERT_NOT_VERIFIED); }
    return;
  }
  if (prof == 1 && isPackage()) {
    s_state = GpsConfig::State::Applied;
    s_error = "";
    Serial.println(v == Verify::AfterApply ? "[GPSCFG] package applied and verified after the module restart"
                                           : "[GPSCFG] package already active (verified)");
    return;
  }
  if (prof == 0 && isFactory()) { s_state = GpsConfig::State::Factory; s_error = ""; Serial.println("[GPSCFG] factory settings (verified)"); return; }
  if (v == Verify::AfterApply) { beginRevert("verification after the module restart failed: " + s_summary); return; }
  if (s_failedLatch) { s_state = GpsConfig::State::Failed; return; }   // report only
  if (prof == 1) {
    if (!queueApply()) { s_state = GpsConfig::State::Failed; fail(s_opError + " - nothing sent, module unchanged"); return; }
    Serial.println("[GPSCFG] applying GPS + Galileo, GLONASS/SBAS off, NMEA 4.1, GLL/VTG off");
    s_verify = Verify::AfterApply;
    s_state = GpsConfig::State::Applying;
    start(Phase::Apply);
  } else {
    Serial.println("[GPSCFG] module is not on its defaults: restoring them");
    queueRevert();
    s_verify = Verify::AfterFactoryRevert;
    s_state = GpsConfig::State::Reverting;
    start(Phase::Revert);
  }
}

void startCheck() {
  queueCheck();
  if (s_state != GpsConfig::State::Applying && s_state != GpsConfig::State::Reverting) s_state = GpsConfig::State::Checking;
  start(Phase::Check);
}

}  // namespace

void GpsConfig::begin(GpsLink &link) {
  s_link = &link;
  link.setUbxHandler(onUbx, nullptr);
  s_bannersSeen = link.stats().banners;
  s_checkAt = max<uint32_t>(millis() + 500, FIRST_CHECK_MS);
  Serial.printf("[GPSCFG] profile %s, first check at %u ms\n", Settings::gpsProfile() ? "galileo" : "factory", (unsigned)s_checkAt);
}

void GpsConfig::update() {
  if (!s_link) return;
  const uint32_t now = millis();
  const uint32_t banners = s_link->stats().banners;
  if (banners != s_bannersSeen) {                        // the module (re)started
    s_bannersSeen = banners;
    if (s_phase == Phase::None) {                        // not our own reset: check it once it is ready
      Serial.println("[GPSCFG] module start-up seen: checking its settings");
      s_checkAt = now + BOOT_SETTLE_MS;
    }
  }
  if (s_phase == Phase::None) {
    if (s_checkAt && (int32_t)(now - s_checkAt) >= 0 && s_link->linkUp(GPS_LINK_TIMEOUT_MS)) { s_checkAt = 0; startCheck(); }
    return;
  }
  if (s_settleUntil) {
    if ((int32_t)(now - s_settleUntil) < 0) return;
    s_settleUntil = 0;
  }
  const Result r = runOp(now);
  if (r == Result::Pending) return;
  if (r == Result::Failed) {
    const Phase ph = s_phase;
    s_phase = Phase::None;
    const Verify v = s_verify;
    s_verify = Verify::None;
    if (ph == Phase::Check && v == Verify::None) {       // plain check: nothing was sent that changes the module
      s_state = State::Failed;
      fail(s_opError + " - nothing changed");
    } else if (ph == Phase::Revert || v == Verify::AfterFailRevert || v == Verify::AfterFactoryRevert) {
      s_state = State::Failed;                           // the revert itself failed: never loop
      fail((v == Verify::AfterFailRevert && s_error.length() ? s_error + "; then " : String()) + s_opError + " - " +
           REVERT_NOT_VERIFIED);
    } else {
      beginRevert(s_opError);                            // apply or its verification failed
    }
    return;
  }
  // Ok
  const Op &o = s_ops[s_step];
  if (o.kind == Kind::Poll && o.id == CFG_MSG) (o.data[1] == NMEA_GLL ? s_gllRate : s_vtgRate) = s_rx.msgRate;
  if (o.settleMs) s_settleUntil = now + o.settleMs;
  s_step++;
  s_tries = 0;
  s_sent = false;
  if (s_step < s_nOps) return;
  // phase finished
  if (s_phase == Phase::Check) { evaluate(); return; }
  // Apply / Revert end with the module restart: verify its settings (after the settle time)
  const Verify v = s_verify;
  queueCheck();
  start(Phase::Check);
  s_verify = v;
  s_settleUntil = now + BOOT_SETTLE_MS;
}

int GpsConfig::profile() { return Settings::gpsProfile(); }

void GpsConfig::setProfile(int p) {
  Settings::setGpsProfile(p ? 1 : 0);
  s_failedLatch = false;
  s_error = "";
  if (s_phase == Phase::None) { s_state = State::Waiting; s_checkAt = millis(); }
  else s_checkAt = millis() + 100;                       // after the running transaction
  Serial.printf("[GPSCFG] profile set to %s\n", p ? "galileo" : "factory");
}

GpsConfig::State GpsConfig::state() { return s_state; }
String GpsConfig::lastError() { return s_error; }
String GpsConfig::moduleSummary() { return s_summary; }

const char *GpsConfig::stateName(State s) {
  switch (s) {
    case State::Waiting: return "waiting";
    case State::Checking: return "checking";
    case State::Applying: return "applying";
    case State::Reverting: return "reverting";
    case State::Applied: return "GPS + Galileo package active (verified)";
    case State::Factory: return "factory settings (verified)";
    case State::Failed: return "FAILED";
  }
  return "?";
}

void GpsConfig::status(Print &o) {
  const GpsLink::Stats &s = s_link->stats();
  o.printf("[GPSCFG] profile %s, state %s\n", profile() ? "galileo" : "factory", stateName(s_state));
  if (s_summary.length()) o.printf("[GPSCFG] module: %s\n", s_summary.c_str());
  if (s_error.length()) o.printf("[GPSCFG] error: %s\n", s_error.c_str());
  o.printf("[GPSCFG] UBX frames ok %u bad %u, module start-ups %u (ordered by NAV-1 %u, unexpected %u)\n",
           (unsigned)s.ubxFrames, (unsigned)s.ubxBad, (unsigned)s.banners, (unsigned)s.commandedRestarts,
           (unsigned)s.receiverRestarts);
}
