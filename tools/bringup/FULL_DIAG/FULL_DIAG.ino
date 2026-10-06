#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>

#define SD_CS   10
#define SD_SCK  12
#define SD_MISO 13
#define SD_MOSI 11

void printLine() {
  Serial.println("--------------------------------------------------");
}

void testChipInfo() {
  printLine();
  Serial.println("CHIP / MEMORY TEST");

  Serial.printf("Chip model: %s\n", ESP.getChipModel());
  Serial.printf("Chip revision: %d\n", ESP.getChipRevision());
  Serial.printf("CPU freq: %d MHz\n", ESP.getCpuFreqMHz());

  Serial.printf("Flash size: %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("Sketch size: %u bytes\n", ESP.getSketchSize());
  Serial.printf("Free sketch space: %u bytes\n", ESP.getFreeSketchSpace());

  Serial.printf("Heap total: %u bytes\n", ESP.getHeapSize());
  Serial.printf("Heap free: %u bytes\n", ESP.getFreeHeap());

  Serial.printf("PSRAM found: %s\n", psramFound() ? "YES" : "NO");
  Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());
  Serial.printf("PSRAM free: %u bytes\n", ESP.getFreePsram());
}

void testSD() {
  printLine();
  Serial.println("SD CARD TEST");
  Serial.printf("Pins: SCK=%d MISO=%d MOSI=%d CS=%d\n", SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, SPI, 1000000)) {
    Serial.println("SD.begin: FAILED");
    return;
  }

  Serial.println("SD.begin: OK");

  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE) {
    Serial.println("Card type: NONE");
    return;
  } else if (cardType == CARD_MMC) {
    Serial.println("Card type: MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("Card type: SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("Card type: SDHC");
  } else {
    Serial.println("Card type: UNKNOWN");
  }

  Serial.printf("Card size: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
  Serial.printf("Filesystem total: %llu MB\n", SD.totalBytes() / (1024ULL * 1024ULL));
  Serial.printf("Filesystem used: %llu MB\n", SD.usedBytes() / (1024ULL * 1024ULL));

  File file = SD.open("/full_diag_test.txt", FILE_WRITE);
  if (!file) {
    Serial.println("Write open: FAILED");
    return;
  }

  file.println("ESP32-S3 FULL DIAG SD WRITE OK");
  file.close();
  Serial.println("Write file: OK");

  file = SD.open("/full_diag_test.txt", FILE_READ);
  if (!file) {
    Serial.println("Read open: FAILED");
    return;
  }

  Serial.print("Read file: ");
  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();

  Serial.println("SD test: DONE");
}

void testWiFi() {
  printLine();
  Serial.println("WIFI TEST");

  WiFi.mode(WIFI_AP_STA);
  delay(500);

  bool apOk = WiFi.softAP("ESP32S3_DIAG", "12345678");

  Serial.printf("WiFi AP: %s\n", apOk ? "OK" : "FAILED");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  Serial.println("Scanning WiFi networks...");
  int n = WiFi.scanNetworks(false, true);

  if (n < 0) {
    Serial.println("WiFi scan: FAILED");
    return;
  }

  Serial.printf("Networks found: %d\n", n);

  for (int i = 0; i < n && i < 10; i++) {
    Serial.printf("%02d | RSSI: %4d dBm | CH: %2d | %s\n",
                  i + 1,
                  WiFi.RSSI(i),
                  WiFi.channel(i),
                  WiFi.SSID(i).c_str());
  }

  Serial.println("WiFi test: DONE");
}

void testBLE() {
  printLine();
  Serial.println("BLE TEST");

  BLEDevice::init("ESP32S3_DIAG_BLE");

  BLEServer *server = BLEDevice::createServer();
  (void)server;

  BLEAdvertising *advertising = BLEDevice::getAdvertising();

  BLEAdvertisementData advData;
  advData.setName("ESP32S3_DIAG_BLE");
  advData.setCompleteServices(BLEUUID((uint16_t)0x180A));

  advertising->setAdvertisementData(advData);
  advertising->setScanResponse(true);
  advertising->start();

  Serial.println("BLE advertising: OK");
  Serial.println("BLE name: ESP32S3_DIAG_BLE");
  Serial.println("Note: ESP32-S3 supports BLE, not Bluetooth Classic.");
}

void scanI2CBus(int sda, int scl) {
  printLine();
  Serial.printf("I2C SCAN: SDA=%d SCL=%d\n", sda, scl);

  Wire.end();
  delay(100);
  Wire.begin(sda, scl, 100000);
  delay(100);

  int found = 0;

  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();

    if (error == 0) {
      Serial.printf("I2C device found at 0x%02X\n", address);
      found++;
    }
  }

  if (found == 0) {
    Serial.println("No I2C devices found on this pin pair.");
  } else {
    Serial.printf("I2C devices found: %d\n", found);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2500);

  Serial.println();
  Serial.println("ESP32-S3 FULL DIAGNOSTIC");
  Serial.println("Board target: ESP32-8048S043 style board");
  Serial.println("Serial baud: 115200");

  testChipInfo();
  testSD();
  testWiFi();
  testBLE();

  // Common touch/I2C candidate on many ESP32-S3 display boards.
  scanI2CBus(19, 20);

  printLine();
  Serial.println("FULL DIAG DONE");
  Serial.println("Check from phone:");
  Serial.println("WiFi network: ESP32S3_DIAG / password 12345678");
  Serial.println("BLE name: ESP32S3_DIAG_BLE");
}

void loop() {
  delay(5000);
  Serial.println("Alive...");
}
