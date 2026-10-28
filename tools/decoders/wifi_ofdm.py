"""Offline 20 MHz legacy 802.11 OFDM receiver for BirriaSDR 40 MSps I/Q.

Original NumPy/SciPy implementation of legacy PHY processing. Training symbols,
pilot sequence, coding and interleaving are IEEE 802.11 protocol constants.
Only MAC frames with a correct FCS are accepted as identified Wi-Fi traffic.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import zlib

import numpy as np
from scipy import signal

from cs16 import load, shift
from wifi_b import CHANNELS, channel_mhz, describe, nearest_channel

FS = 20_000_000
ACTIVE = np.r_[np.arange(-26, 0), np.arange(1, 27)]
PILOTS = np.array([-21, -7, 7, 21])
DATA = np.array([k for k in ACTIVE if k not in PILOTS])
# L-LTF, in order -26..26 including DC.
LONG = np.array([
    1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1,
    -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 0, 1, -1, -1, 1, 1,
    -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1,
    -1, 1, -1, 1, 1, 1, 1], dtype=np.float32)
LTF_BINS = np.zeros(64, dtype=np.complex64)
LTF_BINS[np.arange(-26, 27) % 64] = LONG
LTF = np.fft.ifft(LTF_BINS).astype(np.complex64)
# RATE field interpreted LSB first: (Mbps, coded bits per carrier, data bits/symbol).
RATES = {11: (6, 1, 24), 15: (9, 1, 36), 10: (12, 2, 48),
         14: (18, 2, 72), 9: (24, 4, 96), 13: (36, 4, 144),
         8: (48, 6, 192), 12: (54, 6, 216)}
PILOT_SIGNS = np.array([1, 1, 1, -1])
# Kept mother-code bits per puncturing period, in A0 B0 A1 B1 ... order.
PUNCTURE = {'1/2': [1, 1], '2/3': [1, 1, 1, 0], '3/4': [1, 1, 1, 0, 0, 1],
            '5/6': [1, 1, 1, 0, 0, 1, 1, 0, 0, 1]}

# HT-mixed (802.11n) 20 MHz, one spatial stream: 56 active subcarriers.
HT_ACTIVE = np.r_[np.arange(-28, 0), np.arange(1, 29)]
HT_DATA = np.array([k for k in HT_ACTIVE if k not in PILOTS])
HT_LTF_BINS = np.zeros(64, dtype=np.complex64)
HT_LTF_BINS[np.arange(-28, 29) % 64] = np.r_[1, 1, LONG, -1, -1]
# MCS: (Mbps at 800 ns GI, coded bits per carrier, code rate, data bits/symbol).
HT_MCS = {0: (6.5, 1, '1/2', 26), 1: (13, 2, '1/2', 52), 2: (19.5, 2, '3/4', 78),
          3: (26, 4, '1/2', 104), 4: (39, 4, '3/4', 156), 5: (52, 6, '2/3', 208),
          6: (58.5, 6, '3/4', 234), 7: (65, 6, '5/6', 260)}


def pilot_sequence():
    state, result = 127, []
    for _ in range(127):
        feedback = ((state >> 6) ^ (state >> 3)) & 1
        result.append(1 - 2 * feedback)
        state = ((state << 1) | feedback) & 127
    return np.array(result)


POLARITY = pilot_sequence()


def window_sum(x, width):
    total = np.r_[0, np.cumsum(x, dtype=np.complex128 if np.iscomplexobj(x) else np.float64)]
    return total[width:] - total[:-width]


def detect_stf(x, threshold=0.65, chunk=1_000_000):
    """Find sustained 0.8 us repetition, without an absolute power threshold.

    Use local sums in bounded chunks so memory and numerical drift stay bounded.
    Each chunk owns candidate starts in its core; overlap covers STF boundaries.
    Return (sample index at 20 MSps, coarse CFO in radians/sample).
    """
    last = -1000
    for base in range(0, len(x), chunk):
        start = max(0, base - 256)
        z = x[start:min(len(x), base + chunk + 512)]
        if len(z) < 128:
            continue
        prod = np.conj(z[:-16]) * z[16:]
        cross = window_sum(prod, 64)
        power = np.abs(z) ** 2
        p0, p1 = window_sum(power[:-16], 64), window_sum(power[16:], 64)
        metric = np.abs(cross) / np.maximum(np.sqrt(np.maximum(p0 * p1, 0)), 1e-20)
        hot = metric >= threshold
        edges = np.flatnonzero(np.diff(np.r_[False, hot, False]))
        for a, b in zip(edges[::2], edges[1::2]):
            if b - a < 24:
                continue
            absolute = start + a
            if not base <= absolute < base + chunk or absolute - last < 80:
                continue
            best = a + int(np.argmax(metric[a:min(b, a + 96)]))
            last = absolute
            yield absolute, float(np.angle(cross[best]) / 16)


def deinterleave(values, bpsc, columns=16, carriers=48):
    """Legacy interleaver by default; HT 20 MHz uses 13 columns, 52 carriers."""
    n = carriers * bpsc
    k = np.arange(n)
    first = (n // columns) * (k % columns) + k // columns
    s = max(bpsc // 2, 1)
    second = s * (first // s) + (first + n - (columns * first) // n) % s
    return np.asarray(values).reshape(-1, n)[:, second].ravel()


def depuncture(values, nbits, dbps, pattern=None):
    if pattern is None:
        pattern = PUNCTURE['1/2' if dbps in (24, 48, 96) else ('2/3' if dbps == 192 else '3/4')]
    keep = np.resize(pattern, 2 * nbits).astype(bool)
    out = np.zeros(2 * nbits, dtype=np.float32)
    if len(values) != int(keep.sum()):
        raise ValueError('Incorrect coded-bit count')
    out[keep] = values
    return out.reshape(-1, 2)


# Trellis: input enters the least significant bit of the seven-bit register.
_STATES = np.arange(64)
_PREV = np.stack((_STATES >> 1, (_STATES >> 1) | 32))
_REG = (_PREV << 1) | (_STATES & 1)
_SIGNS = np.array([[[2 * (int(v & poly).bit_count() & 1) - 1
                     for poly in (0o155, 0o117)] for v in row] for row in _REG])


def viterbi(soft, terminated=True):
    """Soft decision K=7 rate-1/2 Viterbi; zero means a punctured erasure."""
    metric = np.full(64, -1e30)
    metric[0] = 0
    trace = np.empty((len(soft), 64), dtype=np.uint8)
    for t, pair in enumerate(soft):
        scores = metric[_PREV] + (_SIGNS * pair).sum(axis=2)
        choice = scores[1] > scores[0]
        trace[t] = choice
        metric = np.where(choice, scores[1], scores[0])
        metric -= metric.max()
    state = 0 if terminated else int(np.argmax(metric))
    bits = np.empty(len(soft), dtype=np.uint8)
    for t in range(len(bits) - 1, -1, -1):
        bits[t] = state & 1
        state = int(_PREV[trace[t, state], state])
    return bits


def constellation(bpsc):
    """Standard Gray-labelled normalized BPSK/QPSK/16-QAM/64-QAM."""
    labels = ((np.arange(2 ** bpsc)[:, None] >> np.arange(bpsc)) & 1)
    if bpsc == 1:
        points = 2 * labels[:, 0] - 1
    else:
        half = bpsc // 2
        def axis(b):
            magnitude = np.ones(len(b))
            for j in range(half - 1, 0, -1):
                magnitude = 2 ** (half - j) - (2 * b[:, j] - 1) * magnitude
            return (2 * b[:, 0] - 1) * magnitude
        points = axis(labels[:, :half]) + 1j * axis(labels[:, half:])
        points /= np.sqrt({2: 2, 4: 10, 6: 42}[bpsc])
    return points, labels


def demap(z, bpsc, reliability):
    points, labels = constellation(bpsc)
    distance = np.abs(z[:, None] - points) ** 2
    # Positive likelihood means bit one. Fade-prone carriers get less weight.
    soft = np.stack([distance[:, labels[:, b] == 0].min(axis=1) -
                     distance[:, labels[:, b] == 1].min(axis=1)
                     for b in range(bpsc)], axis=1)
    return (soft * reliability[:, None]).ravel()


def unscramble(bits):
    """Recover the nonzero seed from the seven known-zero SERVICE bits."""
    for seed in range(1, 128):
        state, prefix = seed, []
        for _ in range(7):
            f = ((state >> 6) ^ (state >> 3)) & 1
            prefix.append(f)
            state = ((state << 1) | f) & 127
        if np.array_equal(prefix, bits[:7]):
            state, plain = seed, np.empty(len(bits), dtype=np.uint8)
            for i, bit in enumerate(bits):
                f = ((state >> 6) ^ (state >> 3)) & 1
                plain[i] = int(bit) ^ f
                state = ((state << 1) | f) & 127
            return plain
    return None


def pilot_phase(raw, expected, trend):
    """Pilot phase error of each symbol as (offset, slope per subcarrier).

    raw holds each symbol's 64 FFT bins, expected its four pilots as received
    through the channel. With trend, a common phase per symbol but one linear
    trend of the phase slope over the frame: four pilots give a noisy slope,
    and a wrong slope tilts the edge subcarriers of a whole symbol, while the
    trend still follows sampling-clock drift. Pilots are weighted by channel
    power, so a faded pilot's amplified noise cannot dominate. Without trend,
    offset and slope are fitted to each symbol's pilots alone."""
    q = [bins[PILOTS % 64] * np.conj(e) for bins, e in zip(raw, expected)]
    if not trend:
        return [tuple(np.polyfit(PILOTS, np.unwrap(np.angle(v)), 1)[::-1]) for v in q]
    slopes = [np.polyfit(PILOTS, np.unwrap(np.angle(v)), 1, w=np.abs(e))[0] for v, e in zip(q, expected)]
    if len(q) > 1:
        n = np.arange(len(q))
        slopes = np.polyval(np.polyfit(n, slopes, 1), n)
    return [(np.angle(np.sum(v * np.exp(-1j * slope * PILOTS))), slope) for v, slope in zip(q, slopes)]


