#!/usr/bin/env python3
"""Summarise a soak.jsonl from tools/soak.py: tools/soak_summary.py RUN_DIR [RUN_DIR ...]

Prints per run: duration, samples, whole-run rate, steady rate (minutes 2..end, realtime clock), overflow total,
rate per hour and the minutes they fell in, timeouts/errors, gain changes, usbipd working-set
start/end/slope (MB per hour, least squares), CPU per second of stream, and the monotonic/realtime
ratio range.
"""
import json
import sys

import numpy as np

for d in sys.argv[1:]:
    rows = [json.loads(l) for l in open(f'{d}/soak.jsonl') if l.strip()]
    head = next(r for r in rows if 'start' in r)
    mins = [r for r in rows if 't_min' in r]
    end = next((r for r in rows if 'end' in r), {})
    if not mins:
        print(d, 'no minute records')
        continue
    final = mins[-1]
    t = np.array([r['t_min'] for r in mins])
    dur = t[-1]
    tot = final.get('totals') or {k: sum(r[k] for r in mins) for k in ('samples', 'overflows', 'timeouts', 'other_errors')}
    steady = [r for r in mins[1:] if not r.get('final')]
    rate = np.mean([r['rate_msps'] for r in steady]) if steady else float('nan')
    ovf_min = [(r['t_min'], r['overflows']) for r in mins if r['overflows']]
    gains = [(r['t_min'], r['gains']) for r in mins]
    changes = [(gains[i][0], gains[i][1]) for i in range(1, len(gains)) if gains[i][1] != gains[i - 1][1]]
    kb = [(r['t_min'], r['usbipd_kb']) for r in mins if r.get('usbipd_kb')]
    slope = (np.polyfit([x for x, _ in kb], [y / 1024 for _, y in kb], 1)[0] * 60) if len(kb) > 2 else float('nan')
    mono = [r['mono_ratio'] for r in mins if r.get('mono_ratio')]
    cpu = final['cpu_s'] / (dur * 60)
    print(f'== {d}')
    print(f'start {head["start"]}  end {end.get("end")}  rc {end.get("rc")}  {head["freq_mhz"]} MHz  settings {head["settings"]}')
    whole = tot['samples'] / (dur * 60) / 1e6
    print(f'duration {dur:.1f} min; samples {tot["samples"]:,}; whole-run rate {whole:.3f} MS/s; '
          f'steady rate {rate:.3f} MS/s (realtime, minutes 2..end)')
    print(f'overflows {tot["overflows"]} = {tot["overflows"] / dur * 60:.2f}/h; in minutes {ovf_min}')
    print(f'timeouts {tot["timeouts"]}; other errors {tot["other_errors"]}')
    print(f'gain {mins[0]["gains"]} at start; {len(changes)} changes: {changes[:10]}')
    if kb:
        print(f'usbipd working set {kb[0][1] / 1024:.1f} -> {kb[-1][1] / 1024:.1f} MB; slope {slope:+.2f} MB/h')
    print(f'CPU {cpu:.3f} core; monotonic/realtime {min(mono):.4f}..{max(mono):.4f}' if mono else f'CPU {cpu:.3f} core')
