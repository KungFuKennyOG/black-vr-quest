#!/usr/bin/env python3
"""pass_traffic.py: the attachment traffic of one frame's render passes from a RECOMP_KVK_PASSLOG log (kvk_backend.c
"PASS f N ..." lines), scaled to another eye size: a tiler's view (LOAD reads the attachment in, STORE writes it out;
CLEAR and DONT_CARE move nothing). Main-target-sized attachments (the log's eye size) are scaled to each --eye size;
offscreen targets (160x120 ...) and the 1280x960 HUD keep theirs.

  pass_traffic.py <log> --frame N --log-eye 480x512 --eye 1680x1760 --eye 2016x2112
"""
import argparse, re, sys

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("--frame", type=int, required=True)
ap.add_argument("--log-eye", default="480x512")
ap.add_argument("--eye", action="append", default=[])
ap.add_argument("--depth-bytes", type=int, default=4, help="bytes per depth texel on the device (D24S8: 4)")
a = ap.parse_args()
lw, lh = map(int, a.log_eye.split("x"))
pass_re = re.compile(r"PASS f (\d+) (\w+)\s+c ([0-9A-F]{8})( \(HUD\))? d ([0-9A-F]{8}) (\d+)x(\d+) x(\d+) \| colour (\S+)/(\S+) depth (\S+)/(\S+)")
ipass_re = re.compile(r"PASS f (\d+) (\w+)\s+(depth as colour \(d2c\) of|colour to depth \(c2d\)|the eye image\(s\)|a flat/HUD layer image) (\d+)x(\d+) x(\d+) \| (\S+)/STORE")
rows = []
for line in open(a.log, errors="replace"):
    m = pass_re.search(line)
    if m and int(m.group(1)) == a.frame:
        st, c, hud, d, w, h, l, cl, cs, dl, ds = m.group(2), m.group(3), m.group(4), m.group(5), int(m.group(6)), int(m.group(7)), int(m.group(8)), m.group(9), m.group(10), m.group(11), m.group(12)
        rows.append(dict(stage=st, what="c %s%s d %s" % (c, hud or "", d), w=w, h=h, l=l, cl=cl, cs=cs, dl=dl if d != "00000000" else "-", ds=ds if d != "00000000" else "-", hasc=c != "00000000", hasd=d != "00000000"))
        continue
    m = ipass_re.search(line)
    if m and int(m.group(1)) == a.frame:
        st, what, w, h, l, load = m.group(2), m.group(3), int(m.group(4)), int(m.group(5)), int(m.group(6)), m.group(7)
        c2d = what.startswith("colour to depth")
        rows.append(dict(stage=st, what=what, w=w, h=h, l=l, cl="-" if c2d else load, cs="-" if c2d else "STORE",
                         dl=load if c2d else "-", ds="STORE" if c2d else "-", hasc=not c2d, hasd=c2d))
if not rows:
    sys.exit("no PASS lines for frame %d" % a.frame)

def mib(w, h, l, bpp): return w * h * l * bpp / (1024.0 * 1024.0)

for eye in a.eye:
    ew, eh = map(int, eye.split("x"))
    tl = ts = 0.0
    main_opens = 0
    print("eye %s (log eye %dx%d), frame %d:" % (eye, lw, lh, a.frame))
    for r in rows:
        w, h = (ew, eh) if (r["w"], r["h"]) == (lw, lh) else (r["w"], r["h"])
        cmb = mib(w, h, r["l"], 4)
        dmb = mib(w, h, r["l"], a.depth_bytes)
        ld = (cmb if r["hasc"] and r["cl"] == "LOAD" else 0) + (dmb if r["hasd"] and r["dl"] == "LOAD" else 0)
        sd = (cmb if r["hasc"] and r["cs"] == "STORE" else 0) + (dmb if r["hasd"] and r["ds"] == "STORE" else 0)
        tl += ld; ts += sd
        if (r["w"], r["h"]) == (lw, lh) and r["what"].startswith("c ") and "(HUD)" not in r["what"]: main_opens += 1
        print("  %-5s %-34s %5dx%-5d x%d  colour %-8s/%-8s depth %-8s/%-8s  load %7.1f  store %7.1f MiB" %
              (r["stage"], r["what"], w, h, r["l"], r["cl"], r["cs"], r["dl"], r["ds"], ld, sd))
    print("  total: %d passes, main target opened %dx, load %.1f + store %.1f = %.1f MiB per frame" % (len(rows), main_opens, tl, ts, tl + ts))
