"""PMTiles v3 + Mapbox Vector Tile reader (prototype of what the NAV-1 map app does on the device)."""
import struct, gzip, math

def varint(b, i):
    r = 0; s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7F) << s
        if c < 0x80: return r, i
        s += 7

def zz(n): return (n >> 1) ^ -(n & 1)

def tile_id(z, x, y):
    # Hilbert curve id (PMTiles spec)
    acc = 0
    for t in range(z): acc += 1 << (2 * t)
    n = 1 << z
    d = 0
    s = n // 2
    while s > 0:
        rx = 1 if (x & s) > 0 else 0
        ry = 1 if (y & s) > 0 else 0
        d += s * s * ((3 * rx) ^ ry)
        if ry == 0:
            if rx == 1:
                x = n - 1 - x; y = n - 1 - y
            x, y = y, x
        s //= 2
    return acc + d

class PMTiles:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(127)
        assert h[:7] == b'PMTiles'
        (self.root_off, self.root_len, self.meta_off, self.meta_len, self.leaf_off, self.leaf_len,
         self.data_off, self.data_len) = struct.unpack('<8Q', h[8:72])
        self.tile_comp = h[98]; self.int_comp = h[97]

    def _dir(self, off, ln):
        self.f.seek(off); raw = self.f.read(ln)
        if self.int_comp == 2: raw = gzip.decompress(raw)
        i = 0
        n, i = varint(raw, i)
        ids = []; last = 0
        for _ in range(n):
            d, i = varint(raw, i); last += d; ids.append(last)
        runs = []
        for _ in range(n):
            v, i = varint(raw, i); runs.append(v)
        lens = []
        for _ in range(n):
            v, i = varint(raw, i); lens.append(v)
        offs = []
        for k in range(n):
            v, i = varint(raw, i)
            if v == 0 and k > 0: offs.append(offs[-1] + lens[k - 1])
            else: offs.append(v - 1)
        return list(zip(ids, runs, lens, offs))

    def get(self, z, x, y):
        tid = tile_id(z, x, y)
        off, ln = self.root_off, self.root_len
        for _ in range(4):
            ents = self._dir(off, ln)
            # binary search last entry with id <= tid
            lo, hi = 0, len(ents) - 1; hit = None
            while lo <= hi:
                m = (lo + hi) // 2
                if ents[m][0] <= tid: hit = m; lo = m + 1
                else: hi = m - 1
            if hit is None: return None
            e = ents[hit]
            if e[1] > 0:
                if tid < e[0] + e[1]:
                    self.f.seek(self.data_off + e[3]); d = self.f.read(e[2])
                    return gzip.decompress(d) if self.tile_comp == 2 else d
                return None
            off, ln = self.leaf_off + e[3], e[2]
        return None

def parse_mvt(b):
    layers = {}
    i = 0
    while i < len(b):
        k, i = varint(b, i)
        f, w = k >> 3, k & 7
        if f == 3 and w == 2:
            ln, i = varint(b, i); layers_b = b[i:i + ln]; i += ln
            name, feats = parse_layer(layers_b)
            layers[name] = feats
        else:
            i = skip(b, i, w)
    return layers

def skip(b, i, w):
    if w == 0: _, i = varint(b, i)
    elif w == 2: ln, i = varint(b, i); i += ln
    elif w == 1: i += 8
    elif w == 5: i += 4
    return i

def parse_layer(b):
    i = 0; name = ''; keys = []; vals = []; feats_raw = []; extent = 4096
    while i < len(b):
        k, i = varint(b, i)
        f, w = k >> 3, k & 7
        if w == 2:
            ln, i = varint(b, i); d = b[i:i + ln]; i += ln
            if f == 1: name = d.decode()
            elif f == 2: feats_raw.append(d)
            elif f == 3: keys.append(d.decode())
            elif f == 4: vals.append(parse_value(d))
        elif w == 0:
            v, i = varint(b, i)
            if f == 5: extent = v
        else: i = skip(b, i, w)
    feats = []
    for d in feats_raw:
        i = 0; typ = 0; tags = []; geom = []
        while i < len(d):
            k, i = varint(d, i); f, w = k >> 3, k & 7
            if w == 2:
                ln, i = varint(d, i); p = d[i:i + ln]; i += ln
                if f == 2:
                    j = 0
                    while j < len(p): v, j = varint(p, j); tags.append(v)
                elif f == 4:
                    j = 0
                    while j < len(p): v, j = varint(p, j); geom.append(v)
            elif w == 0:
                v, i = varint(d, i)
                if f == 3: typ = v
            else: i = skip(d, i, w)
        props = {keys[tags[t]]: vals[tags[t + 1]] for t in range(0, len(tags), 2)}
        feats.append((typ, props, decode_geom(geom, typ)))
    return name, feats

def parse_value(b):
    i = 0
    while i < len(b):
        k, i = varint(b, i); f, w = k >> 3, k & 7
        if w == 2:
            ln, i = varint(b, i); s = b[i:i + ln]; i += ln
            if f == 1: return s.decode()
        elif w == 0:
            v, i = varint(b, i)
            if f in (4, 5): return v
            if f == 6: return zz(v)
            if f == 7: return bool(v)
        elif w == 5: i += 4
        elif w == 1: i += 8
    return None

def decode_geom(g, typ):
    rings = []; cur = []; x = y = 0; i = 0
    while i < len(g):
        c = g[i]; i += 1
        cmd, cnt = c & 7, c >> 3
        for _ in range(cnt):
            if cmd == 1:
                if cur: rings.append(cur)
                cur = []
            if cmd in (1, 2):
                x += zz(g[i]); y += zz(g[i + 1]); i += 2
                cur.append((x, y))
            elif cmd == 7:
                if cur: cur.append(cur[0])
    if cur: rings.append(cur)
    return rings

def lonlat_to_tile(lon, lat, z):
    n = 1 << z
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
    return x, y
