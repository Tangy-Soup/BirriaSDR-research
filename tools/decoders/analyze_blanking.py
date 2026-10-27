"""Relate raw zero-sample runs to verified and failed DSSS payloads.

Failed-frame addresses are diagnostic candidates only. They never establish AP
discoveries or enter the benchmark. Payload intervals are approximate because
the symbol timing tracker can move by a few source samples.
"""
import argparse
from collections import Counter
import json
from pathlib import Path

import numpy as np
import wifi


def zero_runs(path, minimum=16, chunk=1_000_000):
    raw = np.memmap(path, dtype='<i2', mode='r').reshape(-1, 2)
    runs, active = [], None
    for base in range(0, len(raw), chunk):
        mask = np.all(raw[base:base + chunk] == 0, axis=1)
        boundaries = np.r_[0, np.flatnonzero(mask[1:] != mask[:-1]) + 1, len(mask)]
        for a, b in zip(boundaries[:-1], boundaries[1:]):
            if mask[a] and active is None:
                active = base + int(a)
            elif not mask[a] and active is not None:
                if base + a - active >= minimum:
                    runs.append((active, base + int(a)))
                active = None
    if active is not None and len(raw) - active >= minimum:
        runs.append((active, len(raw)))
    return np.array(runs, dtype=np.int64).reshape(-1, 2)


def payload_gaps(runs, start, end):
    if not len(runs):
        return {'runs': 0, 'missing_samples': 0, 'longest_run': 0}
    i = int(np.searchsorted(runs[:, 1], start, side='right'))
    j = int(np.searchsorted(runs[:, 0], end, side='left'))
    widths = np.minimum(runs[i:j, 1], end) - np.maximum(runs[i:j, 0], start)
    return {'runs': len(widths), 'missing_samples': int(widths.sum()),
            'longest_run': int(widths.max()) if len(widths) else 0}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--center', type=float, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    report, packets = wifi.analyze(args.capture, args.center,
        [min(range(1, 15), key=lambda ch: abs(wifi.wifi_b.channel_mhz(ch) - args.center))], phy='dsss')
    runs = zero_runs(args.capture)
    frames, seen = [], []
    # Multiple detector candidates may decode the same failed header. Prefer a
    # valid payload and count each SFD only once in the diagnostic comparison.
    for p in sorted(packets, key=lambda p: (not p['fcs_ok'], p['sync_time_ms'])):
        if 'payload_start_time_ms' not in p or not p['frame']:
            continue
        if any(abs(p['sync_time_ms'] - old) < .008 for old in seen):
            continue
        seen.append(p['sync_time_ms'])
        start, end = round(p['payload_start_time_ms'] * 40000), round(p['payload_end_time_ms'] * 40000)
        gaps = payload_gaps(runs, start, end)
        frame = p['frame']
        candidate = frame[16:22].hex(':') if len(frame) >= 24 and not frame[0] & 0x0c else None
        frames.append({'time_ms': p['time_ms'], 'sync_time_ms': p['sync_time_ms'],
            'fcs_ok': p['fcs_ok'], 'rate_mbps': p['rate_mbps'],
            'length_bytes': len(frame), 'candidate_bssid_unverified_unless_fcs_ok': candidate,
            'payload_duration_us': p['length_us'], 'payload_zero_runs': gaps})
    result = {'capture': str(args.capture), 'duration_seconds': report['duration_seconds'],
              'minimum_zero_run_samples': 16, 'raw_zero_runs': len(runs),
              'raw_zero_width_counts': dict(Counter(map(str, runs[:, 1] - runs[:, 0]))),
              'valid_frames': report['valid_frames'], 'verified_networks': report['networks'],
              'dsss_plcp_rates': report['dsss_plcp_rates'],
              'frame_diagnostics': sorted(frames, key=lambda f: f['sync_time_ms']),
              'deduplication': 'One DSSS SFD per 8 microseconds, preferring an FCS-valid decode'}
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    for good in (True, False):
        group = [f for f in frames if f['fcs_ok'] == good]
        print('valid' if good else 'failed', 'frames', len(group), 'with payload zero runs',
              sum(f['payload_zero_runs']['runs'] > 0 for f in group))
    print('Raw gap widths:', result['raw_zero_width_counts'])


if __name__ == '__main__':
    main()
