"""Protocol and trust-boundary tests for indirect advertisement identities."""
import unittest
import zlib
from unittest import mock
import tempfile
from pathlib import Path

import wifi
from wifi_advertisements import advertised_neighbors, reduced_neighbors


def parent(value, subtype=0x80):
    frame = bytes([subtype, 0, 0, 0]) + bytes.fromhex('ffffffffffff0211223344550211223344550000')
    frame += bytes(12) + bytes([201, len(value)]) + value
    return frame + zlib.crc32(frame).to_bytes(4, 'little')


class AdvertisementTests(unittest.TestCase):
    def test_multiple_neighbors_and_field_lengths(self):
        first = b'\xff' + bytes.fromhex('020000000001') + bytes.fromhex('12345678') + b'\x40\xfe'
        second = b'\xff' + bytes.fromhex('020000000002') + bytes.fromhex('87654321') + b'\x40\xfe'
        report = b'\x10\x0d\x86\x25' + first + second
        for subtype in (0x80, 0x50):
            neighbors, errors = advertised_neighbors(parent(report, subtype))
            self.assertEqual(errors, [])
            self.assertEqual([n['bssid'] for n in neighbors], ['02:00:00:00:00:01', '02:00:00:00:00:02'])
            self.assertEqual(neighbors[0]['operating_class'], 134)
            self.assertEqual(neighbors[0]['reported_channel'], 37)
            self.assertFalse(neighbors[0]['directly_observed'])
        for length in (7, 8, 9, 11, 12, 13, 16):
            self.assertEqual(len(reduced_neighbors(bytes([0, length, 81, 1]) + first[:length].ljust(length, b'\x00'))), 1)

    def test_corruption_truncation_and_unknown_layout(self):
        info = b'\xff' + bytes.fromhex('020000000001') + bytes(6)
        good = b'\x00\x0d\x86\x25' + info
        for cut in range(1, len(good)):
            neighbors, errors = advertised_neighbors(parent(good[:cut]))
            self.assertEqual(neighbors, [])
            self.assertTrue(errors)
        frame = parent(good)
        self.assertEqual(advertised_neighbors(frame[:-1] + bytes([frame[-1] ^ 1])), ([], []))
        protected = frame[:1] + b'\x40' + frame[2:-4]
        protected += zlib.crc32(protected).to_bytes(4, 'little')
        self.assertEqual(advertised_neighbors(protected), ([], []))
        for header in (1, 2, 3):
            self.assertEqual(reduced_neighbors(bytes([header]) + good[1:]), [])
        self.assertEqual(reduced_neighbors(b'\x00\x05\x51\x01' + bytes(5)), [])
        # Valid first block followed by a malformed block cannot leak identities.
        with self.assertRaises(ValueError):
            reduced_neighbors(good + b'\x00')

    def test_neighbors_do_not_enter_direct_network_list(self):
        frame = parent(b'\x00\x07\x51\x06\xff' + bytes.fromhex('020000000001'))
        packet = {'time_ms': 0., 'channel': 1, 'phy': 'OFDM', 'fcs_ok': True, 'frame': frame}
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'small.cs16'
            path.write_bytes(bytes(4096 * 4))
            with mock.patch.object(wifi, 'decode_window', return_value=([packet], [])):
                report, _ = wifi.analyze(path, 2412, [1])
        self.assertEqual([n['bssid'] for n in report['networks']], ['02:11:22:33:44:55'])
        self.assertEqual([n['bssid'] for n in report['advertised_neighbors']], ['02:00:00:00:00:01'])


if __name__ == '__main__':
    unittest.main()
