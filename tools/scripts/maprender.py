"""Prototype of the NAV-1 on-screen street map: render a PMTiles (Protomaps) area to a PNG.
usage: maprender.py <lon> <lat> [out.png] [zoom=15.5] [w=480] [h=688]
Same algorithm the device uses: tiles z15 (extent 4096), scanline polygon fill, thick polylines. (The device version also draws street and place names; this prototype does not.)"""
import sys, math, zlib, struct
import mvtlib as m

import os
PM = os.environ.get('NAV1_PMTILES', 'build/maps/israel.pmtiles')   # the street map made by maps.ps1

# dark theme (RGB)
BG = (18, 22, 32); LAND = (26, 32, 46); PARK = (28, 56, 44); FOREST = (24, 62, 42); WATER = (36, 78, 120)
BUILD = (44, 52, 70); ROAD_CASE = (10, 12, 18)
ROADS = {  # kind -> (fill, width px at 1 unit = 1 px at z15.5)
    'highway': ((255, 170, 60), 9), 'major_road': ((240, 200, 90), 7), 'medium_road': ((210, 214, 224), 5),
    'minor_road': ((150, 158, 176), 3.5), 'path': ((90, 100, 120), 1.6), 'other': ((110, 118, 136), 2.2), 'rail': ((120, 120, 140), 1.5),
}
LAND_COL = {'park': PARK, 'forest': FOREST, 'meadow': PARK, 'nature_reserve': PARK, 'cemetery': PARK, 'grass': PARK, 'garden': PARK,
            'pitch': PARK, 'playground': PARK, 'golf_course': PARK, 'farmland': (30, 44, 40), 'industrial': (34, 38, 50)}

class Canvas:
    def __init__(self, w, h, bg):
        self.w, self.h = w, h
        self.px = bytearray(bytes(bg) * (w * h))
    def span(self, y, x0, x1, c):
        if y < 0 or y >= self.h: return
        x0 = max(0, int(x0)); x1 = min(self.w - 1, int(x1))
        if x1 < x0: return
        o = (y * self.w + x0) * 3
        self.px[o:o + (x1 - x0 + 1) * 3] = bytes(c) * (x1 - x0 + 1)
    def poly(self, rings, c):
        edges = []
        ymin, ymax = 1e9, -1e9
        for r in rings:
            for i in range(len(r) - 1):
                (x0, y0), (x1, y1) = r[i], r[i + 1]
                if y0 == y1: continue
                if y0 > y1: x0, y0, x1, y1 = x1, y1, x0, y0
                edges.append((y0, y1, x0, (x1 - x0) / (y1 - y0)))
                ymin = min(ymin, y0); ymax = max(ymax, y1)
        if not edges: return
        for y in range(max(0, int(math.floor(ymin))), min(self.h - 1, int(math.ceil(ymax))) + 1):
            yc = y + 0.5
            xs = sorted(x0 + (yc - y0) * k for (y0, y1, x0, k) in edges if y0 <= yc < y1)
            for i in range(0, len(xs) - 1, 2): self.span(y, round(xs[i]), round(xs[i + 1]) - 1, c)
    def disc(self, cx, cy, r, c):
        for y in range(int(cy - r), int(cy + r) + 1):
            dx = math.sqrt(max(0, r * r - (y + 0.5 - cy) ** 2))
            self.span(y, round(cx - dx), round(cx + dx) - 1, c)
    def line(self, pts, wd, c):
        for i in range(len(pts) - 1):
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            dx, dy = x1 - x0, y1 - y0; l = math.hypot(dx, dy)
            if l == 0: continue
            nx, ny = -dy / l * wd / 2, dx / l * wd / 2
            self.poly([[(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny), (x0 + nx, y0 + ny)]], c)
        if wd >= 3:
            for (x, y) in pts: self.disc(x, y, wd / 2, c)
    def png(self, path):
        raw = b''.join(b'\x00' + bytes(self.px[y * self.w * 3:(y + 1) * self.w * 3]) for y in range(self.h))
        def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 2, 0, 0, 0)) +
                               ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))

def render(pm, lon, lat, zoom=15.5, w=480, h=688, out='map.png'):
    tz = 15
    scale = 2 ** (zoom - tz) * 256       # px per tile
    fx, fy = m.lonlat_to_tile(lon, lat, tz)
    cv = Canvas(w, h, BG)
    x0 = int(math.floor(fx - w / 2 / scale)); x1 = int(math.floor(fx + w / 2 / scale))
    y0 = int(math.floor(fy - h / 2 / scale)); y1 = int(math.floor(fy + h / 2 / scale))
    layers_all = []
    for ty in range(y0, y1 + 1):
        for tx in range(x0, x1 + 1):
            t = pm.get(tz, tx, ty)
            if not t: continue
            L = m.parse_mvt(t)
            def xf(g, ext=4096, tx=tx, ty=ty):
                k = scale / ext
                ox = (tx - fx) * scale + w / 2; oy = (ty - fy) * scale + h / 2
                return [[(ox + px * k, oy + py * k) for (px, py) in r] for r in g]
            layers_all.append((L, xf))
    for L, xf in layers_all:                       # earth + landuse + water
        for typ, pr, g in L.get('earth', []): cv.poly(xf(g), LAND)
    for L, xf in layers_all:
        for typ, pr, g in L.get('landuse', []):
            c = LAND_COL.get(pr.get('kind'))
            if c and typ == 3: cv.poly(xf(g), c)
        for typ, pr, g in L.get('landcover', []):
            if typ == 3: cv.poly(xf(g), FOREST if pr.get('kind') in ('forest',) else PARK)
        for typ, pr, g in L.get('water', []):
            if typ == 3: cv.poly(xf(g), WATER)
            elif typ == 2: cv.line(xf(g)[0] if len(g) == 1 else [p for r in xf(g) for p in r], 2, WATER)
    for L, xf in layers_all:
        for typ, pr, g in L.get('buildings', []):
            if typ == 3 and zoom >= 15: cv.poly(xf(g), BUILD)
    roads = []
    for L, xf in layers_all:
        for typ, pr, g in L.get('roads', []):
            if typ == 2: roads.append((pr.get('sort_rank', 0), pr, xf(g)))
    roads.sort(key=lambda r: r[0])
    sc = 2 ** (zoom - 15.5)
    for pass_ in (0, 1):
        for rank, pr, g in roads:
            kind = pr.get('kind', 'other')
            if kind not in ROADS: kind = 'other'
            fill, wd = ROADS[kind]; wd *= max(0.6, sc)
            for line in g:
                if pass_ == 0: cv.line(line, wd + 2.5, ROAD_CASE)
                else: cv.line(line, wd, fill)
    cv.disc(w / 2, h / 2, 9, (255, 255, 255)); cv.disc(w / 2, h / 2, 6, (30, 136, 229))
    cv.png(out)

if __name__ == '__main__':
    a = sys.argv
    lon, lat = float(a[1]), float(a[2])
    render(m.PMTiles(PM), lon, lat, float(a[4]) if len(a) > 4 else 15.5, int(a[5]) if len(a) > 5 else 480,
           int(a[6]) if len(a) > 6 else 688, a[3] if len(a) > 3 else 'map.png')
