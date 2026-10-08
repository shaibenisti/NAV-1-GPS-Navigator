// =============================================================================
//  MapRender  -  renders the offline street map (vector tiles from MapTiles) into an RGB565 image in
//  a background task, so the UI never waits for it. North up or heading up, centred on a position, dark
//  theme, street and place names, the trip overlays and pins for saved places. Prototype of the algorithm:
//  tools/scripts/maprender.py.
//
//  The Map app asks for a view (request()); the newest request wins. When a frame is finished,
//  poll() hands out the image once. begin() allocates ~1.4 MB PSRAM and starts the task, end() frees it.
// =============================================================================
#pragma once

#include <stdint.h>

namespace MapRender {
  constexpr int W = 480, H = 688;                 // image size (the Map app content area)

  struct Req {
    double lon = 0, lat = 0;
    float zoom = 15.5f;                           // 13 .. 17.5 (tiles are z15 at most, higher zoom = magnified)
    bool headingUp = false;                       // true: the picture is turned so `heading` is up, the position sits below the middle
    float heading = 0;                            // degrees, used with headingUp
    uint32_t seq = 0;                             // set by request()
  };

  bool begin(const char *pmtilesPath);            // false: file not usable (see status().error)
  void end();
  bool running();
  void request(const Req &r);
  bool poll(const uint16_t **frame, Req *rendered);   // true once per finished frame (image valid until the next poll)

  struct Status { bool busy; bool mapOk; const char *error; uint32_t lastMs, lastTiles, frames, stackFree, labelMs, labels; };
  Status status();

  // Track overlay (the recording trip): drawn on every frame until cleared. n <= 600; copied.
  void setTrack(const float *lat, const float *lon, int n);
  // Route overlay (the trip being followed, Navigator): drawn under the track. n <= 600; copied.
  void setRoute(const float *lat, const float *lon, int n);
  // Pins (saved places, the destination) with their names, drawn above everything. n <= MAX_PINS; copied.
  constexpr int MAX_PINS = 64;
  struct Pin { float lat, lon; char name[44]; uint8_t kind; };   // kind: PIN_PLACE / PIN_DEST
  enum : uint8_t { PIN_PLACE = 0, PIN_DEST = 1 };
  void setPins(const Pin *pins, int n);

  // Metres per image pixel at a latitude and zoom (for the scale bar)
  double metersPerPixel(double lat, float zoom);
  // Where (lon, lat) is in a picture rendered for `centre` (the requested position is at the picture's centre, or lower when heading up)
  void pixelOf(const Req &centre, double lon, double lat, int &x, int &y);
  // Inverse for north-up pictures: the position at pixel offset (dx, dy) from the picture's centre
  void positionAt(const Req &centre, int dx, int dy, double &lon, double &lat);
  // Inverse of pixelOf for any picture (north up or heading up): the position under picture pixel (x, y)
  void positionOfPixel(const Req &centre, int x, int y, double &lon, double &lat);
}
