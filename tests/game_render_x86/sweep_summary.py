#!/usr/bin/env python3
"""sweep_summary.py -- the numbers and pictures of one sweep run (sweep_drive.py): log scan, heaviness, frame checks.

Usage: sweep_summary.py <name> [<name> ...]     (build-vrmenu-x86/sweep/<name>.log and frames-<name>/)
Writes frames-<name>/summary.json, summary.txt and events_montage*.jpg (each scripted event: the frame shown a few
frames later, left | right as the headset shows them), flags_montage.jpg (the flagged frames).
"""
import collections
import json
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

SWEEP = os.environ.get("SWEEP_DIR", os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "build-vrmenu-x86", "sweep"))

SCANS = [
    ("crash", re.compile(r"\[CRASH\]|SIGSEGV(?! trap)|Segmentation fault|Assertion|assert(ion)? fail|abort\(|FATAL", re.I)),
    ("stall", re.compile(r"\bSTALL\b")),
    ("hang", re.compile(r"HANG|no backend frame for")),
    ("vulkan", re.compile(r"VK_ERROR|VUID-|Validation Error|vk\w+ failed|device lost", re.I)),
    ("shader", re.compile(r"glslang.*(error|fail)|shader.*(fail|error)|SPIR-V.*(fail|invalid)|compile(d)? fail", re.I)),
    ("missing", re.compile(r"NtCreateFile FAILED|not on the disc|No such file|cannot open|missing", re.I)),
    ("disc", re.compile(r"PROBLEM WITH THE DISC|disc error|DISC ERROR|dirty disc|read error|STATUS_FILE_CORRUPT|"
                        r"short read|end of file", re.I)),
    ("kernel_err", re.compile(r"\bERROR\b")),
    ("warn", re.compile(r"\bWARN(ING)?\b")),
    ("bvr_check", re.compile(r"\[BLACK_VR\].*(differs|wrong|mismatch|flipped|disagree|refused|fail)", re.I)),
    ("game_returned", re.compile(r"black_game_main returned")),
    ("result", re.compile(r"\[GRX\] RESULT")),
]
READ = re.compile(r"\[READ\] @(\S+) want=(\d+) got=(\d+) st=0x([0-9A-Fa-f]+)")
PROG = re.compile(r"\[GRX\] t=(\d+)s icalls=\d+ \(\+\d+\) \| kvk frames (\d+) \(\+(\d+)\) draws \d+ \(\+(\d+)\) world (\d+) "
                  r"stereo (\d+) hud (\d+) uber (\d+) skipped (\d+) \| pipelines (\d+) vs (\d+) ps (\d+) tex (\d+) \| "
                  r"submit mode (-?\d+)")
IV = re.compile(r"\[PERF\] interval (\S+) wall ([\d.]+) flips (\d+) words (\d+) methods (\d+) draws (\d+) vertices (\d+) "
                r"faults (\d+) fault_us (\d+)")
CPU = re.compile(r"\[PERF\]   cpu (.+?)\s+x(\d+)\s+([\d.]+) ms")
SPR = re.compile(r"\[KVK\] SPRITE f (\d+) (.*)")
FTIME = re.compile(r"frame time (\d+) ms")
STALL_OVER = re.compile(r"STALL over: frames again after (\d+) s")


