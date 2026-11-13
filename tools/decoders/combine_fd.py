"""Per-subcarrier two-chain combining for OFDM (the --combine `fd` branch).

combine.Combiner merges the chains in the time domain, one whitened weight
pair per 10 us block and one slot-1 delay per capture. The two receive
antennas fade independently across the band, and the delay between them
depends on the transmitter, so one broadband weight leaves gain unused on
frequency-selective channels (measured on weak 5 GHz frames: about 1.5 dB
of effective SNR, 2.9 dB on channel 36).

Here every STF candidate of the combined stream is decoded from the two
chains combined per subcarrier:
  1. LTF timing and CFO from the combined stream (decode_candidate's sync).
  2. Both chains' FFTs at that timing: the L-LTF halves and every
     legacy-timed symbol.
  3. Whitened MRC weights per subcarrier, w = Rn^-1 h / sqrt(h^H Rn^-1 h),
     from each chain's smoothed L-LTF estimate h and the capture's flat
     noise covariance Rn (Combiner.noise). Every combined bin then has unit
     noise, so its |h|^2 is its SNR, as the decoder's LLR weights assume.
  4. The combined bins are rebuilt into a CFO-free virtual stream (zeros,
     the L-LTF with its GI2, then each symbol with its cyclic prefix) and
     decoded by wifi_ofdm.decode_candidate unchanged.
  5. A 6 Mbps legacy frame that fails its FCS is re-encoded from its own
     decode; the common phase is fitted on the combined bins and each
     chain's channel is re-estimated over the whole frame (L-LTF at weight
     2, smoothed). New weights, decode again, at most DD_ROUNDS times; a
     pass counts only if its FCS passes.

HT frames go through the same virtual stream first. It keeps legacy symbol
timing and the 52 legacy carriers, which cuts an HT payload wrongly (56
carriers; 72-sample symbols with short GI). So when that decode is a
non-STBC HT PPDU without every MPDU valid, both chains are cut again with
the timing its HT-SIG gives (ht_rebuild):
  6. L-LTF, L-SIG, HT-SIG1/2, HT-STF, HT-LTF at 80 samples, then the data
     symbols at 64 + GI, every FFT window BACKOFF samples early (inside the
     guard interval, so a late sync on the combined stream pulls in no
     inter-symbol interference).
  7. Each chain's channel from wifi_ofdm.ht_channel (HT-LTF, L-LTF averaged
     in when it matches, smoothed), whitened MRC weights on all 56 carriers,
     and the same weights for every symbol, so the virtual stream shows one
     channel from the L-LTF to the last data symbol.
  8. Each rebuilt symbol is advanced cyclically by ADVANCE samples: a
     virtual symbol that came out delayed would make the decoder's own LTF
     search land late and read the next rebuilt prefix; an advance within
     the guard interval is harmless in a perfectly cyclic stream.
  9. The rebuild replaces the first result only when it has more valid MPDUs.
"""
import numpy as np
from scipy import signal

import wifi_ofdm as W
from cs16 import shift
import resample

K = W.ACTIVE % 64
D = W.DATA % 64
DPOS = np.searchsorted(W.ACTIVE, W.DATA)
PPOS = np.searchsorted(W.ACTIVE, W.PILOTS)
LEAD = 160          # zeros before the rebuilt L-LTF's GI2
MAX_SYMBOLS = 1368  # L-SIG + the longest 6 Mbps legacy PSDU, as decode_candidate
DD_ROUNDS = 2


