"""Compare two survey sweeps captured with the same gain (e.g. before/after moving the antenna).

Usage: compare.py DIR_A DIR_B --labels "At the PC" "On extension" --out compare.png
Only capture names present in both folders are compared. Levels are absolute dB (same gain).
"""
import argparse
import os
import re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from survey import KEEP, psd


def centre(name):
    return int(re.search(r"(\d{4})", name).group(1))


def stitch(paths):
    F, M, rows = [], [], []
    for path in sorted(paths, key=lambda p: centre(os.path.basename(p))):
        fc = centre(os.path.basename(path)) * 1e6
        f, mean, peak, _ = psd(path)
        keep = (np.abs(f) <= KEEP) & (np.abs(f) > 0.3e6)
        rows.append((os.path.basename(path), np.median(mean[keep]), np.percentile(mean[keep], 99), peak[keep].max()))
        lo = F[-1][-1] * 1e6 if F else -np.inf
        keep &= (fc + f) > lo
        F.append((fc + f[keep]) / 1e6)
        M.append(mean[keep])
    return np.concatenate(F), np.concatenate(M), rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir_a")
    ap.add_argument("dir_b")
    ap.add_argument("--labels", nargs=2, default=["A", "B"])
    ap.add_argument("--out", default="compare.png")
    args = ap.parse_args()
    names = sorted(set(n for n in os.listdir(args.dir_a) if n.endswith(".cs16"))
                   & set(n for n in os.listdir(args.dir_b) if n.endswith(".cs16")), key=centre)
    fa, ma, ra = stitch([os.path.join(args.dir_a, n) for n in names])
    fb, mb, rb = stitch([os.path.join(args.dir_b, n) for n in names])

    print(f"{'capture':12s} {'floor A':>8} {'floor B':>8} {'Δfloor':>7} {'p99 A':>7} {'p99 B':>7} {'peak A':>7} {'peak B':>7} {'Δpeak':>6}")
    for (n, fl_a, p_a, k_a), (_, fl_b, p_b, k_b) in zip(ra, rb):
        print(f"{n:12s} {fl_a:8.1f} {fl_b:8.1f} {fl_b - fl_a:+7.1f} {p_a:7.1f} {p_b:7.1f} {k_a:7.1f} {k_b:7.1f} {k_b - k_a:+6.1f}")

    surface, ink, ink2 = "#fcfcfb", "#0b0b0b", "#52514e"
    plt.rcParams.update({"font.size": 10, "text.color": ink, "axes.labelcolor": ink2,
                         "xtick.color": ink2, "ytick.color": ink2, "axes.edgecolor": "#d9d8d4"})
    fig, (ax, az) = plt.subplots(2, 1, figsize=(14, 8), facecolor=surface, gridspec_kw={"height_ratios": [1, 1]})
    for a in (ax, az):
        a.set_facecolor(surface)
        for s in ("top", "right"):
            a.spines[s].set_visible(False)
        a.grid(True, axis="y", color="#ecebe8", lw=0.8)
    for a, (lo, hi), title in ((ax, (fa[0], fa[-1]), "Full tuning range"), (az, (2395, 2490), "2.4 GHz Wi-Fi / Bluetooth band")):
        sa, sb = (fa >= lo) & (fa <= hi), (fb >= lo) & (fb <= hi)
        a.plot(fa[sa], ma[sa], color="#2a78d6", lw=1.2, label=args.labels[0])
        a.plot(fb[sb], mb[sb], color="#eb6834", lw=1.2, label=args.labels[1])
        a.set_xlim(lo, hi)
        a.set_ylabel("Average power (dB, fixed gain)")
        a.set_title(title, loc="left", fontsize=12)
        a.legend(loc="upper left", frameon=False)
    az.set_xlabel("Frequency (MHz)")
    fig.tight_layout()
    fig.savefig(args.out, dpi=130, facecolor=surface)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
