#!/usr/bin/env python3
"""Generate port/quest/src/vr_quest_glyphs.h from the user-provided Quest art in port/quest/assets/.

  quest_glyphs.png       white line art on a flat dark background, 4 rows x 9 columns (see GRID below)
  quest_controllers.png  black line art on white: the two controllers, for the Controls page's diagram

Usage (needs Python 3 with Pillow and NumPy; the header is committed, so builds do not run this):
  python3 port/quest/tools/make_quest_glyphs.py

Each glyph becomes a VQG_SIZE x VQG_SIZE map of two 4-bit channels per texel: the line art's coverage (alpha from
luminance: the background goes transparent, anti-aliasing is kept) and the glyph's backdrop (everything the line art
encloses, so the glyph can sit on a dark disc and read over any picture). Each glyph is cropped to its own square
bounding box, so every glyph fills its font cell. The diagram becomes VQC_W x VQC_H 4-bit coverage (inverted
luminance), fitted and centred. Area (box) filtering throughout.
"""
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SHEET = ROOT / "assets/quest_glyphs.png"
CONTROLLERS = ROOT / "assets/quest_controllers.png"
OUT = ROOT / "src/vr_quest_glyphs.h"
VQG_SIZE = 48
VQC_W, VQC_H = 256, 128

# (row, column) -> name; the system (home) button is left out: it belongs to the headset's system, not the game.
GRID = [
    ["L_STICK", "L_STICK_CLICK", "LT", "LT_PRESS", None, "RT_PRESS", "RT", "R_STICK_CLICK", "R_STICK"],
    ["L_STICK_ALL", "L_STICK_DIAG", "LG", "LG_PRESS", None, "RG_PRESS", "RG", "R_STICK_DIAG", "R_STICK_ALL"],
    ["L_STICK_LR", "L_STICK_DIAG_UR", "X", "X_PRESS", "MENU", "A_PRESS", "A", "R_STICK_DIAG_UR", "R_STICK_LR"],
    ["L_STICK_UD", "L_STICK_DIAG_UL", "Y", "Y_PRESS", "MENU_PRESS", "B_PRESS", "B", "R_STICK_DIAG_UL", "R_STICK_UD"],
]


def box_resize(a, w, h):
    """Area-average resize of a float array (values 0..1)."""
    img = Image.fromarray((a * 65535).astype(np.uint16).astype(np.int32), mode="I")
    return np.asarray(img.resize((w, h), Image.BOX), dtype=np.float64) / 65535.0


def dilate(m, r):
    """Square dilation by r pixels."""
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            sh = np.zeros_like(m)
            ys, yd = (slice(dy, None), slice(0, -dy or None)) if dy >= 0 else (slice(0, dy), slice(-dy, None))
            xs, xd = (slice(dx, None), slice(0, -dx or None)) if dx >= 0 else (slice(0, dx), slice(-dx, None))
            sh[ys, xs] = m[yd, xd]
            out |= sh
    return out


def outside_of(line):
    """Pixels reachable from the crop's border without crossing the line art."""
    h, w = line.shape
    out = np.zeros_like(line, dtype=bool)
    stack = [(y, x) for x in range(w) for y in (0, h - 1)] + [(y, x) for y in range(h) for x in (0, w - 1)]
    while stack:
        y, x = stack.pop()
        if y < 0 or x < 0 or y >= h or x >= w or out[y, x] or line[y, x]:
            continue
        out[y, x] = True
        stack.extend(((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)))
    return out


