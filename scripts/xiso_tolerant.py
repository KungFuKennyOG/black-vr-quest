import struct, sys, os
# usage: xiso_tolerant.py <iso> <outdir> [partition offset]
# A full redump image keeps the game partition at 0x18300000; it is found by its magic when no offset is given.
iso, out = sys.argv[1], sys.argv[2]
f = open(iso, 'rb')
base = int(sys.argv[3], 0) if len(sys.argv) > 3 else None
if base is None:
    base = 0
    for off in (0, 0x18300000, 0xFD90000, 0x2080000):
        f.seek(off + 0x10000)
        if f.read(20) == b'MICROSOFT*XBOX*MEDIA': base = off; break
fs = os.path.getsize(iso) - base
f.seek(base + 0x10000 + 0x14); root, rsz = struct.unpack('<II', f.read(8))
ok = []; bad = []; dirs_bad = []
def walk(sec, size, path):
    if (sec * 2048 + size) > fs:
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
        else:
            if s * 2048 + sz > fs: bad.append((p, sz, s*2048+sz-fs))
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
print('extracted', len(ok), 'files; beyond EOF:', len(bad), '; unreadable dirs:', dirs_bad)
for b in bad: print('  short', b)
