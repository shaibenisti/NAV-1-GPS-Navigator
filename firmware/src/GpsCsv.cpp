#include "GpsCsv.h"

#include <stdarg.h>

const char *GpsCsv::header() {
  return "uptime_s,utc_date,utc_time,link,fix,rmc,fix_q,sats_used,sats_view_gps,sats_view_glo,sats_view_gal,"
         "lat,lon,loc_age_ms,speed_kmh,course_deg,alt_m,hdop,ttff_s,"
         "bytes,nmea_ok,bad_crc,overflow,parser_crc_fail,free_heap";
}

// Appends ",<value>" or "," (empty) to the row being built.
namespace {
struct Row {
  char *buf; size_t size; size_t len;
  void add(const char *fmt, ...) {
    if (len >= size) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + len, size - len, fmt, ap);
    va_end(ap);
    if (n > 0) len += (size_t)n;
  }
  void empty() { add(","); }
};
}

const char *GpsCsv::row(char *buf, size_t size, uint32_t uptimeMs,
                        const GpsData &d, const GpsLink::Stats &link) {
  Row r{buf, size, 0};
  buf[0] = '\0';

  r.add("%.1f", uptimeMs / 1000.0);
  if (d.dateValid) r.add(",%04u-%02u-%02u", d.year, d.month, d.day); else r.empty();
  if (d.timeValid) r.add(",%02u:%02u:%02u", d.hour, d.minute, d.second); else r.empty();
  r.add(",%s,%s,%c,%u", d.linkUp ? "UP" : "DOWN", d.fix ? "FIX" : "NOFIX", d.rmcStatus, d.fixQuality);
  r.add(",%u,%u,%u,%u", (unsigned)d.satsUsed, (unsigned)d.satsViewGps, (unsigned)d.satsViewGlonass, (unsigned)d.satsViewGalileo);

  if (d.locValid) r.add(",%.7f,%.7f,%u", d.lat, d.lng, (unsigned)d.locAgeMs); else { r.empty(); r.empty(); r.empty(); }
  if (d.speedValid)  r.add(",%.2f", d.speedKmh);  else r.empty();
  if (d.courseValid) r.add(",%.1f", d.courseDeg); else r.empty();
  if (d.altValid)    r.add(",%.1f", d.altM);      else r.empty();
  if (d.hdopValid)   r.add(",%.2f", d.hdop);      else r.empty();
  if (d.ttffMs)      r.add(",%.1f", d.ttffMs / 1000.0); else r.empty();

  r.add(",%u,%u,%u,%u,%u,%u", (unsigned)link.bytes, (unsigned)link.sentences,
        (unsigned)link.badChecksum, (unsigned)link.overflows,
        (unsigned)d.parserChecksumFail, (unsigned)ESP.getFreeHeap());
  return buf;
}