def glyphs():
    rgb = np.asarray(Image.open(SHEET).convert("RGB"), dtype=np.float64)
    lum = rgb.mean(axis=2)
    bg = np.median(lum)
    alpha = np.clip((lum - bg) / (255.0 - bg), 0.0, 1.0)
    h, w = alpha.shape
    rows, cols = len(GRID), len(GRID[0])
    # the grid: equal cells over the art's extent
    ys, xs = np.nonzero(alpha > 0.25)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    cw, ch = (x1 - x0) / cols, (y1 - y0) / rows
    out = []
    for r in range(rows):
        for c in range(cols):
            name = GRID[r][c]
            if not name:
                continue
            cx0, cy0 = int(x0 + c * cw), int(y0 + r * ch)
            cell = alpha[cy0:int(cy0 + ch), cx0:int(cx0 + cw)]
            gy, gx = np.nonzero(cell > 0.25)
            bx0, bx1, by0, by1 = gx.min(), gx.max() + 1, gy.min(), gy.max() + 1
            side = int(max(bx1 - bx0, by1 - by0) * 1.06) + 2
            mx, my = (bx0 + bx1) // 2 + cx0, (by0 + by1) // 2 + cy0
            sx, sy = mx - side // 2, my - side // 2
            crop = np.zeros((side, side))
            ys0, xs0 = max(sy, 0), max(sx, 0)
            ys1, xs1 = min(sy + side, h), min(sx + side, w)
            crop[ys0 - sy:ys1 - sy, xs0 - sx:xs1 - sx] = alpha[ys0:ys1, xs0:xs1]
            # close small gaps (a press arrow cutting a ring) before finding what the art encloses
            rad = max(2, side // 40)
            backdrop = (~dilate(outside_of(dilate(crop > 0.35, rad)), rad)).astype(np.float64)
            line = box_resize(crop, VQG_SIZE, VQG_SIZE)
            fill = box_resize(backdrop, VQG_SIZE, VQG_SIZE)
            out.append((name, line, fill))
    return out


def diagram():
    """The two controllers side by side, a small gap apart (the drawing's own wide gap closed), fitted and centred."""
    lum = np.asarray(Image.open(CONTROLLERS).convert("L"), dtype=np.float64) / 255.0
    cov = 1.0 - lum
    cols = np.nonzero(cov.max(axis=0) > 0.25)[0]
    gaps = np.nonzero(np.diff(cols) > 8)[0]
    split = (cols[gaps[np.argmax(np.diff(cols)[gaps])]] + cols[gaps[np.argmax(np.diff(cols)[gaps])] + 1]) // 2
    parts = []
    for half in (cov[:, :split], cov[:, split:]):
        ys, xs = np.nonzero(half > 0.25)
        parts.append(half[ys.min():ys.max() + 1, xs.min():xs.max() + 1])
    ph = max(p.shape[0] for p in parts)
    gap = int(round(parts[0].shape[1] * 0.12))
    pair = np.zeros((ph, parts[0].shape[1] + gap + parts[1].shape[1]))
    pair[:parts[0].shape[0], :parts[0].shape[1]] = parts[0]
    pair[:parts[1].shape[0], parts[0].shape[1] + gap:] = parts[1]
    h, w = pair.shape
    scale = min(VQC_W / w, VQC_H / h)
    tw, th = max(1, round(w * scale)), max(1, round(h * scale))
    small = box_resize(pair, tw, th)
    canvas = np.zeros((VQC_H, VQC_W))
    ox, oy = (VQC_W - tw) // 2, (VQC_H - th) // 2
    canvas[oy:oy + th, ox:ox + tw] = small
    # thin lines get thinner when shrunk: lift the coverage so they stay visible
    return np.clip(canvas * 1.6, 0.0, 1.0)


def nib(a):
    return np.clip(np.rint(a * 15.0), 0, 15).astype(np.int64)


def main():
    gl = glyphs()
    dg = diagram()
    lines = [
        "/* vr_quest_glyphs.h - GENERATED by port/quest/tools/make_quest_glyphs.py from the user-provided art in",
        " * port/quest/assets/ (quest_glyphs.png, quest_controllers.png). Do not edit; run the script again.",
        " *",
        " * Glyphs: VQG_SIZE x VQG_SIZE, two hex digits per texel, row-major: the line art's coverage (0..F), then the",
        " * backdrop the art encloses (0..F). The diagram: VQC_W x VQC_H, one hex digit per texel (line coverage): the two",
        " * controllers side by side, a small gap apart, centred.",
        " * vr_prompts.h turns them into font glyphs (vrp_glyph_render) and the Controls page's diagram; any other user",
        " * (a settings page legend) can call vqg_coverage / vqc_coverage, or vrp_glyph_render for the shaded look. */",
        "#ifndef VR_QUEST_GLYPHS_H",
        "#define VR_QUEST_GLYPHS_H",
        "",
        f"#define VQG_SIZE {VQG_SIZE}",
        f"#define VQC_W {VQC_W}",
        f"#define VQC_H {VQC_H}",
        "",
        "enum {",
    ]
    for i, (name, _, _) in enumerate(gl):
        lines.append(f"    VQG_{name} = {i},")
    lines += ["    VQG_COUNT", "};", "", "static const char *const vqg_names[VQG_COUNT] = {"]
    lines += [f'    "{name.lower()}",' for name, _, _ in gl]
    lines += ["};", "", f"static const char vqg_data[VQG_COUNT][VQG_SIZE * VQG_SIZE * 2 + 1] = {{"]
    for name, line, fill in gl:
        a, f = nib(line), nib(fill)
        hexs = "".join(f"{a[y, x]:X}{f[y, x]:X}" for y in range(VQG_SIZE) for x in range(VQG_SIZE))
        lines.append(f"    /* {name.lower()} */")
        for k in range(0, len(hexs), 192):
            lines.append(f'    "{hexs[k:k + 192]}"')
        lines[-1] += ","
    lines += ["};", "", "static const char vqc_data[VQC_W * VQC_H + 1] ="]
    d = nib(dg)
    hexs = "".join(f"{d[y, x]:X}" for y in range(VQC_H) for x in range(VQC_W))
    for k in range(0, len(hexs), 192):
        lines.append(f'    "{hexs[k:k + 192]}"')
    lines[-1] += ";"
    lines += [
        "",
        "static inline int vqg_hex(char c) { return c <= '9' ? c - '0' : c - 'A' + 10; }",
        "/* Line coverage / backdrop of glyph g at texel (x, y) of the master, 0..15. */",
        "static inline int vqg_coverage(int g, int x, int y) { return vqg_hex(vqg_data[g][(y * VQG_SIZE + x) * 2]); }",
        "static inline int vqg_backdrop(int g, int x, int y) { return vqg_hex(vqg_data[g][(y * VQG_SIZE + x) * 2 + 1]); }",
        "/* The diagram's line coverage at (x, y), 0..15. */",
        "static inline int vqc_coverage(int x, int y) { return vqg_hex(vqc_data[y * VQC_W + x]); }",
        "",
        "#endif /* VR_QUEST_GLYPHS_H */",
        "",
    ]
    OUT.write_text("\n".join(lines), encoding="ascii")
    print(f"{OUT}: {len(gl)} glyphs, diagram {VQC_W}x{VQC_H}")
    # previews next to nothing in the repo: only when asked
    import os
    prev = os.environ.get("VQG_PREVIEW")
    if prev:
        tiles = []
        for name, line, fill in gl:
            t = np.zeros((VQG_SIZE, VQG_SIZE, 3))
            t[...] = 60
            t = t * (1 - fill[..., None] * 0.85) + 20 * fill[..., None] * 0.85
            t = t * (1 - line[..., None]) + 240 * line[..., None]
            tiles.append(t)
        rows = [np.concatenate(tiles[i:i + 9] + [np.zeros_like(tiles[0])] * (9 - len(tiles[i:i + 9])), axis=1)
                for i in range(0, len(tiles), 9)]
        Image.fromarray(np.concatenate(rows, axis=0).astype(np.uint8)).resize((9 * 144, len(rows) * 144),
                                                                           Image.NEAREST).save(prev + "_glyphs.png")
        Image.fromarray((20 + dg * 220).astype(np.uint8)).resize((VQC_W * 3, VQC_H * 3), Image.NEAREST).save(prev + "_diagram.png")


if __name__ == "__main__":
    main()