def derotated(corrected, starts, expected, data, trend):
    """Data bins of the symbols at FFT starts, pilot phase removed."""
    raw = [np.fft.fft(corrected[a:a + 64]) for a in starts]
    return [bins[data % 64] * np.exp(-1j * (offset + slope * data))
            for bins, (offset, slope) in zip(raw, pilot_phase(raw, expected, trend))]


def equalize(corrected, h, starts, refs, data, trend=False):
    """One spatial stream: refs are the pilot values sent in each symbol."""
    expected = [h[PILOTS % 64] * ref for ref in refs]
    return [z / h[data % 64] for z in derotated(corrected, starts, expected, data, trend)]


def equalize_stbc(corrected, h1, h2, starts, refs1, refs2, data, trend=False):
    """Alamouti STBC (two space-time streams, one spatial stream): symbol
    pairs carry d0, d1 on stream 1 and -d1*, d0* on stream 2."""
    expected = [h1[PILOTS % 64] * r1 + h2[PILOTS % 64] * r2 for r1, r2 in zip(refs1, refs2)]
    r = derotated(corrected, starts, expected, data, trend)
    a, b = h1[data % 64], h2[data % 64]
    gain = np.maximum(np.abs(a) ** 2 + np.abs(b) ** 2, 1e-20)
    out = []
    for r0, r1 in zip(r[0::2], r[1::2]):
        out += [(np.conj(a) * r0 + b * np.conj(r1)) / gain, (np.conj(a) * r1 - b * np.conj(r0)) / gain]
    return out


