#!/usr/bin/env python3
"""sweep_drive.py -- one scripted run of the whole-game sweep (tests/game_render_x86, build-vrmenu-x86/sweep).

Starts game_render_x86 (nice -n 5) with every presented frame dumped (GRX_VIDEO=1), drives it from what it presents
(<frames>/video/meta.jsonl: present mode, frame count, frame time) by writing the run-time press file
<frames>/press, checks every presented frame as it lands and prunes the dump as it goes:

  stereo frames   luma / black blocks per eye, a block-matching left/right check (blocks of one eye with no match
                  in the other along the epipolar line, beyond normal parallax), one-eye-only change from the previous
                  frame, A-B-A flicker per 16 px block, HUD layer coverage and opaque area, eye luma difference;
  flat frames     luma and black fraction.

Kept: every 8th stereo frame and every 60th flat frame as a composite as the headset shows it (left | right, the HUD
and flat quads projected: make_video.compose) in <frames>/keep/*.jpg; every frame after a scripted event for a while;
raw PNGs for flagged frames and around events (capped) in <frames>/raw/. Everything else of video/ is deleted.
Metrics go to <frames>/metrics.jsonl, events to <frames>/events.jsonl.

Usage: sweep_drive.py --name L00 [--level N] [--plan level|frontend|idle] [--seconds 1500] [--env K=V ...]
The run's log is <sweep>/<name>.log, frames in <sweep>/frames-<name>.
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_video  # noqa: E402  (compose: the layers as the headset shows them)

ROOT = os.path.dirname(os.path.dirname(HERE))                         # the repo (holds game/)
BUILD = os.environ.get("SWEEP_BUILD", os.path.join(ROOT, "build-vrmenu-x86"))
HARNESS = os.path.join(BUILD, "game_render_x86")
ICD = os.environ.get("VK_ICD_FILENAMES", "")                           # SwiftShader's ICD json

FRONT_END = "60:menu,120:a,180:a"     # title -> START MISSION -> NORMAL (as the default script)
FRONT_END_FAST = "45:menu,80:a,115:a"  # the same, sooner (--fast); A every 25 s after 130 s catches a missed one

# ---- plans ------------------------------------------------------------------------------------------------------------
# ("waits", n)            n more stereo frames
# ("waitt", s)            s seconds (wall)
# ("press", btn, hold)    hold = ("f", n) frames (converted with the measured stereo frame time) or ("s", seconds)
# ("event", label)        mark an event (keeps the frames after it)
# ("stereo", timeout, [(btn, s), ...])   until STEREO again; the fallback presses one at a time, every timeout/3 s
# ("cap",)                a harness capture of the next frame
# ("untilmode", m, s)     until the present mode is m, at most s seconds
LEVEL_PLAN = [
    ("waits", 40), ("event", "settled, head sway on"), ("cap",),
    ("press", "fwd", ("f", 70), "walk forward"), ("waits", 76),
    ("press", "right", ("s", 1.0), "snap turn right"), ("waits", 12),
    ("press", "right", ("s", 1.0), "snap turn right 2"), ("waits", 12),
    ("press", "rt", ("f", 4), "fire 1"), ("waits", 14),
        ("press", "rt", ("f", 30), "fire held"), ("waits", 40),
    ("press", "a", ("s", 0.6), "reload"), ("waits", 45),
    ("press", "up", ("s", 1.0), "next weapon"), ("waits", 60),
    ("press", "rt", ("f", 20), "fire after weapon switch"), ("waits", 30),
    ("press", "down", ("s", 1.0), "previous weapon"), ("waits", 50),
    ("press", "lt", ("f", 20), "grenade"), ("waits", 120),
    ("press", "left", ("s", 1.0), "snap turn left"), ("waits", 12),
    ("press", "left", ("s", 1.0), "snap turn left 2"), ("waits", 12),
    ("press", "menu", ("s", 0.3), "pause"), ("untilmode", 1, 90), ("waitt", 4), ("event", "pause menu up"), ("cap",),
    ("waitt", 4),
    ("press", "b", ("s", 0.4), "close pause"), ("stereo", 45, [("menu", 0.3), ("b", 0.4), ("a", 0.4)]),
    ("waits", 10),
    ("press", "menu", ("s", 3.0), "objectives (Menu held)"), ("untilmode", 1, 60), ("waitt", 4), ("event", "objectives up"),
    ("cap",),
    ("stereo", 45, [("b", 0.4), ("menu", 0.3), ("a", 0.4)]),
    ("waits", 10),
    ("press", "rc", ("s", 0.6), "use / pick up"), ("waits", 30),
    ("press", "b", ("s", 0.5), "melee"), ("waits", 40),
    ("press", "fwd", ("f", 90), "walk forward 2"), ("press", "sway", ("f", 90), "hand sway"), ("waits", 96),
    ("press", "rc", ("s", 0.6), "use / pick up 2"), ("waits", 30),
    ("press", "aiml", ("f", 34), "aim left"), ("press", "rt", ("f", 24), "fire aimed left"), ("waits", 40),
    ("press", "duck", ("f", 40), "duck (crouch)"), ("waits", 50),
    ("waits", 20), ("end",),
]

# the same events in about 330 stereo frames (with game_hz = 30 each frame is 1/30 s of game time)
LEVEL_PLAN_FAST = [
    ("waits", 20), ("event", "settled"), ("cap",),
    ("press", "fwd", ("f", 25), "walk forward"), ("waits", 28),
    ("press", "right", ("s", 1.0), "snap turn right"), ("waits", 6),
    ("press", "rt", ("f", 3), "fire 1"), ("waits", 8),
    ("press", "rt", ("f", 12), "fire held"), ("waits", 16),
    ("press", "a", ("s", 0.6), "reload"), ("waits", 20),
    ("press", "up", ("s", 1.0), "next weapon"), ("waits", 25),
    ("press", "rt", ("f", 8), "fire after weapon switch"), ("waits", 12),
    ("press", "down", ("s", 1.0), "previous weapon"), ("waits", 20),
    ("press", "lt", ("f", 10), "grenade"), ("waits", 60),
    ("press", "left", ("s", 1.0), "snap turn left"), ("waits", 6),
    ("press", "menu", ("s", 0.3), "pause"), ("untilmode", 1, 90), ("waitf", 16, 120), ("event", "pause menu up"), ("cap",),
    ("press", "down", ("s", 0.4), "pause: down to OBJECTIVES"), ("waitf", 4, 30),
    ("press", "a", ("s", 0.4), "pause: OBJECTIVES"), ("waitf", 14, 90), ("event", "objectives page"), ("cap",),
    ("press", "b", ("s", 0.4), "objectives: back"), ("waitf", 8, 60),
    ("press", "b", ("s", 0.4), "close pause"), ("stereo", 60, [("menu", 0.3), ("b", 0.4), ("a", 0.4)]),
    ("waits", 4),
    ("press", "menu", ("s", 3.0), "objectives (Menu held)"), ("untilmode", 1, 40), ("waitf", 16, 90), ("event", "objectives up"),
    ("cap",), ("stereo", 60, [("b", 0.4), ("menu", 0.3), ("a", 0.4)]),
    ("waits", 4),
    ("press", "rc", ("s", 0.6), "use / pick up"), ("waits", 12),
    ("press", "b", ("s", 0.5), "melee"), ("waits", 15),
    ("press", "fwd", ("f", 30), "walk forward 2"), ("press", "sway", ("f", 30), "hand sway"), ("waits", 33),
    ("press", "aiml", ("f", 14), "aim left"), ("press", "rt", ("f", 10), "fire aimed left"), ("waits", 15),
    ("press", "duck", ("f", 15), "duck (crouch)"), ("waits", 18),
    ("waits", 6), ("end",),
]

# level 1 in depth: walk on through the level (with turns, shots and use presses) for ~1350 stereo frames
LEVEL_PLAN_DEEP = [("waits", 20), ("event", "settled"), ("cap",)] + [x for i in range(4) for x in (
    ("press", "fwd", ("f", 120), "deep %d: walk" % i), ("waits", 124),
    ("press", "right", ("s", 1.0), "deep %d: snap right" % i), ("waits", 6),
    ("press", "rt", ("f", 3), "deep %d: shot" % i), ("waits", 10),
    ("press", "fwd", ("f", 60), "deep %d: walk 2" % i), ("waits", 64),
    ("press", "left", ("s", 1.0), "deep %d: snap left" % i), ("waits", 6),
    ("press", "left", ("s", 1.0), "deep %d: snap left 2" % i), ("waits", 6),
    ("press", "rc", ("s", 0.6), "deep %d: use" % i), ("waits", 10),
    ("press", "fwd", ("f", 100), "deep %d: walk 3" % i), ("waits", 104),
    ("press", "right", ("s", 1.0), "deep %d: snap right 2" % i), ("waits", 6),
    ("cap",))] + [("waits", 10), ("end",)]

# a scoped rifle (RECOMP_BLACK_TEST_LOADOUT=snr): the scope-raise gesture (FLAT_SCOPE), zoom, fire, pause
LEVEL_PLAN_SCOPE = [
    ("waits", 20), ("event", "settled"), ("cap",),
    ("press", "rt", ("f", 3), "hip fire rifle"), ("waits", 15),
    ("press", "raise", ("f", 70), "raise rifle to the eye"), ("waits", 10), ("event", "scope raised"), ("cap",),
    ("press", "rt", ("f", 3), "fire scoped"), ("waits", 12),
    ("press", "up", ("s", 1.0), "scope zoom up"), ("waits", 10),
    ("press", "down", ("s", 1.0), "scope zoom down"), ("waits", 40), ("event", "rifle lowered"),
    ("press", "raise", ("f", 40), "raise again"), ("press", "aiml", ("f", 40), "raise again, aimed left"), ("waits", 20),
    ("press", "menu", ("s", 0.3), "pause from the scope"), ("untilmode", 1, 60), ("waitf", 14, 90), ("event", "pause"),
    ("press", "b", ("s", 0.4), "close pause"), ("stereo", 60, [("menu", 0.3), ("b", 0.4)]),
    ("waits", 30), ("end",),
]

# the same, raised for longer, zoom steps after the aim has settled
LEVEL_PLAN_SCOPE2 = [
    ("waits", 20), ("event", "settled"),
    ("press", "raise", ("f", 220), "raise rifle to the eye (long)"), ("waits", 60), ("event", "scope settled?"), ("cap",),
    ("press", "up", ("s", 1.0), "scope zoom up 1"), ("waits", 25), ("event", "after zoom up 1"),
    ("press", "up", ("s", 1.0), "scope zoom up 2"), ("waits", 25), ("event", "after zoom up 2"),
    ("press", "rt", ("f", 3), "fire scoped"), ("waits", 20),
    ("press", "down", ("s", 1.0), "scope zoom down"), ("waits", 25),
    ("waits", 80), ("event", "rifle lowered"), ("waits", 20), ("end",),
]

# a rocket launcher (RECOMP_BLACK_TEST_LOADOUT=rpg): explosions
LEVEL_PLAN_RPG = [
    ("waits", 20), ("event", "settled"), ("cap",),
    ("press", "rt", ("f", 3), "fire rocket"), ("waits", 45),
    ("press", "right", ("s", 1.0), "snap right"), ("waits", 6),
    ("press", "rt", ("f", 3), "fire rocket 2"), ("waits", 45),
    ("press", "a", ("s", 0.6), "reload"), ("waits", 40),
    ("press", "aiml", ("f", 25), "aim left"), ("press", "rt", ("f", 3), "fire rocket 3 (aimed left)"), ("waits", 50),
    ("waits", 10), ("end",),
]

FRONTEND_PLAN = [
    ("waitt", 110), ("event", "main menu"),
    ("press", "down", ("s", 0.4), "main: down to OPTIONS"), ("waitt", 4),
    ("press", "a", ("s", 0.4), "OPTIONS"), ("waitt", 8), ("event", "options page"),
    ("press", "a", ("s", 0.4), "CONTROL SETUP"), ("waitt", 8), ("event", "controls page"),
    ("press", "b", ("s", 0.4), "back to options"), ("waitt", 6),
    ("press", "up", ("s", 0.4), "up to VR SETTINGS"), ("waitt", 4), ("event", "VR SETTINGS focused"),
    ("press", "a", ("s", 0.4), "VR SETTINGS"), ("waitt", 8), ("event", "VR SETTINGS list"),
    ("press", "a", ("s", 0.4), "GRAPHICS"), ("waitt", 7), ("event", "GRAPHICS rows"),
    ("press", "b", ("s", 0.4), "back"), ("waitt", 5),
    ("press", "down", ("s", 0.4), "down"), ("waitt", 3),
    ("press", "a", ("s", 0.4), "COMFORT"), ("waitt", 7), ("event", "COMFORT rows"),
    ("press", "b", ("s", 0.4), "back"), ("waitt", 5),
    ("press", "down", ("s", 0.4), "down"), ("waitt", 3),
    ("press", "a", ("s", 0.4), "GESTURES"), ("waitt", 7), ("event", "GESTURES rows"),
    ("press", "b", ("s", 0.4), "back"), ("waitt", 5),
    ("press", "down", ("s", 0.4), "down"), ("waitt", 3),
    ("press", "a", ("s", 0.4), "INFO"), ("waitt", 7), ("event", "INFO rows"),
] + [x for i in range(9) for x in (("press", "down", ("s", 0.4), "info down %d" % i), ("waitt", 2.5))] + [
    ("event", "INFO scrolled"),
    ("press", "b", ("s", 0.4), "back"), ("waitt", 5),
    ("press", "b", ("s", 0.4), "back to options"), ("waitt", 6), ("event", "options after VR SETTINGS"),
    ("press", "down", ("s", 0.4), "down"), ("waitt", 3),
    ("press", "a", ("s", 0.4), "SOUND SETUP"), ("waitt", 7), ("event", "sound page"),
    ("press", "b", ("s", 0.4), "back"), ("waitt", 5),
    ("press", "down", ("s", 0.4), "down"), ("waitt", 3),
    ("press", "a", ("s", 0.4), "PICTURE SETUP"), ("waitt", 7), ("event", "picture page"),
    ("press", "b", ("s", 0.4), "back"), ("waitt", 5),
    ("press", "b", ("s", 0.4), "back to main"), ("waitt", 6), ("event", "main menu again"),
    ("waitt", 5), ("end",),
]

PLANS = {"level": LEVEL_PLAN, "fast": LEVEL_PLAN_FAST, "deep": LEVEL_PLAN_DEEP, "scope": LEVEL_PLAN_SCOPE, "scope2": LEVEL_PLAN_SCOPE2,
         "rpg": LEVEL_PLAN_RPG, "frontend": FRONTEND_PLAN, "idle": [("waits", 200), ("end",)]}


# ---- frame checks -----------------------------------------------------------------------------------------------------
def luma(rgb):
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def half(a):
    h, w = a.shape[0] // 2 * 2, a.shape[1] // 2 * 2
    a = a[:h, :w]
    return (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2]) * 0.25


def blocks(a, b):
    h, w = a.shape[0] // b * b, a.shape[1] // b * b
    return a[:h, :w].reshape(h // b, b, w // b, b)


class Checker:
    def __init__(self):
        self.prev = []          # (lumaL_half, lumaR_half, blockmeansL, blockmeansR) of the last two stereo frames

    def stereo(self, L, R, H):
        out = {}
        yl, yr = luma(L.astype(np.float32)), luma(R.astype(np.float32))
        out["meanL"], out["meanR"] = float(yl.mean()), float(yr.mean())
        out["blackL"], out["blackR"] = float((yl <= 2.0).mean()), float((yr <= 2.0).mean())
        bl, br = blocks(yl, 16).max(axis=(1, 3)), blocks(yr, 16).max(axis=(1, 3))
        out["blkL"], out["blkR"] = int((bl <= 3.0).sum()), int((br <= 3.0).sum())
        # left/right block matching at half resolution: L(x) ~ R(x - d), d in 20..100 half px (40..200 full: the
        # test FOVs put infinity at ~73 px full at 480 wide, nearer is more); vertical offset 0 (same orientation)
        hl, hr = half(yl), half(yr)
        hh, hw = hl.shape
        best = np.full((hh // 8, hw // 8), 1e9, dtype=np.float32)
        bestd = np.zeros_like(best)
        for d in range(20, 101, 2):
            diff = np.full((hh, hw), 255.0, dtype=np.float32)
            diff[:, d:] = np.abs(hl[:, d:] - hr[:, :-d])
            sad = blocks(diff, 8).mean(axis=(1, 3))
            m = sad < best
            best[m] = sad[m]
            bestd[m] = d
        tex = blocks(hl, 8).std(axis=(1, 3))
        valid = np.zeros_like(best, dtype=bool)
        valid[:, 13:] = True                     # x >= 104 half px: a match exists for every allowed d
        un = valid & (tex > 6.0) & (best > np.maximum(14.0, 0.9 * tex))
        out["unmatched"] = int(un.sum())
        out["valid_tex"] = int((valid & (tex > 6.0)).sum())
        if un.any():
            ys, xs = np.nonzero(un)
            out["unmatched_box"] = [int(xs.min() * 16), int(ys.min() * 16), int(xs.max() * 16 + 16), int(ys.max() * 16 + 16)]
        out["disp_med"] = float(np.median(bestd[valid & (tex > 6.0)])) * 2 if (valid & (tex > 6.0)).any() else -1
        mL, mR = blocks(hl, 8).mean(axis=(1, 3)), blocks(hr, 8).mean(axis=(1, 3))
        if self.prev:
            pl, pr, pml, pmr = self.prev[-1]
            dl = float((np.abs(hl - pl) > 25).mean())
            dr = float((np.abs(hr - pr) > 25).mean())
            out["chgL"], out["chgR"] = dl, dr
        if len(self.prev) >= 2:
            (_, _, ml0, mr0), (_, _, ml1, mr1) = self.prev[-2], self.prev[-1]
            # A-B-A on the previous frame: it differs from both neighbours, which agree
            fl = (np.abs(ml1 - ml0) > 30) & (np.abs(ml1 - mL) > 30) & (np.abs(ml0 - mL) < 8)
            fr = (np.abs(mr1 - mr0) > 30) & (np.abs(mr1 - mR) > 30) & (np.abs(mr0 - mR) < 8)
            out["flickL"], out["flickR"] = int(fl.sum()), int(fr.sum())
            # how much of the view is the same two frames apart: flicker only means something in a still view
            out["still"] = float(min((np.abs(ml0 - mL) < 8).mean(), (np.abs(mr0 - mR) < 8).mean()))
        self.prev = (self.prev + [(hl, hr, mL, mR)])[-2:]
        if H is not None:
            a = H[..., 3]
            out["hud_cov"] = float((a > 0).mean())
            # stray HUD content: covered pixels (1/4 res) outside where the HUD usually draws (covered in >= 30 % of
            # this run's frames so far, grown by 3 px): a world draw sent to the HUD layer, or a new HUD element
            c = blocks((a > 0).astype(np.float32), 4).max(axis=(1, 3)) > 0
            if not hasattr(self, "hud_acc") or self.hud_acc.shape != c.shape:
                self.hud_acc, self.hud_n = np.zeros(c.shape, dtype=np.float32), 0
            if self.hud_n >= 30:
                m = self.hud_acc >= 0.3 * self.hud_n
                g = m.copy()
                for dy in range(-3, 4):
                    for dx in range(-3, 4):
                        g |= np.roll(np.roll(m, dy, 0), dx, 1)
                st = c & ~g
                out["hud_stray"] = int(st.sum())
                if st.any():
                    ys, xs = np.nonzero(st)
                    out["hud_stray_box"] = [int(xs.min() * 4), int(ys.min() * 4), int(xs.max() * 4 + 4), int(ys.max() * 4 + 4)]
            self.hud_acc += c
            self.hud_n += 1
            out["hud_opaque"] = float((a >= 250).mean())
            out["hud_opaque_black"] = float(((a >= 250) & (H[..., :3].max(axis=2) < 10)).mean())
        flags = []
        if abs(out["meanL"] - out["meanR"]) > 12:
            flags.append("eye-luma")
        if abs(out["blkL"] - out["blkR"]) > 40 and abs(out["blkL"] - out["blkR"]) > 0.5 * max(out["blkL"], out["blkR"]):
            flags.append("black-blocks-one-eye")
        if out["unmatched"] >= 25:
            flags.append("lr-unmatched")
        if "chgL" in out and max(out["chgL"], out["chgR"]) > 0.01 and min(out["chgL"], out["chgR"]) < 0.3 * max(out["chgL"], out["chgR"]):
            flags.append("one-eye-change")
        if max(out.get("flickL", 0), out.get("flickR", 0)) >= 6 and out.get("still", 0) > 0.85:
            flags.append("flicker")
        if out.get("hud_stray", 0) >= 12:
            flags.append("hud-stray")
        if out.get("hud_opaque", 0) > 0.12:
            flags.append("hud-opaque")
        out["flags"] = flags
        return out

    def flat(self, F):
        y = luma(F.astype(np.float32))
        return {"mean": float(y.mean()), "black": float((y <= 2.0).mean()), "flags": []}


# ---- the run ----------------------------------------------------------------------------------------------------------
class Run:
    def __init__(self, a):
        self.a = a
        self.sweep = os.path.join(BUILD, "sweep")
        self.frames = os.path.join(self.sweep, "frames-" + a.name)
        self.vdir = os.path.join(self.frames, "video")
        self.keep = os.path.join(self.frames, "keep")
        self.raw = os.path.join(self.frames, "raw")
        for d in (self.frames, self.keep, self.raw):
            os.makedirs(d, exist_ok=True)
        self.log_path = os.path.join(self.sweep, a.name + ".log")
        self.metrics = open(os.path.join(self.frames, "metrics.jsonl"), "a")
        self.events = open(os.path.join(self.frames, "events.jsonl"), "a")
        self.drv = open(os.path.join(self.frames, "driver.log"), "a")
        self.meta_pos = 0
        self.queue = []
        self.last = None             # the newest meta record
        self.nstereo = 0
        self.nflat = 0
        self.first_stereo_t = None
        self.stereo_t = []           # wall times of stereo records (frame time)
        self.keep_until = -1         # keep every record up to this seq
        self.raw_marks = set()       # seqs whose raw PNGs are kept
        self.nraw = 0
        self.checker = Checker()
        self.last_mode = None
        self.mode_change_seq = -1
        self.plan = list(PLANS[a.plan])
        self.step = 0
        self.step_t0 = None
        self.step_s0 = None
        self.fallback_i = 0
        self.fallback_t = None
        self.phase = "frontend" if a.plan != "frontend" else "plan"
        self.last_a = 0.0
        self.t_start = time.time()
        self.cinema_frames = 0
        self.done = False
        self.font = None

    def say(self, msg):
        line = "%s t=%.0fs %s" % (time.strftime("%H:%M:%S"), time.time() - self.t_start, msg)
        print(line, flush=True)
        self.drv.write(line + "\n")
        self.drv.flush()

    def start(self):
        env = dict(os.environ)
        env.update({
            "VK_ICD_FILENAMES": ICD, "GRX_SECONDS": str(self.a.seconds), "GRX_AUDIO_WAV": "", "GRX_EYE_W": "480",
            "GRX_EYE_H": "512", "GRX_HAND": "1", "GRX_VIDEO": "1", "GRX_CAPTURE_EVERY": "100000",
            "GRX_INPUT": (FRONT_END_FAST if self.a.fast else FRONT_END) if self.a.plan != "frontend" else "60:menu",
            "GRX_STALL_DUMP": "150", "RECOMP_KVK_SPRITELOG": "1", "BLACKVR_INPUT_TRACE": "1", "BLACKVR_DECIDE_TRACE": "1",
        })
        if self.a.hz30:
            env["GRX_INI"] = (env.get("GRX_INI", "") + "\ngame_hz = 30\n").lstrip("\n")
        if self.a.stale_ms:
            env["BLACKVR_TEST_LOOK_STALE_MS"] = str(self.a.stale_ms)
        if self.a.level is not None:
            env["RECOMP_BLACK_TEST_LEVEL"] = str(self.a.level)
        for kv in self.a.env:
            k, v = kv.split("=", 1)
            env[k] = v
        scratch = self.a.scratch or os.path.join(self.sweep, "scratch")
        self.say("start %s level=%s plan=%s seconds=%s scratch=%s extra=%s hz30=%s stale_ms=%s" % (
            self.a.name, self.a.level, self.a.plan, self.a.seconds, scratch, self.a.env, self.a.hz30, self.a.stale_ms))
        logf = open(self.log_path, "w")
        cmd = ["nice", "-n", "5"]
        cmd += [HARNESS, ROOT, scratch, self.frames]
        self.proc = subprocess.Popen(cmd, cwd=BUILD, env=env,
                                     stdout=logf, stderr=subprocess.STDOUT)
        self.say("harness pid %d" % self.proc.pid)

    def press(self, btn, hold_s):
        tmp = os.path.join(self.frames, "press.tmp")
        dst = os.path.join(self.frames, "press")
        lines = ""
        if os.path.exists(dst):            # not yet read by the harness: append
            try:
                lines = open(dst).read()
            except OSError:
                lines = ""
        with open(tmp, "w") as f:
            f.write(lines + ("%s:%.2f\n" % (btn, hold_s) if hold_s else "%s\n" % btn))
        os.replace(tmp, dst)

    def event(self, label):
        seq = self.last["seq"] if self.last else 0
        rec = {"t": round(time.time() - self.t_start, 1), "wall": time.strftime("%H:%M:%S"), "seq": seq,
               "stereo_frames": self.nstereo, "mode": self.last["mode"] if self.last else None,
               "flips": self.last["flips"] if self.last else None, "label": label}
        self.events.write(json.dumps(rec) + "\n")
        self.events.flush()
        self.keep_until = max(self.keep_until, seq + 8)
        self.raw_marks.add(seq + 3)
        self.say("event %s (seq %d, stereo frame %d)" % (label, seq, self.nstereo))

    def frame_time(self):
        ts = self.stereo_t[-12:]
        if len(ts) < 3:
            return 0.8
        return max(0.2, min(5.0, (ts[-1] - ts[0]) / (len(ts) - 1)))

    def read_meta(self):
        p = os.path.join(self.vdir, "meta.jsonl")
        if not os.path.exists(p):
            return
        with open(p) as f:
            f.seek(self.meta_pos)
            data = f.read()
        nl = data.rfind("\n")
        if nl < 0:
            return
        self.meta_pos += nl + 1
        for line in data[:nl].split("\n"):
            if not line.strip():
                continue
            try:
                r = json.loads(line)
            except ValueError:
                continue
            self.last = r
            if r["mode"] != self.last_mode:
                self.say("present mode %s -> %s at seq %d (flips %d)" % (self.last_mode, r["mode"], r["seq"], r["flips"]))
                self.last_mode = r["mode"]
                self.mode_change_seq = r["seq"]
                self.keep_until = max(self.keep_until, r["seq"] + 3)
            if r["kind"] == "stereo":
                self.nstereo += 1
                self.stereo_t.append(r["t"])
                if self.first_stereo_t is None:
                    self.first_stereo_t = r["t"]
                    self.say("first STEREO frame at harness t=%.1f" % r["t"])
            else:
                self.nflat += 1
            if r["mode"] == 2:
                self.cinema_frames += 1
            self.queue.append(r)

    # ---- per-frame processing (analysis, keep / delete) ----
    def process(self, budget_s):
        t0 = time.time()
        while self.queue and (time.time() - t0 < budget_s or len(self.queue) > 400):
            r = self.queue.pop(0)
            self.process_one(r)

    def process_one(self, r):
        seq = r["seq"]
        names = [r.get("eyeL"), r.get("eyeR")] + [q["image"] for q in r["quads"] if q.get("image")]
        names = [n for n in names if n]
        paths = [os.path.join(self.vdir, n) for n in names]
        m = {"seq": seq, "t": r["t"], "flips": r["flips"], "mode": r["mode"], "kind": r["kind"], "gf": r["game_frame"]}
        try:
            if r["kind"] == "stereo" and r.get("eyeL") and r.get("eyeR"):
                L = np.asarray(Image.open(os.path.join(self.vdir, r["eyeL"])).convert("RGB"))
                R = np.asarray(Image.open(os.path.join(self.vdir, r["eyeR"])).convert("RGB"))
                hq = [q for q in r["quads"] if q["layer"] == "hud" and q.get("image")]
                H = np.asarray(Image.open(os.path.join(self.vdir, hq[0]["image"])).convert("RGBA")) if hq else None
                m.update(self.checker.stereo(L, R, H))
                keep = (self.nstereo_done() % 8 == 0) or seq <= self.keep_until or bool(m["flags"])
            else:
                fq = [q for q in r["quads"] if q["layer"] == "flat" and q.get("image")]
                if fq and (self.nflat_done() % 10 == 0 or seq <= self.keep_until):
                    F = np.asarray(Image.open(os.path.join(self.vdir, fq[0]["image"])).convert("RGB"))
                    m.update(self.checker.flat(F))
                keep = (self.nflat_done() % 60 == 0) or seq <= self.keep_until
        except Exception as e:      # a partial file: count it, move on
            m["error"] = str(e)
            keep = False
        if r["kind"] == "stereo":
            self._ns_done += 1
        else:
            self._nf_done += 1
        self.metrics.write(json.dumps(m) + "\n")
        if keep and "error" not in m:
            try:
                if self.font is None:
                    try:
                        self.font = make_video.ImageFont.load_default(size=14)
                    except TypeError:
                        self.font = make_video.ImageFont.load_default()
                fq = [q for q in r["quads"] if q["layer"] == "flat" and q.get("image")]
                if r["kind"] != "stereo" and fq:
                    # a flat frame: the flat quad's own image (the same in both eyes), readable
                    im = Image.open(os.path.join(self.vdir, fq[0]["image"])).convert("RGB")
                    im = im.resize((800, int(im.size[1] * 800 / im.size[0])))
                    d = make_video.ImageDraw.Draw(im)
                    d.rectangle([0, 0, 800, 20], fill=(0, 0, 0))
                    d.text((6, 2), "game frame %d  %s  (flat quad, both eyes; presented #%d)" % (
                        r["flips"], make_video.MODES.get(r["mode"], "?"), seq), fill=(235, 235, 235), font=self.font)
                else:
                    im = make_video.compose(self.frames, r, self.font, 960)
                im.save(os.path.join(self.keep, "k%06d.jpg" % seq), quality=80)
            except Exception as e:
                self.say("compose %d failed: %s" % (seq, e))
        want_raw = (seq in self.raw_marks or (m.get("flags") and self.nraw < 25)) and self.nraw < 60
        for n, p in zip(names, paths):
            if want_raw and os.path.exists(p):
                os.replace(p, os.path.join(self.raw, n))
            else:
                try:
                    os.unlink(p)
                except OSError:
                    pass
        if want_raw:
            self.nraw += 1
        if m.get("flags"):
            self.say("flag seq %d: %s" % (seq, ",".join(m["flags"])))

    _ns_done = 0
    _nf_done = 0

    def nstereo_done(self):
        return self._ns_done

    def nflat_done(self):
        return self._nf_done

    # ---- the script ----
    def drive(self):
        now = time.time()
        if self.phase == "frontend":
            # GRX_INPUT does title / START MISSION / NORMAL; then A every 25 s until STEREO (intro card, loading),
            # except while a scripted scene (cinema) is still young
            if self.first_stereo_t is not None:
                self.phase = "plan"
                self.say("gameplay plan starts")
                return
            if self.last and self.last["t"] > self.a.no_stereo_s:
                self.say("no STEREO frame by t=%.0f s (mode %s): the level did not start; quit" % (self.last["t"], self.last["mode"]))
                self.event("NO STEREO: level not reached")
                self.press("quit", 0)
                self.phase = "gave-up"
                self.step = len(self.plan)
                return
            if self.last and self.last["t"] > (130 if self.a.fast else 200) and now - self.last_a > 25:
                if self.last["mode"] == 2 and self.cinema_frames < 60:
                    return
                self.last_a = now
                self.press("a", 0.5)
                self.say("A (waiting for STEREO, mode %s, cinema frames %d)" % (self.last["mode"], self.cinema_frames))
            return
        if self.phase != "plan" or self.step >= len(self.plan):
            return
        st = self.plan[self.step]
        if self.step_t0 is None:
            self.step_t0, self.step_s0 = now, self.nstereo
        kind = st[0]
        advance = False
        if kind == "waits":
            advance = self.nstereo - self.step_s0 >= st[1]
            if not advance and now - self.step_t0 > max(240, st[1] * 6):
                self.say("waits %d timed out (mode %s): moving on" % (st[1], self.last["mode"] if self.last else None))
                advance = True
        elif kind == "waitt":
            advance = now - self.step_t0 >= st[1]
        elif kind == "press":
            u, v = st[2]
            hold = v * self.frame_time() if u == "f" else v
            self.press(st[1], hold)
            self.event("%s [%s %.1fs]" % (st[3], st[1], hold))
            advance = True
        elif kind == "event":
            self.event(st[1])
            advance = True
        elif kind == "cap":
            self.press("cap", 0)
            advance = True
        elif kind == "waitf":
            # st[1] more presented frames of any kind (flat menus too), at most st[2] s
            if not hasattr(self, "_wf0") or self._wf_step != self.step:
                self._wf0, self._wf_step = (self.last["seq"] if self.last else 0), self.step
            advance = (self.last is not None and self.last["seq"] - self._wf0 >= st[1]) or now - self.step_t0 > st[2]
        elif kind == "untilmode":
            # until the present mode is st[1] (e.g. the pause menu's FLAT_MENU), at most st[2] s
            advance = (self.last is not None and self.last["mode"] == st[1]) or now - self.step_t0 > st[2]
            if advance and now - self.step_t0 > st[2]:
                self.say("mode %d not reached in %d s (mode %s)" % (st[1], st[2], self.last["mode"] if self.last else None))
        elif kind == "stereo":
            if self.last and self.last["kind"] == "stereo" and self.last["mode"] == 0:
                advance = True
            else:
                fb = st[2]
                if self.fallback_t is None:
                    self.fallback_t = now
                if now - self.fallback_t >= st[1] / 3.0:
                    b, h = fb[self.fallback_i % len(fb)]
                    self.fallback_i += 1
                    self.fallback_t = now
                    self.press(b, h)
                    self.event("not back in STEREO: fallback %s" % b)
                if self.fallback_i > 3 * len(fb):
                    self.say("could not get back to STEREO: moving on")
                    advance = True
            if advance:
                self.fallback_i, self.fallback_t = 0, None
        elif kind == "end":
            self.say("plan done: quit")
            self.press("quit", 0)
            self.step += 1
            return
        if advance:
            self.step += 1
            self.step_t0 = None

    def poll_fds(self):
        """The disc files the harness has open (first seen, with the time): which level folder loads."""
        if not hasattr(self, "_fds"):
            self._fds, self._fdf, self._fdt = set(), open(os.path.join(self.frames, "openfiles.txt"), "a"), 0.0
        if time.time() - self._fdt < 2.0:
            return
        self._fdt = time.time()
        d = "/proc/%d/fd" % self.proc.pid
        try:
            names = os.listdir(d)
        except OSError:
            return
        for n in names:
            try:
                t = os.readlink(os.path.join(d, n))
            except OSError:
                continue
            if "/game/" in t and t not in self._fds:
                self._fds.add(t)
                self._fdf.write("%s t=%.0f seq=%s %s\n" % (time.strftime("%H:%M:%S"), time.time() - self.t_start,
                                                         self.last["seq"] if self.last else "-", t))
                self._fdf.flush()

    def loop(self):
        last_frame_wall = time.time()
        last_seq = -1
        quit_sent = None
        while True:
            self.read_meta()
            if self.last and self.last["seq"] != last_seq:
                last_seq = self.last["seq"]
                last_frame_wall = time.time()
            try:
                self.drive()
            except Exception as e:
                self.say("drive error: %r" % e)
            self.process(0.6)
            self.poll_fds()
            if self.proc.poll() is not None:
                self.read_meta()
                self.process(1e9)
                self.say("harness exited rc=%s" % self.proc.returncode)
                break
            if self.step >= len(self.plan) and quit_sent is None:
                quit_sent = time.time()
            if quit_sent and time.time() - quit_sent > 240:
                self.say("harness did not quit: SIGTERM")
                self.proc.terminate()
            if time.time() - last_frame_wall > self.a.hang_s and self.last is not None:
                self.say("no new frame for %.0f s: treating as a hang, stopping the run" % (time.time() - last_frame_wall))
                self.event("HANG: no frame for %.0f s" % (time.time() - last_frame_wall))
                self.proc.send_signal(signal.SIGTERM)
                time.sleep(5)
                if self.proc.poll() is None:
                    self.proc.kill()
            if self.last is None and time.time() - self.t_start > 400:
                self.say("no frame presented in 400 s: stopping")
                self.proc.terminate()
            time.sleep(0.25)
        # leftovers of video/ (frames dumped after the last meta line)
        for n in os.listdir(self.vdir):
            if n.endswith(".png"):
                try:
                    os.unlink(os.path.join(self.vdir, n))
                except OSError:
                    pass
        self.metrics.close()
        self.events.close()
        self.say("done: %d stereo, %d flat records" % (self.nstereo, self.nflat))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--level", type=int)
    ap.add_argument("--plan", default="level", choices=sorted(PLANS))
    ap.add_argument("--seconds", type=int, default=1500)
    ap.add_argument("--hang-s", type=float, default=420.0)
    ap.add_argument("--no-stereo-s", type=float, default=1000.0, help="give up when no STEREO frame by this harness time")
    ap.add_argument("--scratch")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--fast", action="store_true", help="the front end sooner (FRONT_END_FAST)")
    ap.add_argument("--hz30", action="store_true", help="blackvr.ini game_hz = 30 (twice the game time per frame)")
    ap.add_argument("--stale-ms", type=int, default=0, help="BLACKVR_TEST_LOOK_STALE_MS: an extra wall-clock limit on the look age (not needed since the decision counts world cameras)")
    a = ap.parse_args()
    r = Run(a)
    r.start()
    try:
        r.loop()
    except KeyboardInterrupt:
        r.proc.terminate()
        raise
    return 0


if __name__ == "__main__":
    sys.exit(main())
