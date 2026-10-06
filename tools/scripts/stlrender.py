"""Tiny software renderer for a binary STL (no dependencies): shaded orthographic views as PNG.
usage: stlrender.py model.stl out.png yaw_deg pitch_deg [width=1600] [height=1100] [colour=3D5A9E]
yaw turns the model about the vertical axis, pitch tilts it towards the camera (90 = looking straight down)."""
import sys, struct, math, zlib

def load(path):
    d = open(path, 'rb').read()
    n = struct.unpack('<I', d[80:84])[0]
    tris = []
    for i in range(n):
        o = 84 + i * 50
        v = struct.unpack('<9f', d[o + 12:o + 48])
        tris.append(((v[0], v[1], v[2]), (v[3], v[4], v[5]), (v[6], v[7], v[8])))
    return tris

def png(path, w, h, rows):
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    def ch(t, data): return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))

def render(tris, yaw, pitch, W, H, base):
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    xs = [p[0] for t in tris for p in t]; ys = [p[1] for t in tris for p in t]; zs = [p[2] for t in tris for p in t]
    cx, cyy, cz = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2

    def xf(p):                                    # model (x right, y up the sheet, z up from the bed) -> camera
        x, y, z = p[0] - cx, p[1] - cyy, p[2] - cz
        x1, y1 = x * cy - y * sy, x * sy + y * cy         # yaw about z
        # pitch about the camera's x axis: screen up = z*cp + y1*sp ... depth = y1*cp - z*sp
        return x1, z * cp + y1 * sp, y1 * cp - z * sp
    P = [tuple(xf(p) for p in t) for t in tris]
    sx = [v[0] for t in P for v in t]; sy_ = [v[1] for t in P for v in t]
    scale = min((W * 0.92) / (max(sx) - min(sx)), (H * 0.92) / (max(sy_) - min(sy_)))
    mx, my = (max(sx) + min(sx)) / 2, (max(sy_) + min(sy_)) / 2
    zbuf = [1e30] * (W * H)
    img = bytearray(W * H * 3)
    # background: soft vertical gradient
    for y in range(H):
        t = y / H
        c = bytes((int(246 - 26 * t), int(247 - 24 * t), int(250 - 18 * t)))
        img[y * W * 3:(y + 1) * W * 3] = c * W
    L1 = (-0.45, 0.35, 0.82); L2 = (0.6, -0.2, 0.5)
    n1 = math.sqrt(sum(a * a for a in L1)); L1 = tuple(a / n1 for a in L1)
    n2 = math.sqrt(sum(a * a for a in L2)); L2 = tuple(a / n2 for a in L2)
    br, bg, bb = base
    for t in P:
        a, b, c = t
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        ln = math.sqrt(nx * nx + ny * ny + nz * nz)
        if ln == 0: continue
        nx, ny, nz = nx / ln, ny / ln, nz / ln
        if nz > 0: nx, ny, nz = -nx, -ny, -nz          # facing the camera (depth axis points away)
        # camera-space normal -> light in camera space (screen x right, y up, depth away)
        d1 = max(0.0, nx * L1[0] + ny * L1[1] - nz * L1[2])
        d2 = max(0.0, nx * L2[0] + ny * L2[1] - nz * L2[2])
        lum = 0.38 + 0.62 * d1 + 0.22 * d2
        col = (min(255, int(br * lum)), min(255, int(bg * lum)), min(255, int(bb * lum)))
        pts = [((v[0] - mx) * scale + W / 2, H / 2 - (v[1] - my) * scale, v[2]) for v in t]
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = pts
        minx = max(0, int(min(x0, x1, x2))); maxx = min(W - 1, int(max(x0, x1, x2)) + 1)
        miny = max(0, int(min(y0, y1, y2))); maxy = min(H - 1, int(max(y0, y1, y2)) + 1)
        den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(den) < 1e-9: continue
        for py in range(miny, maxy + 1):
            yy = py + 0.5
            for px in range(minx, maxx + 1):
                xx = px + 0.5
                l0 = ((y1 - y2) * (xx - x2) + (x2 - x1) * (yy - y2)) / den
                l1 = ((y2 - y0) * (xx - x2) + (x0 - x2) * (yy - y2)) / den
                l2 = 1 - l0 - l1
                if l0 < -1e-4 or l1 < -1e-4 or l2 < -1e-4: continue
                z = l0 * z0 + l1 * z1 + l2 * z2
                i = py * W + px
                if z < zbuf[i]:
                    zbuf[i] = z
                    img[i * 3:i * 3 + 3] = bytes(col)
    return [img[y * W * 3:(y + 1) * W * 3] for y in range(H)]

if __name__ == '__main__':
    a = sys.argv
    W = int(a[5]) if len(a) > 5 else 1600
    H = int(a[6]) if len(a) > 6 else 1100
    hexc = a[7] if len(a) > 7 else '3D5A9E'
    base = tuple(int(hexc[i:i + 2], 16) for i in (0, 2, 4))
    tris = load(a[1])
    png(a[2], W, H, render(tris, float(a[3]), float(a[4]), W, H, base))
    print(len(tris), 'triangles ->', a[2])
