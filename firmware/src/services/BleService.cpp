#include "BleService.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <esp_ipc.h>
#include <esp_heap_caps.h>
#include "Settings.h"
#include "WifiService.h"
#include "../GpsLink.h"
#include "../GpsParser.h"
#include "../../config.h"
#include "../../version.h"

namespace {

const char *UUID_SERVICE  = "4750a000-5344-4556-8000-000000000001";
const char *UUID_STATUS   = "4750a001-5344-4556-8000-000000000001";
const char *UUID_LOCATION = "4750a002-5344-4556-8000-000000000001";
const char *UUID_COMMAND  = "4750a003-5344-4556-8000-000000000001";
const char *UUID_RESPONSE = "4750a004-5344-4556-8000-000000000001";

GpsLink *s_link = nullptr;
GpsParser *s_parser = nullptr;
bool s_enabled = false;
bool s_init = false;
String s_name;
BleService::Stats s_stats = {};
String s_lastCommand;

BLEServer *s_server = nullptr;
BLECharacteristic *s_chStatus = nullptr, *s_chLocation = nullptr, *s_chResponse = nullptr;
uint32_t s_lastLocMs = 0, s_lastStatusMs = 0;

// NimBLE task -> update()
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
volatile int s_connected = 0;
volatile bool s_evConnect = false, s_evDisconnect = false;
char s_cmdBuf[64];
volatile bool s_cmdPending = false;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { s_connected = s_connected + 1; s_evConnect = true; }
  void onDisconnect(BLEServer *) override { if (s_connected > 0) s_connected = s_connected - 1; s_evDisconnect = true; }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    const String v = c->getValue();
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_cmdBuf, v.c_str(), sizeof(s_cmdBuf));
    s_cmdPending = true;
    portEXIT_CRITICAL(&s_mux);
  }
};

ServerCallbacks s_serverCb;
CommandCallbacks s_commandCb;

String statusJson() {
  const GpsData d = s_parser->snapshot(s_link->linkUp(GPS_LINK_TIMEOUT_MS));
  char buf[180];
  snprintf(buf, sizeof(buf),
           "{\"fw\":\"%s\",\"up\":%lu,\"gps\":\"%s\",\"sats\":%u,\"wifi\":\"%s\",\"ip\":\"%s\"}",
           FW_VERSION, (unsigned long)(millis() / 1000),
           !d.linkUp ? "no link" : (d.fix ? "fix" : "no fix"), (unsigned)d.satsUsed,
           WifiService::stateName(WifiService::state()), WifiService::ip().c_str());
  return String(buf);
}

String locationJson() {
  const GpsData d = s_parser->snapshot(s_link->linkUp(GPS_LINK_TIMEOUT_MS));
  char buf[180];
  if (!d.fix) {
    snprintf(buf, sizeof(buf), "{\"fix\":false,\"sats\":%u}", (unsigned)d.satsUsed);
  } else {
    snprintf(buf, sizeof(buf),
             "{\"fix\":true,\"lat\":%.6f,\"lon\":%.6f,\"spd\":%.1f,\"crs\":%.0f,\"alt\":%.1f,\"hdop\":%.2f,"
             "\"utc\":\"%02u:%02u:%02u\"}",
             d.lat, d.lng, d.speedValid ? d.speedKmh : 0.0, d.courseValid ? d.courseDeg : 0.0,
             d.altValid ? d.altM : 0.0, d.hdopValid ? d.hdop : 0.0, d.hour, d.minute, d.second);
  }
  return String(buf);
}

void notify(BLECharacteristic *c, const String &value) {
  c->setValue(value);
  if (s_connected > 0) { c->notify(); s_stats.notifies++; }
}

void startStackNow();
volatile bool s_bigIpc = false;          // BT init running: divert its IPC calls (see below)
volatile uint32_t s_bigIpcCalls = 0;

void startStack() {
  if (s_init) return;
  s_bigIpcCalls = 0;
  s_bigIpc = true;
  startStackNow();
  s_bigIpc = false;
  Serial.printf("[BLE] controller set-up: %u IPC call(s) run on a 4 KB stack\n", (unsigned)s_bigIpcCalls);
}

