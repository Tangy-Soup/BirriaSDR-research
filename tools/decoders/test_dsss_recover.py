import unittest
import zlib

import numpy as np

import dsss_recover as R


def synthetic(nbytes=120, seed=0, start=64, noise=0.05):
    """A DBPSK frame with valid FCS: returns (symbols, start, length, frame)."""
    rng = np.random.default_rng(seed)
    body = rng.integers(0, 256, nbytes - 4, dtype=np.uint8).tobytes()
    frame = body + zlib.crc32(body).to_bytes(4, 'little')
    length = nbytes * 8
    d = np.unpackbits(np.frombuffer(frame, np.uint8), bitorder='little')
    raw = np.zeros(start + length + 1, np.int8)
    raw[:start] = rng.integers(0, 2, start)
    for i in range(start, start + length):
        raw[i] = d[i - start] ^ raw[i - 4] ^ raw[i - 7]
    b = np.zeros(len(raw) + 1, np.int8)
    for j in range(len(raw)):
        b[j + 1] = b[j] ^ raw[j]
    phase = 0.3 + 2e-3 * np.arange(len(b))          # slow residual carrier rotation
    sym = (1 - 2 * b) * np.exp(1j * phase)
    sym = sym + noise * (rng.normal(size=len(b)) + 1j * rng.normal(size=len(b)))
    return sym.astype(np.complex64), start, length, frame


def differential_frame(sym, start, length):
    raw = (np.real(sym[1:] * np.conj(sym[:-1])) < 0).astype(np.int8)
    return R.descramble_frame(raw, start, length)


class RecoverTests(unittest.TestCase):
    def test_clean_frame_needs_no_unknowns(self):
        sym, start, length, frame = synthetic()
        self.assertEqual(differential_frame(sym, start, length), frame)
        fixed, info = R.recover(frame, sym, start, length)
        self.assertEqual(fixed, frame)
        self.assertEqual(info['unknowns'], 0)
        self.assertEqual(info['check_bits_left'], 32)

    def test_weak_wrong_symbols_are_corrected(self):
        sym, start, length, frame = synthetic(seed=1)
        bad = sym.copy()
        for k in (start + 50, start + 300, start + 701):  # weak, sign-flipped symbols
            bad[k] = -0.15 * bad[k]
        broken = differential_frame(bad, start, length)
        self.assertNotEqual(broken, frame)
        fixed, info = R.recover(broken, bad, start, length)
        self.assertEqual(fixed, frame)
        self.assertLessEqual(info['unknowns'], 6)
        self.assertGreaterEqual(info['check_bits_left'], 32 - R.MAX_UNKNOWNS)

    def test_erased_symbols_are_preferred(self):
        sym, start, length, frame = synthetic(seed=2)
        bad = sym.copy()
        erased = np.zeros(len(sym))
        for k in (start + 120, start + 121, start + 480):
            bad[k] = 0.4 * np.exp(1j * 2.0)                # garbage where samples were zero
            erased[k] = 1.0
        broken = differential_frame(bad, start, length)
        fixed, _ = R.recover(broken, bad, start, length, erased)
        self.assertEqual(fixed, frame)

    def test_strong_errors_beyond_reach_are_rejected(self):
        sym, start, length, frame = synthetic(seed=3)
        bad = sym.copy()
        rng = np.random.default_rng(9)
        for k in rng.choice(np.arange(start + 1, start + length), 25, replace=False):
            bad[k] = -bad[k]                               # confident wrong symbols
        broken = differential_frame(bad, start, length)
        fixed, info = R.recover(broken, bad, start, length)
        self.assertIsNone(fixed)
        self.assertEqual(info['reason'], 'no solution within limit')

    def test_plcp_history_is_never_flipped(self):
        sym, start, length, frame = synthetic(seed=4)
        bad = sym.copy()
        bad[start + 10] = -0.1 * bad[start + 10]
        fixed, info = R.recover(differential_frame(bad, start, length), bad, start, length)
        self.assertEqual(fixed, frame)
        for raws in info.get('flips', []):
            self.assertTrue(all(r >= start for r in raws))

    def test_unsupported_geometry(self):
        sym, start, length, frame = synthetic()
        self.assertIsNone(R.recover(frame, sym, 3, length)[0])
        self.assertIsNone(R.recover(frame[:-1], sym, start, length)[0])



class ErasureTierTests(unittest.TestCase):
    @staticmethod
    def beacon(seq, bssid=bytes.fromhex('020000000069'), ssid=b'TestAP', channel=11):
        body = (b'\x80\x00\x00\x00' + b'\xff' * 6 + bssid + bssid + (seq << 4).to_bytes(2, 'little')
                + bytes(8) + b'\x64\x00\x11\x04' + bytes([0, len(ssid)]) + ssid + bytes([3, 1, channel]))
        return body + zlib.crc32(body).to_bytes(4, 'little')

    def packet(self, t_ms, seq, check_bits=28, **kw):
        return {'channel': 11, 'sync_time_ms': t_ms, 'fcs_ok': False, 'frame': b'',
                'recovered_frame': self.beacon(seq, **kw).hex(),
                'recovery': {'check_bits_left': check_bits}}

    def test_listing_rules(self):
        import wifi
        one = wifi.erasure_supported([self.packet(10.0, 1)])
        self.assertEqual(len(one), 1)
        self.assertFalse(one[0]['listed'])                       # a single recovery is not enough
        two = wifi.erasure_supported([self.packet(10.0, 1), self.packet(112.4, 2)])
        self.assertTrue(two[0]['listed'])
        dup = wifi.erasure_supported([self.packet(10.0, 1), self.packet(10.001, 1)])
        self.assertEqual(dup[0]['recovered_frames'], 1)          # same SFD counted once
        weak = wifi.erasure_supported([self.packet(10.0, 1, 12), self.packet(112.4, 2, 12)])
        self.assertEqual(weak, [])                               # fewer than 16 check bits left
        clash = wifi.erasure_supported([self.packet(10.0, 1), self.packet(112.4, 2, ssid=b'Other')])
        self.assertFalse(clash[0]['listed'])                     # inconsistent identity

    def test_valid_frame_at_same_sfd_supersedes(self):
        import wifi
        valid = {'channel': 11, 'sync_time_ms': 10.0, 'fcs_ok': True, 'frame': self.beacon(1)}
        self.assertEqual(wifi.erasure_supported([valid, self.packet(10.0, 1)]), [])


if __name__ == '__main__':
    unittest.main()
