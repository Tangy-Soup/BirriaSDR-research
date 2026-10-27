"""Stitch several BirriaSDR captures into one band panorama and list the emissions found.

Usage: survey.py capture1.cs16 capture2.cs16 ... --out panorama.png
The centre frequency is read from the file name: the first 4-digit number is the centre in MHz
(e.g. s2174.cs16, w2417.cs16).
"""
import argparse
import re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from cs16 import FS, load

KEEP = 16e6     # use only the flat middle of each capture
NFFT = 1024     # ~39 kHz bins
BANDS = [  # (start MHz, end MHz, label) -- US allocations, for orientation only
    (2156, 2200, "AWS / LTE band 66 downlink"),
    (2200, 2290, "Space & aero telemetry (S-band)"),
    (2305, 2320, "WCS"),
    (2320, 2345, "SiriusXM satellite radio"),
    (2345, 2360, "WCS"),
    (2400, 2483.5, "2.4 GHz ISM: Wi-Fi, Bluetooth"),
    (2496, 2610, "LTE / 5G band 41 (TDD)"),
]


def psd(path):
    x = load(path)
    win = np.hanning(NFFT).astype(np.float32)
    seg = x[: len(x) // NFFT * NFFT].reshape(-1, NFFT)
    zero = (seg == 0).mean(1) > 0.01      # skip blocks touched by receiver blanking
    spec = np.abs(np.fft.fftshift(np.fft.fft(seg[~zero] * win, axis=1), axes=1)) ** 2 / (win ** 2).sum()
    f = np.fft.fftshift(np.fft.fftfreq(NFFT, 1 / FS))
    return f, 10 * np.log10(spec.mean(0)), 10 * np.log10(spec.max(0)), 10 * np.log10(np.percentile(spec, 90, axis=0))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--out", default="panorama.png")
    ap.add_argument("--threshold", type=float, default=6.0, help="dB above the noise floor to call an emission")
    args = ap.parse_args()

    F, M, P, Q = [], [], [], []
    for path in sorted(args.captures, key=lambda p: int(re.search(r"(\d{4})", p.split("/")[-1]).group(1))):
        fc = int(re.search(r"(\d{4})", path.split("/")[-1]).group(1)) * 1e6
        f, mean, peak, p90 = psd(path)
        keep = (np.abs(f) <= KEEP) & (np.abs(f) > 0.3e6)   # also drop the DC spur
        lo = F[-1][-1] * 1e6 if F else -np.inf
        keep &= (fc + f) > lo                            # overlapping captures: keep the earlier one
        F.append((fc + f[keep]) / 1e6); M.append(mean[keep]); P.append(peak[keep]); Q.append(p90[keep])
    f, mean, peak = np.concatenate(F), np.concatenate(M), np.concatenate(P)
    floor = np.median(mean)

    # emissions: contiguous runs of mean OR peak power above the floor
    hot = (mean > floor + args.threshold) | (peak > np.median(peak) + 2 * args.threshold)
    edges = np.flatnonzero(np.diff(np.r_[0, hot.astype(int), 0]))
    runs = [(a, b) for a, b in zip(edges[::2], edges[1::2])]
    merged = []
    for a, b in runs:
        if merged and f[a] - f[merged[-1][1] - 1] < 0.3:
            merged[-1] = (merged[-1][0], b)
        else:
            merged.append((a, b))
    print(f"noise floor {floor:.1f} dB; emissions more than {args.threshold:g} dB above it (mean) "
          f"or {2 * args.threshold:g} dB (peak hold):")
    print(f"{'start MHz':>10} {'end MHz':>9} {'width':>8} {'mean dB':>8} {'peak dB':>8}  band")
    for a, b in merged:
        lo_f, hi_f = f[a], f[b - 1]
        band = next((n for s, e, n in BANDS if s <= (lo_f + hi_f) / 2 <= e), "")
        print(f"{lo_f:10.2f} {hi_f:9.2f} {hi_f - lo_f + 0.04:7.2f}M {mean[a:b].max() - floor:8.1f} "
              f"{peak[a:b].max() - floor:8.1f}  {band}")

    surface, ink, ink2 = "#fcfcfb", "#0b0b0b", "#52514e"
    blue, orange = "#2a78d6", "#eb6834"
    plt.rcParams.update({"font.size": 10, "text.color": ink, "axes.labelcolor": ink2,
                         "xtick.color": ink2, "ytick.color": ink2, "axes.edgecolor": "#d9d8d4"})
    fig, ax = plt.subplots(figsize=(14, 5.5), facecolor=surface)
    ax.set_facecolor(surface)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ytop = peak.max() - floor + 6
    for i, (s, e, n) in enumerate(BANDS):
        ax.axvspan(s, e, color="#f0efec", lw=0, zorder=0)
        ax.text((s + e) / 2, ytop - (3 if i % 2 else 0), n, ha="center", va="top", fontsize=8, color=ink2)
    ax.plot(f, peak - floor, color=orange, lw=1, label="Peak hold")
    ax.plot(f, mean - floor, color=blue, lw=1.5, label="Average")
    ax.text(f[-1], (peak - floor)[-200:].max() + 1, "peak hold", color=ink2, ha="right", fontsize=9)
    ax.set_xlim(f[0], f[-1])
    ax.set_ylim(-5, ytop)
    ax.grid(True, axis="y", color="#ecebe8", lw=0.8)
    ax.set_xlabel("Frequency (MHz)")
    ax.set_ylabel("dB above noise floor")
    ax.set_title(f"Band survey {f[0]:.0f}–{f[-1]:.0f} MHz (EDUP MT7921AU via BirriaSDR, manual gain)", loc="left", fontsize=12)
    ax.legend(loc="upper left", frameon=False, bbox_to_anchor=(0, 0.93))
    fig.tight_layout()
    fig.savefig(args.out, dpi=130, facecolor=surface)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
