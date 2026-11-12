#!/usr/bin/env python3
"""tests/game_render_x86: analyse frames/audio.wav (what the fake AAudio device in host_stubs.c recorded).

Checks the format (rate, channels, 16-bit PCM, as apu_aaudio.c opens the stream), levels (peak / RMS dBFS per channel
and per section), clipping, DC offset, L/R relationship, spectral roll-off, silence, and dropouts (runs of exact digital
zero inside otherwise sounding audio: what the production callback writes when the ring runs dry). Sections come from
the harness log (run.log: input presses, first stereo frame) if given. Writes <wav>.png: level over time with the
scripted presses marked, and a spectrogram.

Usage: analyse_audio.py <audio.wav> [run.log] [--png out.png]
"""
import argparse
import json
import os
import re
import sys

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as signal


def dbfs(v):
    return 20.0 * np.log10(np.maximum(v, 1e-9) / 32768.0)


def events_from_log(path, start_t):
    """(time on the WAV's clock, label) for the scripted presses and the first stereo frame."""
    ev = []
    if not path or not os.path.exists(path):
        return ev
    for line in open(path, errors="replace"):
        m = re.match(r"\[GRX\] t=([0-9.]+)s: input (\w+) pressed", line)
        if m:
            ev.append((float(m.group(1)) - start_t, m.group(2)))
        m = re.match(r"\[GRX\] t=([0-9.]+)s: present STEREO", line)
        if m and not any(l == "STEREO" for _, l in ev):
            ev.append((float(m.group(1)) - start_t, "STEREO"))
    return sorted(ev)


def runs(mask):
    """(start, length) of the True runs of a boolean array."""
    if not len(mask):
        return []
    d = np.diff(np.concatenate(([0], mask.astype(np.int8), [0])))
    s, e = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    return list(zip(s, e - s))


