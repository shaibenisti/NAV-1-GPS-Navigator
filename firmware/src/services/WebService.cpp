#include "WebService.h"

#include <WebServer.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "../SdLog.h"
#include "Location.h"
#include "Settings.h"
#include "Storage.h"
#include "TripRecorder.h"
#include "WifiService.h"
#include "BleService.h"
#include "Ota.h"
#include "Places.h"
#include "Navigator.h"
#include "Geo.h"
#include "../../config.h"

namespace {

// ---- the web page (served from flash; an /www folder on the SD card can replace it later) ----
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="apple-mobile-web-app-capable" content="yes"><title>NAV-1</title>
<style>
body{margin:0;font-family:-apple-system,system-ui,sans-serif;background:#10141c;color:#eee}
header{padding:14px 16px;background:#1a2340;font-size:20px;font-weight:600}
header small{float:right;font-weight:400;color:#9ab;font-size:14px;margin-top:4px}
.card{background:#1b2130;margin:12px;border-radius:14px;padding:14px}
.state{font-size:22px;font-weight:700}.FIX{color:#43a047}.ACQUIRING,.FIX_LOST{color:#ffb300}.NO_SIGNAL{color:#e53935}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}
.v{font-size:20px}.c{font-size:12px;color:#9ab}
a{color:#64b5f6}table{width:100%;border-collapse:collapse}td{padding:8px 4px;border-top:1px solid #2a3244;font-size:15px}
td.r{text-align:right;white-space:nowrap}.muted{color:#9ab;font-size:13px}
.b{font-size:15px;background:#1e88e5;color:#fff;border:0;border-radius:8px;padding:7px 12px;margin:2px}.b.d{background:#c62828}
input{font-size:16px;background:#10141c;color:#eee;border:1px solid #2a3244;border-radius:8px;padding:7px;width:12em}
</style></head><body>
<header>NAV-1 <small id="dev"></small></header>
<div class="card"><div id="st" class="state">...</div><div class="muted" id="sub"></div>
<div class="grid">
<div><div class="c">Latitude</div><div class="v" id="lat">--</div></div>
<div><div class="c">Longitude</div><div class="v" id="lon">--</div></div>
<div><div class="c">Speed</div><div class="v" id="spd">--</div></div>
<div><div class="c">Altitude</div><div class="v" id="alt">--</div></div>
<div><div class="c">Satellites</div><div class="v" id="sat">--</div></div>
<div><div class="c">Accuracy (HDOP)</div><div class="v" id="hd">--</div></div>
</div><p id="map"></p></div>
<div class="card" id="navc" style="display:none"><b>Navigating to <span id="nvn"></span></b>
<div class="grid"><div><div class="c">Distance</div><div class="v" id="nvd">--</div></div>
<div><div class="c">Arrival in</div><div class="v" id="nve">--</div></div></div>
<p><button class="b" onclick="post('/api/nav/stop')">Stop</button></p></div>
<div class="card"><a href="/map"><b>Offline map</b></a> <span class="muted">- position, trips and places, no internet needed. Long press a point to save it or go there.</span></div>
<div class="card"><b>Places</b> <span class="muted" id="pc"></span><table id="places"></table>
<p><input id="pn" maxlength="40" placeholder="Name (e.g. Car)" dir="auto">
<button class="b" onclick="saveHere()">Save NAV-1's position</button></p></div>
<div class="card"><b>Trips</b> <span class="muted" id="rec"></span><table id="trips"></table></div>
<div class="card muted" id="sys"></div>
<script>
const $=id=>document.getElementById(id);
async function j(u){const r=await fetch(u,{cache:'no-store'});return r.json();}
async function loc(){try{const d=await j('/api/location');if(d.lat!=null)here=[d.lat,d.lon];
$('st').textContent=d.state.replace('_',' ');$('st').className='state '+d.state;
$('sub').textContent=d.utc?('UTC '+d.utc):'';
$('lat').textContent=d.lat!=null?d.lat.toFixed(6):'--';$('lon').textContent=d.lon!=null?d.lon.toFixed(6):'--';
$('spd').textContent=d.speed_kmh!=null?d.speed_kmh.toFixed(1)+' km/h':'--';$('alt').textContent=d.alt_m!=null?Math.round(d.alt_m)+' m':'--';
$('sat').textContent=d.sats_used+' / '+d.sats_view;$('hd').textContent=d.hdop!=null?d.hdop.toFixed(1):'--';
$('map').innerHTML=d.lat!=null?'<a href="https://maps.apple.com/?ll='+d.lat+','+d.lon+'&q=NAV-1">Open in Maps</a>':'';
}catch(e){$('st').textContent='NAV-1 not reachable';$('st').className='state NO_SIGNAL';}}
async function st(){try{const s=await j('/api/status');$('dev').textContent=s.name;
$('rec').textContent=s.trip.recording?('recording: '+s.trip.distance_km.toFixed(2)+' km'):'';
$('sys').innerHTML='Firmware '+s.firmware+'<br>Wi-Fi '+s.wifi.ssid+' ('+s.wifi.rssi+' dBm), Bluetooth '+s.ble+
'<br>Storage: '+s.storage+'<br>Uptime '+Math.round(s.uptime_s/60)+' min';}catch(e){}}
async function trips(){try{const t=await j('/api/trips');$('trips').innerHTML=t.length?t.map(x=>
'<tr><td>'+x.title+'<br><span class="muted">'+x.distance_km.toFixed(2)+' km, '+Math.round(x.duration_s/60)+' min, max '+
Math.round(x.max_kmh)+' km/h</span></td><td class="r"><a href="'+x.gpx+'" download>GPX</a></td></tr>').join(''):
'<tr><td class="muted">No trips yet</td></tr>';}catch(e){}}
var here=null,pl=[];
function dist(a,b,c,d){const r=Math.PI/180,x=(d-b)*r*Math.cos((a+c)/2*r),y=(c-a)*r;return 6371009*Math.sqrt(x*x+y*y);}
function fd(m){return m<1000?Math.round(m)+' m':(m/1000).toFixed(m<10000?2:1)+' km';}
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
async function post(u,a){const b=new URLSearchParams(a||{});try{const r=await fetch(u,{method:'POST',body:b});const j=await r.json();
if(!j.ok)alert(j.error||'failed');}catch(e){alert('NAV-1 not reachable');}places();nav();}
async function places(){try{pl=await j('/api/places');$('pc').textContent=pl.length+' saved';$('places').innerHTML=pl.map((p,i)=>
'<tr><td dir="auto">'+esc(p.name)+'<br><span class="muted">'+(here?fd(dist(here[0],here[1],p.lat,p.lon)):p.lat.toFixed(5)+', '+p.lon.toFixed(5))+
'</span></td><td class="r"><button class="b" onclick="post(\'/api/goto\',{i:'+i+'})">Go</button><button class="b d" onclick="del('+i+')">&times;</button></td></tr>').join('');}catch(e){}}
function del(i){if(confirm('Delete '+pl[i].name+'?'))post('/api/places/delete',{i:i,name:pl[i].name});}
function saveHere(){if(!here){alert('NAV-1 has no GPS position yet');return;}post('/api/places',{name:$('pn').value||'',lat:here[0],lon:here[1]});$('pn').value='';}
async function nav(){try{const n=await j('/api/nav');$('navc').style.display=n.mode=='off'?'none':'block';if(n.mode=='off')return;
$('nvn').textContent=n.name;$('nvd').textContent=n.arrived?'Arrived':(n.dist_m!=null?fd(n.dist_m):'--');
$('nve').textContent=n.eta_s?Math.round(n.eta_s/60)+' min':'--';}catch(e){}}
loc();st();trips();places();nav();setInterval(loc,2000);setInterval(st,10000);setInterval(trips,30000);setInterval(nav,2000);setInterval(places,15000);
</script></body></html>)HTML";

constexpr uint32_t WEB_STACK = 8192;
SdLog *s_sd = nullptr;
WebServer s_server(80);
bool s_started = false;
TaskHandle_t s_task = nullptr;
uint32_t s_requests = 0;                                    // written by the web task only
SemaphoreHandle_t s_lock = nullptr;                        // UI loop <-> web task (a mutex: String copies allocate)
String s_statusJson = "{}", s_locationJson = "{}";          // prepared in the UI loop
String s_placesJson = "[]", s_navJson = "{}";
uint32_t s_placesRev = 0;
uint32_t s_lastPrepMs = 0;

// Commands from the web task, run by the UI loop (one at a time; the web task waits for the answer)
enum class Cmd : uint8_t { None, AddPlace, DeletePlace, GoTo, GoToPlace, Stop };
struct Command { Cmd cmd; int index; double lat, lon; char name[Places::NAME_BYTES + 1]; };
Command s_cmd = {};
volatile bool s_cmdPending = false, s_cmdDone = false;
String s_cmdResult;                                         // JSON answer

String copyLocked(const String &s) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  String c = s;
  xSemaphoreGive(s_lock);
  return c;
}

void setLocked(String &dst, const String &src) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  dst = src;
  xSemaphoreGive(s_lock);
}

String num(bool valid, double v, int decimals) { return valid ? String(v, decimals) : String("null"); }

String jsonText(const char *s) {
  String o = "\"";
  for (; *s; s++) {
    const unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c >= 0x20) o += (char)c;
  }
  return o + "\"";
}

// ---- UI loop side ----------------------------------------------------------------------
void prepare() {
  const GpsData d = Location::snapshot();
  static const char *STATE[] = { "NO_SIGNAL", "ACQUIRING", "FIX", "FIX_LOST" };
  char utc[40] = "";
  if (d.timeValid && d.dateValid)
    snprintf(utc, sizeof(utc), "%04u-%02u-%02u %02u:%02u:%02u", d.year, d.month, d.day, d.hour, d.minute, d.second);
  String loc = String("{\"state\":\"") + STATE[(int)Location::quality(d)] + "\",\"fix\":" + (d.fix ? "true" : "false") +
               ",\"lat\":" + num(d.locValid, d.lat, 7) + ",\"lon\":" + num(d.locValid, d.lng, 7) +
               ",\"speed_kmh\":" + num(d.speedValid && d.fix, d.speedKmh, 1) + ",\"course_deg\":" + num(d.courseValid && d.fix, d.courseDeg, 0) +
               ",\"alt_m\":" + num(d.altValid && d.locValid, d.altM, 1) + ",\"hdop\":" + num(d.hdopValid && d.hdop < 50, d.hdop, 2) +
               ",\"sats_used\":" + String(d.satsUsed) + ",\"sats_view\":" + String(d.satsInView()) +
               ",\"utc\":\"" + utc + "\"}";
  String st = String("{\"name\":\"") + Settings::deviceName() + "\",\"firmware\":\"" + APP_NAME + " " + FW_VERSION +
              "\",\"build\":\"" + FW_GIT_DESCRIBE + "\",\"slot\":\"" + Ota::runningSlot() + "\",\"ota\":\"" +
              (Ota::pendingVerify() ? "verifying" : Ota::stateName(Ota::state())) + "\",\"uptime_s\":" + String(millis() / 1000) + ",\"wifi\":{\"ssid\":\"" + WifiService::ssid() +
              "\",\"ip\":\"" + WifiService::ip() + "\",\"rssi\":" + String(WifiService::rssi()) + ",\"hotspot\":" +
              (WifiService::hotspotOn() ? "true" : "false") + "},\"ble\":\"" +
              (BleService::enabled() ? (BleService::connectedCount() ? "connected" : "on") : "off") +
              "\",\"storage\":\"" + Storage::summary() + "\",\"trip\":{\"recording\":" +
              (TripRecorder::recording() ? "true" : "false") + ",\"duration_s\":" + String(TripRecorder::durationS()) +
              ",\"distance_km\":" + String(TripRecorder::distanceKm(), 3) + "}}";
  setLocked(s_locationJson, loc);
  setLocked(s_statusJson, st);
  if (Places::revision() != s_placesRev) { s_placesRev = Places::revision(); setLocked(s_placesJson, Places::json()); }
  const Navigator::Status n = Navigator::status();
  String nav = String("{\"mode\":\"") + (n.mode == Navigator::Mode::Place ? "place" : n.mode == Navigator::Mode::Route ? "route" : "off") + "\"";
  if (n.mode != Navigator::Mode::None) {
    nav += ",\"name\":" + jsonText(n.name) + ",\"dest_lat\":" + String(n.destLat, 7) + ",\"dest_lon\":" + String(n.destLon, 7) +
           ",\"dist_m\":" + num(n.distM >= 0, n.distM, 0) + ",\"bearing_deg\":" + num(n.distM >= 0, n.bearingDeg, 0) +
           ",\"eta_s\":" + String(n.etaS) + ",\"arrived\":" + (n.arrived ? "true" : "false") + ",\"off_route\":" + (n.offRoute ? "true" : "false") +
           ",\"off_m\":" + String(n.offM, 0) + ",\"progress\":" + String(n.progress, 3) + ",\"reverse\":" + (n.reverse ? "true" : "false");
    const String csv = Navigator::routeCsvUrl();
    if (csv.length()) nav += ",\"route_csv\":\"" + csv + "\"";
  }
  setLocked(s_navJson, nav + "}");
}

// A command from the web task (UI loop)
void serveCommand() {
  if (!s_cmdPending) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const Command c = s_cmd;
  s_cmdPending = false;
  xSemaphoreGive(s_lock);
  bool ok = false;
  String err;
  switch (c.cmd) {
    case Cmd::AddPlace: {
      String name = c.name;
      name.trim();
      if (!name.length()) name = Places::defaultName();
      if (Places::count() >= Places::CAPACITY) err = "the list of places is full";
      else ok = Places::add(name.c_str(), c.lat, c.lon) >= 0;
      if (!ok && !err.length()) err = "bad name or position";
      break;
    }
    case Cmd::DeletePlace: {
      Places::Place p;
      ok = Places::get(c.index, p) && (!c.name[0] || !strcmp(p.name, c.name)) && Places::remove(c.index);   // the name guards against a changed list
      if (!ok) err = "no such place (the list changed?)";
      break;
    }
    case Cmd::GoToPlace: {
      Places::Place p;
      ok = Places::get(c.index, p) && Navigator::goTo(p.name, p.lat, p.lon);
      if (!ok) err = "no such place";
      break;
    }
    case Cmd::GoTo:
      ok = Navigator::goTo(c.name[0] ? c.name : "Phone point", c.lat, c.lon);
      if (!ok) err = "bad position";
      break;
    case Cmd::Stop: Navigator::stop(); ok = true; break;
    default: err = "unknown command"; break;
  }
  setLocked(s_cmdResult, ok ? String("{\"ok\":true}") : "{\"ok\":false,\"error\":" + jsonText(err.c_str()) + "}");
  s_placesRev = 0;                                          // answer with fresh lists
  prepare();
  s_cmdDone = true;
}

// ---- web task side -----------------------------------------------------------------------
void sendJson(const String &json) {
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send(200, "application/json", json);
}


// POST handlers: parse in the web task, run in the UI loop, answer {"ok":..}
void runCommand(const Command &c) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_cmd = c;
  s_cmdDone = false;
  s_cmdPending = true;
  xSemaphoreGive(s_lock);
  for (int i = 0; i < 300 && !s_cmdDone; i++) vTaskDelay(pdMS_TO_TICKS(10));   // the loop prepares every 0.5 s
  if (!s_cmdDone) { s_cmdPending = false; s_server.send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}"); return; }
  sendJson(copyLocked(s_cmdResult));
}

void handlePlaceAdd() {
  s_requests++;
  Command c = {};
  c.cmd = Cmd::AddPlace;
  c.lat = s_server.arg("lat").toDouble();
  c.lon = s_server.arg("lon").toDouble();
  strlcpy(c.name, s_server.arg("name").c_str(), sizeof(c.name));
  runCommand(c);
}

void handlePlaceDelete() {
  s_requests++;
  Command c = {};
  c.cmd = Cmd::DeletePlace;
  c.index = s_server.hasArg("i") ? s_server.arg("i").toInt() : -1;
  strlcpy(c.name, s_server.arg("name").c_str(), sizeof(c.name));
  runCommand(c);
}

void handleGoTo() {
  s_requests++;
  Command c = {};
  if (s_server.hasArg("i")) { c.cmd = Cmd::GoToPlace; c.index = s_server.arg("i").toInt(); }
  else { c.cmd = Cmd::GoTo; c.lat = s_server.arg("lat").toDouble(); c.lon = s_server.arg("lon").toDouble(); }
  strlcpy(c.name, s_server.arg("name").c_str(), sizeof(c.name));
  runCommand(c);
}

void handleNavStop() {
  s_requests++;
  Command c = {};
  c.cmd = Cmd::Stop;
  runCommand(c);
}

void handleTrips() {
  static TripRecorder::Summary trips[30];                   // web task only
  const int n = TripRecorder::list(trips, 30);
  String j = "[";
  for (int i = 0; i < n; i++) {
    const TripRecorder::Summary &t = trips[i];
    const String dir = isdigit((unsigned char)t.name[0]) ? t.name.substring(0, 4) : String("undated");
    if (i) j += ',';
    j += "{\"name\":\"" + t.name + "\",\"title\":\"" + t.title + "\",\"distance_km\":" + String(t.distanceKm, 3) +
         ",\"duration_s\":" + String(t.durationS) + ",\"max_kmh\":" + String(t.maxKmh, 1) + ",\"points\":" + String(t.points) +
         ",\"gpx\":\"/trips/" + dir + "/" + t.name + ".gpx\",\"csv\":\"/trips/" + dir + "/" + t.name + ".csv\"}";
  }
  sendJson(j + "]");
}

// /trips/<dir>/<name>.gpx|.csv - names are checked: only [0-9A-Za-z_-], no path tricks.
void handleFile() {
  const String uri = s_server.uri();
  const int slash = uri.indexOf('/', 7), dot = uri.lastIndexOf('.');
  bool ok = uri.startsWith("/trips/") && slash > 7 && dot > slash + 1;
  const String dir = ok ? uri.substring(7, slash) : "", name = ok ? uri.substring(slash + 1, dot) : "", ext = ok ? uri.substring(dot) : "";
  for (const String *s : { &dir, &name })
    for (size_t i = 0; ok && i < s->length(); i++) { const char c = (*s)[i]; ok = isalnum((unsigned char)c) || c == '_' || c == '-'; }
  ok = ok && (ext == ".gpx" || ext == ".csv");
  const String path = "/data/trips/" + dir + "/" + name + ext;
  const int32_t size = ok ? s_sd->fileSize(path.c_str()) : -1;
  if (size < 0) { s_server.send(404, "text/plain", "not found"); return; }
  s_server.sendHeader("Content-Disposition", "attachment; filename=\"NAV-1_" + name + ext + "\"");
  s_server.setContentLength(size);
  s_server.send(200, ext == ".gpx" ? "application/gpx+xml" : "text/csv", "");
  static uint8_t buf[4096];
  for (int32_t off = 0; off < size;) {
    const int got = s_sd->readChunk(path.c_str(), off, buf, sizeof(buf));
    if (got <= 0) break;
    s_server.sendContent((const char *)buf, got);
    off += got;
  }
}

// ---- offline map: /map/... = /www/map/... (page + libraries), /maps/<file>.pmtiles = /maps/... ----------
// Both from the SD card (copied from the PC, tools/scripts/maps.ps1). The map files are PMTiles
// archives read by the page with HTTP range requests; the file stays open between requests.
uint32_t s_readerUsedMs = 0;

const char *contentType(const String &p) {
  if (p.endsWith(".html")) return "text/html";
  if (p.endsWith(".js")) return "application/javascript";
  if (p.endsWith(".css")) return "text/css";
  if (p.endsWith(".png")) return "image/png";
  if (p.endsWith(".json")) return "application/json";
  return "application/octet-stream";
}

bool safePath(const String &p) {
  if (p.indexOf("..") >= 0 || p.indexOf("//") >= 0) return false;
  for (size_t i = 0; i < p.length(); i++) {
    const char c = p[i];
    if (!isalnum((unsigned char)c) && c != '/' && c != '.' && c != '_' && c != '-') return false;
  }
  return true;
}

void handleStatic(String path) {
  static uint8_t buf[4096];
  uint32_t size = 0;
  if (!safePath(path) || s_sd->readAt(path.c_str(), 0, buf, 0, &size) < 0) { s_server.send(404, "text/plain", "not found"); return; }
  s_readerUsedMs = millis();
  uint32_t from = 0, to = size ? size - 1 : 0;
  const String range = s_server.header("Range");                  // "bytes=a-b" or "bytes=a-"
  const bool partial = range.startsWith("bytes=") && size;
  if (partial) {
    const int dash = range.indexOf('-');
    from = strtoul(range.c_str() + 6, nullptr, 10);
    if (dash > 0 && dash + 1 < (int)range.length()) to = min<uint32_t>(strtoul(range.c_str() + dash + 1, nullptr, 10), size - 1);
    if (from > to) {
      s_server.sendHeader("Content-Range", "bytes */" + String(size));
      s_server.send(416, "text/plain", "");
      return;
    }
    s_server.sendHeader("Content-Range", "bytes " + String(from) + "-" + String(to) + "/" + String(size));
  }
  s_server.sendHeader("Accept-Ranges", "bytes");
  s_server.sendHeader("Cache-Control", "max-age=86400");
  s_server.setContentLength(size ? to - from + 1 : 0);
  s_server.send(partial ? 206 : 200, contentType(path), "");
  for (uint32_t off = from; size && off <= to;) {
    const int got = s_sd->readAt(path.c_str(), off, buf, min<uint32_t>(sizeof(buf), to - off + 1));
    if (got <= 0) break;
    s_server.sendContent((const char *)buf, got);
    off += got;
  }
  s_readerUsedMs = millis();
}

void route() {
  const String uri = s_server.uri();
  if (uri == "/map" || uri == "/map/") handleStatic("/www/map/index.html");
  else if (uri.startsWith("/map/")) handleStatic("/www" + uri);
  else if (uri.startsWith("/maps/")) handleStatic(uri);
  else handleFile();
}

// Firmware upload (multipart "file"): Ota checks the image, writes the inactive slot.
String s_upError;
bool s_upStarted = false;

void handleUpdateUpload() {
  HTTPUpload &u = s_server.upload();
  if (u.status == UPLOAD_FILE_START) {
    s_upError = "";
    s_upStarted = false;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (!s_upStarted && !s_upError.length()) {
      s_upError = Ota::uploadBegin(u.buf, u.currentSize, s_server.clientContentLength());
      s_upStarted = !s_upError.length();
    } else if (s_upStarted && !Ota::uploadWrite(u.buf, u.currentSize)) {
      s_upError = Ota::lastError();
      s_upStarted = false;
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (s_upStarted) s_upError = Ota::uploadEnd();
    else if (!s_upError.length()) s_upError = "empty upload";
    s_upStarted = false;
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Ota::uploadAbort();
    s_upError = "upload aborted";
    s_upStarted = false;
  }
}

void handleUpdateDone() {
  s_requests++;
  String e = s_upError;
  e.replace("\"", "'");
  if (!e.length() && Ota::state() == Ota::State::Done) sendJson("{\"ok\":true,\"message\":\"installed, rebooting\"}");
  else s_server.send(Ota::state() == Ota::State::Idle ? 403 : 400, "application/json", "{\"ok\":false,\"error\":\"" + e + "\"}");
}

void webTask(void *) {
  s_server.on("/", HTTP_GET, [] { s_requests++; s_server.send_P(200, "text/html", PAGE); });
  s_server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  s_server.on("/api/status", HTTP_GET, [] { s_requests++; sendJson(copyLocked(s_statusJson)); });
  s_server.on("/api/location", HTTP_GET, [] { s_requests++; sendJson(copyLocked(s_locationJson)); });
  s_server.on("/api/trips", HTTP_GET, [] { s_requests++; handleTrips(); });
  s_server.on("/api/places", HTTP_GET, [] { s_requests++; sendJson(copyLocked(s_placesJson)); });
  s_server.on("/api/places", HTTP_POST, handlePlaceAdd);
  s_server.on("/api/places/delete", HTTP_POST, handlePlaceDelete);
  s_server.on("/api/nav", HTTP_GET, [] { s_requests++; sendJson(copyLocked(s_navJson)); });
  s_server.on("/api/goto", HTTP_POST, handleGoTo);
  s_server.on("/api/nav/stop", HTTP_POST, handleNavStop);
  s_server.onNotFound([] { s_requests++; route(); });
  s_server.enableCORS(true);
  const char *hdrs[] = { "Range" };
  s_server.collectHeaders(hdrs, 1);
  s_server.begin();
  for (;;) {
    s_server.handleClient();
    if (s_readerUsedMs && millis() - s_readerUsedMs > 5000) { s_sd->readerClose(); s_readerUsedMs = 0; }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void WebService::begin(SdLog &sd) {
  s_sd = &sd;
  s_lock = xSemaphoreCreateMutex();
}

void WebService::update() {
  if (s_cmdPending) serveCommand();
  if (millis() - s_lastPrepMs < 500) return;
  s_lastPrepMs = millis();
  const bool sta = WifiService::state() == WifiService::State::Connected;
  if (!sta && !WifiService::hotspotOn()) return;
  prepare();
  if (!s_started) {                                         // first network: start the server task
    s_started = xTaskCreatePinnedToCore(webTask, "web", WEB_STACK, nullptr, 2, &s_task, 0) == pdPASS;
    if (s_started) {
      Serial.printf("[WEB] serving %s and %s\n", url().c_str(), localUrl().c_str());
    }
  }
}

bool WebService::running() {
  return s_started && (WifiService::state() == WifiService::State::Connected || WifiService::hotspotOn());
}
String WebService::url() {
  if (!running()) return String();
  return "http://" + (WifiService::state() == WifiService::State::Connected ? WifiService::ip() : WifiService::hotspotIp()) + "/";
}
String WebService::hotspotUrl() { return running() && WifiService::hotspotOn() ? "http://" + WifiService::hotspotIp() + "/" : String(); }
String WebService::localUrl() { return "http://" + WifiService::hostname() + ".local/"; }
uint32_t WebService::requests() { return s_requests; }
uint32_t WebService::stackFreeBytes() { return s_task ? uxTaskGetStackHighWaterMark(s_task) : 0; }