def ht_sig_crc(bits):
    """HT-SIG CRC-8 (x^8 + x^2 + x + 1, ones preset), complemented, c7 first."""
    c = [1] * 8
    for m in bits:
        f = int(m) ^ c[7]
        c = [f, c[0] ^ f, c[1] ^ f, c[2], c[3], c[4], c[5], c[6]]
    return np.array([1 - v for v in c[::-1]], dtype=np.uint8)


def quadrature_ratio(symbol, reliability):
    """Weighted Q/I energy; above 1 the symbol is QBPSK (HT-SIG), not BPSK."""
    return float(np.sum(reliability * symbol.imag ** 2) /
                 max(np.sum(reliability * symbol.real ** 2), 1e-20))


def split_ampdu(psdu):
    """MPDUs of an A-MPDU. Each subframe is a 4-byte delimiter (12-bit length
    in bits 4-15, signature 0x4E in byte 3), the MPDU, and padding to 4 bytes.
    Delimiter CRCs are not checked: every MPDU must pass its own FCS, and a
    bad delimiter is skipped 4 bytes at a time as a receiver would."""
    out, pos = [], 0
    while pos + 4 <= len(psdu):
        length = (int.from_bytes(psdu[pos:pos + 2], 'little') >> 4) & 0xFFF
        if psdu[pos + 3] != 0x4E or length == 0 or pos + 4 + length > len(psdu):
            pos += 4
            continue
        out.append(psdu[pos + 4:pos + 4 + length])
        pos += 4 + (length + 3) // 4 * 4
    return out


