#!/usr/bin/env python3
"""grx_prof.py PROF.bin [--from-flip N] [--to-flip N] [--top N] [--per-flip] [--callers FUNC] [--inclusive]

Reads the game-thread samples written by grx_prof.c (GRX_PROF=<file>) and the /proc/self/maps copy next to them, and
prints where the game thread's CPU time went: by function (self), by category, and optionally inclusive by function
and per game frame (flip). Symbols come from `nm` of the executable and of each shared library in the maps.
Test-only (port/notes/perf.md, section 9).
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess
import sys

MAGIC = 0x50524F46


def load_samples(path):
    out = []
    with open(path, "rb") as f:
        data = f.read()
    i = 0
    n = len(data)
    while i + 24 <= n:
        magic, depth, _pad = struct.unpack_from("<IHH", data, i)
        if magic != MAGIC:
            raise SystemExit(f"bad record at {i}")
        flips, pc = struct.unpack_from("<QQ", data, i + 8)
        frames = struct.unpack_from(f"<{depth}Q", data, i + 24)
        i += 24 + 8 * depth
        out.append((flips, pc, frames))
    return out


class Symbols:
    def __init__(self, maps_path):
        self.maps = []   # (lo, hi, offset, path)
        with open(maps_path) as f:
            for line in f:
                parts = line.split()
                if len(parts) < 6 or not parts[5].startswith("/"):
                    continue
                lo, hi = (int(x, 16) for x in parts[0].split("-"))
                self.maps.append((lo, hi, int(parts[2], 16), parts[5]))
        self.maps.sort()
        self.los = [m[0] for m in self.maps]
        self.tabs = {}
        self.cache = {}

    def _table(self, path):
        if path in self.tabs:
            return self.tabs[path]
        addrs, names = [], []
        for args in (["nm", "-n", "--defined-only", path], ["nm", "-n", "-D", "--defined-only", path]):
            try:
                txt = subprocess.run(args, capture_output=True, text=True, timeout=300).stdout
            except Exception:
                txt = ""
            for line in txt.splitlines():
                p = line.split()
                if len(p) >= 3 and p[1] in "tTwWiI":
                    addrs.append(int(p[0], 16))
                    names.append(p[2])
            if addrs:
                break
        z = sorted(zip(addrs, names))
        tab = ([a for a, _ in z], [n for _, n in z])
        # ELF vaddr of the first executable segment vs its file offset (PIE / .so: vaddr == offset for these)
        self.tabs[path] = tab
        return tab

    def name(self, pc):
        if pc in self.cache:
            return self.cache[pc]
        j = bisect.bisect_right(self.los, pc) - 1
        res = ("?", "?")
        if j >= 0:
            lo, hi, off, path = self.maps[j]
            if pc < hi:
                # find the mapping base of this file (lowest mapping with offset 0)
                base = min(m[0] - m[2] for m in self.maps if m[3] == path)
                rel = pc - base
                addrs, names = self._table(path)
                k = bisect.bisect_right(addrs, rel) - 1
                lib = os.path.basename(path)
                res = (names[k] if k >= 0 else "?", lib)
        self.cache[pc] = res
        return res


CATS = [
    ("guest code (sub_*)", lambda n, lib: n.startswith("sub_") or n.startswith("loc_")),
    ("libm", lambda n, lib: lib.startswith("libm")),
    ("libc mem/str", lambda n, lib: lib.startswith("libc") and any(s in n for s in ("mem", "str"))),
    ("libc other", lambda n, lib: lib.startswith("libc")),
    ("indirect dispatch", lambda n, lib: n.startswith("recomp_lookup") or n.startswith("recomp_icall") or "dispatch" in n),
    ("manual / black_*", lambda n, lib: n.startswith("black_") or n.startswith("manual_") or n.startswith("recomp_")),
    ("kernel HLE (xbox_*)", lambda n, lib: n.startswith("xbox_") or n.startswith("kernel_") or n.startswith("Ke") or n.startswith("Rtl")),
    ("kelvin / nv2a", lambda n, lib: n.startswith("kelvin") or n.startswith("nv2a") or n.startswith("kgpu") or n.startswith("kvk")),
    ("apu / dsound", lambda n, lib: n.startswith("apu") or n.startswith("dsound") or n.startswith("mcpx")),
    ("fault / signal", lambda n, lib: "fault" in n or "segv" in n or "sig" in n or n.startswith("a64_") or "mmio" in n),
    ("win32 compat", lambda n, lib: True),
]


def cat_of(n, lib):
    for c, f in CATS:
        if f(n, lib):
            return c
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prof")
    ap.add_argument("--from-flip", type=int, default=0)
    ap.add_argument("--to-flip", type=int, default=1 << 62)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--per-flip", action="store_true")
    ap.add_argument("--inclusive", action="store_true")
    ap.add_argument("--callers", default=None, help="show the callers (stack frames above) of samples in FUNC")
    ap.add_argument("--us", type=float, default=250.0, help="sampling period (GRX_PROF_US)")
    ap.add_argument("--exclude", action="append", default=[],
                    help="leave out samples whose leaf is FUNC (e.g. a guest spin-wait), repeatable")
    a = ap.parse_args()
    sym = Symbols(a.prof + ".maps")
    samples = [s for s in load_samples(a.prof) if a.from_flip <= s[0] < a.to_flip]
    if a.exclude:
        before = len(samples)
        samples = [s for s in samples if sym.name(s[1])[0] not in a.exclude]
        print(f"left out {before - len(samples)} of {before} samples ({(before - len(samples)) * 100.0 / max(before, 1):.1f}%) "
              f"in {', '.join(a.exclude)}")
    if not samples:
        print("no samples in range")
        return
    flips = (samples[-1][0] - samples[0][0] + 1) or 1
    ms = a.us / 1000.0
    total = len(samples)
    print(f"{total} samples = {total * ms:.0f} ms of game-thread CPU over flips {samples[0][0]}..{samples[-1][0]} "
          f"({flips} flips, {total * ms / flips:.2f} ms/flip)")
    self_c = collections.Counter()
    cat_c = collections.Counter()
    incl = collections.Counter()
    per_flip = collections.Counter()
    callers = collections.Counter()
    for fl, pc, frames in samples:
        n, lib = sym.name(pc)
        self_c[(n, lib)] += 1
        cat_c[cat_of(n, lib)] += 1
        per_flip[fl] += 1
        # frames[0..] = handler, trampoline, interrupted pc, callers...
        st = list(frames)
        k = st.index(pc) if pc in st else 2
        chain = [sym.name(x) for x in st[k:]]
        if a.inclusive:
            seen = set()
            for c in chain:
                if c not in seen:
                    incl[c] += 1
                    seen.add(c)
        if a.callers and n == a.callers:
            callers[" <- ".join(c[0] for c in chain[1:7])] += 1
    print("\n== by category (self)")
    for c, v in cat_c.most_common():
        print(f"  {c:24s} {v * 100.0 / total:6.2f}%  {v * ms / flips:8.3f} ms/flip")
    print(f"\n== top {a.top} functions (self)")
    for (n, lib), v in self_c.most_common(a.top):
        print(f"  {v * 100.0 / total:6.2f}%  {v * ms / flips:8.3f} ms/flip  {n}  [{lib}]")
    if a.inclusive:
        print(f"\n== top {a.top} functions (inclusive)")
        for (n, lib), v in incl.most_common(a.top):
            print(f"  {v * 100.0 / total:6.2f}%  {v * ms / flips:8.3f} ms/flip  {n}  [{lib}]")
    if a.callers:
        print(f"\n== callers of {a.callers}")
        for c, v in callers.most_common(a.top):
            print(f"  {v * 100.0 / total:6.2f}%  {c}")
    if a.per_flip:
        print("\n== ms per flip")
        vals = sorted(v * ms for f, v in per_flip.items())
        lo, hi = samples[0][0], samples[-1][0]
        row = []
        for f in range(lo, hi + 1):
            row.append(f"{f}:{per_flip.get(f, 0) * ms:.1f}")
        print("  " + " ".join(row))
        allv = sorted(per_flip.get(f, 0) * ms for f in range(lo, hi + 1))
        if allv:
            print(f"  median {allv[len(allv) // 2]:.2f} ms, p95 {allv[int(len(allv) * 0.95)]:.2f} ms, max {allv[-1]:.2f} ms")


if __name__ == "__main__":
    main()
