"""cardverify.py - compare every file of a folder with the card, reading the card itself (not Windows' cache).

    python cardverify.py <source folder> <card letter, e.g. G>
Locks + dismounts the card volume first (drops the cached copy), then compares MD5 of every file.
"""
import ctypes, hashlib, os, sys
from cardcheck import lock_volume


def main():
    src, letter = sys.argv[1], sys.argv[2]
    k32, h = lock_volume(letter)
    k32.DeviceIoControl(h, 0x9001C, None, 0, None, 0, ctypes.byref(ctypes.c_ulong()), None)   # unlock
    k32.CloseHandle(h)
    root_card = letter + ":" + os.sep
    bad = n = 0
    for root, dirs, files in os.walk(src):
        for f in files:
            p = os.path.join(root, f)
            q = os.path.join(root_card, os.path.relpath(p, src))
            n += 1
            try:
                same = hashlib.md5(open(p, "rb").read()).digest() == hashlib.md5(open(q, "rb").read()).digest()
            except OSError as e:
                same = False
                print("ERROR", q, e)
            if not same:
                bad += 1
                print("DIFF", q)
    print("%d files compared (read from the card after a remount), %d differences" % (n, bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
