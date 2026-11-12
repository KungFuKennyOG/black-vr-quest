#!/usr/bin/env python3
"""tests/game_render_x86: frames/contact.png -- the key captures of a run side by side, labelled.

Reads <frames>/index.tsv (written by game_render_x86) and picks, in order: the first flat (menu) frame, the frame
before every scripted press, the first stereo frame (both eyes + HUD) and the final capture. Usage:
    contact_sheet.py <frames dir> [max tiles]
"""
import csv
import os
import sys

from PIL import Image, ImageDraw

TILE_W, TILE_H = 320, 240


def tile(path, label):
    im = Image.open(path)
    if im.mode == "RGBA":                       # the HUD layer: premultiplied, show over a checker
        bg = Image.new("RGB", im.size, (40, 40, 40))
        d = ImageDraw.Draw(bg)
        for y in range(0, im.size[1], 32):
            for x in range(0, im.size[0], 32):
                if (x // 32 + y // 32) % 2:
                    d.rectangle([x, y, x + 31, y + 31], fill=(70, 70, 70))
        bg.paste(im.convert("RGB"), (0, 0), im.split()[3])
        im = bg
    im = im.convert("RGB")
    im.thumbnail((TILE_W, TILE_H - 18), Image.LANCZOS)
    t = Image.new("RGB", (TILE_W, TILE_H), (16, 16, 16))
    t.paste(im, ((TILE_W - im.size[0]) // 2, 0))
    ImageDraw.Draw(t).text((4, TILE_H - 15), label[:52], fill=(230, 230, 230))
    return t


def main():
    frames = sys.argv[1]
    max_tiles = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    rows = list(csv.DictReader(open(os.path.join(frames, "index.tsv")), delimiter="\t"))
    picks, seen_seq = [], set()

    def add(row):
        key = (row["seq"], row["kind"])
        if key not in seen_seq and os.path.exists(row["path"]):
            seen_seq.add(key)
            picks.append(row)

    flats = [r for r in rows if r["kind"] == "flat"]
    if flats:
        add(flats[0])
    for r in rows:
        if r["tag"].startswith("before-"):
            add(r)
    stereo_seqs = [r["seq"] for r in rows if r["kind"] in ("eyeL", "eyeR", "hud")]
    if stereo_seqs:
        first = stereo_seqs[0]
        for r in rows:
            if r["seq"] == first:
                add(r)
        hud = [r for r in rows if r["kind"] == "hud"]
        if hud and hud[-1]["seq"] != first:
            for r in rows:
                if r["seq"] == hud[-1]["seq"]:
                    add(r)
    for r in rows:
        if r["tag"] == "final":
            add(r)
    picks = picks[-max_tiles:] if len(picks) > max_tiles else picks
    if not picks:
        print("contact_sheet: no captures")
        return 1
    cols = 4
    rows_n = (len(picks) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * TILE_W, rows_n * TILE_H), (0, 0, 0))
    for i, r in enumerate(picks):
        label = "#%s %s %s t=%ss" % (r["seq"], r["tag"], r["kind"], r["t_s"])
        sheet.paste(tile(r["path"], label), ((i % cols) * TILE_W, (i // cols) * TILE_H))
    out = os.path.join(frames, "contact.png")
    sheet.save(out)
    print("contact_sheet: %s (%d tiles)" % (out, len(picks)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