def scan_log(path):
    out = {k: {"n": 0, "ex": []} for k, _ in SCANS}
    reads = {"n": 0, "short": 0, "bad_status": 0, "ex": []}
    prog = []
    perf = collections.defaultdict(lambda: {"wall": 0.0, "flips": 0, "words": 0, "draws": 0, "cpu": collections.defaultdict(float)})
    cur = None
    spr = collections.defaultdict(collections.Counter)
    ftimes = []
    stall_over = []
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("[KVK] SPRITE"):
                m = SPR.match(line)
                if m:
                    fr, rest = int(m.group(1)), m.group(2)
                    if rest.startswith("hud"):
                        spr[fr]["hud"] += 1
                    elif rest.startswith("offscreen"):
                        spr[fr]["offscreen"] += 1
                    elif "-> corrected" in rest:
                        spr[fr]["corrected"] += 1
                    elif "-> full-screen" in rest:
                        spr[fr]["fullscreen"] += 1
                    elif "->" in rest:
                        spr[fr]["other:" + rest.split("->", 1)[1].split("|")[0].strip()] += 1
                    else:
                        spr[fr]["misc"] += 1
                continue
            m = READ.search(line)
            if m:
                reads["n"] += 1
                if int(m.group(3)) < int(m.group(2)) or m.group(4) != "00000000":
                    reads["short" if int(m.group(3)) < int(m.group(2)) else "bad_status"] += 1
                    if len(reads["ex"]) < 8:
                        reads["ex"].append(line.strip()[:240])
                continue
            m = PROG.search(line)
            if m:
                prog.append([int(x) for x in m.groups()])
                continue
            m = IV.search(line)
            if m:
                mode, flips = m.group(1), int(m.group(3))
                cur = perf[mode] if flips > 0 else None
                if cur is not None:
                    cur["wall"] += float(m.group(2))
                    cur["flips"] += flips
                    cur["words"] += int(m.group(4))
                    cur["draws"] += int(m.group(6))
                continue
            m = CPU.search(line)
            if m and cur is not None:
                cur["cpu"][m.group(1).strip()] += float(m.group(3))
                continue
            m = FTIME.search(line)
            if m:
                ftimes.append(int(m.group(1)))
            m = STALL_OVER.search(line)
            if m:
                stall_over.append(int(m.group(1)))
            if "PREFLIGHT" in line:
                continue
            for k, rx in SCANS:
                if rx.search(line):
                    out[k]["n"] += 1
                    if len(out[k]["ex"]) < 6:
                        out[k]["ex"].append(line.strip()[:300])
    out["stall"]["over_s"] = stall_over
    return out, reads, prog, perf, spr, ftimes


def heaviness(prog, perf, spr):
    h = {}
    # progress intervals in STEREO (submit mode 0 with stereo draws growing)
    dd = df = 0
    uber = 0
    for a, b in zip(prog, prog[1:]):
        if b[13] == 0 and b[5] > a[5] and b[2] > 0:
            df += b[2]
            dd += b[3]
            uber += b[7] - a[7]
    h["stereo_frames_in_progress"] = df
    h["draws_per_stereo_frame"] = round(dd / df, 1) if df else None
    h["uber_draws_in_stereo"] = uber
    if prog:
        last = prog[-1]
        h["pipelines"], h["vs"], h["ps"], h["tex_uploads"], h["uber_total"] = last[9], last[10], last[11], last[12], last[7]
    st = perf.get("STEREO")
    if st and st["flips"]:
        fl = st["flips"]
        h["perf_stereo_flips"] = fl
        h["perf_wall_per_flip_ms"] = round(st["wall"] / fl * 1000, 0)
        h["perf_draws_per_flip"] = round(st["draws"] / fl, 1)
        h["perf_words_per_flip"] = round(st["words"] / fl, 0)
        cpu = {k: round(v / fl, 2) for k, v in st["cpu"].items()}
        h["cpu_ms_per_flip"] = dict(sorted(cpu.items(), key=lambda kv: -kv[1]))
        h["pusher_ms_per_flip"] = cpu.get("bx-pusher")
        h["swiftshader_ms_per_flip"] = round(sum(v for k, v in cpu.items() if k.startswith("driver:Thread")), 2)
        h["game_thread_ms_per_flip"] = cpu.get("game(black_game_main)")
    if spr:
        frames = sorted(spr)
        tot = collections.Counter()
        for fr in frames:
            tot.update(spr[fr])
        n = len(frames)
        h["sprite_log_frames"] = n
        h["per_frame"] = {k: round(v / n, 1) for k, v in sorted(tot.items())}
        h["max_corrected_in_a_frame"] = max(spr[fr]["corrected"] for fr in frames)
        h["max_hud_in_a_frame"] = max(spr[fr]["hud"] for fr in frames)
    return h


