#include "WifiService.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include "Settings.h"

namespace {

constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;  // association + DHCP; BLE shares the radio (scans take ~9 s)
constexpr uint32_t RETRY_MIN_MS = 5000, RETRY_MAX_MS = 30000;
constexpr int MAX_NETWORKS = 20;
constexpr int MAX_TRIES = 3;                    // WiFi.begin() calls per attempt window

WifiService::State s_state = WifiService::State::Off;
WifiService::Stats s_stats = {};
WifiService::Network s_nets[MAX_NETWORKS];
int s_netCount = 0;
uint32_t s_scanSerial = 0;
bool s_scanning = false;
uint32_t s_scanStartMs = 0;

String s_ssid, s_pass;                  // target network
int s_channel = 0;                      // known channel of the target (0 = scan all channels)
bool s_saveOnConnect = false;           // new credentials: persist once connected (never before)
uint32_t s_attemptMs = 0;               // start of the current attempt window
uint32_t s_retryMs = RETRY_MIN_MS;      // back-off after a failed window (5 -> 10 -> 20 -> 30 s)
int s_tries = 0, s_authFails = 0;
bool s_mdns = false;
String s_host, s_error;
bool s_ap = false;                      // hotspot wanted (Settings)
bool s_apUp = false;                    // soft AP running

// Set by the Wi-Fi event task, consumed in update().
volatile bool s_evGotIp = false, s_evLost = false;
volatile uint8_t s_evReason = 0;

void onEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    s_evGotIp = true;
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    s_evReason = info.wifi_sta_disconnected.reason;
    s_evLost = true;
  }
}

bool isAuthFailure(uint8_t r) {
  return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT ||
         r == WIFI_REASON_MIC_FAILURE || r == WIFI_REASON_AUTH_EXPIRE;
}
bool isNotFound(uint8_t r) {
  return r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
         r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD || r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

void stopMdns() {
  if (s_mdns) { MDNS.end(); s_mdns = false; }
}

// (Re)announce "<name>.local" + the web service on every interface that is up.
void startMdns() {
  stopMdns();
  s_mdns = MDNS.begin(s_host.c_str());
  if (s_mdns) MDNS.addService("http", "tcp", 80);             // WebService
}

// Station always; + access point while the hotspot is on (WIFI_AP_STA: the station keeps
// reconnecting to the saved network, the AP follows its channel).
void radioOn() {
  const wifi_mode_t want = s_ap ? WIFI_AP_STA : WIFI_STA;
  if (WiFi.getMode() != want) {
    WiFi.mode(want);
    WiFi.setHostname(s_host.c_str());
  }
  if (s_ap && !s_apUp) {
    const int ch = WiFi.status() == WL_CONNECTED ? WiFi.channel() : (s_channel ? s_channel : 1);
    s_apUp = WiFi.softAP(Settings::deviceName().c_str(), Settings::hotspotPass().c_str(), ch, 0, 4);
    Serial.printf("[WIFI] hotspot '%s' %s, ip %s, channel %d\n", Settings::deviceName().c_str(), s_apUp ? "on" : "FAILED",
                  WiFi.softAPIP().toString().c_str(), ch);
    if (s_apUp) startMdns();
  } else if (!s_ap && s_apUp) {
    s_apUp = false;
  }
}

void cancelScan() {
  if (!s_scanning) return;
  esp_wifi_scan_stop();                  // a connect issued during a scan fails silently
  WiFi.scanDelete();
  s_scanning = false;
}

void begin1() {                          // one WiFi.begin() inside the attempt window
  // A retry must first end the previous try, or WiFi.begin() fails with "sta is connecting,
  // cannot set config" and the retry silently does nothing. The STA_LEAVING (36) /
  // ASSOC_LEAVE (8) events this causes are ignored in update().
  if (s_tries > 0) WiFi.disconnect(false, false);
  s_tries++;
  s_stats.connectAttempts++;
  // 1st try on the known channel (skips the all-channel scan, ~9 s with BLE sharing the radio);
  // later tries scan all channels in case the router moved.
  WiFi.begin(s_ssid.c_str(), s_pass.length() ? s_pass.c_str() : nullptr, s_tries == 1 ? s_channel : 0);
}

void startAttempt() {
  radioOn();
  cancelScan();
  s_attemptMs = millis();
  s_tries = s_authFails = 0;
  s_error = "";
  s_evLost = false;                      // stale events from before this attempt
  // Switching networks, or a new connect() while an attempt is still running.
  if (WiFi.status() == WL_CONNECTED || s_state == WifiService::State::Connecting) WiFi.disconnect(false, false);
  s_state = WifiService::State::Connecting;
  begin1();
}

void onConnected() {
  s_state = WifiService::State::Connected;
  s_stats.connects++;
  s_retryMs = RETRY_MIN_MS;
  s_error = "";
  const char *saved = "";
  if (s_saveOnConnect) {
    s_saveOnConnect = false;
    Settings::setWifiNetwork(s_ssid, s_pass);
    Settings::setWifiChannel(WiFi.channel());
    // Read back: the credentials must really be in NVS before we call it saved.
    if (Settings::wifiSsid() == s_ssid && Settings::wifiPass() == s_pass) { s_stats.saves++; saved = ", credentials saved"; }
    else { s_stats.saveFailures++; s_error = "could not save the password"; saved = ", SAVE FAILED"; }
  }
  startMdns();                                                  // announced with every reconnect
  Serial.printf("[WIFI] connected to '%s', ip %s, rssi %d dBm, %lu ms%s\n", s_ssid.c_str(), WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), (unsigned long)(millis() - s_attemptMs), saved);
}

