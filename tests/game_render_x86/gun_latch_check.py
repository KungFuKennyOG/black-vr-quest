#!/usr/bin/env python3
"""gun_latch_check.py <frames dir>: the gun-latch check of a GRX_GUN_TEST=1 run (port/notes/gun-latch.md).

Reads <frames>/gunlatch/{before,after,settled,tint}_{L,R}.png and meta.json (render_main.c). The head holds still; the
right controller turns about the head between the "after" frame's game latch and its late latch:
  before   game hand == late hand: the weapon where the game put it
  after    the game drew the weapon at the old hand; the late latch has the turned one: with gun_latch the weapon is
           moved there, the world is not
  settled  a later frame whose game latch already had the turned hand (no delta): the weapon the game itself placed
  tint     a later frame with BLACKVR_GUN_LATCH_DEBUG's tint on: the draws the backend took for the weapon, magenta
Checks, per eye (the weapon's pixels = the tinted ones):
  1. per textured 64 px patch of the weapon (that the game itself did not move from prev to before), the shift
     before -> after is what the hand delta predicts for those pixels through the submitted eye pose and FOV (depth
     0.15..0.8 m): median error <= 3.5 px, >= 90 % within 8 px (the game's own idle weapon motion between two game
     frames; the geometry is exact to < 1 px in tests/vr_glue);
  2. after matches settled (the latched weapon is where the game itself draws it once it catches up: no shift, small
     residual), so no part of the weapon was left behind;
  3. away from the weapon (at both of its positions) the picture is unchanged: the world did not move;
  4. >= 97 % of the pixels that changed belong to the tinted weapon at one of its two positions.
Writes <frames>/gunlatch_before.png and gunlatch_after.png (left | right eye), gunlatch_settled.png, gunlatch_diff.png,
gunlatch_tint.png and
gunlatch_report.json. Exit 1 when a check fails.
"""
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(d, name, eye):
    return np.asarray(Image.open(d / f"{name}_{eye}.png").convert("RGB")).astype(np.int16)


def qrot(q, v):
    x, y, z, w = q
    u = np.array([x, y, z])
    v = np.asarray(v, dtype=float)
    return v + 2.0 * np.cross(u, np.cross(u, v) + w * v)


def project(eye, fov, p, w, h):
    q = eye["q"]
    inv = (-q[0], -q[1], -q[2], q[3])
    r = qrot(inv, np.asarray(p) - np.asarray(eye["p"]))
    tx, ty = r[0] / -r[2], r[1] / -r[2]
    l, rr, u, d = (math.tan(a) for a in fov)
    return np.array([(tx - l) / (rr - l) * w, (u - ty) / (u - d) * h])


def dilate(m, n):
    out = m.copy()
    for _ in range(n):
        o = out.copy()
        o[1:, :] |= out[:-1, :]; o[:-1, :] |= out[1:, :]
        o[:, 1:] |= out[:, :-1]; o[:, :-1] |= out[:, 1:]
        out = o
    return out


def changed(a, b, thr=24):
    return np.abs(a - b).max(axis=2) > thr


def register(a, b, mask, rx=70, ry=30):
    """The shift d minimising mean |b(p) - a(p - d)| over the mask (b: the moved picture)."""
    ys, xs = np.nonzero(mask)
    h, w = mask.shape
    best = (1e9, 0, 0)
    for dy in range(-ry, ry + 1):
        for dx in range(-rx, rx + 1):
            sy, sx = ys - dy, xs - dx
            ok = (sy >= 0) & (sy < h) & (sx >= 0) & (sx < w)
            if ok.sum() < 0.8 * len(ys):
                continue
            c = np.abs(b[ys[ok], xs[ok]] - a[sy[ok], sx[ok]]).mean()
            if c < best[0]:
                best = (c, dx, dy)
    return best


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qinv(q):
    return (-q[0], -q[1], -q[2], q[3])


def backproject(eye, fov, px, py, depth, w, h):
    l, r, u, d = (math.tan(a) for a in fov)
    tx = l + (px + 0.5) / w * (r - l)
    ty = u - (py + 0.5) / h * (u - d)
    return np.asarray(eye["p"]) + qrot(eye["q"], np.array([tx * depth, ty * depth, -depth]))