def metrics_summary(path):
    s = {"stereo": 0, "flat": 0, "errors": 0, "flags": collections.Counter(), "flagged": [], "series": {}}
    vals = collections.defaultdict(list)
    if not os.path.exists(path):
        return s
    for line in open(path):
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if "error" in m:
            s["errors"] += 1
        if m.get("kind") == "stereo":
            s["stereo"] += 1
            for k in ("unmatched", "blkL", "blkR", "hud_cov", "hud_opaque", "flickL", "flickR", "chgL", "chgR", "meanL",
                      "meanR", "disp_med"):
                if k in m:
                    vals[k].append(m[k])
        else:
            s["flat"] += 1
        for fl in m.get("flags", []):
            s["flags"][fl] += 1
            if len(s["flagged"]) < 400:
                s["flagged"].append((m["seq"], fl))
    for k, v in vals.items():
        v = sorted(v)
        s["series"][k] = {"min": round(v[0], 4), "p50": round(v[len(v) // 2], 4), "p95": round(v[int(len(v) * 0.95)], 4),
                          "max": round(v[-1], 4)}
    s["flags"] = dict(s["flags"])
    return s


def font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def nearest_keep(keep, seq, after=2, span=12):
    have = sorted(int(n[1:7]) for n in os.listdir(keep) if n.endswith(".jpg"))
    for s in have:
        if seq + after <= s <= seq + span:
            return s
    later = [s for s in have if s >= seq]
    return later[0] if later else (have[-1] if have else None)


def montage(items, out, cols=2, w=720):
    """items: [(jpg path, caption)]"""
    if not items:
        return None
    ims = []
    for p, cap in items:
        im = Image.open(p).convert("RGB")
        im = im.resize((w, int(im.size[1] * w / im.size[0])))
        canvas = Image.new("RGB", (w, im.size[1] + 22), (20, 20, 20))
        canvas.paste(im, (0, 22))
        ImageDraw.Draw(canvas).text((6, 3), cap[:110], fill=(240, 220, 120), font=font(14))
        ims.append(canvas)
    rows = (len(ims) + cols - 1) // cols
    ch = max(i.size[1] for i in ims)
    sheet = Image.new("RGB", (cols * w, rows * ch), (0, 0, 0))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % cols) * w, (i // cols) * ch))
    sheet.save(out, quality=82)
    return out


def summarize(name):
    log = os.path.join(SWEEP, name + ".log")
    fr = os.path.join(SWEEP, "frames-" + name)
    keep = os.path.join(fr, "keep")
    scans, reads, prog, perf, spr, ftimes = scan_log(log)
    res = {"name": name, "scans": scans, "reads": reads, "heaviness": heaviness(prog, perf, spr),
           "metrics": metrics_summary(os.path.join(fr, "metrics.jsonl"))}
    evs = [json.loads(l) for l in open(os.path.join(fr, "events.jsonl"))] if os.path.exists(os.path.join(fr, "events.jsonl")) else []
    res["events"] = evs
    items = []
    for e in evs:
        s = nearest_keep(keep, e["seq"]) if os.path.isdir(keep) else None
        if s is not None:
            e["keep"] = os.path.join(keep, "k%06d.jpg" % s)
            items.append((e["keep"], "%s  (seq %d -> shown %d, stereo #%d)" % (e["label"], e["seq"], s, e["stereo_frames"])))
    pages = []
    for i in range(0, len(items), 16):
        p = montage(items[i:i + 16], os.path.join(fr, "events_montage%d.jpg" % (i // 16 + 1)))
        if p:
            pages.append(p)
    res["event_montages"] = pages
    fl = res["metrics"]["flagged"]
    seen, fitems = set(), []
    for seq, f in fl:
        p = os.path.join(keep, "k%06d.jpg" % seq)
        if os.path.exists(p) and seq not in seen and len(fitems) < 16:
            seen.add(seq)
            fitems.append((p, "seq %d: %s" % (seq, ",".join(x for s2, x in fl if s2 == seq))))
    res["flags_montage"] = montage(fitems, os.path.join(fr, "flags_montage.jpg"))
    res["frame_time_ms"] = {"n": len(ftimes), "max": max(ftimes) if ftimes else None}
    json.dump(res, open(os.path.join(fr, "summary.json"), "w"), indent=1, default=str)
    lines = ["== %s" % name]
    for k, v in scans.items():
        if v["n"]:
            lines.append("  %-13s %6d  e.g. %s" % (k, v["n"], v["ex"][0][:200]))
    lines.append("  stalls over after (s): %s" % scans["stall"].get("over_s"))
    lines.append("  reads %d, short %d, bad status %d %s" % (reads["n"], reads["short"], reads["bad_status"], reads["ex"][:2]))
    lines.append("  heaviness " + json.dumps(res["heaviness"]))
    m = res["metrics"]
    lines.append("  frames: %d stereo, %d flat, %d unreadable; flags %s" % (m["stereo"], m["flat"], m["errors"], m["flags"]))
    for k, v in m["series"].items():
        lines.append("    %-10s %s" % (k, v))
    lines.append("  events %d, montages %s, flags montage %s" % (len(evs), pages, res["flags_montage"]))
    open(os.path.join(fr, "summary.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    for n in sys.argv[1:]:
        summarize(n)
