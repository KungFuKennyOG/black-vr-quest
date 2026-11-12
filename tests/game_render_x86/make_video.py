#!/usr/bin/env python3
"""tests/game_render_x86: frames/run.mp4 -- the run as the headset would show it.

Reads <frames>/video/meta.jsonl and the layer images game_render_x86 dumped for every newly presented frame, and
composes each one per eye, left | right: the eye image of the projection layer (stereo frames) or black (flat frames),
then every quad layer in submission order (the flat menu / cinema / scope quad, the HUD quad), projected with the
submitted eye pose and FOV from where the shell placed it (world- or view-space pose and size, metres), blended
premultiplied as the compositor would. A caption carries the game frame and the present mode.

Timing follows the game's frame clock, not SwiftShader's: frame n of the backend (one flip per main-loop iteration,
1/60 s each under RECOMP_BLACK_60HZ) is shown at n / 60 s, resampled to a constant frame rate. Stereo frames, which
SwiftShader draws at ~1.5 s each here, are shown at --stereo-fps game frames per second instead (default 12, 0 = the
game clock too), and say so in the caption; the head sway was driven by the wall clock, so at the game clock it
would race.

--flat-clock wall shows flat frames (title, menus, loading) at the harness's wall clock instead ("t" in meta.jsonl), so
they line up with audio recorded on the wall clock; --audio <wav> muxes the fake AAudio device's recording (frames/
audio.wav, host_stubs.c) as AAC: the flat section in sync (wall clock to wall clock), the stereo section from the wall
time of its first frame onwards at the audio's own rate -- the picture runs at --stereo-fps game frames/s while the
audio runs in real time, so the two drift apart there (by the ratio of SwiftShader's frame time to 1/stereo-fps).

Usage: make_video.py <frames dir> [out.mp4] [--fps 30] [--width 1664] [--flat-clock game|wall] [--audio wav]
                     [--ffmpeg /usr/bin/ffmpeg]
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

MODES = {0: "STEREO", 1: "FLAT-MENU", 2: "FLAT-CINEMA", 3: "FLAT-SCOPE"}


def q_rot(q, v):
    x, y, z, w = q
    u = np.array([x, y, z])
    v = np.asarray(v, dtype=np.float64)
    return v + 2.0 * np.cross(u, np.cross(u, v) + w * v)


def q_conj(q):
    return (-q[0], -q[1], -q[2], q[3])


def to_eye(eye, p_world):
    return q_rot(q_conj(eye["pose"]["q"]), np.asarray(p_world) - np.asarray(eye["pose"]["p"]))


def project(eye, p_eye, w, h):
    l, r, u, d = [np.tan(a) for a in eye["fov"]]
    z = -p_eye[2]
    if z <= 1e-4:
        return None
    tx, ty = p_eye[0] / z, p_eye[1] / z
    return ((tx - l) / (r - l) * w, (u - ty) / (u - d) * h)


def homography(src, dst):
    """3x3 H with dst ~ H src (4 point pairs)."""
    a = []
    for (x, y), (X, Y) in zip(src, dst):
        a.append([x, y, 1, 0, 0, 0, -X * x, -X * y, -X])
        a.append([0, 0, 0, x, y, 1, -Y * x, -Y * y, -Y])
    _, _, vt = np.linalg.svd(np.asarray(a, dtype=np.float64))
    hm = vt[-1].reshape(3, 3)
    return hm / hm[2, 2]


def quad_corners_world(quad, head):
    sw, sh = quad["size"]
    local = [(-sw / 2, sh / 2, 0), (sw / 2, sh / 2, 0), (sw / 2, -sh / 2, 0), (-sw / 2, -sh / 2, 0)]
    pts = [np.asarray(quad["pose"]["p"]) + q_rot(quad["pose"]["q"], c) for c in local]
    if quad["space"] == "view":          # view space = the head
        pts = [np.asarray(head["p"]) + q_rot(head["q"], p) for p in pts]
    return pts


def composite_quad(canvas, img, eye, quad, head):
    """canvas: float32 HxWx3 (gamma-space values, as the swapchain bytes); img: RGBA uint8 (premultiplied for the HUD)."""
    h, w = canvas.shape[:2]
    corners = []
    for p in quad_corners_world(quad, head):
        c = project(eye, to_eye(eye, p), w, h)
        if c is None:
            return
        corners.append(c)
    iw, ih = img.size
    src = [(0, 0), (iw, 0), (iw, ih), (0, ih)]
    hinv = homography(corners, src)                      # output pixel -> input pixel
    coeffs = (hinv / hinv[2, 2]).flatten()[:8]
    warped = img.transform((w, h), Image.PERSPECTIVE, tuple(coeffs), Image.BILINEAR, fillcolor=(0, 0, 0, 0))
    a = np.asarray(warped, dtype=np.float32) / 255.0
    rgb, alpha = a[..., :3], a[..., 3:4]
    if not quad.get("premultiplied", True):
        rgb = rgb * alpha
    canvas[:] = rgb + canvas * (1.0 - alpha)


def compose(frames_dir, rec, font, out_w, note=""):
    vdir = os.path.join(frames_dir, "video")
    ew, eh = rec["eye_w"], rec["eye_h"]
    eyes = rec["eyes"]
    head = {"q": eyes[0]["pose"]["q"],
            "p": list((np.asarray(eyes[0]["pose"]["p"]) + np.asarray(eyes[1]["pose"]["p"])) / 2.0)}
    views = []
    for e, key in enumerate(("eyeL", "eyeR")):
        if rec["kind"] == "stereo" and rec[key]:
            canvas = np.asarray(Image.open(os.path.join(vdir, rec[key])).convert("RGB"), dtype=np.float32) / 255.0
        else:
            canvas = np.zeros((eh, ew, 3), dtype=np.float32)
        for quad in rec["quads"]:
            if not quad["image"]:
                continue
            img = Image.open(os.path.join(vdir, quad["image"]))
            img = img.convert("RGBA") if img.mode != "RGBA" else img
            if quad["layer"] == "flat":
                img.putalpha(255)
            composite_quad(canvas, img, eyes[e], quad, head)
        views.append(canvas)
    frame = np.concatenate(views, axis=1)
    im = Image.fromarray(np.clip(frame * 255.0 + 0.5, 0, 255).astype(np.uint8))
    if im.size[0] > out_w:
        im = im.resize((out_w, int(round(im.size[1] * out_w / im.size[0] / 2)) * 2), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    cap = "game frame %d   %s   (cam %d, presented #%d)" % (rec["flips"], MODES.get(rec["mode"], "?"),
                                                           rec["game_frame"], rec["seq"])
    d.rectangle([0, 0, im.size[0], 26], fill=(0, 0, 0))
    d.text((8, 4), cap, fill=(235, 235, 235), font=font)
    if note:
        d.text((im.size[0] // 2 + 110, 4), note, fill=(200, 170, 90), font=font)
    d.text((im.size[0] // 2 + 8, 4), "right eye", fill=(160, 160, 160), font=font)
    return im


_CTX = None
_FONT = None


def _compose_idx(i):
    global _FONT
    frames, width, note, recs = _CTX
    if _FONT is None:
        try:
            _FONT = ImageFont.load_default(size=18)
        except TypeError:
            _FONT = ImageFont.load_default()
    r = recs[i]
    return compose(frames, r, _FONT, width, note if r["kind"] == "stereo" else "")


def _compose_bytes(i):
    return _compose_idx(i).tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--width", type=int, default=1664)
    ap.add_argument("--game-hz", type=float, default=60.0)
    ap.add_argument("--stereo-fps", type=float, default=12.0)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--ffmpeg", default="/usr/bin/ffmpeg")
    ap.add_argument("--flat-clock", choices=("game", "wall"), default="game")
    ap.add_argument("--audio")
    ap.add_argument("--max-seconds", type=float, default=0.0, help="cut the output after this many seconds")
    args = ap.parse_args()
    out = args.out or os.path.join(args.frames, "run.mp4")
    recs = [json.loads(l) for l in open(os.path.join(args.frames, "video", "meta.jsonl")) if l.strip()]
    recs = [r for r in recs if (r["kind"] == "stereo" and r["eyeL"]) or any(q["image"] for q in r["quads"])]
    if not recs:
        print("make_video: no frames")
        return 1
    # the timeline: game clock between flat frames, --stereo-fps for each stereo frame
    times = [0.0]
    for a, b in zip(recs, recs[1:]):
        if b["kind"] == "stereo" and args.stereo_fps > 0:
            dt = 1.0 / args.stereo_fps
        elif args.flat_clock == "wall":
            dt = max(0.0, b["t"] - a["t"])
        else:
            dt = max(0, b["flips"] - a["flips"]) / args.game_hz
        times.append(times[-1] + dt)
    total = times[-1] + 1.0 / args.fps
    if args.max_seconds > 0:
        total = min(total, args.max_seconds)
    nout = max(1, int(total * args.fps + 0.5))
    # which record each output frame shows, and so which records need composing
    pick, i = [], 0
    for k in range(nout):
        t = k / args.fps
        while i + 1 < len(recs) and times[i + 1] <= t:
            i += 1
        pick.append(i)
    needed = sorted(set(pick))
    note = ("stereo at %g game frames/s (SwiftShader: ~1.5 s each)" % args.stereo_fps) if args.stereo_fps > 0 else ""
    global _CTX
    _CTX = (args.frames, args.width, note, recs)
    first = _compose_idx(needed[0])
    w, h = first.size
    cmd = [args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
           "-s", "%dx%d" % (w, h), "-r", "%g" % args.fps, "-i", "-"]
    if args.audio:
        # the audio track on the video's timeline: video time v of record i <-> wall time t_i + (v - times[i]),
        # piecewise per flat record (wall clock), one continuous piece from the first stereo record on
        side = {}
        if os.path.exists(args.audio + ".json"):
            side = json.load(open(args.audio + ".json"))
        a0 = side.get("start_t", 0.0)
        pieces = []
        st = next((i for i, r in enumerate(recs) if r["kind"] == "stereo"), len(recs))
        # (with --flat-clock game the flat section's audio is simply laid from its first frame on: not in sync)
        pieces.append((0.0, recs[0]["t"] - a0, (times[st] if st < len(recs) else total)))
        if st < len(recs):
            pieces.append((times[st], recs[st]["t"] - a0, total))
        flt, labels = [], []
        for k, (v0, w0, v1) in enumerate(pieces):
            if v1 <= v0:
                continue
            flt.append("[1:a]atrim=start=%.4f:duration=%.4f,asetpts=PTS-STARTPTS,adelay=%d|%d[a%d]"
                       % (max(0.0, w0), v1 - v0, int(v0 * 1000), int(v0 * 1000), k))
            labels.append("[a%d]" % k)
        flt.append("%samix=inputs=%d:normalize=0:duration=longest[aout]" % ("".join(labels), len(labels)))
        cmd += ["-i", args.audio, "-filter_complex", ";".join(flt), "-map", "0:v", "-map", "[aout]",
                "-c:a", "aac", "-b:a", "160k", "-t", "%.3f" % total]
        print("make_video: audio pieces (video s, wav s, until video s): %s" % pieces)
    cmd += ["-c:v", "libx264", "-preset", "veryfast",
            "-crf", "23", "-pix_fmt", "yuv420p", "-movflags", "+faststart", out]
    enc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    import multiprocessing as mp
    with mp.get_context("fork").Pool(max(1, args.jobs)) as pool:
        it = pool.imap(_compose_bytes, needed, chunksize=4)
        cur_i, cur_bytes, pos = None, None, 0
        for k in range(nout):
            while cur_i != pick[k]:
                cur_bytes = next(it)
                cur_i = needed[pos]
                pos += 1
            enc.stdin.write(cur_bytes)
    enc.stdin.close()
    rc = enc.wait()
    size = os.path.getsize(out) if os.path.exists(out) else 0
    print("make_video: %s: %d presented frames -> %d frames at %g fps (%.1f s of game time), %dx%d, %.1f MB, ffmpeg rc %d"
          % (out, len(recs), nout, args.fps, nout / args.fps, w, h, size / 1e6, rc))
    return rc


if __name__ == "__main__":
    sys.exit(main())
