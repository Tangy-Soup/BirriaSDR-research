"""Front-end filters for complex64 streams: a compiled resample_poly and an overlap-add FIR.

`resample_poly(x, up, down)` returns exactly what
`scipy.signal.resample_poly(x, up, down).astype(np.complex64)` returns for a
1-D complex64 `x` (default Kaiser window, zero padding), bit for bit. SciPy
casts the filter to complex64 and accumulates each output in single precision
over its taps in ascending input order. The numba kernel does the same, with
real taps: a complex64 tap with zero imaginary part multiplies exactly like
a real float32 one. It runs 3-5 times faster. Without numba it falls back to SciPy.

`fir(x, h, mode)` is `scipy.signal.fftconvolve(x, h, mode)` cast to complex64,
computed by overlap-add (`oaconvolve`). Short kernels on long streams then use
small FFTs instead of one transform the length of the window. The result
differs from fftconvolve by single-precision rounding only, and is closer to a
double-precision convolution (see test_resample.py).
"""
import numpy as np
from scipy import signal

try:
    from numba import njit
except ImportError:          # pragma: no cover - numba is in requirements.txt
    njit = None

_filters = {}


def _output_len(len_h, n_in, up, down):
    """Length of upfirdn's full output (scipy.signal._upfirdn._output_len)."""
    return ((n_in - 1) * up + len_h - 1) // down + 1


def _design(up, down):
    """resample_poly's padded filter, phase-transposed and flipped as upfirdn stores it."""
    key = (up, down)
    if key not in _filters:
        max_rate = max(up, down)
        half_len = 10 * max_rate
        # SciPy casts the float64 design to x's dtype (complex64), then scales by up.
        h = signal.firwin(2 * half_len + 1, 1. / max_rate, window=('kaiser', 5.0)).astype(np.float32)
        h *= np.float32(up)
        n_pre_pad = down - half_len % down
        n_pre_remove = (half_len + n_pre_pad) // down
        # resample_poly grows a post pad until the output covers n_out + n_pre_remove samples.
        # That depends on the input length only through a fixed margin, so solve for it per call.
        _filters[key] = (h, n_pre_pad, n_pre_remove)
    return _filters[key]


def _transposed(h, up):
    """upfirdn's internal arrangement: per phase, the taps reversed (scipy _pad_h)."""
    full = np.zeros(len(h) + (-len(h) % up), np.float32)
    full[:len(h)] = h
    return np.ascontiguousarray(full.reshape(-1, up).T[:, ::-1].ravel())


BLOCK = 16   # outputs of one filter phase accumulated side by side (SIMD across outputs)

if njit is not None:
    @njit(cache=True, nogil=True)
    def _upfirdn(x_re, x_im, h, up, down, out_re, out_im, first, count):
        """upfirdn's constant-zero-mode loop for outputs first..first+count-1.

        Each output accumulates from zero over ascending input index with its
        phase's flipped taps, as in SciPy's _apply_impl, so the sums round the
        same way. Output y uses phase (y * down) % up and ends at input
        (y * down) // up; outputs y, y + up, y + 2 up ... share a phase and
        step down inputs apart, so BLOCK of them are summed together.
        """
        len_x = len(x_re)
        per_phase = len(h) // up
        acc_re = np.zeros(BLOCK, np.float32)
        acc_im = np.zeros(BLOCK, np.float32)
        end = first + count
        for r in range(up):
            y = first + (r - first) % up
            h0 = (y * down) % up * per_phase
            while y < end:
                x_idx = (y * down) // up
                n = min(BLOCK, (end - 1 - y) // up + 1)
                lo = x_idx - per_phase + 1
                if n == BLOCK and lo >= 0 and x_idx + down * (BLOCK - 1) < len_x:
                    acc_re[:] = 0
                    acc_im[:] = 0
                    for i in range(per_phase):
                        tap = h[h0 + i]
                        at = lo + i
                        for j in range(BLOCK):
                            acc_re[j] = acc_re[j] + x_re[at + down * j] * tap
                            acc_im[j] = acc_im[j] + x_im[at + down * j] * tap
                    for j in range(BLOCK):
                        out_re[y - first + up * j] = acc_re[j]
                        out_im[y - first + up * j] = acc_im[j]
                else:
                    # Edges (and a short last block): one output at a time, the zero
                    # padding outside x skipped as SciPy skips or adds zeros.
                    for j in range(n):
                        yj = y + up * j
                        xj = (yj * down) // up
                        h_idx = h0
                        start = xj - per_phase + 1
                        if start < 0:
                            h_idx -= start
                            start = 0
                        a = np.float32(0)
                        b = np.float32(0)
                        for i in range(start, min(xj + 1, len_x)):
                            a = a + x_re[i] * h[h_idx]
                            b = b + x_im[i] * h[h_idx]
                            h_idx += 1
                        out_re[yj - first] = a
                        out_im[yj - first] = b
                y += up * n


def resample_poly(x, up, down):
    """scipy.signal.resample_poly(x, up, down).astype(np.complex64) for a 1-D complex64 x."""
    x = np.asarray(x)
    if njit is None or x.dtype != np.complex64 or x.ndim != 1:
        return signal.resample_poly(x, up, down).astype(np.complex64)
    from math import gcd
    g = gcd(up, down)
    up, down = up // g, down // g
    if up == down == 1:
        return x.copy()
    n_in = len(x)
    n_out = n_in * up // down + bool(n_in * up % down)
    h, n_pre_pad, n_pre_remove = _design(up, down)
    n_post_pad = 0
    while _output_len(len(h) + n_pre_pad + n_post_pad, n_in, up, down) < n_out + n_pre_remove:
        n_post_pad += 1
    padded = np.concatenate((np.zeros(n_pre_pad, np.float32), h, np.zeros(n_post_pad, np.float32)))
    flipped = _transposed(padded, up)
    pairs = x.view(np.float32).reshape(-1, 2)
    x_re, x_im = np.ascontiguousarray(pairs[:, 0]), np.ascontiguousarray(pairs[:, 1])
    out = np.empty(n_out, np.complex64)
    o = out.view(np.float32).reshape(-1, 2)
    re, im = np.empty(n_out, np.float32), np.empty(n_out, np.float32)
    _upfirdn(x_re, x_im, flipped, up, down, re, im, n_pre_remove, n_out)
    o[:, 0], o[:, 1] = re, im
    return out


def fir(x, h, mode='full'):
    """fftconvolve(x, h, mode) as complex64, by overlap-add."""
    return signal.oaconvolve(x, h, mode).astype(np.complex64, copy=False)