def ht_ltf(corrected, streams):
    """Per-subcarrier channel of each space-time stream from its HT-LTFs.
    STBC sends two HT-LTFs with stream signs (+, -) and (+, +)."""
    y = [np.fft.fft(corrected[128 + 80 * (4 + i) + 16:][:64]) for i in range(streams)]
    for v in y:
        v[HT_ACTIVE % 64] /= HT_LTF_BINS[HT_ACTIVE % 64]
    return y if streams == 1 else [(y[0] - y[1]) / 2, (y[0] + y[1]) / 2]


# A channel estimate is projected onto cyclic impulse responses of -12..+16
# samples: HT cyclic shifts reach -600 ns, and delay spread is kept out to the
# 800 ns guard interval. Noise outside those 29 of 64 taps is dropped.
TAPS = np.arange(-12, 17)


def _projector(carriers):
    taps = np.exp(-2j * np.pi * np.outer(carriers, TAPS) / 64)
    return (taps @ np.linalg.pinv(taps)).astype(np.complex64)


SMOOTH, LEGACY_SMOOTH = _projector(HT_ACTIVE), _projector(ACTIVE)
LEGACY_MATCH = 4.0


def legacy_match(corrected, h):
    """The L-LTF estimate scaled to h, and the per-dimension energy of their
    difference inside the smoothing space over that outside it. Noise and
    transmitter error spread evenly (ratio about 1; 99% of live channel 11
    PPDUs below 3.1); a different channel, such as other cyclic shifts on the
    legacy fields, is smooth and lands inside (simulated: 10 or more even at
    12 dB SNR). LEGACY_MATCH sits between the two."""
    y = [np.fft.fft(corrected[a:a + 64])[ACTIVE % 64] / LTF_BINS[ACTIVE % 64] for a in (0, 64)]
    hl = (y[0] + y[1]) / 2
    scale = np.vdot(hl, h[ACTIVE % 64]) / max(np.vdot(hl, hl).real, 1e-20)
    r = h[ACTIVE % 64] - scale * hl
    inside = LEGACY_SMOOTH @ r
    return scale * hl, float(np.sum(np.abs(inside) ** 2) / len(TAPS) /
                             max(np.sum(np.abs(r - inside) ** 2) / (len(ACTIVE) - len(TAPS)), 1e-20))


