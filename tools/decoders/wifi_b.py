"""Decode 802.11b DSSS frames; --cck adds experimental 5.5/11 Mbps payloads.

Frames that fail the FCS are demodulated again with a RAKE despreader
(`ALTERNATIVES`); the first FCS-valid result is kept.

Usage: wifi_b.py capture.cs16 --center 2437 [--channel 5 6 7]
The Wi-Fi channel defaults to the one at the capture centre.
"""
import argparse
import zlib
import numpy as np
from scipy import signal
import wifi_cck
import dsss_recover

from cs16 import load, shift

SPC = 4          # samples per chip at 44 MSps
SPS = 11 * SPC   # samples per symbol
BARKER = np.array([1, -1, 1, 1, -1, 1, 1, 1, -1, -1, -1], dtype=np.float32)
SFD = [(0xF3A0 >> i) & 1 for i in range(16)]
SMOOTH_HALF = 4      # smooth_timing: blocks of 16 symbols summed on each side
SIGNALS = {0x0A: 1, 0x14: 2, 0x37: 5.5, 0x6E: 11}
SUBTYPES = {(0, 0): "assoc-req", (0, 1): "assoc-resp", (0, 4): "probe-req", (0, 5): "probe-resp",
            (0, 8): "beacon", (0, 10): "disassoc", (0, 11): "auth", (0, 12): "deauth", (0, 13): "action",
            (1, 11): "RTS", (1, 12): "CTS", (1, 13): "ACK", (1, 9): "block-ack",
            (2, 0): "data", (2, 4): "null", (2, 8): "qos-data", (2, 12): "qos-null"}


# 2.4 GHz channels, then the 5 GHz 20 MHz channels (5000 + 5n MHz).
CHANNELS = (*range(1, 15), *range(32, 69, 4), *range(96, 145, 4), *range(149, 178, 4))


def channel_mhz(ch):
    if ch not in CHANNELS:
        raise ValueError(f'Unknown 20 MHz Wi-Fi channel {ch}')
    return 2484 if ch == 14 else 2407 + 5 * ch if ch < 14 else 5000 + 5 * ch


def nearest_channel(center):
    return min(CHANNELS, key=lambda ch: abs(channel_mhz(ch) - center))


def bits_to_int(bits):
    return int(sum(int(b) << i for i, b in enumerate(bits)))


def to_bytes(bits):
    return bytes(bits_to_int(bits[k:k + 8]) for k in range(0, len(bits) - 7, 8))


def crc16_plcp(bits):
    crc = 0xFFFF
    for b in bits:
        fb = ((crc >> 15) & 1) ^ int(b)
        crc = (crc << 1) & 0xFFFF
        if fb:
            crc ^= 0x1021
    crc ^= 0xFFFF
    return [(crc >> (15 - i)) & 1 for i in range(16)]


def descramble(s):
    s = np.asarray(s, dtype=np.int8)
    d = s.copy()
    d[7:] = s[7:] ^ s[3:-4] ^ s[:-7]
    return d


