"""Download EOX Sentinel-2 cloudless 2024 tiles for Israel into an MBTiles file (resumable, 4 threads).
License: CC BY-NC-SA 4.0 (personal, non-commercial use, attribution below)."""
import math, sqlite3, sys, time, urllib.request, concurrent.futures as cf

W, S, E, N = 34.2, 29.45, 35.95, 33.35
MAXZ = 14
URL = "https://tiles.maps.eox.at/wmts/1.0.0/s2cloudless-2024_3857/default/g/{z}/{y}/{x}.jpg"
ATTR = "Sentinel-2 cloudless 2024 by EOX IT Services GmbH (contains modified Copernicus Sentinel data 2024), CC BY-NC-SA 4.0"

def tile(lon, lat, z):
    n = 2 ** z
    x = int((lon + 180) / 360 * n)
    y = int((1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n)
    return x, y

def tiles():
    for z in range(0, MAXZ + 1):
        x0, y0 = tile(W, N, z)
        x1, y1 = tile(E, S, z)
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                yield z, x, y

db = sqlite3.connect(sys.argv[1])
db.execute("create table if not exists metadata (name text, value text)")
db.execute("create table if not exists tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob)")
db.execute("create unique index if not exists t on tiles (zoom_level, tile_column, tile_row)")
if not db.execute("select 1 from metadata").fetchone():
    for k, v in {"name": "Israel satellite", "format": "jpg", "type": "baselayer", "minzoom": "0", "maxzoom": str(MAXZ),
                 "bounds": f"{W},{S},{E},{N}", "attribution": ATTR}.items():
        db.execute("insert into metadata values (?, ?)", (k, v))
done = {(z, x, (2 ** z - 1) - r) for z, x, r in db.execute("select zoom_level, tile_column, tile_row from tiles")}
todo = [t for t in tiles() if t not in done]
print(f"{len(done)} done, {len(todo)} to fetch", flush=True)

def get(t):
    z, x, y = t
    for attempt in range(4):
        try:
            req = urllib.request.Request(URL.format(z=z, x=x, y=y), headers={"User-Agent": "NAV-1 personal offline map (one-time)"})
            with urllib.request.urlopen(req, timeout=30) as r:
                return t, r.read()
        except Exception as e:
            err = e
            time.sleep(2 * (attempt + 1))
    return t, None

n = fail = 0
with cf.ThreadPoolExecutor(4) as pool:
    for (z, x, y), data in pool.map(get, todo):
        if data:
            db.execute("insert or replace into tiles values (?, ?, ?, ?)", (z, x, (2 ** z - 1) - y, data))
        else:
            fail += 1
        n += 1
        if n % 1000 == 0:
            db.commit()
            print(f"{n}/{len(todo)} fetched, {fail} failed", flush=True)
db.commit()
print(f"finished: {n} fetched, {fail} failed", flush=True)