def ht_channel(corrected, streams):
    """Channel estimate per space-time stream, smoothed by SMOOTH. One HT-LTF
    is a noisy estimate, so a single stream also averages in the two L-LTF
    symbols (weight 2:1) when they show the same channel; a transmitter that
    sends the legacy fields differently fails the match and keeps its own."""
    hs = ht_ltf(corrected, streams)
    if streams == 1:
        hl, ratio = legacy_match(corrected, hs[0])
        if ratio < LEGACY_MATCH:
            hs[0][ACTIVE % 64] = (hs[0][ACTIVE % 64] + 2 * hl) / 3
    for h in hs:
        h[HT_ACTIVE % 64] = SMOOTH @ h[HT_ACTIVE % 64]
    return hs


def fcs_ok(frame):
    return len(frame) >= 4 and zlib.crc32(frame[:-4]) == int.from_bytes(frame[-4:], 'little')


def decode_candidate(x, candidate, coarse):
    """Synchronize LTF, verify L-SIG, decode legacy payload, check MAC FCS."""
    lo = max(0, candidate + 32)
    hi = min(len(x), candidate + 480)
    if hi - lo < 192:
        return None
    z = x[lo:hi] * np.exp(-1j * coarse * np.arange(lo, hi))
    corr = signal.correlate(z, LTF, mode='valid', method='direct')
    energy = window_sum(np.abs(z) ** 2, 64)
    match = np.abs(corr) ** 2 / np.maximum(energy * np.vdot(LTF, LTF).real, 1e-20)
    pair = np.minimum(match[:-64], match[64:])
    at = int(np.argmax(pair))
    if pair[at] < 0.22:
        return None
    ltf = lo + at
    train = x[ltf:ltf + 128] * np.exp(-1j * coarse * np.arange(128))
    fine = np.angle(np.vdot(train[:64], train[64:])) / 64
    omega = float(coarse + fine)
    # At most 4095-byte legacy PSDU at 6 Mbps, plus preamble.
    end = min(len(x), ltf + 128 + 80 + 80 * 1367)
    corrected = x[ltf:end] * np.exp(-1j * omega * np.arange(end - ltf))
    h = (np.fft.fft(corrected[:64]) + np.fft.fft(corrected[64:128])) / 2
    h[ACTIVE % 64] /= LTF_BINS[ACTIVE % 64]
    reliability = np.abs(h[DATA % 64]) ** 2
    reliability /= max(float(np.median(reliability)), 1e-20)
    h = np.where(np.abs(h) > 1e-12, h, 1e-12)

    def legacy(first, count):
        """FFT starts and expected pilots of legacy-timed symbols (L-SIG = 0)."""
        index = range(first, first + count)
        return [128 + 80 * i + 16 for i in index], [PILOT_SIGNS * POLARITY[i % 127] for i in index]

    if 128 + 80 + 64 > len(corrected):
        return None
    lsig = equalize(corrected, h, *legacy(0, 1), DATA)[0]
    header = viterbi(deinterleave(demap(lsig, 1, reliability), 1).reshape(-1, 2))
    rate_code = int(header[:4] @ (1 << np.arange(4)))
    length = int(header[5:17] @ (1 << np.arange(12)))
    if (rate_code not in RATES or header[4] or header[:18].sum() % 2 or
            header[18:].any() or not 4 <= length <= 4095):
        return None
    rate, bpsc, dbps = RATES[rate_code]
    result = {'sample': int(max(0, ltf - 192)), 'time_ms': float(max(0, ltf - 192) / 20000),
              'rate_mbps': rate, 'length': length, 'cfo_hz': float(omega * FS / (2 * np.pi)),
              'ltf_match': float(pair[at]), 'fcs_ok': False}

    # Legacy parameters; an HT-mixed preamble replaces them below.
    nsym = (16 + 8 * length + 6 + dbps - 1) // dbps
    starts, refs = legacy(1, nsym)
    data, code, columns, carriers, aggregated, unclear = DATA, None, 16, 48, False, None

    def equalizer(trend):
        return equalize(corrected, h, starts, refs, data, trend)
    if rate == 6 and 128 + 80 * 3 <= len(corrected):
        # HT-SIG follows L-SIG as two QBPSK symbols; VHT-SIG-A rotates only the
        # second. Noise can rotate a weak legacy symbol, so only a passing
        # HT-SIG CRC skips the legacy decode; otherwise the label applies
        # only if the legacy decode fails too.
        sig = equalize(corrected, h, *legacy(1, 2), DATA)
        rotated = [quadrature_ratio(z, reliability) > 1 for z in sig]
        if rotated[1] and not rotated[0]:
            unclear = 'vht'
        elif rotated[0] and rotated[1]:
            soft = np.concatenate([deinterleave(demap(z * -1j, 1, reliability), 1) for z in sig])
            bits = viterbi(soft.reshape(-1, 2))
            if not np.array_equal(ht_sig_crc(bits[:34]), bits[34:42]) or bits[42:].any():
                unclear = 'ht_bad_sig'
            else:
                def field(a, b):
                    return int(bits[a:b] @ (1 << np.arange(b - a)))
                mcs, length, short_gi, aggregated = field(0, 7), field(8, 24), bool(bits[31]), bool(bits[27])
                stbc = field(28, 30)
                result.update(ofdm_format='ht', mcs=mcs, length=length, short_gi=short_gi, aggregated=aggregated,
                              stbc=stbc, smoothing=bool(bits[24]))
                if mcs not in HT_MCS or bits[7] or stbc > 1 or bits[30] or field(32, 34) or not length:
                    return dict(result, status='ht_unsupported', cbw40=bool(bits[7]), ldpc=bool(bits[30]),
                                extension_streams=field(32, 34))
                rate, bpsc, code, dbps = HT_MCS[mcs]
                result['rate_mbps'] = round(rate * (10 / 9 if short_gi else 1), 1)
                # HT-STF in symbol slot 3, one HT-LTF per space-time stream, then data.
                streams, gi = 1 + stbc, 8 if short_gi else 16
                nsym = streams * -(-(16 + 8 * length + 6) // (streams * dbps))
                first = 128 + 80 * (4 + streams)
                starts = [first + (64 + gi) * n + gi for n in range(nsym)]
                if len(corrected) < 128 + 80 * (4 + streams):
                    return dict(result, status='truncated')
                estimates = ht_channel(corrected, streams)
                # Pilots rotate one position per symbol; polarity starts at p3.
                polarity = POLARITY[(np.arange(nsym) + 3) % 127]
                if stbc:
                    h1, h2 = estimates
                    psi1, psi2 = np.array([1, 1, -1, -1]), np.array([1, -1, -1, 1])
                    refs1 = [np.roll(psi1, -(n % 4)) * polarity[n] for n in range(nsym)]
                    refs2 = [np.roll(psi2, -(n % 4)) * polarity[n] for n in range(nsym)]
                    reliability = (np.abs(h1) ** 2 + np.abs(h2) ** 2)[HT_DATA % 64]

                    def equalizer(trend):
                        return equalize_stbc(corrected, h1, h2, starts, refs1, refs2, HT_DATA, trend)
                else:
                    h_ht = np.where(np.abs(estimates[0]) > 1e-12, estimates[0], 1e-12)
                    refs = [np.roll(PILOT_SIGNS, -(n % 4)) * polarity[n] for n in range(nsym)]
                    reliability = np.abs(h_ht[HT_DATA % 64]) ** 2

                    def equalizer(trend):
                        return equalize(corrected, h_ht, starts, refs, HT_DATA, trend)
                reliability /= max(float(np.median(reliability)), 1e-20)
                columns, carriers = 13, 52

    if starts[-1] + 64 > len(corrected):
        return dict(result, status='truncated')
    pattern = PUNCTURE[code] if code else None

    def payload(symbols):
        soft = np.concatenate([deinterleave(demap(symbol, bpsc, reliability), bpsc, columns, carriers)
                               for symbol in symbols])
        decoded = viterbi(depuncture(soft, nsym * dbps, dbps, pattern)[:16 + 8 * length + 6])
        plain = unscramble(decoded)
        if plain is None or plain[:16].any():
            return dict(result, status='bad_service')
        psdu = np.packbits(plain[16:16 + 8 * length], bitorder='little').tobytes()
        if not aggregated:
            good = fcs_ok(psdu)
            return dict(result, status='ok' if good else 'bad_fcs', fcs_ok=good, frame=psdu)
        mpdus = split_ampdu(psdu)
        valid = [m for m in mpdus if fcs_ok(m)]
        counts = dict(ampdu_subframes=len(mpdus), ampdu_valid=len(valid))
        if not valid:
            return dict(result, status='bad_fcs', frame=psdu, **counts)
        return dict(result, status='ok', fcs_ok=True, frame=valid[0], more_frames=valid[1:], **counts)

    def complete(p):
        return p['fcs_ok'] and p.get('ampdu_valid') == p.get('ampdu_subframes')

    packet = payload(equalizer(trend=True))
    if not complete(packet):
        # The slope trend misses frames whose slope is not linear over time;
        # a few short frames decode only with per-symbol fits. Accept that
        # decode only when the FCS proves it, otherwise report the first.
        retry = payload(equalizer(trend=False))
        if retry.get('ampdu_valid', int(retry['fcs_ok'])) > packet.get('ampdu_valid', int(packet['fcs_ok'])):
            packet = dict(retry, tracking='per_symbol')
    if unclear and not packet['fcs_ok']:
        packet = dict(packet, status=unclear)
    return packet


def decode_channel(x, center, channel, threshold=0.65):
    off = (channel_mhz(channel) - center) * 1e6
    if abs(off) > 9e6:
        raise ValueError('Channel is too far from capture center for a complete 20 MHz channel')
    y = signal.resample_poly(shift(x, off), 1, 2).astype(np.complex64)
    packets, candidates, previous = [], 0, -1000
    for at, cfo in detect_stf(y, threshold):
        candidates += 1
        packet = decode_candidate(y, at, cfo)
        if packet is None or abs(packet['sample'] - previous) < 80:
            continue
        previous = packet['sample']
        packet['channel'] = channel
        more = packet.pop('more_frames', [])
        if 'ampdu_subframes' in packet:
            packet['mpdu_index'] = 0
        packets.append(packet)
        # Further FCS-valid MPDUs of an A-MPDU become their own records.
        packets.extend(dict(packet, frame=frame, mpdu_index=i) for i, frame in enumerate(more, 1))
    return packets, candidates


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture')
    ap.add_argument('--center', type=float, required=True)
    ap.add_argument('--channel', type=int, nargs='+', choices=CHANNELS, metavar='N',
                    help='2.4 or 5 GHz channel number(s) (default: the one at the centre)')
    ap.add_argument('--stf-threshold', type=float, default=0.65)
    ap.add_argument('--json', type=Path, help='Write packet results; only valid frames include identities')
    args = ap.parse_args()
    if not 0 < args.stf_threshold < 1:
        ap.error('--stf-threshold must be between zero and one')
    x, results = load(args.capture), []
    for ch in args.channel or [nearest_channel(args.center)]:
        packets, candidates = decode_channel(x, args.center, ch, args.stf_threshold)
        print(f'channel {ch}: {candidates} STF candidates; {len(packets)} valid L-SIG headers')
        for p in packets:
            fields = describe(p['frame']) if p['fcs_ok'] else []
            print(f"{p['time_ms']:10.3f} ms rate={p['rate_mbps']:2} length={p['length']:4} "
                  f"status={p['status']} " + ' '.join(fields))
            results.append({k: v for k, v in p.items() if k != 'frame'} | {'fields': fields})
        print('summary:', dict(Counter(p['status'] for p in packets)))
    if args.json:
        args.json.write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
