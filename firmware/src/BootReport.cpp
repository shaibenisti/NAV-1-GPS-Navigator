#include "BootReport.h"

#include <Arduino.h>
#include "../config.h"

static constexpr uint32_t EXPECTED_FLASH = 16UL * 1024 * 1024;
static constexpr uint32_t EXPECTED_PSRAM = 8UL * 1024 * 1024;

bool printBootReport() {
  const uint32_t flash = ESP.getFlashChipSize();
  const uint32_t psram = psramFound() ? ESP.getPsramSize() : 0;
  const bool ok = (flash == EXPECTED_FLASH) && (psram == EXPECTED_PSRAM);

  Serial.println();
  Serial.println("==================================================");
  Serial.printf(" %s  v%s  (%s, built %s %s)\n", APP_NAME, FW_VERSION, FW_GIT_DESCRIBE, __DATE__, __TIME__);
  Serial.printf(" Chip  : %s rev %d @ %u MHz\n", ESP.getChipModel(), ESP.getChipRevision(), (unsigned)ESP.getCpuFreqMHz());
  Serial.printf(" Flash : %u MB\n", (unsigned)(flash / (1024 * 1024)));
  Serial.printf(" PSRAM : %u bytes\n", (unsigned)psram);
  Serial.printf(" Board config: %s\n", ok ? "OK" : "MISMATCH - check FQBN (see docs/HARDWARE.md)");
  Serial.println("==================================================");
  return ok;
}
