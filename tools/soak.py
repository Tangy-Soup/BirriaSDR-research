#!/usr/bin/env python3
"""Soak test: stream through SoapySDR for a long time and discard the samples.

  tools/soak.py [--minutes 120] [--freq-mhz 2437] [--rate 40e6] [--chains 0|0,1] [--out DIR]

Usually started through tools/soak.sh, which waits for the dongle to be idle first.

Opens driver=birriasdr with the shipped defaults (automatic gain = host AGC), streams CS16 at
--rate on chain 0, and once a minute appends one JSON line to DIR/soak.jsonl:
  t_min, samples, rate_msps (by CLOCK_REALTIME: under WSL2 the monotonic clock has run 2-10%
  slow against Windows, so rates timed with it read high), mono_ratio (monotonic / realtime
  seconds in the minute), overflows, timeouts, other_errors, gain_db, gain elements,
  rms (LSB, from one block), usbipd_kb (usbipd.exe working set from tasklist.exe under WSL,
  read on a side thread; null elsewhere), cpu_s (this process).
Rates per minute scatter when realtime is stepped; use the whole-run totals.
Ctrl-C or SIGTERM ends the run cleanly with a final line. Exits 1 after 20 consecutive timeouts.
"""
import argparse
import json
import os
import re
import resource
import signal
import subprocess
import sys
import threading
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_OVERFLOW, SOAPY_SDR_RX, SOAPY_SDR_TIMEOUT

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--minutes', type=float, default=120)
ap.add_argument('--freq-mhz', type=float, default=2437)
ap.add_argument('--rate', type=float, default=40e6)
ap.add_argument('--chains', default='0', help='comma-separated channels, e.g. 0,1 for both chains')
ap.add_argument('--out', default='.')
a = ap.parse_args()
chans = [int(c) for c in a.chains.split(',')]
os.makedirs(a.out, exist_ok=True)
log = open(os.path.join(a.out, 'soak.jsonl'), 'a', buffering=1)

TASKLIST = '/mnt/c/Windows/System32/tasklist.exe'
usbipd_kb = [None]
stop = threading.Event()


def usbipd_watch():
    while not stop.is_set():
        try:
            out = subprocess.run([TASKLIST, '/FI', 'IMAGENAME eq usbipd.exe', '/FO', 'CSV', '/NH'],
                                 capture_output=True, text=True, timeout=20).stdout
            m = re.search(r'"([\d,.]+) K"', out)
            usbipd_kb[0] = int(re.sub(r'[,.]', '', m.group(1))) if m else None
        except Exception:
            usbipd_kb[0] = None
        stop.wait(60)


def on_signal(*_):
    stop.set()


signal.signal(signal.SIGTERM, on_signal)
signal.signal(signal.SIGINT, on_signal)

dev = SoapySDR.Device('driver=birriasdr')
dev.setSampleRate(SOAPY_SDR_RX, 0, a.rate)
dev.setFrequency(SOAPY_SDR_RX, 0, a.freq_mhz * 1e6)
for c in chans:
    dev.setGainMode(SOAPY_SDR_RX, c, True)
elements = list(dev.listGains(SOAPY_SDR_RX, 0))
settings = {s.key: dev.readSetting(s.key) for s in dev.getSettingInfo()}
log.write(json.dumps({'start': time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'freq_mhz': a.freq_mhz,
                      'rate': dev.getSampleRate(SOAPY_SDR_RX, 0), 'chains': chans, 'settings': settings,
                      'auto': dev.getGainMode(SOAPY_SDR_RX, 0), 'elements': elements}) + '\n')
if os.path.exists(TASKLIST):
    threading.Thread(target=usbipd_watch, daemon=True).start()

st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, chans)
dev.activateStream(st)
bufs = [np.zeros(2 * 131072, np.int16) for _ in chans]
buf = bufs[0]
t0 = time.time()
m_prev = time.monotonic()
end = t0 + a.minutes * 60
next_mark = t0 + 60
tot = {'samples': 0, 'overflows': 0, 'timeouts': 0, 'other_errors': 0}
win = dict(tot)
win_t = t0
consecutive_timeouts = 0
rms = None
rms_all = None
want_rms = True
rc = 0


def mark(now, final=False):
    global win, win_t, want_rms, m_prev
    span = now - win_t
    m_now = time.monotonic()
    mono_ratio = round((m_now - m_prev) / span, 4) if span > 0 else None
    m_prev = m_now
    gains = {e: round(dev.getGain(SOAPY_SDR_RX, 0, e), 1) for e in elements}
    if len(chans) > 1:
        gains.update({'ch%d_%s' % (c, e): round(dev.getGain(SOAPY_SDR_RX, c, e), 1)
                      for c in chans[1:] for e in elements})
    ru = resource.getrusage(resource.RUSAGE_SELF)
    rec = {'t_min': round((now - t0) / 60, 2), 'samples': win['samples'],
           'rate_msps': round(win['samples'] / span / 1e6, 4) if span > 0 else None,
           'mono_ratio': mono_ratio, 'overflows': win['overflows'], 'timeouts': win['timeouts'], 'other_errors': win['other_errors'],
           'gain_db': round(dev.getGain(SOAPY_SDR_RX, 0), 1), 'gains': gains, 'rms': rms, 'rms_all': rms_all,
           'usbipd_kb': usbipd_kb[0], 'cpu_s': round(ru.ru_utime + ru.ru_stime, 1)}
    if final:
        rec['final'] = True
        rec['totals'] = tot
    log.write(json.dumps(rec) + '\n')
    win = {k: 0 for k in win}
    win_t = now
    want_rms = True


while not stop.is_set():
    now = time.time()
    if now >= end:
        break
    if now >= next_mark:
        mark(now)
        next_mark += 60
    r = dev.readStream(st, bufs, 131072, timeoutUs=1_000_000)
    if r.ret > 0:
        consecutive_timeouts = 0
        win['samples'] += r.ret
        tot['samples'] += r.ret
        if want_rms:
            x = buf[:2 * r.ret].astype(np.float32)
            rms = round(float(np.sqrt(np.mean(x * x) * 2)), 1)
            rms_all = [round(float(np.sqrt(np.mean(b[:2 * r.ret].astype(np.float32) ** 2) * 2)), 1) for b in bufs]
            want_rms = False
    elif r.ret == SOAPY_SDR_OVERFLOW:
        win['overflows'] += 1
        tot['overflows'] += 1
    elif r.ret == SOAPY_SDR_TIMEOUT:
        win['timeouts'] += 1
        tot['timeouts'] += 1
        consecutive_timeouts += 1
        if consecutive_timeouts >= 20:
            rc = 1
            break
    else:
        win['other_errors'] += 1
        tot['other_errors'] += 1
        if win['other_errors'] > 100:
            rc = 1
            break

mark(time.time(), final=True)
stop.set()
dev.deactivateStream(st)
dev.closeStream(st)
del dev
log.write(json.dumps({'end': time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'rc': rc}) + '\n')
sys.exit(rc)
