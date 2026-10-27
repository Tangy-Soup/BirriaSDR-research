import os
import unittest
from pathlib import Path
from unittest import mock
import zlib

import numpy as np

import wifi
import wifi_b
from diagnose_capture import gap_structure
from probe_preamble_carrier import analyze_refined


class ResearchDiagnosticsTests(unittest.TestCase):
    def test_boundary_phase_counts_and_crossings(self):
        report = gap_structure(np.array([[2040, 2088], [4096, 4144], [65530, 65578]]), 70000)
        self.assertEqual(report['zero_run_samples'], 144)
        self.assertEqual(report['boundary_phases']['2048']['starts_at_boundary'], 1)
        self.assertEqual(report['boundary_phases']['2048']['runs_crossing_boundary'], 2)
        self.assertEqual(report['boundary_phases']['65536']['runs_crossing_boundary'], 1)
        self.assertEqual(report['start_spacing_samples'][0]['value'], 2056)

    def test_experimental_override_restored_on_error(self):
        decode, carrier = wifi_b.decode_ppdu, wifi_b.correct_dsss_carrier
        with mock.patch.object(wifi, 'analyze', side_effect=RuntimeError('injected decode failure')):
            with self.assertRaisesRegex(RuntimeError, 'injected'):
                analyze_refined(Path('unused.cs16'), 11)
        self.assertIs(wifi_b.decode_ppdu, decode)
        self.assertIs(wifi_b.correct_dsss_carrier, carrier)

    def test_over_air_fixture_with_independent_fcs(self):
        # An over-air channel-11 capture centred at 2462 MHz holding one beacon.
        # Recorded fixtures carry real network identities, so none ship here.
        path = Path(os.environ.get('BIRRIASDR_RX_FIXTURE', ''))
        if not path.is_file():
            self.skipTest('Set BIRRIASDR_RX_FIXTURE to an over-air channel-11 capture')
        report, packets = wifi.analyze(path, 2462, [11])
        good = [p for p in packets if p['fcs_ok']]
        self.assertEqual(len(good), 1)
        frame = good[0]['frame']
        self.assertEqual(zlib.crc32(frame[:-4]), int.from_bytes(frame[-4:], 'little'))
        self.assertEqual(report['networks'][0]['advertised_channel'], 11)


if __name__ == '__main__':
    unittest.main()
