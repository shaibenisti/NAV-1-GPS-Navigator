// =============================================================================
//  GpsCsv  -  One CSV row per GpsData snapshot (+ link stats)
//  Invalid values are written as empty fields, never as fake zeros.
// =============================================================================
#pragma once

#include <Arduino.h>
#include "GpsParser.h"
#include "GpsLink.h"

namespace GpsCsv {
  // Column header line (no newline).
  const char *header();

  // Formats one row into buf (no newline). Returns buf.
  const char *row(char *buf, size_t size, uint32_t uptimeMs,
                  const GpsData &d, const GpsLink::Stats &link);
}
