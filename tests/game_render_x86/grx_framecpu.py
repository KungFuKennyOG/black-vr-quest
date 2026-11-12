#!/usr/bin/env python3
"""grx_framecpu.py FILE [FILE...] [--mode 0|1] [--skip N] -- the game thread's CPU per Kelvin flip (GRX_FRAMECPU log of
tests/game_render_x86): per run and pooled, median / p95 / mean in ms. Mode 0 = STEREO (level 1), 1 = FLAT (menus).
The first --skip flips of the mode (default 10: the level's first frames load) are left out. port/notes/perf.md 9."""
import argparse


def frames(path, mode, skip):
    rows = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) == 4:
                rows.append((int(p[0]), int(p[1]), int(p[2]), int(p[3])))
    # rows before the first flat (mode 1) frame are the boot (the shell reports mode 0 until the renderer runs)
    first_flat = next((i for i, r in enumerate(rows) if r[3] == 1), len(rows))
    rows = rows[first_flat:]
    out = []
    for a, b in zip(rows, rows[1:]):
        if b[3] != mode or a[3] != mode:
            continue
        n = b[0] - a[0]
        if n <= 0:
            continue
        out.append(((b[1] - a[1]) / 1e6 / n, (b[2] - a[2]) / 1e6 / n, n))
    return out[skip:]


def stats(v):
    v = sorted(v)
    if not v:
        return "no frames"
    return (f"n={len(v)} median {v[len(v) // 2]:.2f} p95 {v[min(len(v) - 1, int(len(v) * 0.95))]:.2f} "
            f"mean {sum(v) / len(v):.2f} max {v[-1]:.2f} ms")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--mode", type=int, default=0)
    ap.add_argument("--skip", type=int, default=10)
    a = ap.parse_args()
    pooled = []
    for p in a.files:
        fr = frames(p, a.mode, a.skip)
        pooled += [x[0] for x in fr]
        print(f"{p}: game-thread CPU per flip {stats([x[0] for x in fr])}; wall per flip median "
              f"{sorted(x[1] for x in fr)[len(fr) // 2] if fr else 0:.0f} ms")
    if len(a.files) > 1:
        print(f"pooled: {stats(pooled)}")


if __name__ == "__main__":
    main()
