#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sustained-rate and timestamp check through the SoapySDR module.

  SOAPY_SDR_PLUGIN_PATH=build/soapy soapy_rate_test.py [--rate HZ] [--freq MHZ]
      [--seconds S] [--bandwidth HZ] [--channel-filter] [--tone MHZ] [--args ARGS]

Streams CS16 for SECONDS and counts samples, overflows and timeouts. Checks
that timeNs is present, never runs backwards, and between overflows advances
by exactly the samples read at the nominal rate. With --tone, the FFT peak of
the last 256 Ki samples is compared with TONE - FREQ. Exits 1 on any failure.
Adapted from Mtk79ICAP's userspace/SoapyMT7921/soapy_rate_test.py.
"""
import argparse
import sys
import time

import numpy as np
import SoapySDR
from SoapySDR import (SOAPY_SDR_CS16, SOAPY_SDR_HAS_TIME, SOAPY_SDR_OVERFLOW, SOAPY_SDR_RX,
                      SOAPY_SDR_TIMEOUT)

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--rate', type=float, default=40e6)
ap.add_argument('--freq', type=float, default=2437.0, help='centre in MHz')
ap.add_argument('--seconds', type=float, default=10.0)
ap.add_argument('--bandwidth', type=float, help='filter profile in Hz (20e6 or 40e6)')
ap.add_argument('--channel-filter', action='store_true', help='capture after the digital channel filter')
ap.add_argument('--tone', type=float, help='CW tone frequency in MHz')
ap.add_argument('--args', default='driver=birriasdr', help='device arguments')
a = ap.parse_args()

SoapySDR.Device.enumerate()          # loads the modules; make() alone does not
dev = SoapySDR.Device(a.args)        # string form: dict args fail on some bindings
dev.setSampleRate(SOAPY_SDR_RX, 0, a.rate)
dev.setFrequency(SOAPY_SDR_RX, 0, a.freq * 1e6)
if a.bandwidth:
    dev.setBandwidth(SOAPY_SDR_RX, 0, a.bandwidth)
if a.channel_filter:
    dev.writeSetting('channel_filter', 'true')
rate = dev.getSampleRate(SOAPY_SDR_RX, 0)
st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
dev.activateStream(st)
n = 1 << 18
buf = np.zeros(2 * n, np.int16)
got = ovf = tmo = 0
bad_time = []
expected_ns = None
previous_ns = None
t0 = None
tail = []
while True:
    r = dev.readStream(st, [buf], n, timeoutUs=1_000_000)
    if r.ret == SOAPY_SDR_OVERFLOW:
        ovf += 1
        expected_ns = None           # a gap may add lost time; re-anchor
        continue
    if r.ret == SOAPY_SDR_TIMEOUT:
        tmo += 1
        continue
    if r.ret < 0:
        print('readStream error', r.ret)
        break
    if t0 is None:
        t0 = time.monotonic()
    if not r.flags & SOAPY_SDR_HAS_TIME:
        bad_time.append('missing HAS_TIME')
    else:
        if previous_ns is not None and r.timeNs < previous_ns:
            bad_time.append(f'time ran backwards: {r.timeNs} < {previous_ns}')
        if expected_ns is not None and abs(r.timeNs - expected_ns) > 1:
            bad_time.append(f'time {r.timeNs} != expected {expected_ns}')
        previous_ns = r.timeNs
        expected_ns = r.timeNs + round(r.ret * 1e9 / rate)
    got += r.ret
    tail.append(buf[:2 * r.ret].copy())
    while sum(len(t) for t in tail) > 4 * n:
        tail.pop(0)
    if time.monotonic() - t0 >= a.seconds:
        break
dt = time.monotonic() - t0
dev.deactivateStream(st)
dev.closeStream(st)
print(f'{rate / 1e6:.4f} MS/s requested: {got} samples in {dt:.2f} s = {got / dt / 1e6:.3f} MS/s; '
      f'overflows {ovf}, timeouts {tmo}, timestamp problems {len(bad_time)}')
for problem in bad_time[:5]:
    print('  ', problem)
ok = got > 0.95 * rate * a.seconds and not tmo and not bad_time
if a.tone is not None and tail:
    x = np.concatenate(tail)[-2 * n:].astype(np.float32).view(np.complex64)
    sp = np.abs(np.fft.fft(x * np.hanning(len(x))))
    f = np.fft.fftfreq(len(x), 1 / rate)[np.argmax(sp)]
    expected = (a.tone - a.freq) * 1e6
    print(f'tone at {f / 1e6:+.4f} MHz (expected {expected / 1e6:+.4f})')
    ok = ok and abs(f - expected) < 2 * rate / len(x)
sys.exit(0 if ok else 1)
