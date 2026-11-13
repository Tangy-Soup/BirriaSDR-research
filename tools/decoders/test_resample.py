import unittest

import numpy as np
from scipy import signal

import resample


def noise(n, seed):
    rng = np.random.default_rng(seed)
    return (rng.normal(size=n) + 1j * rng.normal(size=n)).astype(np.complex64)


class ResamplePolyTests(unittest.TestCase):
    RATES = ((11, 10), (1, 2), (1, 10), (2, 1), (3, 7), (22, 20))

    def test_bit_identical_to_scipy(self):
        for n in (1, 2, 3, 10, 11, 20, 21, 40, 41, 43, 222, 1001, 5003):
            x = noise(n, n)
            for up, down in self.RATES:
                with self.subTest(n=n, up=up, down=down):
                    want = signal.resample_poly(x, up, down).astype(np.complex64)
                    got = resample.resample_poly(x, up, down)
                    self.assertEqual(got.dtype, np.complex64)
                    self.assertEqual(got.shape, want.shape)
                    self.assertEqual(got.tobytes(), want.tobytes())

    def test_bit_identical_with_zero_runs_and_large_values(self):
        x = noise(20_000, 7) * np.float32(8000)
        x[3000:3049] = 0      # blanking runs, as in BirriaSDR captures
        x[-49:] = 0
        for up, down in ((11, 10), (1, 2)):
            want = signal.resample_poly(x, up, down).astype(np.complex64)
            self.assertEqual(resample.resample_poly(x, up, down).tobytes(), want.tobytes())

    def test_other_dtypes_fall_back_to_scipy(self):
        x = noise(500, 3).astype(np.complex128)
        want = signal.resample_poly(x, 11, 10).astype(np.complex64)
        np.testing.assert_array_equal(resample.resample_poly(x, 11, 10), want)

    def test_equal_rates_copy(self):
        x = noise(50, 4)
        y = resample.resample_poly(x, 3, 3)
        np.testing.assert_array_equal(y, x)
        self.assertIsNot(y, x)


class FirTests(unittest.TestCase):
    def test_matches_fftconvolve_and_is_no_less_accurate(self):
        x = noise(300_000, 5) * np.float32(1000)
        lowpass = signal.firwin(101, 10e6, fs=44e6).astype(np.float32)
        barker = np.repeat(np.array([1, -1, 1, 1, -1, 1, 1, 1, -1, -1, -1], np.float32), 4)[::-1]
        for h, mode in ((lowpass, 'same'), (barker, 'full')):
            with self.subTest(taps=len(h), mode=mode):
                exact = signal.oaconvolve(x.astype(np.complex128), h.astype(np.float64), mode)
                old = signal.fftconvolve(x, h, mode).astype(np.complex64)
                new = resample.fir(x, h, mode)
                self.assertEqual(new.dtype, np.complex64)
                self.assertEqual(new.shape, old.shape)
                scale = np.sqrt(np.mean(np.abs(exact) ** 2))
                err_old = np.sqrt(np.mean(np.abs(old - exact) ** 2)) / scale
                err_new = np.sqrt(np.mean(np.abs(new - exact) ** 2)) / scale
                self.assertLess(err_new, 1e-6)
                self.assertLessEqual(err_new, err_old * 1.05)


if __name__ == '__main__':
    unittest.main()
