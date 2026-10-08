// =============================================================================
//  MapTiles  -  reads vector tiles (Mapbox Vector Tile, gzip) from a PMTiles v3 file on the SD card
//  (/maps/israel.pmtiles, Protomaps basemap). Header and root directory are read once; leaf directories
//  and tiles on demand with random reads. All buffers live in PSRAM. One caller at a time (the map
//  render task). Prototype of the algorithm: tools/scripts/mvtlib.py.
// =============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace MapTiles {
  struct Buf {                       // growable PSRAM buffer
    uint8_t *p = nullptr;
    size_t len = 0, cap = 0;
  };

  bool open(const char *path);       // false: no card / no file / not a PMTiles v3 MVT/gzip file
  void close();
  bool isOpen();
  // Decompressed MVT of tile z/x/y into `out` (len = 0: tile not in the file). false = read/decode error.
  bool get(int z, int x, int y, Buf &out);
  void release(Buf &b);              // free the buffer

  struct Stats { uint32_t tiles, tileMsSum, dirReads, bytesRead; int maxZoom; uint32_t readUsSum, inflateUsSum, dirUsSum; };
  Stats stats();
  const char *lastError();
}
