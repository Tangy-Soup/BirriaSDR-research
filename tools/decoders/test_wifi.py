"""Receiver tests using generated RF waveforms, noise, multipath and bad frames.

Run: .venv/bin/python -m unittest discover -s . -p 'test_wifi.py' -v
No hardware or stored multi-megabyte fixtures are required.
"""
import unittest
import zlib
import tempfile
from pathlib import Path
import struct
from unittest import mock
import contextlib
import io
import json
import subprocess

import numpy as np
from scipy import signal

import wifi_ofdm as rx
import wifi_b
import wifi
import combine
import cs16
import wifi_scan
import wifi_benchmark
import analyze_blanking


def beacon():
    mac = bytes.fromhex('80000000ffffffffffff0211223344550211223344551000')
    return mac + bytes(8) + bytes.fromhex('64000100') + b'\x00\x08Test-AP!\x03\x01\x01'


def encode(bits):
    state, out = 0, []
    for bit in bits:
        state = ((state << 1) | int(bit)) & 127
        out.extend([(state & 0o155).bit_count() % 2, (state & 0o117).bit_count() % 2])
    return np.array(out, dtype=np.uint8)


def interleave(bits, bpsc):
    n = 48 * bpsc
    out = np.empty(n, dtype=np.uint8)
    for k in range(n):
        i = (n // 16) * (k % 16) + k // 16
        s = max(bpsc // 2, 1)
        j = s * (i // s) + (i + n - (16 * i) // n) % s
        out[j] = bits[k]
    return out


def mapped(bits, bpsc):
    b = np.asarray(bits).reshape(-1, bpsc).astype(int)
    if bpsc == 1:
        return 2 * b[:, 0] - 1
    half = bpsc // 2
    def axis(v):
        if half == 1:
            return 2 * v[:, 0] - 1
        if half == 2:
            return (2 * v[:, 0] - 1) * (3 - 2 * v[:, 1])
        return (2 * v[:, 0] - 1) * (4 - (2 * v[:, 1] - 1) * (3 - 2 * v[:, 2]))
    return (axis(b[:, :half]) + 1j * axis(b[:, half:])) / np.sqrt({2: 2, 4: 10, 6: 42}[bpsc])


def ofdm_wave(rate, corrupt=False, bad_parity=False, padding=0):
    code, (_, bpsc, dbps) = next((k, v) for k, v in rx.RATES.items() if v[0] == rate)
    body = beacon() + bytes(padding)
    frame = body + zlib.crc32(body).to_bytes(4, 'little')
    if corrupt:
        frame = frame[:-1] + bytes([frame[-1] ^ 1])
    header = [(code >> i) & 1 for i in range(4)] + [0]
    header += [(len(frame) >> i) & 1 for i in range(12)]
    header += [sum(header) % 2 ^ int(bad_parity)] + [0] * 6
    payload = np.r_[np.zeros(16, dtype=np.uint8), np.unpackbits(np.frombuffer(frame, dtype=np.uint8), bitorder='little')]
    total = int(np.ceil((len(payload) + 6) / dbps)) * dbps
    bits = np.pad(payload, (0, total - len(payload)))
    state = 93
    for i in range(len(bits)):
        feedback = ((state >> 6) ^ (state >> 3)) & 1
        bits[i] ^= feedback
        state = ((state << 1) | feedback) & 127
    bits[len(payload):len(payload) + 6] = 0
    coded = encode(bits)
    keep = [1, 1] if dbps in (24, 48, 96) else ([1, 1, 1, 0] if dbps == 192 else [1, 1, 1, 0, 0, 1])
    coded = coded[np.resize(keep, len(coded)).astype(bool)]
    rows = [mapped(interleave(encode(header), 1), 1)]
    for row in coded.reshape(-1, 48 * bpsc):
        rows.append(mapped(interleave(row, bpsc), bpsc))
    # Standard short training tones: twelve carriers spaced four bins apart.
    short = np.zeros(64, complex)
    short[np.array([-24, -20, -16, -12, -8, -4, 4, 8, 12, 16, 20, 24]) % 64] = (
        np.array([1, -1, 1, -1, -1, 1, -1, -1, 1, 1, 1, 1]) * (1 + 1j) * np.sqrt(13 / 6))
    training = np.fft.ifft(short)
    wave = [np.tile(training[:16], 10), rx.LTF[-32:], rx.LTF, rx.LTF]
    state = 127
    for points in rows:
        feedback = ((state >> 6) ^ (state >> 3)) & 1
        bins = np.zeros(64, complex)
        bins[rx.DATA % 64] = points
        bins[rx.PILOTS % 64] = np.array([1, 1, 1, -1]) * (1 - 2 * feedback)
        state = ((state << 1) | feedback) & 127
        time = np.fft.ifft(bins)
        wave.extend([time[-16:], time])
    return np.concatenate(wave), frame


def ht_interleave(bits, bpsc):
    n = 52 * bpsc
    out = np.empty(n, dtype=np.uint8)
    for k in range(n):
        i = (n // 13) * (k % 13) + k // 13
        s = max(bpsc // 2, 1)
        out[s * (i // s) + (i + n - (13 * i) // n) % s] = bits[k]
    return out


def ampdu(mpdus):
    """A-MPDU subframes: delimiter (12-bit length, zero CRC byte, 0x4E), MPDU, padding."""
    out = b''
    for i, m in enumerate(mpdus):
        out += (len(m) << 4).to_bytes(2, 'little') + b'\x00\x4e' + m
        out += bytes(-len(m) % 4 if i < len(mpdus) - 1 else 0)
    return out


def ht_wave(mcs, short_gi=False, mpdus=None, stbc=False, corrupt_sig=False):
    """HT-mixed 20 MHz PPDU, one spatial stream (two space-time streams with
    STBC; stream 2 reaches the receiver through a fixed complex gain and the
    standard cyclic shifts)."""
    _, bpsc, code, dbps = rx.HT_MCS[mcs]
    frame = beacon() + zlib.crc32(beacon()).to_bytes(4, 'little')
    psdu = ampdu(mpdus) if mpdus else frame
    sig = [(mcs >> i) & 1 for i in range(7)] + [0] + [(len(psdu) >> i) & 1 for i in range(16)]
    sig += [1, 1, 1, int(bool(mpdus)), int(stbc), 0, 0, int(short_gi), 0, 0]
    sig += list(rx.ht_sig_crc(sig)) + [0] * 6
    if corrupt_sig:
        sig[9] ^= 1
    streams, gi = 1 + int(stbc), 8 if short_gi else 16
    payload = np.r_[np.zeros(16, dtype=np.uint8), np.unpackbits(np.frombuffer(psdu, dtype=np.uint8), bitorder='little')]
    nsym = streams * int(np.ceil((len(payload) + 6) / (streams * dbps)))
    bits = np.pad(payload, (0, nsym * dbps - len(payload)))
    state = 93
    for i in range(len(bits)):
        feedback = ((state >> 6) ^ (state >> 3)) & 1
        bits[i] ^= feedback
        state = ((state << 1) | feedback) & 127
    bits[len(payload):len(payload) + 6] = 0
    coded = encode(bits)
    coded = coded[np.resize(rx.PUNCTURE[code], len(coded)).astype(bool)]
    points = [mapped(ht_interleave(row, bpsc), bpsc) for row in coded.reshape(nsym, 52 * bpsc)]
    # L-SIG at 6 Mbps covers the whole PPDU.
    txtime = 20 + 8 + 4 + 4 * streams + nsym * (64 + gi) / 20
    lsig_length = int(np.ceil((txtime - 20) / 4)) * 3 - 3
    header = [1, 1, 0, 1, 0] + [(lsig_length >> i) & 1 for i in range(12)]
    header += [sum(header) % 2] + [0] * 6
    lsig = encode(header)
    coded_sig = encode(sig)

    def symbol(values, data, pilots, guard=16, shift=0):
        bins = np.zeros(64, complex)
        bins[data % 64] = values
        bins[rx.PILOTS % 64] = pilots
        if shift:
            bins *= np.exp(-2j * np.pi * np.fft.fftfreq(64) * 64 * shift / 64)
        t = np.fft.ifft(bins)
        return np.r_[t[-guard:], t]

    short = np.zeros(64, complex)
    short[np.array([-24, -20, -16, -12, -8, -4, 4, 8, 12, 16, 20, 24]) % 64] = (
        np.array([1, -1, 1, -1, -1, 1, -1, -1, 1, 1, 1, 1]) * (1 + 1j) * np.sqrt(13 / 6))
    training = np.fft.ifft(short)
    legacy = [np.tile(training[:16], 10), rx.LTF[-32:], rx.LTF, rx.LTF,
              symbol(mapped(interleave(lsig, 1), 1), rx.DATA, rx.PILOT_SIGNS * rx.POLARITY[0])]
    for i in range(2):
        legacy.append(symbol(1j * mapped(interleave(coded_sig[48 * i:48 * i + 48], 1), 1), rx.DATA,
                             rx.PILOT_SIGNS * rx.POLARITY[i + 1]))
    legacy = np.concatenate(legacy)
    ltf_values = rx.HT_LTF_BINS[rx.HT_DATA % 64], rx.HT_DATA, rx.HT_LTF_BINS[rx.PILOTS % 64]
    polarity = rx.POLARITY[(np.arange(nsym) + 3) % 127]
    if not stbc:
        ht = [np.tile(training[:16], 5), symbol(*ltf_values)]
        for n, values in enumerate(points):
            ht.append(symbol(values, rx.HT_DATA, np.roll(rx.PILOT_SIGNS, -(n % 4)) * polarity[n], gi))
        return np.r_[legacy, np.concatenate(ht)], frame
    # Stream 2: HT-LTF signs (+, +) against stream 1's (+, -); Alamouti data
    # pairs; its own pilot pattern; -400 ns cyclic shift in the HT portion and
    # -200 ns on the legacy fields from the second antenna.
    psi = np.array([1, 1, -1, -1]), np.array([1, -1, -1, 1])
    s1 = [np.tile(training[:16], 5), symbol(*ltf_values), -symbol(*ltf_values)]
    s2 = [np.tile(training[:16], 5), symbol(*ltf_values, shift=-8), symbol(*ltf_values, shift=-8)]
    for n in range(0, nsym, 2):
        d0, d1 = points[n], points[n + 1]
        for k, (a, b) in enumerate(((d0, -np.conj(d1)), (d1, np.conj(d0)))):
            m = n + k
            s1.append(symbol(a, rx.HT_DATA, np.roll(psi[0], -(m % 4)) * polarity[m], gi))
            s2.append(symbol(b, rx.HT_DATA, np.roll(psi[1], -(m % 4)) * polarity[m], gi, shift=-8))
    gain = .6 - .5j
    legacy2 = np.r_[np.zeros(4), legacy[:-4]]   # a short echo stands in for the legacy cyclic shift
    return np.r_[legacy + gain * legacy2, np.concatenate(s1) + gain * np.concatenate(s2)], frame


class OfdmTests(unittest.TestCase):
    def received(self, rate, **kwargs):
        wave, frame = ofdm_wave(rate, **kwargs)
        # Mild multipath, oscillator offset and approximately 26 dB SNR.
        wave = np.convolve(wave, [1, 0.12 + .08j, 0, -.06j])
        wave = np.r_[np.zeros(300), wave, np.zeros(300)]
        rng = np.random.default_rng(rate)
        wave *= np.exp(1j * (0.7 + 2 * np.pi * 85000 * np.arange(len(wave)) / rx.FS))
        wave += .004 * (rng.normal(size=len(wave)) + 1j * rng.normal(size=len(wave)))
        return wave.astype(np.complex64), frame

    def test_all_legacy_rates_with_noise_multipath_and_cfo(self):
        for rate in (6, 9, 12, 18, 24, 36, 48, 54):
            with self.subTest(rate=rate):
                wave, frame = self.received(rate)
                packets = [rx.decode_candidate(wave, at, cfo) for at, cfo in rx.detect_stf(wave)]
                good = [p for p in packets if p is not None and p['fcs_ok']]
                self.assertEqual(len(good), 1)
                self.assertEqual(good[0]['frame'], frame)
                self.assertEqual(good[0]['rate_mbps'], rate)

    def test_long_frame_with_sampling_clock_offset(self):
        # A 40 ppm clock error drifts about 1.5 samples over this 1500-byte
        # 6 Mbps frame; pilot tracking must follow the growing phase slope.
        wave, frame = ofdm_wave(6, padding=1450)
        wave = np.r_[np.zeros(300), wave, np.zeros(300)]
        t = np.arange(len(wave) - 4) * (1 + 40e-6)
        wave = np.interp(t, np.arange(len(wave)), wave.real) + 1j * np.interp(t, np.arange(len(wave)), wave.imag)
        rng = np.random.default_rng(40)
        wave = wave + .004 * (rng.normal(size=len(wave)) + 1j * rng.normal(size=len(wave)))
        packets = [rx.decode_candidate(wave.astype(np.complex64), a, c) for a, c in rx.detect_stf(wave)]
        good = [p for p in packets if p is not None and p['fcs_ok']]
        self.assertEqual([p['frame'] for p in good], [frame])

    def test_timing_step_mid_frame_uses_per_symbol_retry(self):
        # Two repeated samples at a symbol boundary step the phase slope
        # halfway through; one linear trend cannot follow that, so only the
        # FCS-gated per-symbol retry recovers the frame.
        wave, frame = ofdm_wave(6, padding=400)
        wave = np.r_[np.zeros(300), wave, np.zeros(300)]
        step = 300 + 320 + 80 * ((len(wave) - 920) // 160)
        wave = np.insert(wave, step, wave[step:step + 2])
        rng = np.random.default_rng(40)
        wave = wave + .004 * (rng.normal(size=len(wave)) + 1j * rng.normal(size=len(wave)))
        packets = [rx.decode_candidate(wave.astype(np.complex64), a, c) for a, c in rx.detect_stf(wave)]
        good = [p for p in packets if p is not None and p['fcs_ok']]
        self.assertEqual([p['frame'] for p in good], [frame])
        self.assertEqual(good[0]['tracking'], 'per_symbol')

    def test_bad_fcs_is_not_accepted(self):
        wave, _ = self.received(6, corrupt=True)
        packets = [rx.decode_candidate(wave, a, c) for a, c in rx.detect_stf(wave)]
        self.assertTrue(any(p and p['status'] == 'bad_fcs' for p in packets))
        self.assertFalse(any(p and p['fcs_ok'] for p in packets))

    def test_bad_signal_parity_is_rejected(self):
        wave, _ = self.received(12, bad_parity=True)
        self.assertFalse(any(rx.decode_candidate(wave, a, c) for a, c in rx.detect_stf(wave)))

    def test_truncated_frame(self):
        wave, _ = self.received(6)
        wave = wave[:800]
        packets = [rx.decode_candidate(wave, a, c) for a, c in rx.detect_stf(wave)]
        self.assertTrue(any(p and p['status'] == 'truncated' for p in packets))

    def test_noise_and_continuous_tone_do_not_create_frames(self):
        rng = np.random.default_rng(4)
        for wave in (rng.normal(size=10000) + 1j * rng.normal(size=10000),
                     np.exp(.1j * np.arange(10000)), np.zeros(10000)):
            self.assertFalse(any(rx.decode_candidate(wave, a, c) for a, c in rx.detect_stf(wave)))

    def test_detector_chunk_boundary_and_40_msps_input(self):
        wave, frame = self.received(6)
        wave = np.r_[np.zeros(699), wave]
        candidates = list(rx.detect_stf(wave, chunk=1000))
        self.assertEqual(sum(bool(p and p['fcs_ok']) for p in
                             (rx.decode_candidate(wave, a, c) for a, c in candidates)), 1)
        up = signal.resample_poly(wave, 2, 1).astype(np.complex64)
        packets, _ = rx.decode_channel(up, 2412, 1)
        self.assertEqual([p['frame'] for p in packets if p['fcs_ok']], [frame])


class HtTests(unittest.TestCase):
    def received(self, wave, seed=0):
        # Same impairments as the legacy tests: multipath, CFO, ~26 dB SNR.
        wave = np.convolve(wave, [1, 0.12 + .08j, 0, -.06j])
        wave = np.r_[np.zeros(300), wave, np.zeros(300)]
        rng = np.random.default_rng(seed)
        wave *= np.exp(1j * (0.7 + 2 * np.pi * 85000 * np.arange(len(wave)) / rx.FS))
        wave += .004 * (rng.normal(size=len(wave)) + 1j * rng.normal(size=len(wave)))
        return wave.astype(np.complex64)

    def decode(self, wave):
        return [p for p in (rx.decode_candidate(wave, a, c) for a, c in rx.detect_stf(wave)) if p]

    def test_all_mcs_single_stream_and_stbc(self):
        for stbc in (False, True):
            for mcs in range(8):
                with self.subTest(mcs=mcs, stbc=stbc):
                    wave, frame = ht_wave(mcs, short_gi=mcs % 2 == 1, stbc=stbc)
                    packets = self.decode(self.received(wave, mcs))
                    self.assertEqual(len(packets), 1)
                    p = packets[0]
                    self.assertTrue(p['fcs_ok'])
                    self.assertEqual(p['frame'], frame)
                    self.assertEqual((p['ofdm_format'], p['mcs'], p['stbc'], p['short_gi']),
                                     ('ht', mcs, int(stbc), mcs % 2 == 1))
                    self.assertEqual(p['rate_mbps'], round(rx.HT_MCS[mcs][0] * (10 / 9 if mcs % 2 else 1), 1))

    def test_ampdu_valid_mpdus_become_records(self):
        _, frame = ht_wave(0)
        bad = frame[:30] + bytes([frame[30] ^ 1]) + frame[31:]
        other = frame[:-5] + b'Z' + frame[-5:-4]
        other += zlib.crc32(other).to_bytes(4, 'little')
        wave, _ = ht_wave(4, mpdus=[frame, bad, other])
        p, = self.decode(self.received(wave))
        self.assertEqual((p['status'], p['ampdu_subframes'], p['ampdu_valid']), ('ok', 3, 2))
        up = signal.resample_poly(self.received(wave), 2, 1).astype(np.complex64)
        packets, _ = rx.decode_channel(up, 2412, 1)
        self.assertEqual([(p['frame'], p['mpdu_index']) for p in packets], [(frame, 0), (other, 1)])

    def test_legacy_fields_with_other_cyclic_shifts_are_not_averaged_in(self):
        # Two antennas, one stream: the second antenna delays the legacy fields
        # by 4 samples and the HT fields by 8, so the L-LTF shows a different
        # channel from the HT-LTF. Averaging them in loses every such frame.
        wave, frame = ht_wave(7)
        second = np.zeros(len(wave) + 16, complex)
        second[4:4 + 560] += wave[:560]
        second[568:568 + len(wave) - 560] += wave[560:]
        packets = self.decode(self.received(np.r_[wave, np.zeros(16)] + (.6 - .5j) * second))
        self.assertEqual([(p['fcs_ok'], p['frame']) for p in packets], [(True, frame)])
        with mock.patch.object(rx, 'LEGACY_MATCH', np.inf):
            self.assertFalse(any(p['fcs_ok'] for p in self.decode(
                self.received(np.r_[wave, np.zeros(16)] + (.6 - .5j) * second))))

    def test_bad_ht_sig_crc_is_rejected(self):
        wave, _ = ht_wave(3, corrupt_sig=True)
        statuses = [p['status'] for p in self.decode(self.received(wave))]
        self.assertEqual(statuses, ['ht_bad_sig'])


class MacTests(unittest.TestCase):
    def test_malformed_information_element_is_ignored(self):
        self.assertEqual(wifi_b.parse_ies(b'\x00\x10abc'), [])

    def test_short_mac_header(self):
        self.assertEqual(wifi_b.describe(b''), ['truncated-MAC-header'])


def dsss_wave(rate=1, short=False, header_signal=None, bad_fcs=False, extra_bytes=0, pbcc=False):
    body = beacon()
    body += bytes(range(extra_bytes))
    frame = body + zlib.crc32(body).to_bytes(4, 'little')
    transmitted = frame[:-1] + bytes([frame[-1] ^ 1]) if bad_fcs else frame
    length = int(np.ceil(len(frame) * 8 / rate))
    sig = header_signal if header_signal is not None else {1: 0x0a, 2: 0x14, 5.5: 0x37, 11: 0x6e}[rate]
    h = [sig >> i & 1 for i in range(8)]
    extension = int(rate == 11 and length * 11 // 8 > len(frame))
    h += [0, 0, 0, int(pbcc), 0, 0, 0, extension] + [(length >> i) & 1 for i in range(16)]
    h += wifi_b.crc16_plcp(h)
    sync = [0] * 56 if short else [1] * 128
    sfd = 0x05CF if short else 0xF3A0
    preamble = sync + [(sfd >> i) & 1 for i in range(16)]
    plain = preamble + h + list(np.unpackbits(np.frombuffer(transmitted, dtype=np.uint8), bitorder='little'))
    coded = [1] * 7
    for bit in plain:
        coded.append(int(bit) ^ coded[-4] ^ coded[-7])
    coded = coded[7:]
    phases, angle = [1 + 0j], 0
    boundary = len(preamble) if short else len(preamble) + 48
    i = 0
    payload_at = len(preamble) + 48
    while i < (payload_at if rate in (5.5, 11) else len(coded)):
        if i < boundary or (not short and rate == 1):
            angle += np.pi * coded[i]
            i += 1
        else:
            pair = tuple(coded[i:i + 2])
            angle += {(0, 0): 0, (0, 1): np.pi / 2, (1, 1): np.pi, (1, 0): -np.pi / 2}[pair]
            i += 2
        phases.append(np.exp(1j * angle))
    chips = (np.array(phases)[:, None] * wifi_b.BARKER).ravel()
    if rate in (5.5, 11):
        # Independent serial encoder from the CCK phase equations.
        cck_chips = []
        width = 4 if rate == 5.5 else 8
        for number, at in enumerate(range(payload_at, len(coded), width)):
            bits = coded[at:at + width]
            angle += {(0, 0): 0, (0, 1): np.pi / 2, (1, 1): np.pi,
                      (1, 0): -np.pi / 2}[tuple(bits[:2])] + (number % 2) * np.pi
            if rate == 5.5:
                p2, p3, p4 = bits[2] * np.pi + np.pi / 2, 0, bits[3] * np.pi
            else:
                p2, p3, p4 = [(2 * bits[j] + bits[j + 1]) * np.pi / 2 for j in [2, 4, 6]]
            phases_cck = [p2 + p3 + p4, p3 + p4, p2 + p4, p4 + np.pi,
                          p2 + p3, p3, p2 + np.pi, 0]
            cck_chips.extend(np.exp(1j * (angle + np.array(phases_cck))))
        chips = np.r_[chips, cck_chips]
    x44 = np.r_[np.zeros(8800), np.repeat(chips, 4), np.zeros(8800)]
    x = signal.resample_poly(x44, 10, 11).astype(np.complex64)
    rng = np.random.default_rng(9)
    x += .002 * (rng.normal(size=len(x)) + 1j * rng.normal(size=len(x)))
    return x, frame


class DsssTests(unittest.TestCase):
    def test_long_and_short_preambles(self):
        for rate, short in [(1, False), (2, False), (2, True)]:
            with self.subTest(rate=rate, short=short):
                wave, frame = dsss_wave(rate, short)
                packets = []
                wifi_b.decode_channel(wave, 2412, 1, packets=packets, quiet=True)
                good = [p for p in packets if p['fcs_ok']]
                self.assertEqual([p['frame'] for p in good], [frame])
                self.assertEqual(good[0]['preamble'], 'short' if short else 'long')

    def test_packet_below_broadband_power_threshold(self):
        wave, frame = dsss_wave(1, False)
        rng = np.random.default_rng(81)
        wave = (.3 * wave + .3 * (rng.normal(size=len(wave)) + 1j * rng.normal(size=len(wave)))).astype(np.complex64)
        baseline, improved = [], []
        wifi_b.decode_channel(wave, 2412, 1, packets=baseline, quiet=True, sync_search=False)
        wifi_b.decode_channel(wave, 2412, 1, packets=improved, quiet=True)
        self.assertFalse(any(p['fcs_ok'] for p in baseline))
        self.assertEqual([p['frame'] for p in improved if p['fcs_ok']], [frame])

    def test_timing_leaves_barker_sidelobe(self):
        # A candidate phase two chips off the Barker peak used to be held there by
        # the +/-2-sample tracker for the whole frame (round six, c06_p1 776 ms).
        def front(x):
            y = signal.resample_poly(x, 11, 10).astype(np.complex64)
            y = signal.fftconvolve(y, signal.firwin(101, 10e6, fs=44e6).astype(np.float32), 'same').astype(np.complex64)
            return signal.fftconvolve(y, np.repeat(wifi_b.BARKER, wifi_b.SPC)[::-1], 'full')[wifi_b.SPS - 1:].astype(np.complex64)
        wave, frame = dsss_wave(1, False, extra_bytes=200)
        clean = front(wave)
        base = 8800 + 20 * wifi_b.SPS
        peak = base + int(np.argmax(np.abs(clean[base:base + 100 * wifi_b.SPS]).reshape(-1, wifi_b.SPS).sum(axis=0)))
        self.assertLessEqual(abs(wifi_b.acquire_timing(clean, peak + 8) - peak), 1)
        self.assertLessEqual(abs(wifi_b.acquire_timing(clean, peak) - peak), 1)
        rng = np.random.default_rng(104)
        sd = np.sqrt(np.mean(np.abs(wave[9000:-9000]) ** 2) / 10 ** .6 / 2)   # +6 dB per sample
        noisy = (wave + sd * (rng.normal(size=len(wave)) + 1j * rng.normal(size=len(wave)))).astype(np.complex64)
        r = wifi_b.decode_ppdu(front(noisy), peak - 40 * wifi_b.SPS, len(clean), phase_hint=8)
        self.assertTrue(r['fcs_ok'])
        self.assertEqual(r['frame'], frame)
        noise = front((rng.normal(size=400000) + 1j * rng.normal(size=400000)).astype(np.complex64))
        jumps = sum(wifi_b.acquire_timing(noise, s) != s for s in range(1000, 390000, 1500))
        self.assertEqual(jumps, 0)

    def test_misleading_acquisition_falls_back_to_unmoved_start(self):
        # Off-tune frames have smeared correlation peaks; a wrong acquisition
        # must not lose a frame the unacquired path decodes (round six, c04_p1).
        wave, frame = dsss_wave(1, False, extra_bytes=200)
        rng = np.random.default_rng(104)
        sd = np.sqrt(np.mean(np.abs(wave[9000:-9000]) ** 2) / 10 ** .6 / 2)   # +6 dB per sample
        wave = (wave + sd * (rng.normal(size=len(wave)) + 1j * rng.normal(size=len(wave)))).astype(np.complex64)
        # Every acquisition lands two chips off: only the unmoved retry can decode.
        with mock.patch.object(wifi_b, 'acquire_timing', lambda c, start, **k: start + 8):
            packets = []
            wifi_b.decode_channel(wave, 2412, 1, packets=packets, quiet=True)
        self.assertEqual([p['frame'] for p in packets if p['fcs_ok']], [frame])

    def test_rake_collects_delayed_path(self):
        # A second path ~1.4 chips late at -3 dB (bench APs showed one at -6 to
        # -9 dB, round seven). At -5 dB the single-lag despreader decoded 1 of
        # these 6; the RAKE fallback decodes all of them.
        wave, frame = dsss_wave(1, False, extra_bytes=200)
        power = np.mean(np.abs(wave[8000:-8000]) ** 2)
        demods = []
        for seed in range(6):
            rng = np.random.default_rng(seed)
            x = wave + np.r_[np.zeros(5), wave[:-5]] * 10 ** -.15 * np.exp(1j * rng.uniform(0, 2 * np.pi))
            sd = np.sqrt(power / 10 ** -.5 / 2)
            x = (x + sd * (rng.normal(size=len(x)) + 1j * rng.normal(size=len(x)))).astype(np.complex64)
            packets = []
            wifi_b.decode_channel(x, 2412, 1, packets=packets, quiet=True)
            valid = [p for p in packets if p['fcs_ok']]
            with self.subTest(seed=seed):
                self.assertEqual([p['frame'] for p in valid], [frame])
            demods += [p.get('demod', 'track') for p in valid]
        self.assertTrue(any(d.endswith('rake') for d in demods))

    def test_dqpsk_carrier_offset(self):
        for short, hz in [(False, 180000), (True, -180000)]:
            with self.subTest(short=short, hz=hz):
                wave, frame = dsss_wave(2, short)
                wave *= np.exp(2j * np.pi * hz * np.arange(len(wave)) / 40000000)
                packets = []
                wifi_b.decode_channel(wave, 2412, 1, packets=packets, quiet=True)
                good = [p for p in packets if p['fcs_ok']]
                self.assertEqual([p['frame'] for p in good], [frame])
                self.assertAlmostEqual(good[0]['cfo_hz'], hz, delta=1000)

    def test_corrupt_payload_is_not_repaired_to_match_crc(self):
        wave, _ = dsss_wave(1, False)
        wave[18000:18040] *= -1
        packets = []
        wifi_b.decode_channel(wave, 2412, 1, packets=packets, quiet=True)
        self.assertTrue(packets)
        self.assertFalse(any(p['fcs_ok'] for p in packets))

    def test_noise_and_zero_samples_do_not_create_dsss_frames(self):
        rng = np.random.default_rng(37)
        for wave in [np.zeros(40000, dtype=np.complex64),
                     (.1 * (rng.normal(size=40000) + 1j * rng.normal(size=40000))).astype(np.complex64)]:
            packets = []
            wifi_b.decode_channel(wave, 2412, 1, packets=packets, quiet=True)
            self.assertFalse(any(p['fcs_ok'] for p in packets))


class OutputTests(unittest.TestCase):
    def test_pbcc_flag_does_not_produce_a_cck_frame(self):
        wave, _ = dsss_wave(11, True, pbcc=True)
        packets = []
        wifi_b.decode_channel(wave, 2412, 1, packets=packets, quiet=True, cck=True)
        self.assertTrue(packets)
        self.assertTrue(all(p['status'] == 'unsupported PBCC payload' for p in packets))
        self.assertFalse(any(p['frame'] or p['fcs_ok'] for p in packets))

    def test_cck_payloads_long_short_cfo_length_extension_and_bad_fcs(self):
        for rate in [5.5, 11]:
            for short in [False, True]:
                for extra in range(4):
                    with self.subTest(rate=rate, short=short, extra=extra):
                        wave, frame = dsss_wave(rate, short, extra_bytes=extra)
                        wave *= np.exp(2j * np.pi * 50000 * np.arange(len(wave)) / 40e6)
                        packets = []
                        wifi_b.decode_channel(wave, 2412, 1, packets=packets, quiet=True, cck=True)
                        self.assertEqual([p['frame'] for p in packets if p['fcs_ok']], [frame])
                wave, _ = dsss_wave(rate, short, bad_fcs=True)
                packets = []
                wifi_b.decode_channel(wave, 2412, 1, packets=packets, quiet=True, cck=True)
                self.assertFalse(any(p['fcs_ok'] for p in packets))

    def test_cck_plcp_is_reported_without_inventing_a_payload(self):
        for sig, rate in [(0x37, 5.5), (0x6e, 11)]:
            wave, _ = dsss_wave(header_signal=sig)
            packets = []
            wifi_b.decode_channel(wave, 2412, 1, packets=packets, quiet=True)
            headers = [p for p in packets if p['rate_mbps'] == rate]
            self.assertTrue(headers)
            self.assertFalse(any(p['fcs_ok'] or p['frame'] for p in headers))
            self.assertTrue(all(p['status'] == 'unsupported CCK payload' for p in headers))

    def test_window_boundaries_do_not_lose_or_duplicate_frames(self):
        for phy, cck in [('dsss', False), ('ofdm', False), ('dsss', True)]:
            with self.subTest(phy=phy, cck=cck), tempfile.TemporaryDirectory() as folder:
                if phy == 'dsss':
                    wave, frame = dsss_wave(11 if cck else 1, cck)
                    if cck:
                        wave = np.r_[np.zeros(5200), wave]  # Put the CCK payload across 0.4 ms.
                else:
                    wave20, frame = ofdm_wave(6)
                    wave = signal.resample_poly(np.r_[np.zeros(7900), wave20, np.zeros(200)], 2, 1)
                iq = np.empty(2 * len(wave), dtype='<i2')
                iq[::2], iq[1::2] = np.round(wave.real * 3000), np.round(wave.imag * 3000)
                path = Path(folder) / 'boundary.cs16'
                iq.tofile(path)
                result, packets = wifi.analyze(path, 2412, [1], phy=phy, window_samples=16000, cck=cck)
                self.assertEqual([p['frame'] for p in packets if p['fcs_ok']], [frame])
                self.assertEqual(result['valid_frames'], 1)

    def test_pcap_contains_only_verified_frames(self):
        frame = beacon()
        good = {'fcs_ok': True, 'time_ms': 1001.25, 'frame': frame}
        bad = {'fcs_ok': False, 'time_ms': 1.0, 'frame': b'bad'}
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'valid.pcap'
            wifi.write_pcap(path, [good, bad])
            raw = path.read_bytes()
        self.assertEqual(struct.unpack('<IHHIIII', raw[:24])[-1], 105)
        self.assertEqual(struct.unpack('<IIII', raw[24:40]), (1, 1250, len(frame), len(frame)))
        self.assertEqual(raw[40:], frame)

    def test_ap_identity_requires_beacon_or_probe_response(self):
        body = beacon()
        frame = body + zlib.crc32(body).to_bytes(4, 'little')
        net = wifi.network(frame)
        self.assertEqual(net['ssid'], 'Test-AP!')
        self.assertEqual(net['bssid'], '02:11:22:33:44:55')
        self.assertIsNone(wifi.network(bytes([0xD4]) + frame[1:]))  # ACK is not an AP advertisement

    def test_unified_decoder_and_json_serialization(self):
        import json
        wave, frame = ofdm_wave(6)
        wave = np.r_[np.zeros(1000), wave, np.zeros(1000)]
        up = signal.resample_poly(wave, 2, 1)
        iq = np.empty(2 * len(up), dtype='<i2')
        iq[::2], iq[1::2] = np.round(up.real * 16000), np.round(up.imag * 16000)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'synthetic.cs16'
            iq.tofile(path)
            result, packets = wifi.analyze(path, 2412, [1], phy='ofdm')
            self.assertEqual(result['valid_frames'], 1)
            self.assertEqual(result['networks'][0]['ssid'], 'Test-AP!')
            json.dumps(result)
            with self.assertRaises(ValueError):
                wifi.analyze(path, 2412, [11], phy='ofdm')

    def test_5ghz_channels_decode_ofdm_only(self):
        self.assertEqual([wifi_b.channel_mhz(c) for c in (1, 13, 14, 36, 149, 177)],
                         [2412, 2472, 2484, 5180, 5745, 5885])
        self.assertEqual([wifi_b.nearest_channel(f) for f in (2437, 2484, 5180, 5790)], [6, 14, 36, 157])
        with self.assertRaises(ValueError):
            wifi_b.channel_mhz(37)
        wave, frame = ofdm_wave(6)
        up = signal.resample_poly(np.r_[np.zeros(1000), wave, np.zeros(1000)], 2, 1)
        iq = np.empty(2 * len(up), dtype='<i2')
        iq[::2], iq[1::2] = np.round(up.real * 16000), np.round(up.imag * 16000)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'synthetic.cs16'
            iq.tofile(path)
            result, _ = wifi.analyze(path, 5180, [36])
            self.assertEqual(result['valid_frames'], 1)
            self.assertEqual([d['phy'] for d in result['diagnostics']], ['OFDM'])
            with self.assertRaises(ValueError):
                wifi.analyze(path, 5180, [36], phy='dsss')


class DcBlockTests(unittest.TestCase):
    """BirriaSDR --chain both slots carry a static DC offset, about (-165, +152) LSB on slot 0."""

    @staticmethod
    def write(folder, x):
        iq = np.empty(2 * len(x), dtype='<i2')
        iq[::2], iq[1::2] = np.round(x.real), np.round(x.imag)
        path = Path(folder) / 'dc.cs16'
        iq.tofile(path)
        return path

    def test_static_offset_frames_decode_with_dc_block(self):
        rng = np.random.default_rng(0)
        for phy in ('ofdm', 'dsss'):
            if phy == 'ofdm':
                wave20, frame = ofdm_wave(6)
                wave = signal.resample_poly(np.r_[np.zeros(4000), wave20, np.zeros(4000)], 2, 1)
            else:
                wave, frame = dsss_wave(1)
                wave = np.r_[np.zeros(8000), wave, np.zeros(8000)]
            wave = wave / np.sqrt(np.mean(np.abs(wave[np.abs(wave) > 1e-6]) ** 2))
            noise = rng.normal(size=len(wave)) + 1j * rng.normal(size=len(wave))
            x = 200 * wave + 32 / np.sqrt(2) * noise + (-165 + 152j)
            for dc_block, window in [(65536, 20_000_000), (4096, 16000)]:
                with self.subTest(phy=phy, dc_block=dc_block), tempfile.TemporaryDirectory() as folder:
                    result, packets = wifi.analyze(self.write(folder, x), 2412, [1], phy=phy,
                                                   window_samples=window, dc_block=dc_block)
                    self.assertEqual([p['frame'] for p in packets if p['fcs_ok']], [frame])
                    self.assertEqual(result['dc_block_samples'], dc_block)

    def test_dc_block_is_window_independent_and_keeps_blanking(self):
        rng = np.random.default_rng(13)
        x = 40 * (rng.normal(size=30000) + 1j * rng.normal(size=30000)) + (-165 + 152j)
        x[10000:12000] = 0  # receiver blanking
        x[20000:] += 30 - 60j  # the offset may step between blocks
        with tempfile.TemporaryDirectory() as folder:
            path = self.write(folder, x)
            whole = wifi.load(path, dc_block=4096)
            np.testing.assert_array_equal(wifi.load(path), wifi.load(path, dc_block=0))
            self.assertEqual(len(whole), len(x))
            self.assertTrue(np.all(whole[10000:12000 + cs16.BLANK_EDGE] == 0))
            self.assertNotEqual(whole[12000 + cs16.BLANK_EDGE], 0)
            self.assertEqual(np.count_nonzero(whole == 0), np.count_nonzero(x == 0) + cs16.BLANK_EDGE)
            for s in range(0, len(x), 4096):
                live = whole[s:s + 4096][whole[s:s + 4096] != 0]
                self.assertLess(abs(live.mean()) * 8192, .01)
            for start, count in [(0, 5), (4095, 2), (5000, 9000), (12001, -1), (12288, 4), (29990, 100)]:
                end = len(x) if count < 0 else start + count
                np.testing.assert_array_equal(wifi.load(path, start, count, dc_block=4096), whole[start:end])
            for bad in (1, 4095, -1):
                with self.assertRaises(ValueError):
                    wifi.analyze(path, 2412, [1], phy='ofdm', dc_block=bad)

    def test_blanking_ramp_leaves_no_offset_spike(self):
        # The receiver brings the whole raw sample, offset included, back on a
        # ramp after a blanking run; subtracting the full offset there would
        # leave a spike of about minus the offset.
        rng = np.random.default_rng(21)
        dc = -48 + 296j
        x = 17 * (rng.normal(size=20000) + 1j * rng.normal(size=20000)) / np.sqrt(2) + dc
        for run in (5000, 9000, 12280):  # the last edge crosses a 4096 boundary
            x[run:run + 46] = 0
            x[run + 46:run + 49] *= np.array([-.05, .61, .09])
        x[15000:15003] = 0  # a short zero run is not blanking and keeps its neighbours
        with tempfile.TemporaryDirectory() as folder:
            path = self.write(folder, x)
            whole = wifi.load(path, dc_block=4096)
            for run in (5000, 9000, 12280):
                self.assertTrue(np.all(whole[run:run + 46 + cs16.BLANK_EDGE] == 0))
            self.assertNotEqual(whole[15003], 0)
            self.assertLess(np.abs(whole).max() * 8192, 6 * 17)
            np.testing.assert_array_equal(wifi.load(path, 12280 + 46 + 1, 100, dc_block=4096),
                                          whole[12327:12427])


def two_chain(weak_amp, seed, rho=.5, h1=1j, delay=.5):
    """birria_rx --chain both-like slots: two strong DSSS frames and a weak one between them.

    Slot 1 sees the signal times h1, delayed by `delay` samples; the noise (32 LSB rms
    per slot) correlates at `rho` between slots, and each slot carries a static DC offset.
    """
    norm = lambda w: w / np.sqrt(np.mean(np.abs(w[np.abs(w) > 1e-6]) ** 2))
    weak, frame = dsss_wave(1)
    strong = [norm(dsss_wave(1, extra_bytes=n)[0]) for n in (8, 16)]
    gap = np.zeros(20000)
    s = np.r_[gap, 2000 * strong[0], gap, weak_amp * norm(weak), gap, 2000 * strong[1], gap]
    rng = np.random.default_rng(seed)
    g0, g1 = [(rng.normal(size=len(s)) + 1j * rng.normal(size=len(s))) / np.sqrt(2) for _ in range(2)]
    f = np.fft.fftfreq(len(s))
    x0 = s + 32 * g0 + (-165 + 152j)
    x1 = np.fft.ifft(np.fft.fft(h1 * s) * np.exp(-2j * np.pi * f * delay)) \
        + 32 * (rho * g0 + np.sqrt(1 - rho ** 2) * g1) + (-49 + 68j)
    return x0, x1, frame


def write_interleaved(folder, x0, x1, name='both.cs16'):
    iq = np.empty((len(x0), 2, 2), dtype='<i2')
    for k, x in enumerate((x0, x1)):
        iq[:, k, 0], iq[:, k, 1] = np.round(x.real), np.round(x.imag)
    path = Path(folder) / name
    iq.tofile(path)
    return path


class CombineTests(unittest.TestCase):
    """--chain both slots: correlated noise and a fractional slot-1 delay (round thirteen)."""

    def test_combined_branch_decodes_a_frame_neither_chain_decodes(self):
        x0, x1, frame = two_chain(7.5, 0)
        with tempfile.TemporaryDirectory() as folder:
            result, packets = wifi.analyze(write_interleaved(folder, x0, x1), 2412, [1], phy='dsss',
                                           dc_block=65536, combine=True)
        valid = [p for p in packets if p['fcs_ok']]
        weak = [p for p in valid if p['frame'] == frame]
        self.assertEqual(len(weak), 1)
        self.assertEqual(weak[0]['branches'], ['mrc'])
        self.assertEqual(len(valid), 3)  # strong frames count once, from every branch
        self.assertTrue(all(p['branches'] == ['wf0', 'wf1', 'mrc'] for p in valid if p['frame'] != frame))
        info = result['combine']['channels']['1']
        self.assertEqual(info['delay_samples'], -.5)  # slot 1 lagged by half a sample
        self.assertGreater(info['noise_correlation'], .3)
        self.assertEqual(result['combine']['branch_valid_frames'],
                         {'wf0': 2, 'wf1': 2, 'mrc': 3, 'mrc_only': 1})
        self.assertEqual(result['valid_frames'], 3)

    def test_whitened_weights_reach_the_optimum_with_correlated_noise(self):
        noise = (1.0, .5, .35 + .2j)  # Rn00, Rn11, E[n0 n1*]
        rn = np.array([[noise[0], noise[2]], [np.conj(noise[2]), noise[1]]])
        for h in (np.array([1, .5j]), np.array([.3, 1 - .4j]), np.array([1, -.8])):
            r = 4 * np.outer(h, np.conj(h)) + rn
            w0, w1 = combine.whitened_weights(np.array([r[0, 0].real]), np.array([r[1, 1].real]),
                                              np.array([r[0, 1]]), noise)
            w = np.array([w0[0], w1[0]])
            self.assertAlmostEqual((np.conj(w) @ rn @ w).real, 1, places=9)
            optimum = 4 * (np.conj(h) @ np.linalg.solve(rn, h)).real
            self.assertAlmostEqual(abs(np.conj(w) @ (2 * h)) ** 2, optimum, places=9)
            self.assertGreater(optimum, 4 * max(abs(h[0]) ** 2 / rn[0, 0], abs(h[1]) ** 2 / rn[1, 1].real))

    def test_combined_stream_is_window_independent_and_validated(self):
        x0, x1, _ = two_chain(7.5, 1)
        x0[60000:61000] = 0  # blanking on both slots stays zero
        x1[60000:61000] = 0
        with tempfile.TemporaryDirectory() as folder:
            path = write_interleaved(folder, x0, x1)
            c = combine.Combiner(path, 0.0, 65536)
            self.assertTrue(c.usable)
            whole = c.samples(0, len(x0))
            self.assertEqual(len(whole), len(x0))
            self.assertTrue(np.all(whole[60000:61000] == 0))
            for start, count in [(0, 500), (39999, 3000), (100000, 50000), (len(x0) - 10, 100)]:
                end = min(len(x0), start + count)
                np.testing.assert_allclose(c.samples(start, count), whole[start:end], atol=1e-6)
            # A dead slot 1 (WF1 off) is not combined; chain 0 alone still decodes.
            dead = write_interleaved(folder, x0, np.zeros_like(x1), 'dead.cs16')
            result, packets = wifi.analyze(dead, 2412, [1], phy='dsss', dc_block=65536, combine=True)
            self.assertFalse(result['combine']['channels']['1']['status'] == 'combined')
            self.assertEqual(len([p for p in packets if p['fcs_ok']]), 2)
            self.assertEqual(result['combine']['branch_valid_frames']['mrc'], 0)
            with self.assertRaises(ValueError):
                wifi.analyze(path, 2412, [1], phy='dsss', combine=True)  # no DC block
            odd = Path(folder) / 'odd.cs16'
            odd.write_bytes(path.read_bytes()[:-4])
            with self.assertRaises(ValueError):
                wifi.analyze(odd, 2412, [1], phy='dsss', dc_block=65536, combine=True)


class SlotTests(unittest.TestCase):
    """--slot K --of N decodes one stream of an interleaved recording without a split copy."""

    @staticmethod
    def streams():
        rng = np.random.default_rng(4)
        norm = lambda w: w / np.sqrt(np.mean(np.abs(w[np.abs(w) > 1e-6]) ** 2))
        (a, frame_a), (b, frame_b) = dsss_wave(1), dsss_wave(1, extra_bytes=12)
        n = max(len(a), len(b)) + 20000
        pad = lambda w: np.r_[np.zeros(9000), w, np.zeros(n - 9000 - len(w))]
        noise = lambda: 32 / np.sqrt(2) * (rng.normal(size=n) + 1j * rng.normal(size=n))
        x0 = 200 * pad(norm(a)) + noise() + (-165 + 152j)
        x1 = 200 * np.roll(pad(norm(b)), 3000) + noise() + (-49 + 68j)
        return x0, x1, frame_a, frame_b

    def test_slot_decodes_match_split_copies(self):
        x0, x1, frame_a, frame_b = self.streams()
        with tempfile.TemporaryDirectory() as folder:
            both = write_interleaved(folder, x0, x1)
            raw = np.fromfile(both, dtype='<i2').reshape(-1, 2)
            for slot, frame in ((0, frame_a), (1, frame_b)):
                split = Path(folder) / f's{slot}.cs16'
                raw[slot::2].tofile(split)
                for dc_block in (0, 4096):
                    for start, count in [(0, -1), (4095, 9000), (len(x0) - 7, 20)]:
                        np.testing.assert_array_equal(
                            wifi.load(both, start, count, dc_block, slot=slot, of=2),
                            wifi.load(split, start, count, dc_block))
                with self.subTest(slot=slot):
                    result, packets = wifi.analyze(both, 2412, [1], phy='dsss', dc_block=65536,
                                                   window_samples=40000, slot=slot, of=2)
                    reference, expected = wifi.analyze(split, 2412, [1], phy='dsss', dc_block=65536,
                                                       window_samples=40000)
                    self.assertEqual([p['frame'] for p in packets if p['fcs_ok']], [frame])
                    self.assertEqual([(p['time_ms'], p['frame']) for p in packets],
                                     [(p['time_ms'], p['frame']) for p in expected])
                    self.assertEqual((result['slot'], result['interleaved_slots']), (slot, 2))
                    self.assertEqual(result['duration_seconds'], reference['duration_seconds'])
            # Read as one stream, the interleaved file is two receivers at 80 MS/s: nothing decodes.
            result, _ = wifi.analyze(both, 2412, [1], phy='dsss', dc_block=65536)
            self.assertEqual(result['valid_frames'], 0)
            for slot, of in ((2, 2), (-1, 2), (0, 0), (1, 1)):
                with self.assertRaises(ValueError):
                    wifi.analyze(both, 2412, [1], phy='dsss', slot=slot, of=of)
            partial = Path(folder) / 'partial.cs16'
            partial.write_bytes(both.read_bytes()[:-4])  # whole I,Q pairs, but not whole slot pairs
            with self.assertRaises(ValueError):
                wifi.analyze(partial, 2412, [1], phy='dsss', of=2)


class ScanTests(unittest.TestCase):
    def test_benchmark_requires_the_reference_channel_in_a_full_sweep(self):
        reference = {'02:11:22:33:44:55': {'Channel': 4}}
        found = [{'bssid': '02:11:22:33:44:55', 'advertised_channel': 6}]
        result = wifi_benchmark.score(found, reference, list(range(1, 12)))
        self.assertEqual(result['matched'], 0)
        self.assertEqual(result['missed_bssids'], list(reference))
        self.assertEqual(result['channel_mismatches'], list(reference))

    def test_zero_runs_and_payload_intersections_across_chunks(self):
        raw = np.ones((100, 2), dtype='<i2')
        raw[0:18] = 0
        raw[29:52] = 0
        raw[60:62] = 0  # Quantization zeros are below the minimum run length.
        raw[80:] = 0
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'gaps.cs16'
            raw.tofile(path)
            runs = analyze_blanking.zero_runs(path, chunk=10)
        np.testing.assert_array_equal(runs, [[0, 18], [29, 52], [80, 100]])
        self.assertEqual(analyze_blanking.payload_gaps(runs, 17, 32),
                         {'runs': 2, 'missing_samples': 4, 'longest_run': 3})
        self.assertEqual(analyze_blanking.payload_gaps(runs, 52, 80),
                         {'runs': 0, 'missing_samples': 0, 'longest_run': 0})

    def test_benchmark_scores_bssids_and_channels_without_using_ssid(self):
        reference = {'02:11:22:33:44:55': {'Channel': 4, 'SSID': 'reference-name'},
                     '02:11:22:33:44:66': {'Channel': 11, 'SSID': 'another-name'}}
        found = [{'bssid': '02:11:22:33:44:55', 'ssid': 'different-name', 'advertised_channel': 4},
                 {'bssid': '02:11:22:33:44:66', 'advertised_channel': 11}]
        result = wifi_benchmark.score(found, reference, [4])
        self.assertEqual(result['expected'], 1)
        self.assertEqual(result['matched_bssids'], ['02:11:22:33:44:55'])
        self.assertEqual(result['missed_bssids'], [])

    def test_capture_retry_and_disk_cleanup(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'new-survey'
            sentinel = Path(folder) / 'existing.cs16'
            sentinel.write_bytes(b'preserve this')
            attempts = []
            def run(command, **kwargs):
                if '--frequency' in command:
                    attempts.append(command)
                    index = next(i for i, text in enumerate(command) if str(text).endswith('.cs16'))
                    path = Path(command[index])
                    with path.open('wb') as f:
                        f.truncate(17 if len(attempts) == 1 else 4 * int(command[index + 1]))
                    return subprocess.CompletedProcess(command, 1 if len(attempts) == 1 else 0, '', 'simulated status')
                report = {'valid_frames': 3, 'networks': [{'bssid': '02:11:22:33:44:55',
                    'ssid': 'synthetic-AP', 'ssid_hex': '73796e7468657469632d4150',
                    'advertised_channel': 1, 'valid_frames': 1, 'phys': ['DSSS'], 'observed_channels': [1]}]}
                Path(command[command.index('--json') + 1]).write_text(json.dumps(report))
                return subprocess.CompletedProcess(command, 0)
            with mock.patch('sys.argv', ['wifi_scan.py', '--channels', '1', '--seconds', '.05', '--passes', '2', '--buffered', '--output', str(out)]), \
                 mock.patch.object(wifi_scan.subprocess, 'run', side_effect=run), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(wifi_scan.main(), 0)
            report = json.loads((out / 'survey.json').read_text())
            self.assertEqual(report['status'], 'complete')
            self.assertTrue(report['buffered'])
            self.assertEqual(report['samples_per_channel'] % 2048, 0)
            self.assertTrue(all('--buffered' in command for command in attempts))
            self.assertEqual(len(report['captures'][0]['attempts']), 2)
            self.assertEqual(len(report['captures']), 2)
            self.assertEqual(report['networks']['02:11:22:33:44:55']['valid_frames'], 2)
            self.assertEqual(report['networks']['02:11:22:33:44:55']['seen_passes'], [1, 2])
            self.assertEqual(list(out.glob('*.cs16')), [])
            self.assertEqual(sentinel.read_bytes(), b'preserve this')

    def test_existing_output_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            with mock.patch('sys.argv', ['wifi_scan.py', '--output', folder]), \
                 contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as exc:
                    wifi_scan.main()
            self.assertEqual(exc.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