def expected_shift(meta_after, ei, mask, depth, w, h):
    """Mean image shift of the weapon pixels `mask` (where it is drawn after) for the hand delta of the "after" frame,
    D = hand * inverse(game_hand): each pixel's point at `depth` metres, taken back by inverse(D), projected."""
    e = meta_after["eyes"][ei]
    gh, lh = meta_after["game_hand"], meta_after["hand"]
    dq = qmul(lh["q"], qinv(gh["q"]))
    dp = np.asarray(lh["p"]) - qrot(dq, gh["p"])
    ys, xs = np.nonzero(mask)
    sel = np.linspace(0, len(xs) - 1, min(len(xs), 400)).astype(int)
    sh = []
    for i in sel:
        X = backproject(e["pose"], e["fov"], xs[i], ys[i], depth, w, h)
        Xo = qrot(qinv(dq), X - dp)
        sh.append(np.array([xs[i] + 0.5, ys[i] + 0.5]) - project(e["pose"], e["fov"], Xo, w, h))
    return np.mean(sh, axis=0)


def shifted(m, dx, dy):
    out = np.zeros_like(m)
    h, w = m.shape
    ys, xs = np.nonzero(m)
    ys, xs = ys + dy, xs + dx
    ok = (ys >= 0) & (ys < h) & (xs >= 0) & (xs < w)
    out[ys[ok], xs[ok]] = True
    return out


