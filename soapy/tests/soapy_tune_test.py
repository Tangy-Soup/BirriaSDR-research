#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Retune through the SoapySDR module while streaming.

  SOAPY_SDR_PLUGIN_PATH=build/soapy soapy_tune_test.py [--tone MHZ] [--args ARGS]
      STEP [STEP ...]

STEP is a centre frequency in MHz. The stream stays active; each step calls
setFrequency, then reads 1 Mi samples. Prints the time from setFrequency to
the first sample read after it, the overflows, and with --tone the FFT peak
(expected TONE - FREQ). BirriaSDR retunes live with no host NCO, so the
first sample read after the call can still predate the new frequency: the
peak comes from the whole block. Adapted from Mtk79ICAP's soapy_tune_test.py.
"""
import argparse
import sys
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CF32, SOAPY_SDR_OVERFLOW, SOAPY_SDR_RX, SOAPY_SDR_TIMEOUT

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('steps', nargs='+', type=float, help='centre frequencies in MHz')
ap.add_argument('--tone', type=float, help='CW tone frequency in MHz')
ap.add_argument('--rate', type=float, default=40e6)
ap.add_argument('--args', default='driver=birriasdr', help='device arguments')
a = ap.parse_args()

SoapySDR.Device.enumerate()
dev = SoapySDR.Device(a.args)
dev.setSampleRate(SOAPY_SDR_RX, 0, a.rate)
dev.setFrequency(SOAPY_SDR_RX, 0, a.steps[0] * 1e6)
rate = dev.getSampleRate(SOAPY_SDR_RX, 0)
st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0])
dev.activateStream(st)
buf = np.zeros(65536, np.complex64)


def collect(count, t_start):
    out, first, ovf, got = [], None, 0, 0
    while got < count:
        r = dev.readStream(st, [buf], len(buf), timeoutUs=1_000_000)
        if r.ret == SOAPY_SDR_OVERFLOW:
            ovf += 1
            continue
        if r.ret == SOAPY_SDR_TIMEOUT or r.ret < 0:
            raise SystemExit(f'readStream: {r.ret}')
        if first is None:
            first = time.monotonic() - t_start
        out.append(buf[:r.ret].copy())
        got += r.ret
    return np.concatenate(out)[:count], first, ovf


collect(1 << 21, time.monotonic())   # settle after activation
failures = 0
for step in a.steps:
    t0 = time.monotonic()
    dev.setFrequency(SOAPY_SDR_RX, 0, step * 1e6)
    call_ms = (time.monotonic() - t0) * 1e3
    actual = dev.getFrequency(SOAPY_SDR_RX, 0)
    x, first, ovf = collect(1 << 20, t0)
    line = (f'{step:9.3f} MHz: getFrequency {actual / 1e6:.6f}, setFrequency {call_ms:.1f} ms, '
            f'first sample {first * 1e3:.1f} ms, overflows {ovf}')
    if abs(actual - step * 1e6) > 1:
        failures += 1
    if a.tone is not None:
        spectrum = np.abs(np.fft.fftshift(np.fft.fft(x * np.hanning(len(x)))))
        off = (np.argmax(spectrum) - len(x) / 2) / len(x) * rate
        expected = (a.tone - step) * 1e6
        line += f'; tone at {off / 1e6:+.4f} MHz (expected {expected / 1e6:+.4f})'
        if abs(off - expected) > 2 * rate / len(x):
            failures += 1
    print(line)
dev.deactivateStream(st)
dev.closeStream(st)
sys.exit(1 if failures else 0)
