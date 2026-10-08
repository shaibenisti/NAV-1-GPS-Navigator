// =============================================================================
//  Odometer  -  the Drive dashboard's trip computer: distance, moving time, average and top speed
//  since the last reset (or since start-up). Counts whenever there is a GPS fix, recording or not;
//  RAM only. Movement below MOVE_KMH is ignored, so a standing device does not collect GPS jitter.
// =============================================================================
#pragma once

#include <Arduino.h>

namespace Odometer {
  constexpr float MOVE_KMH = 2.0f;

  void update();                         // UI loop (works once per second)
  void reset();
  float distanceKm();
  uint32_t movingS();
  uint32_t sinceS();                     // since the reset / start-up
  float maxKmh();
  float avgKmh();                        // moving average, 0 = not enough data
}
