import unittest

import numpy as np
from scipy.signal import resample_poly

import ble
from cs16 import FS, blanking_mask


def adv_waveform(pdu, chan, spc=4):
    """GFSK (BT 0.5, h 0.5) LE 1M packet at spc samples per symbol: preamble, AA, whitened PDU + CRC."""
    pdu_bits = np.unpackbits(np.frombuffer(pdu, np.uint8), bitorder='little').astype(np.int8)
    crc = ble.crc24(pdu_bits)
    body = np.r_[pdu_bits, [(crc >> (23 - i)) & 1 for i in range(24)]].astype(np.int8)
    body ^= ble.whitening(chan, len(body))
    bits = np.r_[[0, 1] * 4, ble.bits_lsb(ble.AA, 32), body, [0] * 4]
    t = np.arange(-1.5 * spc, 1.5 * spc + 1) / spc
    sigma = np.sqrt(np.log(2)) / (2 * np.pi * 0.5)
    g = np.exp(-t ** 2 / (2 * sigma ** 2))
    freq = np.convolve(np.repeat(2.0 * bits - 1, spc), g / g.sum(), 'same')
    return np.exp(1j * np.cumsum(freq * np.pi / 2 / spc))


def capture(pdu, chan, center, snr_db=20, pad_s=2e-3, seed=0):
    """The packet at FS, offset from center, with pad_s of noise on each side."""
    rng = np.random.default_rng(seed)
    y = resample_poly(adv_waveform(pdu, chan), int(FS / 4e6), 1)
    pad = int(pad_s * FS)
    y = np.r_[np.zeros(pad), y, np.zeros(pad)]
    n = np.arange(len(y))
    y = y * np.exp(2j * np.pi * (ble.ADV_FREQ[chan] - center) * n / FS)
    # resample_poly keeps unit amplitude; the decoder's 700 kHz filter passes ~1/20 of the 40 MHz noise
    p = 10 ** (-snr_db / 10) * FS / 1.4e6
    y = y + np.sqrt(p / 2) * (rng.standard_normal(len(y)) + 1j * rng.standard_normal(len(y)))
    return (y * 1000).astype(np.complex64)


class LegacyAdvTests(unittest.TestCase):
    def test_37_byte_payload_is_crc_valid(self):
        """FAA Remote ID adverts always fill the 37-byte legacy payload (42 bytes with header and CRC)."""
        adva = bytes.fromhex('c0ffee123456')
        message = bytes([0x02, 0x12]) + b'1581F5FKD229400B'.ljust(20, b'\0') + bytes(3)
        ad = bytes([0x1E, 0x16, 0xFA, 0xFF, 0x0D, 0x07]) + message
        payload = adva + ad
        self.assertEqual(len(payload), 37)
        pdu = bytes([0x42, len(payload)]) + payload
        center = 2412e6
        x = capture(pdu, 37, center)
        found = ble.decode_channel(x, blanking_mask(x), center, 37)
        self.assertEqual(len(found), 1)
        _, chan, ok, blanked, raw, snr = found[0]
        self.assertEqual(chan, 37)
        self.assertTrue(ok)
        self.assertFalse(blanked)
        self.assertEqual(raw, pdu)
        self.assertGreater(snr, 10)


if __name__ == '__main__':
    unittest.main()