def sync(x, candidate, coarse):
    """decode_candidate's LTF timing and CFO. The match threshold is left to
    the decode of the combined stream, which is stronger than x."""
    lo, hi = max(0, candidate + 32), min(len(x), candidate + 480)
    if hi - lo < 192:
        return None
    z = x[lo:hi] * np.exp(-1j * coarse * np.arange(lo, hi))
    corr = signal.correlate(z, W.LTF, mode='valid', method='direct')
    energy = W.window_sum(np.abs(z) ** 2, 64)
    match = np.abs(corr) ** 2 / np.maximum(energy * np.vdot(W.LTF, W.LTF).real, 1e-20)
    pair = np.minimum(match[:-64], match[64:])
    ltf = lo + int(np.argmax(pair))
    train = x[ltf:ltf + 128] * np.exp(-1j * coarse * np.arange(128))
    return ltf, float(coarse + np.angle(np.vdot(train[:64], train[64:])) / 64)


def chain_bins(y, ltf, omega):
    """CFO-corrected FFT bins: L-LTF halves (2, 64) and symbols (n, 64) from L-SIG on."""
    n = min(MAX_SYMBOLS, (len(y) - ltf - 128) // 80)
    if n < 1:
        return None, None
    end = ltf + 128 + 80 * n
    c = y[ltf:end] * np.exp(-1j * omega * np.arange(end - ltf))
    return np.fft.fft(c[:128].reshape(2, 64), axis=1), np.fft.fft(c[128:].reshape(n, 80)[:, 16:], axis=1)


def weights(h0, h1, noise):
    """Unit-noise whitened MRC weights per subcarrier; noise = (Rn00, Rn11, E[n0 n1*])."""
    n00, n11, n01 = noise
    det = n00 * n11 - abs(n01) ** 2
    g0 = (n11 * h0 - n01 * h1) / det
    g1 = (n00 * h1 - np.conj(n01) * h0) / det
    gamma = np.maximum(np.real(np.conj(h0) * g0 + np.conj(h1) * g1), 1e-30)
    return g0 / np.sqrt(gamma), g1 / np.sqrt(gamma)


def virtual(b0, b1, s0, s1, w0, w1):
    """The combined, CFO-free stream decode_candidate reads (L-LTF at LEAD + 32)."""
    def comb(a, b):
        z = np.zeros(a.shape, complex)
        z[..., K] = np.conj(w0) * a[..., K] + np.conj(w1) * b[..., K]
        return np.fft.ifft(z, axis=-1)
    t, s = comb(b0, b1), comb(s0, s1)
    body = np.concatenate([s[:, -16:], s], axis=1).ravel()
    return np.concatenate([np.zeros(LEAD), t[0, -32:], t[0], t[1], body]).astype(np.complex64)


def transmitted(z1, hc, length, frame):
    """L-SIG and data symbols (n, 52 over ACTIVE) of a 6 Mbps legacy frame from
    its PSDU; the scrambler seed is the one that best explains data symbol z1."""
    nsym = (16 + 8 * length + 6 + 23) // 24
    header = np.zeros(24, np.uint8)
    header[:4] = [1, 1, 0, 1]
    header[5:17] = [(length >> i) & 1 for i in range(12)]
    header[17] = header[:17].sum() & 1
    psdu = np.unpackbits(np.frombuffer(frame, np.uint8), bitorder='little')[:8 * length]
    plain = np.zeros(nsym * 24, np.uint8)
    plain[16:16 + len(psdu)] = psdu
    eq = z1[DPOS] / np.where(np.abs(hc[DPOS]) > 1e-12, hc[DPOS], 1e-12)
    seed = max(range(1, 128), key=lambda s: np.real(
        eq * W.bpsk_symbols(W.encode(W.scramble(plain[:24], s)))[0]).sum())
    bits = W.scramble(plain, seed)
    bits[16 + 8 * length:16 + 8 * length + 6] = 0
    X = np.zeros((nsym + 1, 52))
    X[0, DPOS] = W.bpsk_symbols(W.encode(header))[0]
    X[1:, DPOS] = W.bpsk_symbols(W.encode(bits))
    for i in range(nsym + 1):
        X[i, PPOS] = W.PILOT_SIGNS * W.POLARITY[i % 127]
    return X


def common_phase(Z, X, h):
    """Per-symbol derotation (phase + slope trend over the frame), as reestimate fits it."""
    q = Z * X * np.conj(h)
    n = np.arange(len(Z))
    raw = np.array([np.polyfit(W.ACTIVE, np.unwrap(np.angle(v)), 1, w=np.abs(h))[0] for v in q])
    beta = np.polyval(np.polyfit(n, raw, 1), n) if len(n) > 2 else raw
    phi = np.angle((q * np.exp(-1j * beta[:, None] * W.ACTIVE)).sum(1))
    return np.exp(-1j * (phi[:, None] + beta[:, None] * W.ACTIVE))


HK = W.HT_ACTIVE % 64
BACKOFF, ADVANCE = 3, 5  # HT rebuild: FFT windows early, then rebuilt symbols advanced (samples)
_ADVANCE = np.exp(2j * np.pi * np.fft.fftfreq(64) * ADVANCE)


def valid_mpdus(p):
    return 0 if p is None else p.get('ampdu_valid', int(bool(p['fcs_ok'])))


def ht_cut(y, ltf, omega, nsym, gi):
    """CFO-corrected samples from L-LTF start - BACKOFF, and the FFT starts of
    L-SIG, HT-SIG1/2, HT-STF, HT-LTF and nsym data symbols of 64 + gi samples."""
    ltf = max(0, ltf - BACKOFF)
    first = 128 + 80 * 5
    end = ltf + first + (64 + gi) * nsym
    if end > len(y):
        return None, None
    c = y[ltf:end] * np.exp(-1j * omega * np.arange(end - ltf))
    return c, [128 + 80 * i + 16 for i in range(5)] + [first + (64 + gi) * n + gi for n in range(nsym)]


def ht_virtual(c0, c1, starts, w0, w1, gi):
    """The HT-timed virtual stream: both chains' bins combined on HT_ACTIVE,
    advanced by ADVANCE, prefixes copied from each symbol's tail."""
    def comb(a, b):
        z = np.zeros(64, complex)
        z[HK] = np.conj(w0) * np.fft.fft(a)[HK] + np.conj(w1) * np.fft.fft(b)[HK]
        return np.fft.ifft(z * _ADVANCE)
    t = [comb(c0[a:a + 64], c1[a:a + 64]) for a in (0, 64)]
    parts = [np.zeros(LEAD), t[0][-32:], t[0], t[1]]
    for i, a in enumerate(starts):
        s = comb(c0[a:a + 64], c1[a:a + 64])
        g = 16 if i < 5 else gi
        parts += [s[-g:], s]
    parts.append(np.zeros(160))
    return np.concatenate(parts).astype(np.complex64)


def ht_rebuild(y0, y1, ltf, omega, packet, noise):
    """Decode a non-STBC HT PPDU again from both chains cut on its HT timing
    (module docstring, steps 6-9). Returns the better of packet and the rebuild."""
    _, _, _, dbps = W.HT_MCS[packet['mcs']]
    nsym, gi = -(-(16 + 8 * packet['length'] + 6) // dbps), (8 if packet['short_gi'] else 16)
    c0, starts = ht_cut(y0, ltf, omega, nsym, gi)
    c1, _ = ht_cut(y1, ltf, omega, nsym, gi)
    if c0 is None or c1 is None:
        return packet
    w = weights(W.ht_channel(c0, 1)[0][HK], W.ht_channel(c1, 1)[0][HK], noise)
    p = W.decode_candidate(ht_virtual(c0, c1, starts, *w, gi), 0, 0.0)
    if p is None or p.get('ofdm_format') != 'ht' or valid_mpdus(p) <= valid_mpdus(packet):
        return packet
    p.update(sample=packet['sample'], time_ms=packet['time_ms'], cfo_hz=packet['cfo_hz'],
             fd_rounds=packet['fd_rounds'], fd_ht=True)
    return p


def decode_candidate(y0, y1, yb, candidate, coarse, noise):
    """Decode one STF candidate of combined stream yb from chains y0, y1 (20 MSps channel grid)."""
    found = sync(yb, candidate, coarse)
    if found is None:
        return None
    ltf, omega = found
    p = _decode_legacy_timed(y0, y1, ltf, omega, noise)
    if (p is not None and p.get('ofdm_format') == 'ht' and p['status'] != 'ht_unsupported'
            and not p['stbc']
            and not (p['fcs_ok'] and p.get('ampdu_valid') == p.get('ampdu_subframes'))):
        p = ht_rebuild(y0, y1, ltf, omega, p, noise)
    return p


def _decode_legacy_timed(y0, y1, ltf, omega, noise):
    """Steps 2-5: the legacy-timed virtual stream and the 6 Mbps re-estimation rounds."""
    b0, s0 = chain_bins(y0, ltf, omega)
    b1, s1 = chain_bins(y1, ltf, omega)
    if b0 is None or b1 is None:
        return None
    raw = [((b[0] + b[1]) / 2)[K] / W.LTF_BINS[K] for b in (b0, b1)]
    h = [W.LEGACY_SMOOTH @ r for r in raw]

    def attempt(h0, h1, rounds):
        w = weights(h0, h1, noise)
        p = W.decode_candidate(virtual(b0, b1, s0, s1, *w), 0, 0.0)
        if p is not None:
            p.update(sample=int(max(0, ltf - 192)), time_ms=float(max(0, ltf - 192) / 20000),
                     cfo_hz=float(omega * W.FS / (2 * np.pi)), fd_rounds=rounds)
        return p, w

    packet, w = attempt(*h, 0)
    first = packet
    for r in range(1, DD_ROUNDS + 1):
        if (packet is None or packet['fcs_ok'] or packet['rate_mbps'] != 6 or 'ofdm_format' in packet
                or 'frame' not in packet):
            break
        length = packet['length']
        nsym = (16 + 8 * length + 6 + 23) // 24
        if nsym + 1 > len(s0):
            break
        Z = np.conj(w[0]) * s0[:nsym + 1, K] + np.conj(w[1]) * s1[:nsym + 1, K]
        hc = np.conj(w[0]) * h[0] + np.conj(w[1]) * h[1]
        X = transmitted(Z[1], hc, length, packet['frame'])
        rot = common_phase(Z, X, hc)
        h = [W.LEGACY_SMOOTH @ ((2 * r0 + (s[:nsym + 1, K] * rot * X).sum(0)) / (3 + nsym))
             for r0, s in zip(raw, (s0, s1))]
        packet, w = attempt(*h, r)
        if packet is not None and packet['fcs_ok']:
            return packet
    return first


def decode_channel(x0, x1, xb, center, channel, noise, threshold=0.5):
    """wifi_ofdm.decode_channel for the fd branch: candidates of xb, decoded from x0 and x1
    (40 MSps slots, slot 1 delayed like xb; xb from combine.Combiner)."""
    off = (W.channel_mhz(channel) - center) * 1e6
    if abs(off) > 9e6:
        raise ValueError('Channel is too far from capture center for a complete 20 MHz channel')
    y0, y1, yb = (resample.resample_poly(shift(x, off), 1, 2) for x in (x0, x1, xb))
    packets, candidates, previous = [], 0, -1000
    for at, cfo in W.detect_stf(yb, threshold):
        candidates += 1
        packet = decode_candidate(y0, y1, yb, at, cfo, noise)
        if packet is None or abs(packet['sample'] - previous) < 80:
            continue
        previous = packet['sample']
        packet['channel'] = channel
        more = packet.pop('more_frames', [])
        if 'ampdu_subframes' in packet:
            packet['mpdu_index'] = 0
        packets.append(packet)
        packets.extend(dict(packet, frame=frame, mpdu_index=i) for i, frame in enumerate(more, 1))
    return packets, candidates
