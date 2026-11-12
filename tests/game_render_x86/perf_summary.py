#!/usr/bin/env python3
"""perf_summary.py LOG [LOG...] -- aggregate the [PERF] interval reports of tests/game_render_x86 (grx_perf.c) by mode.

For each mode (FLAT = title/menus/loading on the flat quad, STEREO = level 1) it sums the intervals and prints, per game
frame (flip): pushbuffer words, methods, draws, trapped-MMIO faults (total and by device/register/direction/thread),
handler time, and CPU time per thread class. Intervals with no flip are left out (a stalled or loading interval says
nothing per frame). See port/notes/perf.md.
"""
import re
import sys
from collections import defaultdict

IV = re.compile(r"\[PERF\] interval (\S+) wall ([\d.]+) flips (\d+) words (\d+) methods (\d+) draws (\d+) vertices (\d+) "
                r"faults (\d+) fault_us (\d+)")
CPU = re.compile(r"\[PERF\]   cpu (.+?)\s+x(\d+)\s+([\d.]+) ms")
FLT = re.compile(r"\[PERF\]   fault (\S+)\s+(\S+)\s+([RW]) (.+?)\s+(\d+)\s+[\d.]+/flip\s+([\d.]+) us")


def main(paths):
    agg = defaultdict(lambda: {"n": 0, "wall": 0.0, "flips": 0, "words": 0, "methods": 0, "draws": 0, "faults": 0,
                               "fault_us": 0.0, "cpu": defaultdict(float), "flt": defaultdict(lambda: [0, 0.0])})
    for path in paths:
        cur = None
        with open(path, errors="replace") as f:
            for line in f:
                m = IV.search(line)
                if m:
                    mode, wall, flips = m.group(1), float(m.group(2)), int(m.group(3))
                    cur = agg[mode] if flips > 0 else None
                    if cur is None:
                        continue
                    cur["n"] += 1
                    cur["wall"] += wall
                    cur["flips"] += flips
                    cur["words"] += int(m.group(4))
                    cur["methods"] += int(m.group(5))
                    cur["draws"] += int(m.group(6))
                    cur["faults"] += int(m.group(8))
                    cur["fault_us"] += float(m.group(9))
                    continue
                if cur is None:
                    continue
                m = CPU.search(line)
                if m:
                    cur["cpu"][m.group(1).strip()] += float(m.group(3))
                    continue
                m = FLT.search(line)
                if m:
                    key = (m.group(1), m.group(2), m.group(3), m.group(4).strip())
                    cur["flt"][key][0] += int(m.group(5))
                    cur["flt"][key][1] += int(m.group(5)) * float(m.group(6))
    for mode, a in sorted(agg.items()):
        fl = a["flips"]
        print(f"== {mode}: {a['n']} intervals, {a['wall']:.0f} s, {fl} flips ({fl / a['wall']:.1f}/s)")
        print(f"   per flip: {a['words'] / fl:.0f} pushbuffer words, {a['methods'] / fl:.0f} methods, {a['draws'] / fl:.1f} draws, "
              f"{a['faults'] / fl:.1f} trapped accesses ({a['faults'] / a['wall']:.0f}/s), handler time "
              f"{a['fault_us'] / fl:.0f} us/flip (locks included)")
        print("   CPU per flip by thread (ms):")
        for k, v in sorted(a["cpu"].items(), key=lambda kv: -kv[1]):
            print(f"     {k:34s} {v / fl:8.2f}   ({v / a['wall'] / 10:.1f}% of a core)")
        print("   trapped accesses per flip (top 25):")
        for k, (n, us) in sorted(a["flt"].items(), key=lambda kv: -kv[1][0])[:25]:
            print(f"     {k[0]:11s} {k[1]:9s} {k[2]} {k[3]:24s} {n / fl:8.2f}   {us / n:8.2f} us avg")


if __name__ == "__main__":
    main(sys.argv[1:])
