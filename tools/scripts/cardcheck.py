"""cardcheck.py - is an SD card really as big as it claims? (like GRC ValiDrive, non-destructive)

Run as administrator; the card volume is locked + dismounted while it runs (Windows remounts it):
    python cardcheck.py <disk number> <samples> <size in bytes> <volume letter, e.g. G>
For each of <samples> spots spread over the whole card: read the 4 KB there (kept), write a unique
pattern; after ALL writes, read every spot back (a fake card returns zeros/old data beyond its real
capacity, or the pattern of another spot when high addresses wrap onto low ones); finally write the
original 4 KB back everywhere. Prints the result per region and the highest spot that worked.
"""
import hashlib, os, sys

BLOCK = 4096


def lock_volume(letter):
    """Removable cards cannot be set offline: lock + dismount the volume instead (handle kept open)."""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    h = k32.CreateFileW(r"\\.\%s:" % letter, 0xC0000000, 3, None, 3, 0, None)   # RW, share RW, OPEN_EXISTING
    if h in (None, wintypes.HANDLE(-1).value):
        raise OSError("cannot open volume %s: (error %d)" % (letter, ctypes.get_last_error()))
    ret = wintypes.DWORD()
    for code, name in ((0x90018, "lock"), (0x90020, "dismount")):          # FSCTL_LOCK/DISMOUNT_VOLUME
        if not k32.DeviceIoControl(h, code, None, 0, None, 0, ctypes.byref(ret), None):
            raise OSError("%s %s: failed (error %d)" % (name, letter, ctypes.get_last_error()))
    return k32, h


def main():
    disk = int(sys.argv[1])
    samples = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
    size = int(sys.argv[3])                         # bytes, from Get-Disk (seek-to-end fails on raw disks)
    vol = lock_volume(sys.argv[4]) if len(sys.argv) > 4 else None
    path = r"\\.\PhysicalDrive%d" % disk
    fd = os.open(path, os.O_RDWR | os.O_BINARY)
    blocks = size // BLOCK
    # spots: evenly spread from 64 MiB to the last block (the first 64 MiB hold partition + file system)
    first = (64 << 20) // BLOCK
    spots = sorted({first + (blocks - 1 - first) * i // (samples - 1) for i in range(samples)})

    def rd(b):
        os.lseek(fd, b * BLOCK, os.SEEK_SET)
        return os.read(fd, BLOCK)

    def wr(b, data):
        os.lseek(fd, b * BLOCK, os.SEEK_SET)
        n = os.write(fd, data)
        if n != BLOCK:
            raise IOError("short write at block %d" % b)

    def pattern(b):
        seed = hashlib.sha256(b"NAV-1 cardcheck %d" % b).digest()
        return (seed * (BLOCK // len(seed)))[:BLOCK]

    print("disk %d: %d bytes (%.2f GiB), %d spots" % (disk, size, size / 2**30, len(spots)), flush=True)
    orig = {}
    try:
        for b in spots:
            orig[b] = rd(b)
        for b in spots:
            wr(b, pattern(b))
        os.fsync(fd)
        good, bad = [], []
        for b in spots:
            (good if rd(b) == pattern(b) else bad).append(b)
    finally:
        restored = 0
        for b, data in orig.items():
            try:
                wr(b, data)
                restored += 1
            except Exception as e:
                print("RESTORE FAILED at block %d: %s" % (b, e))
        os.fsync(fd)
        os.close(fd)
        if vol:
            import ctypes
            vol[0].DeviceIoControl(vol[1], 0x9001C, None, 0, None, 0, ctypes.byref(ctypes.c_ulong()), None)  # unlock
            vol[0].CloseHandle(vol[1])
        print("original contents written back at %d of %d spots" % (restored, len(orig)))

    gib = lambda b: b * BLOCK / 2**30
    print("spots OK: %d, failed: %d" % (len(good), len(bad)))
    if good:
        print("highest spot that worked: %.2f GiB" % gib(max(good)))
    if bad:
        print("lowest spot that failed:  %.2f GiB" % gib(min(bad)))
        print("RESULT: FAKE or FAULTY card - real capacity about %.1f GiB (claims %.1f GiB)"
              % (gib(min(bad)), size / 2**30))
    else:
        print("RESULT: every spot across the whole card stored its data - capacity looks GENUINE")


if __name__ == "__main__":
    main()
