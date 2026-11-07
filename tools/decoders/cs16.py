"""Shared helpers for BirriaSDR .cs16 captures (interleaved little-endian int16 I/Q, 14-bit, 40 MSps)."""
from pathlib import Path

import numpy as np

FS = 40e6
BLANK_RUN = 8    # zero runs at least this long are receiver blanking
BLANK_EDGE = 3   # samples after a blanking run that are still ramping back

# Single-chain captures: after each blanking run the receiver's DC removal is
# briefly off, so samples carry the raw offset on top of the intact signal: on
# the EDUP about 300 LSB for about 20 samples at 40 MSps, against 200 LSB rms,
# in the first data symbols of most OFDM frames. Frames arrive with random phase,
# so the mean over many runs is that offset alone and can be subtracted.
REPAIR_EDGES = True    # wifi.py --no-edge-repair turns this off for comparisons
EDGE_RUN = 40          # zero runs at least this long start a transient (49 at 40 and 80 MSps)
EDGE_SPAN = 64         # samples after a run examined for an offset
EDGE_PAD = EDGE_SPAN + 64  # read before a window, to recognise a run whose transient reaches into it
EDGE_MIN_RUNS = 32     # fewer runs can't separate the offset from the frames
EDGE_MAX_RUNS = 4000   # enough for about 4 LSB of noise on the mean
EDGE_SIGMA = 5         # subtract only where the mean clearly exceeds its own noise
_edge_profiles = {}


def _run_ends(x):
    """Index after the last zero of each run of at least EDGE_RUN exact zeros."""
    z = np.flatnonzero(np.diff(np.r_[0, (x == 0).astype(np.int8), 0]))
    return z[1::2][z[1::2] - z[::2] >= EDGE_RUN]


def edge_profile(path):
    """Mean offset at each position after a blanking run, zero where it's not significant.

    Measured once per file over its first EDGE_MAX_RUNS runs, so every window of a
    file gets the same correction. All zero without enough runs (5 GHz captures have
    none), or when nothing stands out of the frames, or on a file with no offset.
    """
    p = Path(path)
    key = (str(p), p.stat().st_size, p.stat().st_mtime_ns)
    if key in _edge_profiles:
        return _edge_profiles[key]
    total = p.stat().st_size // 4
    chunk = 4_000_000
    sums, powers, runs = np.zeros(EDGE_SPAN, complex), np.zeros(EDGE_SPAN), 0
    for at in range(0, total, chunk):
        raw = np.fromfile(p, dtype='<i2', count=2 * (chunk + EDGE_PAD), offset=4 * at).astype(np.float32)
        x = (raw[0::2] + 1j * raw[1::2]).astype(np.complex64) / 8192
        ends = _run_ends(x)
        ends = ends[(ends < chunk) & (ends + EDGE_SPAN <= len(x))]
        ends = ends[:EDGE_MAX_RUNS - runs]
        if len(ends):
            window = x[ends[:, None] + np.arange(EDGE_SPAN)]
            sums += window.sum(axis=0)
            powers += (np.abs(window) ** 2).sum(axis=0)
            runs += len(ends)
        if runs >= EDGE_MAX_RUNS:
            break
    profile = np.zeros(EDGE_SPAN, np.complex64)
    if runs >= EDGE_MIN_RUNS:
        mean = sums / runs
        noise = np.sqrt(powers / runs / runs)
        keep = np.abs(mean) >= EDGE_SIGMA * noise
        profile[keep] = mean[keep].astype(np.complex64)
    _edge_profiles[key] = profile
    return profile


def _repair_edges(path, x, start):
    """x holds samples from `start`; subtract the file's offset after each run that reaches them."""
    profile = edge_profile(path)
    if not profile.any():
        return x
    pad = min(start, EDGE_PAD)
    before = np.fromfile(path, dtype='<i2', count=2 * pad, offset=4 * (start - pad)).astype(np.float32)
    y = np.r_[(before[0::2] + 1j * before[1::2]).astype(np.complex64) / 8192, x]
    live = y != 0
    positions = _run_ends(y)[:, None] + np.nonzero(profile)[0]
    values = np.tile(profile[np.nonzero(profile)[0]], (positions.shape[0], 1))
    inside = positions < len(y)
    np.subtract.at(y, positions[inside], values[inside])
    y[~live] = 0
    return y[pad:]


