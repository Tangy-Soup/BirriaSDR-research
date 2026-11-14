#!/usr/bin/env python3
"""SoapySDR part of tools/hw_smoke.sh --soapy (chain 0, shipped ranges only).

Opens driver=birriasdr, checks the frequency and gain ranges, does a dB gain
round trip while idle, streams 2 s at 40 MSps on 2437 MHz with the default
automatic gain (host AGC), then 1 s at 5240 MHz, and closes. Exits nonzero
on any failed check; the last line printed is the summary.
"""
import sys

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CF32, SOAPY_SDR_OVERFLOW, SOAPY_SDR_RX, SOAPY_SDR_TIMEOUT

fails = []


def check(ok, text):
    print(('ok   ' if ok else 'FAIL ') + text)
    if not ok:
        fails.append(text)


dev = SoapySDR.Device('driver=birriasdr')
ranges = [(r.minimum() / 1e6, r.maximum() / 1e6) for r in dev.getFrequencyRange(SOAPY_SDR_RX, 0)]
print('frequency ranges MHz', ranges)
check(len(ranges) == 2 and ranges[0][1] < 3000 < ranges[1][0], 'two frequency bands')
check(ranges[1][1] >= 6615, 'high band reaches 6615 MHz')

dev.setSampleRate(SOAPY_SDR_RX, 0, 40e6)
dev.setFrequency(SOAPY_SDR_RX, 0, 2437e6)
print('settings', {s.key: dev.readSetting(s.key) for s in dev.getSettingInfo()})
check(dev.hasGainMode(SOAPY_SDR_RX, 0), 'has automatic gain mode')

gr = dev.getGainRange(SOAPY_SDR_RX, 0)
print('gain range dB', gr.minimum(), gr.maximum())
dev.setGainMode(SOAPY_SDR_RX, 0, False)
for want in (gr.minimum(), (gr.minimum() + gr.maximum()) / 2, gr.maximum()):
    dev.setGain(SOAPY_SDR_RX, 0, want)
    got = dev.getGain(SOAPY_SDR_RX, 0)
    check(abs(got - want) <= 2.5, f'gain round trip {want:.1f} -> {got:.1f} dB')
dev.setGainMode(SOAPY_SDR_RX, 0, True)


def stream(freq, seconds):
    dev.setFrequency(SOAPY_SDR_RX, 0, freq)
    st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0])
    dev.activateStream(st)
    buf = np.zeros(65536, np.complex64)
    got = ovf = 0
    power = 0.0
    want = int(seconds * 40e6)
    while got < want:
        r = dev.readStream(st, [buf], len(buf), timeoutUs=2_000_000)
        if r.ret == SOAPY_SDR_OVERFLOW:
            ovf += 1
            continue
        if r.ret == SOAPY_SDR_TIMEOUT or r.ret < 0:
            check(False, f'readStream at {freq / 1e6:.0f} MHz returned {r.ret}')
            break
        power += float(np.sum(np.abs(buf[:r.ret]) ** 2))
        got += r.ret
    dev.deactivateStream(st)
    dev.closeStream(st)
    rms = (power / max(got, 1)) ** 0.5
    check(got >= want, f'{freq / 1e6:.0f} MHz: {got} samples')
    check(ovf == 0, f'{freq / 1e6:.0f} MHz: {ovf} overflows')
    check(rms > 0, f'{freq / 1e6:.0f} MHz: rms {rms:.5f} of full scale')
    return got, ovf


a = stream(2437e6, 2.0)
b = stream(5240e6, 1.0)
del dev
print(f'soapy: {len(fails)} failed; 2437 {a[0]} samples {a[1]} ovf; 5240 {b[0]} samples {b[1]} ovf')
sys.exit(1 if fails else 0)
