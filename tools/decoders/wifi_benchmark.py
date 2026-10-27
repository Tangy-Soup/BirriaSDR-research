"""Compare receiver gain profiles against a saved Windows managed-mode BSSID list.

Collect verified recordings and score FCS-valid advertised BSSIDs, not just frame
counts. RF gain indices are uncalibrated: profile order is not a gain ranking.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
from wifi_b import channel_mhz


def reference_rows(path):
    data = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    if isinstance(data, dict):
        data = data.get('AccessPoints', data.get('networks', []))
    return {row['BSSID'].lower(): row for row in data}


def score(networks, reference, channels):
    expected = {b for b, r in reference.items() if r['Channel'] in channels}
    found = {n['bssid'].lower() for n in networks
             if n.get('advertised_channel') in channels}
    matched = sorted(b for b in expected & found if any(
        n['bssid'].lower() == b and n.get('advertised_channel') == reference[b]['Channel']
        for n in networks))
    return {'expected': len(expected), 'matched': len(matched), 'matched_bssids': matched,
            'missed_bssids': sorted(expected - set(matched)), 'additional_bssids': sorted(found - expected),
            'channel_mismatches': sorted((expected & found) - set(matched))}


def iq_metrics(path):
    iq = np.memmap(path, dtype='<i2', mode='r').reshape(-1, 2)
    # Bound the statistics working set; samples cover the entire recording.
    points = iq[::max(1, len(iq) // 500000)].astype(np.float32)
    return {'rms_lsb': float(np.sqrt(np.mean(points ** 2))),
            'peak_lsb': float(np.max(np.abs(points))),
            'clip_fraction': float(np.mean(np.any(np.abs(points) >= 8191, axis=1))),
            'zero_fraction': float(np.mean(np.all(points == 0, axis=1)))}


def main():
    root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reference', type=Path, required=True)
    ap.add_argument('--channels', type=int, nargs='+', choices=range(1, 15), default=[11, 4, 6])
    ap.add_argument('--seconds', type=float, default=.35)
    ap.add_argument('--profiles', nargs='+', default=['2,8,0', '0,8,0', '1,8,0', '3,8,0', '2,8,6', 'released'])
    ap.add_argument('--phy', choices=['dsss', 'ofdm', 'both'], default='both')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--keep-iq', action='store_true')
    ap.add_argument('--bandwidth', type=int, choices=[20000000, 40000000], default=40000000)
    args = ap.parse_args()
    if not .05 <= args.seconds <= 5:
        ap.error('--seconds must be between 0.05 and 5')
    profiles = []
    for text in args.profiles:
        if text == 'released':
            profiles.append((text, None))
            continue
        try:
            gain = tuple(int(n) for n in text.split(','))
        except ValueError:
            ap.error(f'Invalid gain profile {text}')
        if len(gain) != 3 or any(n < 0 or n > m for n, m in zip(gain, (3, 8, 12))):
            ap.error(f'Invalid gain profile {text}')
        profiles.append((text, gain))
    reference = reference_rows(args.reference)
    out = args.output or root / 'captures' / datetime.now().astimezone().strftime('benchmark_%Y%m%d_%H%M%S')
    out.mkdir(parents=True, exist_ok=False)
    out = out.resolve()
    report = {'reference': str(args.reference), 'seconds': args.seconds, 'bandwidth_hz': args.bandwidth,
              'channels': args.channels, 'runs': [], 'status': 'running'}
    def save():
        (out / 'benchmark.json').write_text(json.dumps(report, indent=2) + '\n')
    save()
    try:
        for ch in args.channels:
            for label, gain in profiles:
                stem = out / f'ch{ch}_g{label.replace(",", "-")}'
                path = stem.with_suffix('.cs16')
                entry = {'channel': ch, 'profile': label,
                         'started_at': datetime.now().astimezone().isoformat(), 'attempts': []}
                report['runs'].append(entry)
                try:
                    for attempt in range(3):
                        cmd = [str(root / 'build/birria_rx'), '--frequency', str(channel_mhz(ch) * 1000000)]
                        cmd += ['--bandwidth', str(args.bandwidth)]
                        if gain is not None:
                            cmd += ['--gain', *map(str, gain)]
                        cmd += [str(path), str(round(args.seconds * 40000000))]
                        result = subprocess.run(cmd, capture_output=True, text=True, timeout=args.seconds + 40)
                        entry['attempts'].append({'returncode': result.returncode, 'log': result.stdout + result.stderr})
                        save()
                        if not result.returncode and path.exists() and path.stat().st_size == round(args.seconds * 40000000) * 4:
                            break
                        if path.exists():
                            path.unlink()
                    else:
                        raise RuntimeError(f'Capture failed: channel {ch}, gain {label}')
                    entry['iq_metrics'] = iq_metrics(path)
                    cmd = [sys.executable, str(root / 'tools/decoders/wifi.py'), str(path), '--center', str(channel_mhz(ch)),
                           '--phy', args.phy, '--json', str(stem.with_suffix('.json')),
                           '--pcap', str(stem.with_suffix('.pcap'))]
                    with stem.with_suffix('.decode.log').open('w') as log:
                        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=600)
                    if result.returncode:
                        raise RuntimeError(f'Decoding failed: channel {ch}, gain {label}')
                    decoded = json.loads(stem.with_suffix('.json').read_text())
                    entry['valid_frames'] = decoded['valid_frames']
                    entry['networks'] = decoded['networks']
                    entry['score'] = score(decoded['networks'], reference, [ch])
                    save()
                    print(f"ch{ch} gain={label}: {entry['score']['matched']}/{entry['score']['expected']} reference BSSIDs; "
                          f"{entry['valid_frames']} frames; rms={entry['iq_metrics']['rms_lsb']:.1f}; "
                          f"clipped={entry['iq_metrics']['clip_fraction']:.2g}", flush=True)
                finally:
                    if path.exists() and not args.keep_iq:
                        path.unlink()
        report['status'] = 'complete'
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        report['status'], report['error'] = 'failed', str(exc)
        print(str(exc), file=sys.stderr)
    finally:
        report['finished_at'] = datetime.now().astimezone().isoformat()
        save()
    print(f'Results: {out}', flush=True)
    return 0 if report['status'] == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