def load(path, start=0, count=-1, dc_block=0, slot=0, of=1):
    """Return complex64 samples scaled to +/-1 full scale.

    dc_block > 0 subtracts the mean of each dc_block-sample block (BirriaSDR's
    --chain both slots carry a large static DC offset). The block grid is fixed
    to the file start, so every window of a file gets the same correction.
    Exact zeros (receiver blanking) stay zero and are left out of the means.
    The BLANK_EDGE samples after each blanking run are zeroed too: the
    receiver brings the whole raw sample, offset included, back on a ramp
    (about -0.05, 0.61, 0.09 of full value on the EDUP, in both slots), so
    subtracting the full offset there would leave a spike of about minus
    the offset (~300 LSB on chain 0) in the first data symbols of most frames.

    of > 1 reads slot `slot` of a recording whose I,Q pairs interleave `of`
    streams (birria_rx --chain both: of=2, slot 0 = chain 0, slot 1 = chain 1).
    `start`, `count` and `dc_block` then count that slot's samples, exactly as
    for a split copy of the slot.

    Single-chain recordings also get the offset after each blanking run removed
    (REPAIR_EDGES, edge_profile), so decodes differ from the samples as recorded
    only there. Both-chain slots carry their DC all the time and don't need it.
    """
    if dc_block > 0:
        total = Path(path).stat().st_size // 4 // of
        end = total if count < 0 else min(total, start + count)
        first = start // dc_block * dc_block
        # Load enough before the block to recognise a run whose edge falls inside it.
        pad = min(first, BLANK_RUN + BLANK_EDGE)
        x = load(path, first - pad, max(0, min(total, -(-end // dc_block) * dc_block) - first + pad),
                 slot=slot, of=of)
        z = np.flatnonzero(np.diff(np.r_[0, (x == 0).astype(np.int8), 0]))
        ends = z[1::2][z[1::2] - z[::2] >= BLANK_RUN]
        for k in range(BLANK_EDGE):
            x[ends[ends + k < len(x)] + k] = 0
        x = x[pad:]
        for s in range(0, len(x), dc_block):
            seg = x[s:s + dc_block]
            live = seg != 0
            n = np.count_nonzero(live)
            if n:
                seg[live] -= np.complex64(seg.sum(dtype=np.complex128) / n)
        return x[start - first:end - first]
    if of > 1:
        total = Path(path).stat().st_size // 4 // of
        n = max(0, total - start if count < 0 else min(count, total - start))
        raw = np.fromfile(path, dtype="<i2", count=2 * of * n, offset=4 * of * start)
        raw = raw.reshape(-1, of, 2)[:, slot].astype(np.float32)
        return (raw[:, 0] + 1j * raw[:, 1]).astype(np.complex64) / 8192
    raw = np.fromfile(path, dtype="<i2", count=-1 if count < 0 else 2 * count, offset=4 * start)
    raw = raw.astype(np.float32)
    x = (raw[0::2] + 1j * raw[1::2]).astype(np.complex64) / 8192
    return _repair_edges(path, x, start) if REPAIR_EDGES else x


def blanking_mask(x):
    """Exact-zero samples; long runs can also occur inside failed payloads."""
    return x == 0


def shift(x, offset_hz, chunk=4_000_000):
    """Move a signal at +offset_hz from the capture centre down to 0 Hz."""
    if offset_hz == 0:
        return x
    out = np.empty_like(x)
    for s in range(0, len(x), chunk):
        n = np.arange(s, min(s + chunk, len(x)), dtype=np.float64)
        out[s:s + len(n)] = x[s:s + len(n)] * np.exp(-2j * np.pi * offset_hz * n / FS).astype(np.complex64)
    return out
