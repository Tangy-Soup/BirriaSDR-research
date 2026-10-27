"""Two-chain combining for BirriaSDR --chain both recordings (two interleaved slots).

The two receive chains do not see independent noise: in round thirteen's
recordings the in-band noise of WF0 and WF1 correlated at |rho| 0.45-0.57,
and slot 1 lagged slot 0 by a fraction of a sample that changes per capture
(0 to 0.9 samples). Plain maximal-ratio combining then does worse than the
better chain alone, so the weights here whiten the noise first:

  w = Rn^-1 h,  normalized to w^H Rn w = 1,  output = w^H x * sqrt(Rn00)

Rn is the 2x2 noise covariance (the quietest fifth of 10 us blocks of the
file), and h is the principal direction of the signal covariance (block
covariance minus Rn) over +/-HALF blocks, with its chain-0 component made
real so the output keeps chain 0's phase. Blocks without a signal estimate
fall back to chain 0. Weights are interpolated between block centres. All
statistics come from the band around the decoded channel; the combined
stream carries the full band at chain 0's noise level.

The statistics are computed in one pre-pass over the whole file, on a block
grid fixed to the file start, so every decode window of a file sees the same
weights. The slot-1 delay is estimated once from the strongest bursts that
both chains receive well; without two such bursts it is zero.
"""
from pathlib import Path

import numpy as np
from scipy import signal

from cs16 import FS, load, shift

BLOCK = 400                    # 10 us statistics blocks
HALF = 5                       # signal covariance over +/-5 blocks (110 us)
CHUNK = 4_000_000              # pre-pass chunk, a multiple of BLOCK
MARGIN = 256                   # filter context loaded around every chunk
DELAYS = np.round(np.arange(-1.5, 1.501, .125), 3)
LOWPASS = signal.firwin(129, 10e6, fs=FS).astype(np.float32)


def fractional_delay(x, d, taps=48):
    """Delay x by d samples (|d| <= 2) with a Kaiser-windowed sinc; same length."""
    if abs(d) < 1e-9:
        return x
    n = np.arange(-taps, taps + 1)
    h = np.sinc(n - d) * np.kaiser(len(n), 8)
    return signal.fftconvolve(x, (h / h.sum()).astype(np.float32), 'same').astype(np.complex64)


def _smooth(v, half):
    c = np.r_[0, np.cumsum(v)]
    i = np.arange(len(v))
    lo, hi = np.clip(i - half, 0, len(v)), np.clip(i + half + 1, 0, len(v))
    return (c[hi] - c[lo]) / (hi - lo)


def whitened_weights(r00, r11, r01, noise):
    """Per-block (w0, w1) with w^H Rn w = 1; R01 and Rn01 are E[x0 x1*]."""
    n00, n11, n01 = noise
    s00, s11, s01 = r00 - n00, r11 - n11, r01 - n01
    lam = (s00 + s11) / 2 + np.sqrt((s00 - s11) ** 2 / 4 + np.abs(s01) ** 2)
    use_row = np.abs(lam - s11) > 1e-30
    h0 = np.where(use_row, lam - s11, s01)
    h1 = np.where(use_row, np.conj(s01), lam - s00)
    anchor = np.exp(-1j * np.angle(h0))
    h0, h1 = h0 * anchor, h1 * anchor
    det = n00 * n11 - abs(n01) ** 2
    w0 = (n11 * h0 - n01 * h1) / det
    w1 = (n00 * h1 - np.conj(n01) * h0) / det
    q = np.abs(w0) ** 2 * n00 + np.abs(w1) ** 2 * n11 + 2 * np.real(np.conj(w0) * n01 * w1)
    fallback = (lam <= 0) | ~(q > 1e-30)
    q = np.where(fallback, 1, q)
    return (np.where(fallback, 1 / np.sqrt(n00), w0 / np.sqrt(q)),
            np.where(fallback, 0, w1 / np.sqrt(q)))


