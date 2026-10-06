"""Move a recorded NMEA session to another place (for demos and screenshots without real locations).
usage: shiftnmea.py in.txt out.NMEA <target_lat> <target_lon> [altitude_offset_m]
The first valid position of the session is put at the target; all later positions keep their offsets.
Checksums are recomputed. Input may be a raw console capture (only lines starting with '$' are used)."""
import sys, re

src, dst, tlat, tlon = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
dalt = float(sys.argv[5]) if len(sys.argv) > 5 else 0.0

def csum(body):
    c = 0
    for ch in body.encode('ascii'):
        c ^= ch
    return '%02X' % c

def parse_lat(v, h):
    d = float(v[:2]) + float(v[2:]) / 60.0
    return -d if h == 'S' else d

def parse_lon(v, h):
    d = float(v[:3]) + float(v[3:]) / 60.0
    return -d if h == 'W' else d

def fmt_lat(x):
    h = 'S' if x < 0 else 'N'; x = abs(x)
    d = int(x); m = (x - d) * 60
    return '%02d%010.7f' % (d, m), h

def fmt_lon(x):
    h = 'W' if x < 0 else 'E'; x = abs(x)
    d = int(x); m = (x - d) * 60
    return '%03d%010.7f' % (d, m), h

lines = [l.strip() for l in open(src, encoding='latin-1', errors='ignore').read().split('\n')]
lines = [l for l in lines if l.startswith('$') and '*' in l]
off = None
out = []
for l in lines:
    body, _ = l[1:].split('*', 1)
    f = body.split(',')
    kind = f[0][2:]
    if kind in ('RMC', 'GGA', 'GLL'):
        la_i, lo_i = {'RMC': (3, 5), 'GGA': (2, 4), 'GLL': (1, 3)}[kind]
        if len(f) > lo_i + 1 and f[la_i] and f[lo_i]:
            lat = parse_lat(f[la_i], f[la_i + 1]); lon = parse_lon(f[lo_i], f[lo_i + 1])
            if off is None:
                off = (tlat - lat, tlon - lon)
            lat += off[0]; lon += off[1]
            f[la_i], f[la_i + 1] = fmt_lat(lat)
            f[lo_i], f[lo_i + 1] = fmt_lon(lon)
            if kind == 'GGA' and f[9]:
                f[9] = '%.1f' % (float(f[9]) + dalt)
    nb = ','.join(f)
    out.append('$%s*%s' % (nb, csum(nb)))
open(dst, 'w', newline='\n').write('\n'.join(out) + '\n')
print(len(out), 'sentences, offset', off)
