// BOARD_CHECK - one-shot sanity check of the canonical FQBN and the GPS link.
// Expected (2026-09-27): ESP32-S3 rev 2 240 MHz, Flash 16 MB, PSRAM YES 8388608,
// App partition 3072 KB, GPS IO17@9600 ~675 bytes / ~24 sentences in 3 s.
#include <Arduino.h>
#include <esp_partition.h>

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== BOARD_CHECK ===");
  Serial.printf("Chip: %s rev %d, %d MHz\n", ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz());
  Serial.printf("Flash: %u MB, mode %d\n", ESP.getFlashChipSize() / (1024 * 1024), ESP.getFlashChipMode());
  Serial.printf("PSRAM: %s, %u bytes\n", psramFound() ? "YES" : "NO", ESP.getPsramSize());
  const esp_partition_t *app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  Serial.printf("App partition: %u KB\n", app ? app->size / 1024 : 0);

  Serial1.begin(9600, SERIAL_8N1, 17, -1);
  uint32_t n = 0, dollars = 0, t0 = millis();
  while (millis() - t0 < 3000) {
    while (Serial1.available()) { if (Serial1.read() == '$') dollars++; n++; }
  }
  Serial.printf("GPS IO17@9600: %u bytes, %u sentences in 3 s\n", n, dollars);
  Serial.println("=== DONE ===");
}

void loop() {}
