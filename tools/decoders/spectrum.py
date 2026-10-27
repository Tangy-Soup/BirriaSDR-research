"""Plot the average spectrum and a spectrogram of a BirriaSDR capture.

Usage: spectrum.py capture.cs16 --center 2437 [--out plot.png] [--title text]
"""
import argparse
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("capture")
ap.add_argument("--center", type=float, required=True, help="capture centre frequency in MHz")
ap.add_argument("--out", help="PNG path (default: capture name with .png)")
ap.add_argument("--title", default="")
args = ap.parse_args()
path, out = args.capture, args.out or args.capture.rsplit(".", 1)[0] + ".png"
fs, fc = 40e6, args.center * 1e6
raw = np.fromfile(path, dtype="<i2").astype(np.float32)
i, q = raw[0::2], raw[1::2]
x = (i + 1j * q) / 8192.0
n = len(x)

print(f"samples: {n}  duration: {n / fs * 1e3:.2f} ms")
print(f"I range: {i.min():.0f}..{i.max():.0f}  Q range: {q.min():.0f}..{q.max():.0f}")
clip = np.mean((np.abs(i) >= 8191) | (np.abs(q) >= 8191))
print(f"clipped fraction: {clip:.2e}")
print(f"all-zero samples: {np.mean((i == 0) & (q == 0)):.2e}")
print(f"DC offset: I={i.mean():.1f} Q={q.mean():.1f} LSB")
print(f"RMS: I={i.std():.1f} Q={q.std():.1f} LSB  (I/Q gain ratio {i.std() / q.std():.3f})")
p = np.abs(x) ** 2
print(f"mean power: {10 * np.log10(p.mean()):.1f} dBFS  peak: {10 * np.log10(p.max()):.1f} dBFS")

# Welch PSD
nfft = 4096
win = np.hanning(nfft)
segs = x[: (n // nfft) * nfft].reshape(-1, nfft)
spec = np.abs(np.fft.fftshift(np.fft.fft(segs * win, axis=1), axes=1)) ** 2
spec /= (win ** 2).sum() * nfft
psd = 10 * np.log10(spec.mean(axis=0) + 1e-20)
f_mhz = (fc + np.fft.fftshift(np.fft.fftfreq(nfft, 1 / fs))) / 1e6
edge = np.abs(f_mhz - fc / 1e6) > 18
print(f"PSD median in-band: {np.median(psd[~edge]):.1f} dB  max: {psd.max():.1f} dB at {f_mhz[psd.argmax()]:.2f} MHz")

# Spectrogram: 256-point FFTs averaged over 4 to keep the image readable
sn = 256
sw = np.hanning(sn)
rows = x[: (n // sn) * sn].reshape(-1, sn)
sg = np.abs(np.fft.fftshift(np.fft.fft(rows * sw, axis=1), axes=1)) ** 2
sg = sg[: (len(sg) // 4) * 4].reshape(-1, 4, sn).mean(axis=1)
sg = 10 * np.log10(sg + 1e-20)

surface, ink, ink2, blue = "#fcfcfb", "#0b0b0b", "#52514e", "#2a78d6"
ramp = LinearSegmentedColormap.from_list("blue", ["#fcfcfb", "#cde2fb", "#86b6ef", "#2a78d6", "#1c5cab", "#0d366b"])
plt.rcParams.update({"font.size": 10, "text.color": ink, "axes.labelcolor": ink2,
                     "xtick.color": ink2, "ytick.color": ink2, "axes.edgecolor": "#d9d8d4"})
title = f", {args.title}" if args.title else ""
fig, (a1, a2) = plt.subplots(2, 1, figsize=(10, 8), facecolor=surface,
                             gridspec_kw={"height_ratios": [1, 1.4]})
for a in (a1, a2):
    a.set_facecolor(surface)
    for s in ("top", "right"):
        a.spines[s].set_visible(False)

a1.plot(f_mhz, psd, color=blue, lw=1.5)
a1.grid(True, color="#ecebe8", lw=0.8)
a1.set_xlim(f_mhz[0], f_mhz[-1])
a1.set_ylabel("Power (dB, relative)")
a1.set_title(f"Average spectrum, {fc / 1e6:g} MHz centre, 40 MSps{title}", loc="left", fontsize=12)
a1.axvspan(f_mhz[0], fc / 1e6 - 18, color="#f0efec", lw=0)
a1.axvspan(fc / 1e6 + 18, f_mhz[-1], color="#f0efec", lw=0)
a1.text(fc / 1e6 + 18.3, a1.get_ylim()[1], "filter rolloff", color=ink2, va="top", fontsize=9)

t_ms = len(sg) * 4 * sn / fs * 1e3
lo, hi = np.percentile(sg, [5, 99.9])
im = a2.imshow(sg, aspect="auto", origin="lower", cmap=ramp, vmin=lo, vmax=hi,
               extent=[f_mhz[0], f_mhz[-1], 0, t_ms])
a2.set_xlabel("Frequency (MHz)")
a2.set_ylabel("Time (ms)")
a2.set_title("Spectrogram (darker = stronger)", loc="left", fontsize=12)
cb = fig.colorbar(im, ax=a2, pad=0.01)
cb.set_label("dB, relative")
cb.outline.set_visible(False)
fig.tight_layout()
fig.savefig(out, dpi=130, facecolor=surface)