class Combiner:
    """Whitened two-chain combination of one interleaved recording around one channel."""

    def __init__(self, path, offset_hz, dc_block):
        self.path, self.offset_hz, self.dc_block = Path(path), offset_hz, dc_block
        self.total = self.path.stat().st_size // 8
        r00, r11, _, live = self._block_stats(0.0)
        self.delay, self.delay_bursts = self._estimate_delay(r00, r11, live)
        r00, r11, r01, live = self._block_stats(self.delay)
        info = {'delay_samples': self.delay, 'delay_bursts': self.delay_bursts,
                'statistics_blocks': len(r00), 'live_blocks': int(live.sum())}
        self.usable = False
        if live.sum() >= 10:
            tot = r00 + r11
            quiet = live & (tot <= np.percentile(tot[live], 20))
            noise = (r00[quiet].mean(), r11[quiet].mean(), r01[quiet].mean())
            det = noise[0] * noise[1] - abs(noise[2]) ** 2
            info |= {'quiet_blocks': int(quiet.sum()),
                     'noise_dbfs': [float(10 * np.log10(max(noise[0], 1e-30))),
                                    float(10 * np.log10(max(noise[1], 1e-30)))],
                     'noise_correlation': float(abs(noise[2]) / np.sqrt(max(noise[0] * noise[1], 1e-60)))}
            self.usable = bool(min(noise[0], noise[1]) > 1e-16 and det > 1e-6 * noise[0] * noise[1])
        if self.usable:
            w0, w1 = whitened_weights(_smooth(r00, HALF), _smooth(r11, HALF), _smooth(r01, HALF), noise)
            scale = np.sqrt(noise[0])
            self.w0, self.w1 = (w0 * scale).astype(np.complex64), (w1 * scale).astype(np.complex64)
            info['chain0_fallback_blocks'] = int(np.count_nonzero(w1 == 0))
        info['status'] = 'combined' if self.usable else 'not combined: a slot has no usable noise estimate'
        self.info = info

    def _band_slots(self, start, end, delay):
        """Band-limited slots for [start, end), slot 1 delayed, plus the raw live mask."""
        lo, hi = max(0, start - MARGIN), min(self.total, end + MARGIN)
        x0 = load(self.path, lo, hi - lo, self.dc_block, slot=0, of=2)
        x1 = load(self.path, lo, hi - lo, self.dc_block, slot=1, of=2)
        live = (x0 != 0) & (x1 != 0)
        y0, y1 = (signal.fftconvolve(shift(x, self.offset_hz), LOWPASS, 'same').astype(np.complex64)
                  for x in (x0, x1))
        y1 = fractional_delay(y1, delay)
        a, b = start - lo, end - lo
        return y0[a:b], y1[a:b], live[a:b]

    def _block_stats(self, delay):
        nb = self.total // BLOCK
        r00, r11 = np.zeros(nb), np.zeros(nb)
        r01, live = np.zeros(nb, np.complex128), np.zeros(nb, bool)
        for s in range(0, nb * BLOCK, CHUNK):
            e = min(nb * BLOCK, s + CHUNK)
            y0, y1, ok = self._band_slots(s, e, delay)
            k = slice(s // BLOCK, e // BLOCK)
            a, b = y0.reshape(-1, BLOCK), y1.reshape(-1, BLOCK)
            r00[k], r11[k] = (np.abs(a) ** 2).mean(1), (np.abs(b) ** 2).mean(1)
            r01[k] = (a * np.conj(b)).mean(1, dtype=np.complex128)
            live[k] = ok.reshape(-1, BLOCK).all(1)
        return r00, r11, r01, live

    def _estimate_delay(self, r00, r11, live):
        """Slot-1 delay maximizing the median coherence of the strongest two-chain bursts."""
        if live.sum() < 10:
            return 0.0, 0
        n0, n1 = np.percentile(r00[live], 20), np.percentile(r11[live], 20)
        busy = live & (r00 > 30 * n0) & (r11 > 30 * n1)
        edges = np.flatnonzero(np.diff(np.r_[0, busy.astype(np.int8), 0]))
        runs = [(s, min(t, s + 80)) for s, t in zip(edges[::2], edges[1::2]) if t - s >= 4]
        runs = sorted(runs, key=lambda r: -r11[r[0]:r[1]].mean())[:10]
        if len(runs) < 2:
            return 0.0, len(runs)
        table = np.zeros((len(runs), len(DELAYS)))
        for i, (s, t) in enumerate(runs):
            lo = max(0, s * BLOCK - 128)
            y0, y1, _ = self._band_slots(lo, min(self.total, t * BLOCK + 128), 0.0)
            core = slice(s * BLOCK - lo, t * BLOCK - lo)
            a = y0[core]
            for j, d in enumerate(DELAYS):
                c = fractional_delay(y1, d)[core]
                table[i, j] = abs(np.vdot(a, c)) ** 2 / max(np.vdot(a, a).real * np.vdot(c, c).real, 1e-60)
        return float(DELAYS[int(np.argmax(np.median(table, 0)))]), len(runs)

    def samples(self, start, count):
        """Combined complex64 stream for [start, start + count); exact zeros where both slots are zero."""
        end = min(self.total, start + count)
        lo, hi = max(0, start - MARGIN), min(self.total, end + MARGIN)
        x0 = load(self.path, lo, hi - lo, self.dc_block, slot=0, of=2)
        x1 = load(self.path, lo, hi - lo, self.dc_block, slot=1, of=2)
        x1d = fractional_delay(x1, self.delay)
        a, b = start - lo, end - lo
        x0, x1, x1d = x0[a:b], x1[a:b], x1d[a:b]
        if not self.usable:
            return x0
        t = np.arange(start, end, dtype=np.float64)
        centres = BLOCK * np.arange(len(self.w0)) + (BLOCK - 1) / 2
        w0 = np.interp(t, centres, self.w0.real) + 1j * np.interp(t, centres, self.w0.imag)
        w1 = np.interp(t, centres, self.w1.real) + 1j * np.interp(t, centres, self.w1.imag)
        out = (np.conj(w0) * x0 + np.conj(w1) * x1d).astype(np.complex64)
        out[(x0 == 0) & (x1 == 0)] = 0
        return out