def section_stats(x, rate, t0, t1):
    a, b = int(max(0, t0) * rate), int(min(len(x) / rate, t1) * rate)
    seg = x[a:b]
    if len(seg) < rate // 10:
        return None
    w = rate // 20                                   # 50 ms windows
    n = len(seg) // w
    win = seg[: n * w].reshape(n, w, -1)
    wrms = np.sqrt((win ** 2).mean(axis=(1, 2)))
    return {
        "t0": round(t0, 1), "t1": round(min(len(x) / rate, t1), 1),
        "rms_dbfs": [round(float(dbfs(np.sqrt((seg[:, c] ** 2).mean()))), 1) for c in range(seg.shape[1])],
        "peak_dbfs": [round(float(dbfs(np.abs(seg[:, c]).max())), 1) for c in range(seg.shape[1])],
        "silent_pct": round(100.0 * float((dbfs(wrms) < -60).mean()), 1),
        "loud_p95_dbfs": round(float(dbfs(np.percentile(wrms, 95))), 1),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("log", nargs="?")
    ap.add_argument("--png")
    args = ap.parse_args()
    rate, raw = wavfile.read(args.wav)
    side = {}
    if os.path.exists(args.wav + ".json"):
        side = json.load(open(args.wav + ".json"))
    start_t = side.get("start_t", 0.0)
    if raw.ndim == 1:
        raw = raw[:, None]
    rep = {"rate": rate, "channels": raw.shape[1], "dtype": str(raw.dtype), "frames": int(raw.shape[0]),
           "seconds": round(raw.shape[0] / rate, 2), "device_start_t": start_t,
           "format_ok": rate == 48000 and raw.shape[1] == 2 and raw.dtype == np.int16}
    x = raw.astype(np.float64)
    nch = x.shape[1]
    rep["peak_dbfs"] = [round(float(dbfs(np.abs(x[:, c]).max())), 2) for c in range(nch)]
    rep["rms_dbfs"] = [round(float(dbfs(np.sqrt((x[:, c] ** 2).mean()))), 2) for c in range(nch)]
    rep["clipped_samples"] = [int(((raw[:, c] >= 32767) | (raw[:, c] <= -32768)).sum()) for c in range(nch)]
    sounding = np.abs(x).max(axis=1) > 0
    rep["dc_offset"] = [round(float(x[sounding, c].mean()), 2) if sounding.any() else 0.0 for c in range(nch)]
    rep["dc_offset_dbfs"] = [round(float(dbfs(abs(v))), 1) for v in rep["dc_offset"]]
    if nch == 2 and sounding.any():
        l, r = x[sounding, 0], x[sounding, 1]
        rep["lr_correlation"] = round(float(np.corrcoef(l, r)[0, 1]), 3) if l.std() > 0 and r.std() > 0 else None
        rep["lr_identical_pct"] = round(100.0 * float((raw[sounding, 0] == raw[sounding, 1]).mean()), 1)
    # spectrum of the sounding part: where 99% of the energy is below (a wrong rate shows as a wrong roll-off)
    if sounding.sum() > rate:
        f, p = signal.welch(x[sounding].mean(axis=1), fs=rate, nperseg=4096)
        c = np.cumsum(p) / p.sum()
        rep["rolloff99_hz"] = int(f[np.searchsorted(c, 0.99)])
        rep["rolloff999_hz"] = int(f[np.searchsorted(c, 0.999)])
        rep["centroid_hz"] = int((f * p).sum() / p.sum())
    # dropouts: exact-zero runs of >= 2 ms with sound within 100 ms on both sides
    z = np.abs(raw).max(axis=1) == 0
    gaps = []
    for s, n in runs(z):
        if n < rate // 500 or s == 0 or s + n >= len(z):
            continue
        a, b = max(0, s - rate // 10), min(len(z), s + n + rate // 10)
        if (~z[a:s]).sum() > rate // 50 and (~z[s + n:b]).sum() > rate // 50 and n < rate // 2:
            gaps.append((round(s / rate, 3), round(n * 1000.0 / rate, 1)))
    rep["dropouts"] = len(gaps)
    rep["dropout_examples"] = gaps[:12]
    # silence overall
    w = rate // 20
    nw = len(x) // w
    wrms = np.sqrt((x[: nw * w].reshape(nw, w, nch) ** 2).mean(axis=(1, 2)))
    rep["silent_windows_pct"] = round(100.0 * float((dbfs(wrms) < -60).mean()), 1)
    ev = events_from_log(args.log, start_t)
    rep["events"] = [(round(t, 2), l) for t, l in ev]
    # sections: title (before the first press), front end (presses to STEREO), level 1 (after)
    st = next((t for t, l in ev if l == "STEREO"), None)
    first_press = ev[0][0] if ev else None
    secs = {}
    if first_press is not None:
        secs["title"] = section_stats(x, rate, 0, first_press)
        secs["front_end"] = section_stats(x, rate, first_press, st if st is not None else 1e9)
    if st is not None:
        secs["level1"] = section_stats(x, rate, st, 1e9)
    # click around each press: peak in the 1.5 s after vs the 1.5 s before
    clicks = []
    for t, l in ev:
        if l == "STEREO":
            continue
        a, b, c = int(max(0, t - 1.5) * rate), int(t * rate), int((t + 1.5) * rate)
        if c <= len(x) and b > a:
            pre = float(np.sqrt((x[a:b] ** 2).mean()))
            post = x[b:c]
            k = rate // 50
            pk = max(float(np.sqrt((post[i:i + k] ** 2).mean())) for i in range(0, len(post) - k, k // 2))
            clicks.append({"t": round(t, 2), "button": l, "before_rms_dbfs": round(float(dbfs(pre)), 1),
                           "after_max20ms_rms_dbfs": round(float(dbfs(pk)), 1)})
    rep["sections"] = secs
    rep["presses"] = clicks
    print(json.dumps(rep, indent=1))

    png = args.png or os.path.splitext(args.wav)[0] + "_spectrogram.png"
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.colors import LinearSegmentedColormap
    ink, muted, grid, surf = "#1f2328", "#6e7781", "#d8dee4", "#ffffff"
    seq = LinearSegmentedColormap.from_list("seq", ["#ffffff", "#c6dbef", "#6baed6", "#2171b5", "#08306b"])
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(16, 7.5), sharex=True, gridspec_kw={"height_ratios": [1, 2.4]},
                                   facecolor=surf)
    hop = rate // 10
    nh = len(x) // hop
    t = (np.arange(nh) + 0.5) * hop / rate
    for c, (name, col) in enumerate((("left", "#2171b5"), ("right", "#d9822b"))[:nch]):
        lev = dbfs(np.sqrt((x[: nh * hop, c].reshape(nh, hop) ** 2).mean(axis=1)))
        ax1.plot(t, lev, lw=1.2, color=col, label=name)
    ax1.set_ylim(-80, 0)
    ax1.set_ylabel("RMS per 100 ms (dBFS)", color=ink)
    ax1.legend(loc="upper right", frameon=False, ncol=2)
    mono = x.mean(axis=1)
    f, tt, sx = signal.spectrogram(mono, fs=rate, nperseg=2048, noverlap=1024, scaling="spectrum")
    sdb = 10 * np.log10(np.maximum(sx, 1e-3) / 32768.0 ** 2)
    ax2.pcolormesh(tt, f / 1000.0, sdb, cmap=seq, vmin=-95, vmax=-25, shading="auto", rasterized=True)
    ax2.set_ylabel("frequency (kHz)", color=ink)
    ax2.set_xlabel("seconds since the audio device started (wall clock)", color=ink)
    for ax in (ax1, ax2):
        ax.set_facecolor(surf)
        for s in ax.spines.values():
            s.set_color(grid)
        ax.tick_params(colors=muted)
        for te, l in ev:
            ax.axvline(te, color=muted, lw=0.8, ls="--")
    ax1.grid(axis="y", color=grid, lw=0.6)
    for te, l in ev:
        ax1.text(te + 1, -6, l, fontsize=9, color=ink, va="top")
    ax1.set_title("BLACK headless run: what the game played (frames/audio.wav, 48 kHz stereo s16)", color=ink, loc="left")
    fig.tight_layout()
    fig.savefig(png, dpi=110)
    print("wrote", png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