void startStackNow() {
  BLEDevice::init(s_name);
  BLEDevice::setMTU(185);
  s_server = BLEDevice::createServer();
  s_server->setCallbacks(&s_serverCb);
  BLEService *svc = s_server->createService(UUID_SERVICE);
  s_chStatus = svc->createCharacteristic(UUID_STATUS, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  s_chLocation = svc->createCharacteristic(UUID_LOCATION, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLECharacteristic *cmd = svc->createCharacteristic(UUID_COMMAND,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  cmd->setCallbacks(&s_commandCb);
  s_chResponse = svc->createCharacteristic(UUID_RESPONSE, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  s_chStatus->setValue(statusJson());
  s_chLocation->setValue(locationJson());
  s_chResponse->setValue("ready");
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(UUID_SERVICE);
  adv->setScanResponse(true);
  s_init = true;
}

void handleCommand(const char *cmd) {
  s_stats.commands++;
  s_lastCommand = cmd;
  String reply;
  if (!strcmp(cmd, "ping")) reply = "pong";
  else if (!strcmp(cmd, "version")) reply = String(FW_VERSION);
  else if (!strcmp(cmd, "status")) reply = statusJson();
  else reply = String("unknown: ") + cmd;
  notify(s_chResponse, reply);
}

}  // namespace

// ---- BT controller IPC on a real stack --------------------------------------------------
// The BT controller (libbt, pinned to core 0) allocates its interrupt with
// esp_ipc_call_blocking(): that runs in the ipc task, whose stack is 1 KB in the prebuilt
// Arduino libraries (CONFIG_ESP_IPC_TASK_STACK_SIZE). esp_intr_alloc -> heap_caps_malloc takes
// ~720 B of it; an interrupt that fires as the heap lock is released saves ~300 B more ->
// "Stack canary watchpoint triggered (ipc0)" / heap corruption on ~2 % of boots (2026-09-29,
// backtrace ... multi_heap_malloc <- esp_intr_alloc <- btdm_intr_alloc <- ipc_task).
// idf/main/CMakeLists.txt links with -Wl,--wrap=esp_ipc_call_blocking; while BLE initialises, the call runs
// in a short-lived task with a 4 KB stack on the requested core instead (same semantics:
// another task on that core, the caller blocks until it returns). Other IPC calls are untouched.
extern "C" esp_err_t __real_esp_ipc_call_blocking(uint32_t cpu_id, esp_ipc_func_t func, void *arg);

extern "C" esp_err_t __wrap_esp_ipc_call_blocking(uint32_t cpu_id, esp_ipc_func_t func, void *arg) {
  if (!s_bigIpc || cpu_id >= portNUM_PROCESSORS) return __real_esp_ipc_call_blocking(cpu_id, func, arg);
  struct Job { esp_ipc_func_t func; void *arg; SemaphoreHandle_t done; };
  Job job = { func, arg, xSemaphoreCreateBinary() };
  auto run = [](void *p) {
    Job *j = (Job *)p;
    j->func(j->arg);
    xSemaphoreGive(j->done);
    vTaskDelete(nullptr);
  };
  if (!job.done || xTaskCreatePinnedToCore(run, "bt_ipc", 4096, &job, configMAX_PRIORITIES - 1, nullptr, cpu_id) != pdPASS) {
    if (job.done) vSemaphoreDelete(job.done);
    return __real_esp_ipc_call_blocking(cpu_id, func, arg);
  }
  xSemaphoreTake(job.done, portMAX_DELAY);
  vSemaphoreDelete(job.done);
  s_bigIpcCalls = s_bigIpcCalls + 1;
  return ESP_OK;
}

void BleService::begin(GpsLink &link, GpsParser &parser) {
  s_link = &link;
  s_parser = &parser;
  s_name = Settings::deviceName();
  if (Settings::bleEnabled()) setEnabled(true);
}

void BleService::update() {
  if (!s_init) return;
  if (s_evConnect) {
    s_evConnect = false;
    s_stats.connects++;
    // Advertise on: a 2nd client may connect (NimBLE max 3); also keeps us visible.
    if (s_enabled) BLEDevice::startAdvertising();
  }
  if (s_evDisconnect) {
    s_evDisconnect = false;
    s_stats.disconnects++;
    if (s_enabled) BLEDevice::startAdvertising();
  }
  if (s_cmdPending) {
    char cmd[64];
    portENTER_CRITICAL(&s_mux);
    strlcpy(cmd, s_cmdBuf, sizeof(cmd));
    s_cmdPending = false;
    portEXIT_CRITICAL(&s_mux);
    handleCommand(cmd);
  }
  const uint32_t now = millis();
  if (now - s_lastLocMs >= 1000) { s_lastLocMs = now; notify(s_chLocation, locationJson()); }
  if (now - s_lastStatusMs >= 5000) {
    s_lastStatusMs = now;
    notify(s_chStatus, statusJson());
    if (s_connected > 0) s_stats.mtu = BLEDevice::getMTU();
  }
}

// Starting the stack takes ~67 KB of internal RAM (controller + host, host buffers in PSRAM; measured
// 2026-09-30). Without it the init fails and Arduino's BLE layer crashes on a null pointer - so refuse
// with a message instead (Settings shows it) and leave the setting off.
constexpr uint32_t BLE_START_MIN_FREE_KB = 80;
String s_refusal;

void BleService::setEnabled(bool on) {
  if (on && !s_init) {
    const uint32_t freeKb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    if (freeKb < BLE_START_MIN_FREE_KB) {
      s_refusal = "Not enough memory to start Bluetooth (" + String(freeKb) + " KB free, needs " +
                  String(BLE_START_MIN_FREE_KB) + " KB).\nTurn the hotspot off or restart NAV-1, then try again.";
      Serial.printf("[BLE] not started: %u KB internal RAM free, needs %u KB\n", (unsigned)freeKb,
                    (unsigned)BLE_START_MIN_FREE_KB);
      s_enabled = false;
      return;
    }
  }
  s_refusal = "";
  Settings::setBleEnabled(on);
  s_enabled = on;
  if (on) {
    startStack();
    BLEDevice::startAdvertising();
  } else if (s_init) {
    // The stack stays initialised (deinit/re-init is not reliable at runtime); it is not
    // started at the next boot. Stop being visible and drop clients.
    BLEDevice::getAdvertising()->stop();
    if (s_server) {
      for (auto &peer : s_server->getPeerDevices(false)) s_server->disconnect(peer.first);
    }
  }
}

bool BleService::enabled() { return s_enabled; }
bool BleService::initialized() { return s_init; }
bool BleService::advertising() { return s_init && BLEDevice::getAdvertising()->isAdvertising(); }
int BleService::connectedCount() { return s_connected; }
String BleService::name() { return s_name; }
String BleService::lastCommand() { return s_lastCommand; }
String BleService::refusal() { return s_refusal; }
const BleService::Stats &BleService::stats() { return s_stats; }