def acquire_timing(c, start, span=32, margin=2.0, skip=16):
    """Move `start` to the Barker main peak if it sits on a sidelobe.

    The +/-2-sample tracker in `symbols` cannot leave a sidelobe (they are flat
    at about 1/11 of the peak), so a candidate phase chosen on one would hold
    there for the whole frame. Search the full symbol over `span` symbols from
    `skip` symbols in (sync candidates start 16 symbols before the detected
    sync) and jump only if the best offset is at least `margin` times stronger
    than the current one and despreads to sync structure (22 of 24 consecutive
    descrambled bits constant; the window may begin before the frame). On
    filtered noise a 1.5x margin still jumped 2% of the time and 2x never did
    (round six); the sync test keeps a weaker frame from being pulled onto a
    stronger overlapping one.
    """
    n = min(span + 8, (len(c) - start - SPS // 2 - 1) // SPS - skip)
    if n < 16 or start < SPS // 2:
        return start
    offsets = np.arange(-(SPS // 2), SPS // 2 + 1)
    centers = start + SPS * (skip + np.arange(n))
    rows = c[centers[:, None] + offsets]
    strength = np.abs(rows[8:]).sum(axis=0)
    best = int(np.argmax(strength))
    if best == SPS // 2 or strength[best] < margin * strength[SPS // 2]:
        return start
    sym = rows[:, best]
    raw = np.real(sym[1:] * np.conj(sym[:-1])) < 0
    plain = (raw[7:] ^ raw[3:-4] ^ raw[:-7]).astype(np.int32)
    counts = np.convolve(plain, np.ones(24, dtype=np.int32), 'valid')
    if not len(counts) or np.maximum(counts, 24 - counts).max() < 22:
        return start
    return start + int(offsets[best])


def _interp(c, p):
    """Catmull-Rom cubic interpolation of `c` at fractional sample positions `p`."""
    i = np.clip(np.floor(p).astype(np.int64), 1, len(c) - 3)
    t = (p - i).astype(np.float32)
    c0, c1, c2, c3 = c[i - 1], c[i], c[i + 1], c[i + 2]
    return c1 + .5 * t * (c2 - c0 + t * (2 * c0 - 5 * c1 + 4 * c2 - c3 + t * (3 * (c1 - c2) + c3 - c0)))


def smooth_timing(c, pos0, count, block=16, half=None, reach=10):
    """Fractional symbol positions from Barker energy smoothed over neighbouring blocks.

    Per block of `block` symbols, |c|^2 is summed at offsets -reach..reach from
    the nominal grid; those block energies are summed over +/-`half` blocks
    before the peak is chosen, so noise does not move the timing. The peak
    must stay within 2 samples of the previous block's (within 2 of the start
    for the first) and is refined with a parabola; positions between block
    centres are interpolated linearly. Returns None if `c` is too short.
    """
    half = SMOOTH_HALF if half is None else half
    n = min(count, (len(c) - pos0 - reach - 4) // SPS)
    if n < 1 or pos0 < reach + 2:
        return None
    grid = pos0 + SPS * np.arange(n)
    offs = np.arange(-reach, reach + 1)
    e = np.abs(c[grid[:, None] + offs]) ** 2
    nb = -(-n // block)
    e = np.vstack([e, np.zeros((nb * block - n, len(offs)), dtype=e.dtype)])
    blocks = e.reshape(nb, block, len(offs)).sum(1)
    sums = np.vstack([np.zeros((1, len(offs))), np.cumsum(blocks, axis=0)])
    lo, hi = np.clip(np.arange(nb) - half, 0, nb), np.clip(np.arange(nb) + half + 1, 0, nb)
    smooth = sums[hi] - sums[lo]
    tau, prev = np.empty(nb), 0
    for k in range(nb):
        a, b = max(1, prev + reach - 2), min(len(offs) - 2, prev + reach + 2)
        i = a + int(np.argmax(smooth[k, a:b + 1]))
        den = smooth[k, i - 1] - 2 * smooth[k, i] + smooth[k, i + 1]
        frac = .5 * (smooth[k, i - 1] - smooth[k, i + 1]) / den if den < 0 else 0.
        tau[k] = offs[i] + min(.5, max(-.5, frac))
        prev = int(offs[i])
    centres = block * np.arange(nb) + (block - 1) / 2
    return grid + np.interp(np.arange(n), centres, tau)


def symbols(c, start, count, positions=None, acquire=True, timing='track', combine=False):
    """Track Barker timing with a block average, avoiding noise-driven random walk.

    Clock drift is slow; a per-symbol maximum follows noise and sample blanking.
    Re-estimate the peak every 16 symbols using the surrounding correlation
    energy, then take one timing step. Maximum tracking rate exceeds 1000 ppm.
    Unless `acquire` is false, the start is first moved off any Barker
    sidelobe (`acquire_timing`).
    With `timing='smooth'`, positions come from `smooth_timing` instead and
    the correlation is interpolated between samples. With `combine`, each
    symbol is a `rake` combination of chip-spaced lags.
    If `positions` is a list, the sample index of each symbol is appended.
    """
    pos = acquire_timing(c, start) if acquire else start
    if timing == 'smooth':
        p = smooth_timing(c, pos, count)
        if p is not None:
            if positions is not None:
                positions.extend(np.rint(p).astype(int).tolist())
            return rake(c, p) if combine else _interp(c, p)
    out, track = [], []
    for index in range(count):
        if pos + 2 >= len(c):
            break
        if index % 16 == 0 and pos >= 2:
            centers = pos + SPS * np.arange(min(16, (len(c) - pos - 3) // SPS))
            if len(centers):
                offsets = np.arange(-2, 3)
                strength = np.abs(c[centers[:, None] + offsets]).sum(axis=0)
                step = int(np.argmax(strength)) - 2
                pos += max(-1, min(1, step))
        out.append(c[pos])
        track.append(pos)
        pos += SPS
    if positions is not None:
        positions.extend(track)
    return rake(c, track) if combine and track else np.array(out)


RAKE_LAGS = (-8, -4, 4, 8, 12)   # chip-spaced lags (44 MS/s samples) combined with the tracked one


def rake(c, positions):
    """Combine chip-spaced Barker correlation lags with per-frame channel weights.

    Some APs reach the receiver with a delayed path about one chip late at
    -6 to -9 dB (round seven, bench profile.py), energy the single-lag
    despreader loses. Each lag is weighted by its cross-correlation with the
    tracked lag over all symbols (the data and carrier cancel within a
    symbol), i.e. maximal-ratio combining. Barker sidelobes keep the noise
    at chip-spaced lags nearly uncorrelated. `positions` may be fractional.
    """
    p = np.asarray(positions, dtype=np.float64)
    lags = np.array((0,) + RAKE_LAGS)
    ok = (p + lags.min() >= 1) & (p + lags.max() < len(c) - 3)
    if ok.sum() < 16:
        return _interp(c, p)
    taps = np.stack([_interp(c, np.clip(p + l, 1, len(c) - 4)) for l in lags], axis=1)
    main = taps[ok, 0]
    w = (taps[ok].T @ np.conj(main)) / max(float(np.sum(np.abs(main) ** 2)), 1e-30)
    w[0] = 1
    return taps @ np.conj(w)


def parse_ies(b):
    out, i = [], 0
    while i + 2 <= len(b):
        eid, ln = b[i], b[i + 1]
        if i + 2 + ln > len(b):
            break
        v = b[i + 2:i + 2 + ln]
        if eid == 0:
            out.append(f"SSID={v.decode('utf-8', 'replace')!r}" if ln and any(v) else "SSID=<hidden>")
        elif eid == 1:
            out.append("rates=" + ",".join(f"{(r & 0x7f) / 2:g}{'*' if r & 0x80 else ''}" for r in v))
        elif eid == 3 and v:
            out.append(f"channel={v[0]}")
        elif eid == 7 and len(v) >= 2:
            out.append(f"country={v[:2].decode('ascii', 'replace')}")
        elif eid == 48:
            out.append("RSN")
        elif eid == 221 and len(v) >= 4 and v[:4] == b"\x00\x50\xf2\x01":
            out.append("WPA1")
        i += 2 + ln
    return out


def describe(f):
    if len(f) < 2:
        return ['truncated-MAC-header']
    fc0, fc1 = f[0], f[1]
    typ, sub = (fc0 >> 2) & 3, (fc0 >> 4) & 15
    parts = [SUBTYPES.get((typ, sub), f"type{typ}/sub{sub}") + (" [encrypted]" if fc1 & 0x40 else "")]
    to_ds, from_ds = fc1 & 1, (fc1 >> 1) & 1
    mac = lambda b: b.hex(":")
    if typ == 2 and from_ds and not to_ds and len(f) >= 22:
        parts += [f"to={mac(f[4:10])}", f"via-AP={mac(f[10:16])}", f"src={mac(f[16:22])}"]
    elif typ == 2 and to_ds and not from_ds and len(f) >= 22:
        parts += [f"to-AP={mac(f[4:10])}", f"src={mac(f[10:16])}", f"dst={mac(f[16:22])}"]
    else:
        if len(f) >= 10:
            parts.append(f"to={mac(f[4:10])}")
        if len(f) >= 16 and typ != 1:
            parts.append(f"from={mac(f[10:16])}")
        elif len(f) >= 16 and typ == 1 and sub in (8, 9, 10, 11):
            parts.append(f"from={mac(f[10:16])}")
        if len(f) >= 22 and typ != 1:
            parts.append(f"bssid={mac(f[16:22])}")
    if typ == 0 and sub in (8, 5) and len(f) > 36:
        parts.append(f"interval={int.from_bytes(f[32:34], 'little')}TU")
        parts += parse_ies(f[36:-4])
    elif typ == 0 and sub == 4 and len(f) > 24:
        parts += parse_ies(f[24:-4])
    return parts


def dqpsk_bits(sym, start, count):
    ph = sym[start:start + count + 1]
    q = np.mod(np.round(np.angle(ph[1:] * np.conj(ph[:-1])) / (np.pi / 2)), 4).astype(int)
    table = np.array([[0, 0], [0, 1], [1, 1], [1, 0]], dtype=np.int8)
    return table[q].ravel()


def correct_dsss_carrier(sym, phase=None):
    """Estimate differential carrier rotation from the DBPSK sync field.

    Squaring removes BPSK data modulation. Weight by amplitude so quiet samples
    preceding the packet do not dominate. Acquisition range is +/-250 kHz.
    """
    if phase is None:
        diff = sym[1:min(65, len(sym))] * np.conj(sym[:min(64, len(sym) - 1)])
        phase = float(.5 * np.angle(np.sum(diff ** 2 / np.maximum(np.abs(diff), 1e-20))))
    return sym * np.exp(-1j * phase * np.arange(len(sym))), phase


LEGACY = 'track'
# Demodulator modes: (timing, rake). Round seven: the RAKE fallbacks added 153
# FCS-valid frames and lost none over the 23-recording sub-0 dB benchmark.
MODES = {'track': ('track', False), 'smooth': ('smooth', False),
         'track_rake': ('track', True), 'smooth_rake': ('smooth', True)}
PRIMARY = LEGACY                         # demodulator tried first
ALTERNATIVES = 'track_rake,smooth_rake'  # tried in order, only while no FCS-valid result


def decode_ppdu(c, a, b, phase_hint=None, cck_samples=None, soft=None):
    """Find long/short preambles and decode payloads after a CRC-valid PLCP.

    If `soft` is a dict and the payload is 1 Mbps DBPSK, it receives the
    carrier-corrected symbols, their sample positions in `c`, the differential
    decisions and the payload offset, for `dsss_recover`.

    If timing acquisition moved the start and decoding then fails (no
    preamble, bad PLCP or bad FCS), the frame is decoded again from the
    unmoved start and that result is kept if its FCS passes. Smeared
    correlation peaks (frames decoded 10 MHz off-tune) can mislead
    acquisition, and the retry keeps what the unacquired path found.

    The `PRIMARY` demodulator runs first; each of `ALTERNATIVES` runs only
    while no FCS-valid result exists, and the first FCS-valid result is kept
    (`demod` names its mode). Otherwise the primary result is returned.
    """
    r = _decode_mode(c, a, b, phase_hint, cck_samples, soft, PRIMARY)
    if r is None or r.get('fcs_ok'):
        return r
    for mode in filter(None, ALTERNATIVES.split(',')):
        alt_soft = {} if soft is not None else None
        r2 = _decode_mode(c, a, b, phase_hint, cck_samples, alt_soft, mode)
        if r2 and r2.get('fcs_ok'):
            if soft is not None:
                soft.clear()
                soft.update(alt_soft)
            return r2
    return r


def _decode_mode(c, a, b, phase_hint, cck_samples, soft, mode):
    timing, combine = MODES[mode]
    r = _decode_ppdu(c, a, b, phase_hint, cck_samples, soft, True, timing, combine)
    phase = r.pop('_phase', None) if r else None
    if r is not None and mode != LEGACY:
        r['demod'] = mode
    if r is None or r.get('fcs_ok'):
        return r
    if phase is None:   # no preamble or truncated: phase is recomputed the same way
        phase = phase_hint if phase_hint is not None else _start_phase(c, a)
    if phase is None or acquire_timing(c, a + phase) == a + phase:
        return r
    retry_soft = {} if soft is not None else None
    r0 = _decode_ppdu(c, a, b, phase_hint, cck_samples, retry_soft, False, timing, combine)
    if r0:
        r0.pop('_phase', None)
        if mode != LEGACY:
            r0['demod'] = mode
    if r0 and r0.get('fcs_ok'):
        if soft is not None:
            soft.clear()
            soft.update(retry_soft)
        return r0
    return r


def _start_phase(c, a):
    """Symbol phase with the most Barker correlation energy in the first 120 us."""
    seg = np.abs(c[a:a + 120 * SPS])
    if len(seg) < 60 * SPS:
        return None
    return int(np.argmax(seg[: len(seg) // SPS * SPS].reshape(-1, SPS).sum(0)))


def _decode_ppdu(c, a, b, phase_hint, cck_samples, soft, acquire, timing='track', combine=False):
    if len(c[a:a + 120 * SPS]) < 60 * SPS:
        return None
    phase = _start_phase(c, a) if phase_hint is None else phase_hint
    # Reject false candidates after the header, without walking an entire burst.
    sym = symbols(c, a + phase, min(320, (b - a) // SPS), acquire=acquire, timing=timing, combine=combine)
    if len(sym) < 96:
        return None
    sym, carrier_phase = correct_dsss_carrier(sym)
    raw = (np.real(sym[1:] * np.conj(sym[:-1])) < 0).astype(np.int8)
    d = descramble(raw)
    text = ''.join(map(str, d[:300]))
    k = text.find(''.join(map(str, SFD)))
    short = False
    if k >= 0:
        h = d[k + 16:k + 64]
        start, history = k + 64, raw[k + 57:k + 64]
    else:
        k = text.find(''.join(str((0x05CF >> i) & 1) for i in range(16)))
        if k < 7:
            return {'error': 'no supported DSSS preamble'}
        short = True
        header_start = k + 16
        coded_header = dqpsk_bits(sym, header_start, 24)
        h = descramble(np.r_[raw[header_start - 7:header_start], coded_header])[7:]
        start, history = header_start + 24, coded_header[-7:]
    if len(h) != 48:
        return {'error': 'truncated PLCP'}
    sig, length = bits_to_int(h[:8]), bits_to_int(h[16:32])
    res = {"rate": SIGNALS.get(sig, f"0x{sig:02x}"), "length_us": length,
           'preamble': 'short' if short else 'long',
           'cfo_hz': carrier_phase * 1e6 / (2 * np.pi),
           "plcp_ok": sig in SIGNALS and (not short or sig != 0x0A) and
           list(h[32:48]) == crc16_plcp(h[:32])}
    res['_phase'] = phase
    if not res["plcp_ok"]:
        return res
    res['sfd_sample'] = a + phase + k * SPS
    res['service'] = bits_to_int(h[8:16])
    res['payload_sample'] = a + phase + (start + 1) * SPS
    res["end"] = a + phase + (start + length + 2) * SPS
    positions = [] if soft is not None and res['rate'] == 1 else None
    if res['rate'] in (1, 2) and (start + length + 1 > len(sym) or positions is not None):
        sym, _ = correct_dsss_carrier(
            symbols(c, a + phase, min(start + length + 2, (b - a) // SPS), positions, acquire, timing, combine),
            carrier_phase)
        raw = (np.real(sym[1:] * np.conj(sym[:-1])) < 0).astype(np.int8)
        d = descramble(raw)
    if res["rate"] == 1:
        pb = d[start:start + length]
        if positions is not None:
            soft.update(sym=sym, positions=np.array(positions), raw=raw, start=start, length=length)
    elif res["rate"] == 2:
        sc = np.r_[history, dqpsk_bits(sym, start, length)]
        pb = descramble(sc)[7:]
    elif cck_samples is not None and not h[11]:  # SERVICE bit 3 selects unsupported PBCC.
        octets = (length * 11 // (16 if res['rate'] == 5.5 else 8))
        if res['rate'] == 11:
            octets -= int(h[15])  # SERVICE length extension disambiguates byte count.
        coded = wifi_cck.decode(cck_samples, res['payload_sample'], res['rate'], octets,
                               carrier_phase, a + phase, sym[start])
        pb = descramble(np.r_[history, coded])[7:]
    else:
        res['payload_status'] = 'unsupported PBCC payload' if h[11] else 'unsupported CCK payload'
        return res
    f = to_bytes(pb)
    res["frame"] = f
    res["fcs_ok"] = len(f) > 4 and zlib.crc32(f[:-4]) == int.from_bytes(f[-4:], "little")
    return res


def detect_dsss_sync(c, chunk_symbols=8192):
    """Find scrambled sync structure after Barker despreading, below the power gate.

    Try all 44 sample phases in bounded chunks. A 32-symbol window must contain
    at least 29 decoded sync bits (ones for long, zeros for short). This is only
    a candidate test: PLCP CRC and MAC FCS still establish a valid packet.
    """
    total = len(c) // SPS
    last = -10000
    for base in range(0, total, chunk_symbols):
        lo, hi = max(0, base - 160), min(total, base + chunk_symbols + 160)
        rows = c[lo * SPS:hi * SPS].reshape(-1, SPS)
        if len(rows) < 48:
            continue
        raw = np.real(rows[1:] * np.conj(rows[:-1])) < 0
        plain = raw[7:] ^ raw[3:-4] ^ raw[:-7]
        sums = np.vstack([np.zeros((1, SPS), dtype=np.int32), np.cumsum(plain, axis=0, dtype=np.int32)])
        counts = sums[32:] - sums[:-32]
        scores = np.maximum(counts, 32 - counts)
        best = scores.max(axis=1)
        hot = best >= 29
        edges = np.flatnonzero(np.diff(np.r_[False, hot, False]))
        for a, b in zip(edges[::2], edges[1::2]):
            absolute = lo + a + 7
            if not base <= absolute < base + chunk_symbols or absolute - last < 32:
                continue
            # Within the first sync window, rank matching phases by correlation
            # energy as well, avoiding arbitrary ties on a broad sampling peak.
            score = scores[a].astype(float)
            energy = np.mean(np.abs(rows[a + 7:a + 39]) ** 2, axis=0)
            if energy.max() < 1e-18:
                continue
            phase = int(np.argmax(score + .1 * energy / max(float(energy.max()), 1e-20)))
            last = absolute
            pos = max(0, (absolute - 16) * SPS)
            yield pos, phase


def erased_fraction(zero44, positions):
    """Fraction of exact-zero input samples inside each symbol's Barker window."""
    csum = np.r_[0, np.cumsum(zero44, dtype=np.int64)]
    lo = np.clip(positions, 0, len(zero44))
    hi = np.clip(positions + SPS, 0, len(zero44))
    return (csum[hi] - csum[lo]) / SPS


def decode_channel(x, center, ch, threshold_db=6, min_us=60, packets=None, quiet=False, sync_search=True,
                   cck=False, recover=False):
    """Decode every 802.11b frame on one channel; returns {bssid: SSID field} for valid beacons.

    With `recover`, failed 1 Mbps payloads with a CRC-valid PLCP get a bounded
    FCS-constrained correction attempt (`dsss_recover`). Results are reported
    separately as `recovered_frame`; `fcs_ok` stays False for them.
    """
    off = (channel_mhz(ch) - center) * 1e6
    if abs(off) > 9e6:
        print(f"warning: channel {ch} is {off / 1e6:+.0f} MHz from centre; part of it is outside the capture")
    y = signal.resample_poly(shift(x, off), 11, 10).astype(np.complex64)
    y = signal.fftconvolve(y, signal.firwin(101, 10e6, fs=44e6).astype(np.float32), "same").astype(np.complex64)
    c = signal.fftconvolve(y, np.repeat(BARKER, SPC)[::-1], "full")[SPS - 1:].astype(np.complex64)
    zero44 = None
    if recover:
        zero40 = (x == 0)
        zero44 = zero40[np.minimum(np.arange(len(y)) * 10 // 11, len(zero40) - 1)]

    blk = 440  # 10 us
    p = (np.abs(y[: len(y) // blk * blk]) ** 2).reshape(-1, blk).mean(1)
    # A busy channel can put the median inside a packet. Estimate quiet power
    # from the lower fifth, excluding receiver-blanked blocks where possible.
    nonzero = p[p > 1e-16]
    floor = max(float(np.percentile(nonzero, 20)) if len(nonzero) else 0, 1e-20)
    idx = np.where(10 * np.log10(np.maximum(p, 1e-20) / floor) > threshold_db)[0]
    groups = np.split(idx, np.where(np.diff(idx) > 2)[0] + 1)
    bursts = [(g[0] * blk, (g[-1] + 1) * blk) for g in groups
              if len(g) and (g[-1] - g[0] + 1) * 10 >= min_us]
    if not quiet:
        print(f"channel {ch} ({channel_mhz(ch)} MHz): {len(bursts)} bursts >= {min_us:g} us; threshold {threshold_db:g} dB")

    # Structure-based candidates also reach packets too weak to cross the
    # broadband power threshold. Keep energy candidates for existing behavior.
    candidates = [(max(a - 2 * SPS, 0), b, None) for a, b in bursts]
    if sync_search:
        candidates.extend((a, len(c), phase) for a, phase in detect_dsss_sync(c))
    candidates.sort(key=lambda item: item[0])
    stats, nets, seen = {}, {}, []
    for a, b, phase in candidates:
        pos = a
        while pos < b - 60 * SPS:
            soft = {} if recover else None
            r = decode_ppdu(c, pos, b, phase, y if cck else None, soft)
            if r is None:
                break
            t = pos / 44e3
            if "error" in r:
                if pos <= a:
                    key = 'unrecognized burst (not identified as OFDM)'
                    stats[key] = stats.get(key, 0) + 1
                break
            pos = r.get("end", b)
            if not r["plcp_ok"]:
                stats["bad PLCP header"] = stats.get("bad PLCP header", 0) + 1
                continue
            if any(abs(r['sfd_sample'] - old) < 8 * SPS for old in seen):
                break
            if r.get('fcs_ok'):
                seen.append(r['sfd_sample'])
            key = f"{r['rate']} Mbps" + (" (FCS ok)" if r.get("fcs_ok") else "")
            stats[key] = stats.get(key, 0) + 1
            line = f"{t:9.3f} ms  rate={r['rate']}  {r['length_us']}us"
            f = r.get('frame', b'')
            if "frame" in r:
                fields = describe(f)
                line += f"  fcs={'OK ' if r['fcs_ok'] else 'BAD'}  {len(f):4d}B  " + " ".join(fields)
                ssid = next((x for x in fields if x.startswith("SSID=")), None)
                if ssid and r["fcs_ok"] and fields[0].startswith("beacon"):
                    nets.setdefault(f[16:22].hex(':'), ssid)
            recovered = None
            if recover and soft and 'frame' in r and not r['fcs_ok']:
                erased = erased_fraction(zero44, soft['positions'])
                fixed, info = dsss_recover.recover(r['frame'], soft['sym'], soft['start'], soft['length'], erased)
                span = erased[soft['start']:soft['start'] + soft['length'] + 1]
                info['erased_symbols'] = int((span > 0.5).sum())
                recovered = {'recovery': info} | ({'recovered_frame': fixed.hex()} if fixed is not None else {})
            if packets is not None:
                packets.append({'time_ms': float(t), 'channel': ch, 'rate_mbps': r['rate'],
                                    'sync_time_ms': r['sfd_sample'] / 44000,
                                    'length_us': r['length_us'],
                                    'payload_start_time_ms': r['payload_sample'] / 44000,
                                    'payload_end_time_ms': r['payload_sample'] / 44000 + r['length_us'] / 1000,
                                    'phy': 'DSSS', 'preamble': r['preamble'], 'demod': r.get('demod', LEGACY),
                                    'cfo_hz': r['cfo_hz'],
                                    'status': 'decoded' if 'frame' in r else r['payload_status'],
                                    'fcs_ok': bool(r.get('fcs_ok')), 'frame': f, **(recovered or {})})
            if not quiet:
                print(line)
    if not quiet:
        print("summary:", stats)
    if nets and not quiet:
        print("networks:", "; ".join(f"{s} ({b})" for b, s in nets.items()))
    return nets


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--center", type=float, required=True, help="capture centre frequency in MHz")
    ap.add_argument("--channel", type=int, nargs="+", choices=range(1, 15), metavar="N",
                    help="Wi-Fi channel(s) 1-14 to decode (default: the one at the centre)")
    ap.add_argument('--threshold-db', type=float, default=6, help='Burst power above lower-fifth floor (default: 6 dB)')
    ap.add_argument('--min-us', type=float, default=60, help='Minimum burst duration (default: 60 us)')
    ap.add_argument('--no-sync-search', action='store_true', help='Disable sensitivity fallback for comparison')
    ap.add_argument('--cck', action='store_true', help='Enable experimental CCK payload decoding')
    args = ap.parse_args()

    channels = args.channel or [nearest_channel(args.center)]
    if max(channels) > 14:
        ap.error('802.11b DSSS exists only at 2.4 GHz; decode 5 GHz captures with wifi.py')
    x = load(args.capture)
    for ch in channels:
        print()
        decode_channel(x, args.center, ch, args.threshold_db, args.min_us,
                       sync_search=not args.no_sync_search, cck=args.cck)


if __name__ == "__main__":
    main()