// The attempt window ended without a connection.
void attemptFailed(const char *why) {
  WiFi.disconnect(false, false);
  s_state = WifiService::State::Failed;
  s_error = why;
  Serial.printf("[WIFI] could not connect to '%s': %s (%d tries, last reason %u)%s\n", s_ssid.c_str(), why, s_tries,
                (unsigned)s_stats.lastDisconnectReason, s_saveOnConnect ? ", not saved" : "");
  if (s_saveOnConnect) {                 // new credentials never worked: keep the previous ones
    s_saveOnConnect = false;
    s_ssid = Settings::wifiSsid();
    s_pass = Settings::wifiPass();
    s_channel = Settings::wifiChannel();
  }
  s_attemptMs = millis();
}

void collectScan(int n) {
  s_netCount = 0;
  for (int i = 0; i < n; i++) {
    const String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;                              // hidden
    int dup = -1;
    for (int k = 0; k < s_netCount; k++) if (ssid == s_nets[k].ssid) { dup = k; break; }
    const int8_t rssi = (int8_t)WiFi.RSSI(i);
    if (dup >= 0) {
      if (rssi > s_nets[dup].rssi) { s_nets[dup].rssi = rssi; s_nets[dup].channel = WiFi.channel(i); }
      continue;
    }
    if (s_netCount == MAX_NETWORKS) continue;
    WifiService::Network &net = s_nets[s_netCount++];
    strlcpy(net.ssid, ssid.c_str(), sizeof(net.ssid));
    net.rssi = rssi;
    net.channel = WiFi.channel(i);
    net.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  // strongest first
  for (int a = 1; a < s_netCount; a++)
    for (int b = a; b > 0 && s_nets[b].rssi > s_nets[b - 1].rssi; b--) {
      const WifiService::Network t = s_nets[b]; s_nets[b] = s_nets[b - 1]; s_nets[b - 1] = t;
    }
  s_scanSerial++;
}

}  // namespace

void WifiService::begin() {
  s_host = Settings::deviceName();
  s_host.toLowerCase();
  WiFi.persistent(false);                                      // credentials live in our own NVS keys
  WiFi.setAutoReconnect(false);                                // reconnects are handled in update()
  WiFi.onEvent(onEvent);
  s_ssid = Settings::wifiSsid();
  s_pass = Settings::wifiPass();
  s_channel = Settings::wifiChannel();
  s_ap = Settings::hotspotEnabled();
  if (!Settings::wifiEnabled()) { s_state = State::Off; WiFi.mode(WIFI_OFF); return; }
  radioOn();
  if (s_ssid.length()) startAttempt();                         // saved network: connect automatically
  else s_state = State::Idle;
}

void WifiService::update() {
  // scan completion
  if (s_scanning) {
    const int16_t r = WiFi.scanComplete();
    if (r >= 0) {
      collectScan(r);
      WiFi.scanDelete();
      s_scanning = false;
      s_stats.lastScanMs = millis() - s_scanStartMs;
    } else if (r == WIFI_SCAN_FAILED || millis() - s_scanStartMs > 15000) {
      s_scanning = false;
      s_stats.scanFails++;
      WiFi.scanDelete();
    }
  }

  if (s_state == State::Off) { s_evGotIp = false; s_evLost = false; return; }

  // Connected: by event, or by status (an event can be missed or arrive out of order).
  const bool up = s_evGotIp || (s_state == State::Connecting && WiFi.status() == WL_CONNECTED &&
                                WiFi.localIP() != IPAddress(0, 0, 0, 0));
  s_evGotIp = false;
  if (up && s_state != State::Connected) onConnected();

  if (s_evLost) {
    s_evLost = false;
    const uint8_t reason = s_evReason;
    s_stats.lastDisconnectReason = reason;
    if (s_state == State::Connected) {
      s_stats.disconnects++;
      if (!s_apUp) stopMdns();
      s_state = State::Failed;
      s_error = "connection lost";
      Serial.printf("[WIFI] connection to '%s' lost (reason %u), reconnecting\n", s_ssid.c_str(), (unsigned)reason);
      s_attemptMs = millis() - s_retryMs + 2000;               // first reconnect after 2 s
    } else if (s_state == State::Connecting && reason != WIFI_REASON_ASSOC_LEAVE &&
               reason != WIFI_REASON_STA_LEAVING) {            // not caused by our own disconnect()
      if (isAuthFailure(reason)) s_authFails++;
      if (s_authFails >= 2) attemptFailed("wrong password?");
      else if (isNotFound(reason) && s_tries >= 2) attemptFailed("network not found");
      else if (s_tries < MAX_TRIES) begin1();                  // try again within the window
      else attemptFailed(isAuthFailure(reason) ? "wrong password?" : "could not connect");
    }
  }

  if (s_state == State::Connecting && millis() - s_attemptMs > CONNECT_TIMEOUT_MS) attemptFailed("timed out");

  // A reconnect attempt takes the radio off the hotspot's channel: not while a phone is on it.
  if (s_state == State::Failed && !s_scanning && s_ssid.length() && millis() - s_attemptMs > s_retryMs &&
      !(s_apUp && WiFi.softAPgetStationNum() > 0)) {
    s_retryMs = min(s_retryMs * 2, RETRY_MAX_MS);
    startAttempt();
  }
}

