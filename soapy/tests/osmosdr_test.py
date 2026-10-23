#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""GNU Radio smoke test through gr-osmosdr, the path gqrx uses.

  SOAPY_SDR_PLUGIN_PATH=build/soapy osmosdr_test.py [--rate HZ] [--freq MHZ]
      [--seconds S] [--retune MHZ] [--min-buffer ITEMS]

Runs osmosdr.source('soapy=0,driver=birriasdr') into a null sink and a rate
probe, optionally retuning halfway, and reports the median delivered rate over
250 ms windows after the first second. Exits 1 below 90% of nominal.
--min-buffer sets the source's minimum output buffer; at 80 MSps the default
GNU Radio buffers are too small and about 1 << 20 items are needed.
Adapted from Mtk79ICAP's userspace/SoapyMT7921/osmosdr_test.py.
"""
import argparse
import sys
import time

import numpy as np
import osmosdr
from gnuradio import blocks, gr

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--rate', type=float, default=20e6)
ap.add_argument('--freq', type=float, default=2437.0, help='centre in MHz')
ap.add_argument('--seconds', type=float, default=2.0)
ap.add_argument('--retune', type=float, help='centre in MHz to switch to halfway')
ap.add_argument('--args', default='soapy=0,driver=birriasdr')
ap.add_argument('--min-buffer', type=int, help='minimum output buffer of the source, in items')
a = ap.parse_args()

tb = gr.top_block()
src = osmosdr.source(args=a.args)
src.set_sample_rate(a.rate)
src.set_center_freq(a.freq * 1e6)
if a.min_buffer:
    src.set_min_output_buffer(a.min_buffer)
rate = src.get_sample_rate()
# alpha 1: each reading is the rate over the last 250 ms window, not an
# average that starts from zero and lags.
count = blocks.probe_rate(gr.sizeof_gr_complex, 250.0, 1.0)
sink = blocks.null_sink(gr.sizeof_gr_complex)
tb.connect(src, sink)
tb.connect(src, count)
tb.start()
t0 = time.monotonic()
readings = []
retuned = False
while time.monotonic() - t0 < a.seconds:
    time.sleep(0.25)
    if a.retune and not retuned and time.monotonic() - t0 >= a.seconds / 2:
        src.set_center_freq(a.retune * 1e6)
        retuned = True
    if time.monotonic() - t0 > 1.0:      # skip device boot and startup
        readings.append(count.rate())
tb.stop()
tb.wait()
measured = float(np.median(readings)) if readings else 0.0
print(f'osmosdr: nominal {rate / 1e6:.3f} MS/s, median {measured / 1e6:.3f} MS/s over '
      f'{len(readings)} windows (min {min(readings, default=0) / 1e6:.3f}); '
      f'centre now {src.get_center_freq() / 1e6:.3f} MHz')
sys.exit(0 if measured > 0.9 * rate else 1)
