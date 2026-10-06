"""cardfill.py - DESTRUCTIVE full test of the first N GiB of a card (every byte written and read back).

Use after cardcheck.py showed a fake card, to prove the part that will be used really stores data:
    python cardfill.py <disk number> <GiB to test> <expected disk size in bytes> [volume letter to lock]
Writes a unique pattern to every 1 MiB, then reads everything back. Erases that area (partition table
included) - back the card up first. Afterwards the disk has no partition (create one with diskpart).
"""
import hashlib, os, sys, time
from cardcheck import lock_volume

CHUNK = 1 << 20


def pattern(i):
    seed = hashlib.sha256(b"NAV-1 cardfill %d" % i).digest()
    return (seed * (CHUNK // len(seed)))[:CHUNK]


def main():
    disk, gib, expect = int(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
    vol = lock_volume(sys.argv[4]) if len(sys.argv) > 4 else None
    n = int(gib * 1024)
    fd = os.open(r"\\.\PhysicalDrive%d" % disk, os.O_RDWR | os.O_BINARY)
    print("disk %d (expected %d bytes): testing %d MiB" % (disk, expect, n), flush=True)
    t0 = time.time()
    os.lseek(fd, 0, os.SEEK_SET)
    for i in range(n):
        if os.write(fd, pattern(i)) != CHUNK:
            raise IOError("short write at MiB %d" % i)
        if i % 1024 == 1023:
            print("  written %d MiB (%.0f MB/s)" % (i + 1, (i + 1) * CHUNK / 1e6 / (time.time() - t0)), flush=True)
    os.fsync(fd)
    t1 = time.time()
    os.lseek(fd, 0, os.SEEK_SET)
    bad = []
    for i in range(n):
        if os.read(fd, CHUNK) != pattern(i):
            bad.append(i)
    t2 = time.time()
    os.lseek(fd, 0, os.SEEK_SET)
    os.write(fd, bytes(CHUNK))                     # blank the first MiB: no stale partition table
    os.fsync(fd)
    os.close(fd)
    if vol:
        import ctypes
        vol[0].DeviceIoControl(vol[1], 0x9001C, None, 0, None, 0, ctypes.byref(ctypes.c_ulong()), None)
        vol[0].CloseHandle(vol[1])
    print("write %.0f MB/s, read %.0f MB/s" % (n * CHUNK / 1e6 / (t1 - t0), n * CHUNK / 1e6 / (t2 - t1)))
    if bad:
        print("RESULT: %d of %d MiB FAILED, first at %.2f GiB" % (len(bad), n, bad[0] / 1024))
    else:
        print("RESULT: all %d MiB stored and read back correctly" % n)


if __name__ == "__main__":
    main()