def main():
    frames = Path(sys.argv[1] if len(sys.argv) > 1 else "build-render-x86/frames")
    d = frames / "gunlatch"
    meta = json.loads((d / "meta.json").read_text())
    report = {"eyes": {}, "ok": True}
    fails = []
    for need in ("prev", "before", "after", "settled", "tint"):
        if need not in meta:
            print(f"gun_latch_check: {need} frame missing")
            return 1
    for k in ("prev", "before", "after", "settled", "tint"):
        m = meta[k]
        print(f"{k:8s}: game frame {m['game_frame']}, weapon draws {m['vm_draws']}, latch valid {m['gun_valid']}, "
              f"game hand {np.round(m['game_hand']['p'], 4).tolist()} late hand {np.round(m['hand']['p'], 4).tolist()}")
    pairs = {}
    for ei, eye in enumerate(("L", "R")):
        P, A, B, S, T = (load(d, n, eye) for n in ("prev", "before", "after", "settled", "tint"))
        h, w = A.shape[:2]
        # the weapon as the backend classified it: the debug tint (magenta over the picture)
        weapon = (T[..., 0] - T[..., 1] > 40) & (T[..., 2] - T[..., 1] > 40)
        if weapon.sum() < 2000:
            fails.append(f"eye {eye}: the tint frame shows {int(weapon.sum())} weapon pixels")
            continue
        near = dilate(weapon, 10)
        # the textured weapon pixels that changed (the dark arms carry little texture)
        m_ab = changed(A, B) & near
        m_pa = changed(P, A) & near
        # the weapon's overall shift before -> after (for the masks and the report)
        c_ab, dx_ab, dy_ab = register(A, B, m_ab)
        c_ab0 = float(np.abs(B[m_ab] - A[m_ab]).mean())
        exp = {z: expected_shift(meta["after"], ei, m_ab, z, w, h) for z in (0.25, 0.4, 0.8)}
        ex = np.stack(list(exp.values()))
        # 1: per 64 px patch of the weapon with texture, where the game itself did not move it from prev to before
        # (its animation is in the vertex data): the patch's shift before -> after == the hand delta's for its pixels
        patches = []
        lum = A.mean(axis=2)
        for y0 in range(0, h - 63, 32):
            for x0 in range(0, w - 63, 32):
                pm = np.zeros((h, w), bool)
                pm[y0:y0 + 64, x0:x0 + 64] = True
                pm &= weapon
                if pm.sum() < 0.6 * 64 * 64 or lum[pm].std() < 6.0:
                    continue
                _, gx, gy = register(P, A, pm, rx=8, ry=8)
                if abs(gx) > 1 or abs(gy) > 1:
                    continue
                _, mx, my = register(A, B, pm, rx=60, ry=25)
                # the delta is a turn about the head, not the eye: a little parallax, so the patch's depth (unknown,
                # 0.15 .. 0.8 m: the forearms reach the eye) matters; the closest of those predictions
                pes = [expected_shift(meta["after"], ei, pm, z, w, h) for z in (0.15, 0.2, 0.3, 0.4, 0.6, 0.8)]
                best = min(pes, key=lambda v: math.hypot(mx - v[0], my - v[1]))
                patches.append((x0, y0, mx, my, round(float(best[0]), 2), round(float(best[1]), 2)))
        err = np.array([math.hypot(p[2] - p[4], p[3] - p[5]) for p in patches]) if patches else np.array([99.0])
        # 2: after vs settled (the game itself at the turned hand, a few frames on: its turn sway included)
        c_bs, dx_bs, dy_bs = register(S, B, m_ab, rx=20, ry=20)
        # 3: the world: away from the weapon (at both of its positions), not animated by itself (prev vs before)
        both = dilate(weapon | shifted(weapon, -dx_ab, -dy_ab), 24)   # the arms animate between the frames
        animated = dilate(changed(P, A) & ~both, 2)
        world = ~both & ~animated
        wd = np.abs(B - A).max(axis=2)[world]
        world_mean, world_frac = float(wd.mean()), float((wd > 24).mean())
        # ... and what it would have been had the world been moved like the weapon
        sh = np.roll(np.roll(A, dy_ab, axis=0), dx_ab, axis=1)
        moved_world_frac = float((np.abs(sh - A).max(axis=2)[world] > 24).mean())
        # 4: what changed (and is not the world's own animation) is the tinted weapon at one of its two positions
        moved = (np.abs(B - A).max(axis=2) > 24) & ~animated
        moved_in = float((moved & both).sum() / max(1, moved.sum()))
        e_rep = {
            "weapon_pixels_tinted": int(weapon.sum()),
            "patches": [list(p) for p in patches],
            "patch_error_px": {"median": round(float(np.median(err)), 2), "within_3px": round(float((err <= 3).mean()), 3),
                               "within_8px": round(float((err <= 8).mean()), 3)},
            "expected_hand_shift_px": {str(z): [round(float(v[0]), 2), round(float(v[1]), 2)] for z, v in exp.items()},
            "before_to_after_shift_px": [dx_ab, dy_ab], "registered_cost": round(float(c_ab), 2),
            "unshifted_cost": round(c_ab0, 2),
            "after_to_settled_shift_px": [dx_bs, dy_bs], "after_vs_settled_cost": round(float(c_bs), 2),
            "world_mean_absdiff": round(world_mean, 3), "world_changed_fraction": round(world_frac, 6),
            "world_changed_fraction_if_moved_like_the_weapon": round(moved_world_frac, 4),
            "moved_pixels": int(moved.sum()), "moved_pixels_on_weapon": round(moved_in, 4),
        }
        # tolerance: the game animates the weapon (idle motion, in its vertex data) by a few millimetres from one game
        # frame to the next, which shows most on the forearms near the eye; the geometry itself is checked to < 1 px
        # by tests/vr_glue
        if len(patches) < 4 or np.median(err) > 3.5 or (err <= 8).mean() < 0.9:
            fails.append(f"eye {eye}: {len(patches)} weapon patches, median error {np.median(err):.1f} px, "
                         f"{100 * (err <= 8).mean():.0f} % within 8 px of the hand delta")
        if not (abs(dx_bs) < 0.35 * abs(dx_ab) and c_bs < 0.6 * c_ab0):
            fails.append(f"eye {eye}: after is {dx_bs},{dy_bs} px / cost {c_bs:.1f} from the settled frame (unshifted {c_ab0:.1f})")
        if world_mean > 1.0 or world_frac > 0.0005:
            fails.append(f"eye {eye}: the world changed (mean {world_mean:.2f}, {100 * world_frac:.3f} % of pixels)")
        if moved_in < 0.99:
            fails.append(f"eye {eye}: only {100 * moved_in:.1f} % of what moved is the tinted weapon")
        e4 = exp[0.4]
        print(f"eye {eye}: weapon ({int(weapon.sum())} px tinted) moved {dx_ab:+d},{dy_ab:+d} px before->after (cost {c_ab:.1f}, "
              f"unshifted {c_ab0:.1f}; the hand delta, mean over those pixels: {e4[0]:+.1f},{e4[1]:+.1f}); {len(patches)} textured "
              f"patches vs the hand delta: median error {np.median(err):.1f} px, {100 * (err <= 3).mean():.0f} % within 3 px; after vs settled "
              f"{dx_bs:+d},{dy_bs:+d} px cost {c_bs:.1f}; world mean |diff| {world_mean:.3f}, {100 * world_frac:.4f} % changed "
              f"({100 * moved_world_frac:.1f} % had it moved like the weapon); {100 * moved_in:.2f} % of moved pixels on the weapon")
        report["eyes"][eye] = e_rep
        for name, img in (("prev", P), ("before", A), ("after", B), ("settled", S), ("tint", T)):
            pairs.setdefault(name, []).append(img)
        diff = np.clip(np.abs(B - A).max(axis=2) * 4, 0, 255).astype(np.uint8)
        pairs.setdefault("diff", []).append(np.stack([diff] * 3, axis=2))
    for name, imgs in pairs.items():
        side = np.concatenate([np.asarray(i, dtype=np.uint8) for i in imgs], axis=1)
        Image.fromarray(side).save(frames / f"gunlatch_{name}.png")
    report["fails"] = fails
    report["ok"] = not fails
    (frames / "gunlatch_report.json").write_text(json.dumps(report, indent=1))
    for f in fails:
        print("FAIL:", f)
    print("gun_latch_check:", "PASS" if not fails else "FAIL", f"-> {frames}/gunlatch_before.png, gunlatch_after.png")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
