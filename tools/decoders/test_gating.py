import unittest

import numpy as np

import gating_vs_packets as g


def synthetic(seconds_us=3000, seed=1):
    """Noise with one on-channel and one -5 MHz burst, raw int16 I/Q pairs."""
    rng = np.random.default_rng(seed)
    n = seconds_us * g.BLOCK
    z = (rng.normal(size=n) + 1j * rng.normal(size=n)) * 30
    t = np.arange(n) / g.FS

    def band(a_us, b_us, f0):
        a, b = a_us * g.BLOCK, b_us * g.BLOCK
        w = rng.normal(size=b - a) + 1j * rng.normal(size=b - a)
        w = np.convolve(w, np.ones(4) / 4, mode='same')       # roughly 10 MHz wide
        z[a:b] += 300 * w * np.exp(2j * np.pi * f0 * t[a:b])

    band(500, 1000, 0.0)
    band(1800, 2300, -5e6)
    raw = np.empty((n, 2), np.int16)
    raw[:, 0], raw[:, 1] = np.real(z), np.imag(z)
    return raw


class GatingTests(unittest.TestCase):
    def test_bursts_and_classes(self):
        raw = synthetic()
        power = g.envelope(raw)
        bursts, floor, smooth = g.find_bursts(power)
        self.assertEqual(len(bursts), 2)
        (a0, b0), (a1, b1) = bursts
        self.assertLess(abs(a0 - 500), 6)
        self.assertLess(abs(b1 - 2300), 6)
        noise = g.noise_spectrum(raw, smooth, floor, np.random.default_rng(0))
        self.assertEqual(g.classify(g.burst_spectrum(raw, a0, b0, noise)), 'on')
        self.assertEqual(g.classify(g.burst_spectrum(raw, a1, b1, noise)), 'adjacent')

    def test_zero_runs_do_not_split_bursts(self):
        raw = synthetic()
        for start_us in range(560, 1000, 60):          # 48-sample runs inside burst 1
            raw[start_us * g.BLOCK:start_us * g.BLOCK + 48] = 0
        bursts, _, _ = g.find_bursts(g.envelope(raw))
        self.assertEqual(len(bursts), 2)

    def test_per_burst_timing(self):
        events = np.array([100.0, 525.0, 700.0, 1003.0])
        row = g.per_burst(events, 500, 1000)
        self.assertEqual(row['runs_inside'], 2)
        self.assertEqual(row['first_run_us'], 25.0)
        self.assertEqual(row['runs_after_skip'], 1)
        self.assertEqual(row['resume_us'], 3.0)
        self.assertEqual(row['prior_us'], 400.0)

    def test_rate_curve_counts_only_active_time(self):
        curve = g.rate_curve(np.array([510.0, 1050.0]), [(500, 600)], 'onset', (-100, 200), bin_us=50)
        self.assertEqual(curve['runs'], [0, 0, 1, 0, 0, 0])
        self.assertEqual(curve['exposure_us'][3], 50.0)   # 50..100 us still inside
        self.assertEqual(curve['exposure_us'][4], 0.0)    # beyond the 100 us burst


if __name__ == '__main__':
    unittest.main()