void WifiService::setEnabled(bool on) {
  Settings::setWifiEnabled(on);
  if (on) {
    if (s_state != State::Off) return;
    radioOn();
    s_ssid = Settings::wifiSsid();
    s_pass = Settings::wifiPass();
    s_retryMs = RETRY_MIN_MS;
    if (s_ssid.length()) startAttempt();
    else s_state = State::Idle;
  } else {
    stopMdns();
    cancelScan();
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    s_apUp = false;
    s_state = State::Off;
  }
}

void WifiService::setHotspot(bool on) {
  Settings::setHotspotEnabled(on);
  s_ap = on;
  if (on && s_state == State::Off) { setEnabled(true); return; }   // radioOn() starts the AP
  if (s_state == State::Off) return;
  if (!on && s_apUp) {
    WiFi.softAPdisconnect(true);
    s_apUp = false;
    Serial.println("[WIFI] hotspot off");
    if (s_state != State::Connected) stopMdns();
  }
  radioOn();
}

bool WifiService::hotspotOn() { return s_apUp; }
String WifiService::hotspotSsid() { return Settings::deviceName(); }
String WifiService::hotspotPass() { return Settings::hotspotPass(); }
String WifiService::hotspotIp() { return s_apUp ? WiFi.softAPIP().toString() : String(); }
int WifiService::hotspotClients() { return s_apUp ? WiFi.softAPgetStationNum() : 0; }

bool WifiService::enabled() { return s_state != State::Off; }

bool WifiService::startScan() {
  if (s_state == State::Off || s_scanning) return false;
  if (s_state == State::Connecting) return false;              // would disturb the attempt
  s_scanning = WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING;
  if (s_scanning) { s_scanStartMs = millis(); s_stats.scans++; }
  else s_stats.scanFails++;
  return s_scanning;
}

bool WifiService::scanning() { return s_scanning; }
int WifiService::networkCount() { return s_netCount; }
const WifiService::Network &WifiService::network(int i) { return s_nets[constrain(i, 0, MAX_NETWORKS - 1)]; }
uint32_t WifiService::scanSerial() { return s_scanSerial; }

void WifiService::connect(const char *ssid, const char *pass, int channel) {
  if (s_state == State::Off) setEnabled(true);
  s_ssid = ssid;
  s_pass = pass ? pass : "";
  s_channel = channel;
  if (!s_channel)                                              // from the last scan, if listed
    for (int i = 0; i < s_netCount; i++) if (s_ssid == s_nets[i].ssid) { s_channel = s_nets[i].channel; break; }
  s_saveOnConnect = true;
  s_retryMs = RETRY_MIN_MS;
  stopMdns();
  startAttempt();
}

void WifiService::forget() {
  Serial.printf("[WIFI] saved network '%s' forgotten\n", s_ssid.c_str());
  Settings::setWifiNetwork("", "");
  Settings::setWifiChannel(0);
  s_ssid = s_pass = "";
  s_channel = 0;
  s_saveOnConnect = false;
  s_error = "";
  stopMdns();
  if (s_state != State::Off) {
    cancelScan();
    WiFi.disconnect(false, true);
    s_state = State::Idle;
  }
}

WifiService::State WifiService::state() { return s_state; }

const char *WifiService::stateName(State s) {
  switch (s) {
    case State::Off: return "off";
    case State::Idle: return "not configured";
    case State::Connecting: return "connecting";
    case State::Connected: return "connected";
    case State::Failed: return "not connected";
  }
  return "?";
}

String WifiService::ssid() { return s_ssid; }
String WifiService::ip() { return s_state == State::Connected ? WiFi.localIP().toString() : String(); }
int WifiService::rssi() { return s_state == State::Connected ? WiFi.RSSI() : 0; }
int WifiService::channel() { return s_state == State::Connected ? WiFi.channel() : 0; }
String WifiService::hostname() { return s_host; }
bool WifiService::hasSavedNetwork() { return Settings::wifiSsid().length() > 0; }
String WifiService::lastError() { return s_error; }
const WifiService::Stats &WifiService::stats() { return s_stats; }
