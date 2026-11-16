"""Extract an Xbox disc image, tolerating truncation (files past the end are listed, not extracted).

Works on a trimmed xiso (game partition at offset 0) and on a full Redump image (video partition first,
game partition at 0x18300000); the partition is found by its MICROSOFT*XBOX*MEDIA header.
Usage: xiso_tolerant.py <iso> <outdir>
"""
import struct, sys, os
iso, out = sys.argv[1], sys.argv[2]
f = open(iso, 'rb'); fs = os.path.getsize(iso)
MAGIC = b'MICROSOFT*XBOX*MEDIA'
base = None
for cand in (0, 0x18300000, 0x2080000, 0xFD90000):   # xiso, Redump XGD2, XGD1, XGD3
    if cand + 0x10000 + 20 > fs: continue
    f.seek(cand + 0x10000)
    if f.read(20) == MAGIC: base = cand; break
if base is None: sys.exit('no Xbox media header found (not an Xbox disc image?)')
f.seek(base + 0x10000 + 0x14); root, rsz = struct.unpack('<II', f.read(8))
ok = []; bad = []; dirs_bad = []
def walk(sec, size, path):
    if (base + sec * 2048 + size) > fs:
        dirs_bad.append(path); return
    f.seek(base + sec * 2048); data = f.read(size); seen = set()
    def rec(o):
        if o in seen or o + 14 > len(data): return
        seen.add(o)
        l, r, s, sz, attr, nl = struct.unpack_from('<HHIIBB', data, o)
        if l == 0xffff: return
        name = data[o+14:o+14+nl].decode('latin1')
        if l: rec(l * 4)
        p = path + name
        if attr & 0x10:
            if sz: walk(s, sz, p + '/')
            else: os.makedirs(os.path.join(out, p), exist_ok=True)
        else:
            if base + s * 2048 + sz > fs: bad.append((p, sz, base + s*2048+sz-fs))
            else: ok.append((p, s, sz))
        if r: rec(r * 4)
    rec(0)
walk(root, rsz, '')
for p, s, sz in ok:
    d = os.path.join(out, p); os.makedirs(os.path.dirname(d), exist_ok=True)
    f.seek(base + s * 2048)
    with open(d, 'wb') as o:
        n = sz
        while n:
            c = f.read(min(n, 1 << 20)); o.write(c); n -= len(c)
print('partition at', hex(base), '; extracted', len(ok), 'files; beyond EOF:', len(bad), '; unreadable dirs:', dirs_bad)
for b in bad: print('  short', b)
