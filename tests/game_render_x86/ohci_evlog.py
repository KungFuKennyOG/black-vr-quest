#!/usr/bin/env python3
"""Print the OHCI controller event ring (RECOMP_USB_EVLOG=<file>, usb/ohci.c), oldest first.

  python3 tests/game_render_x86/ohci_evlog.py <ev.bin> [last N events, default 200] [--ed ED_VA]

Each record: seq, ms, type, a, b, c, d, phase (0 between frames, 1 in the list walk, 2 writing the done queue back).
"""
import struct
import sys

NAMES = {1: "SF", 2: "RETIRE", 3: "PUBLISH", 4: "HELD", 5: "rd IS", 6: "wr IS", 7: "wr IE", 8: "HEADP", 9: "GATE",
         10: "ED", 11: "wr CS", 12: "FNO"}


def fmt(t, a, b, c, d):
    n = NAMES.get(t, "?%d" % t)
    if t == 1:
        return "%-8s frame %5u status %08X" % (n, a, b)
    if t == 2:
        return "%-8s td %08X ed %08X cc %X info %08X urb %08X" % (n, a, b, c >> 28, c, d)
    if t in (3, 4):
        return "%-8s head %08X status %08X" % (n, a, b)
    if t == 5:
        return "%-8s = %08X (enable %08X) frame %5u held %08X" % (n, a, d, c, b)
    if t == 6:
        return "%-8s %08X (was %08X)" % (n, a, b)
    if t == 7:
        return "%-8s %s %08X" % (n, "set" if b else "clear", a)
    if t == 8:
        return "%-8s ed %08X %08X -> %08X" % (n, a, b, c)
    if t == 9:
        return "%-8s frame held for %u ticks, status %08X" % (n, a, b)
    if t == 10:
        return "%-8s %08X info %08X head %08X tail %08X" % (n, a, b, c, d)
    return "%-8s %08X %08X %08X %08X" % (n, a, b, c, d)


def main():
    data = open(sys.argv[1], "rb").read()
    last = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 200
    ed = int(sys.argv[sys.argv.index("--ed") + 1], 16) if "--ed" in sys.argv else None
    recs = [struct.unpack_from("<8I", data, o) for o in range(0, len(data) - 31, 32)]
    recs = sorted((r for r in recs if r[0]), key=lambda r: r[0])
    if ed is not None:
        recs = [r for r in recs if r[2] in (1, 3, 4, 5, 6, 7, 9, 11, 12) or ed in (r[3], r[4])]
    t0 = recs[0][1] if recs else 0
    for seq, ms, t, a, b, c, d, ph in recs[-last:]:
        print("%7u %8.3f p%u %s" % (seq, (ms - t0) / 1000.0, ph, fmt(t, a, b, c, d)))


if __name__ == "__main__":
    main()
