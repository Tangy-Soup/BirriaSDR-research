"""Shared helpers for BirriaSDR .cs16 captures (interleaved little-endian int16 I/Q, 14-bit, 40 MSps)."""
from pathlib import Path

import numpy as np

FS = 40e6
BLANK_RUN = 8    # zero runs at least this long are receiver blanking
BLANK_EDGE = 3   # samples after a blanking run that are still ramping back


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
    return (raw[0::2] + 1j * raw[1::2]).astype(np.complex64) / 8192


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
