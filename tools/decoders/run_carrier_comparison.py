"""Receive-only held-out, paired-input carrier-estimator comparison.

Two successful half-second observations on channels 2, 11 and positive control
4. Both decoder arms see identical samples; arm order alternates. No receiver
register changes. Only new temporary captures from this run are removed, after
JSON, PCAP, metrics and attempt logs have been saved.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import subprocess

import wifi
from wifi_b import channel_mhz
from wifi_benchmark import iq_metrics, reference_rows, score
from analyze_blanking import zero_runs, payload_gaps
from diagnose_capture import gap_structure
from probe_preamble_carrier import analyze_refined


def packet_metrics(packets, runs):
    distinct, seen = [], []
    for p in sorted((p for p in packets if p['phy'] == 'DSSS'), key=lambda p: (not p['fcs_ok'], p['sync_time_ms'])):
        if any(abs(p['sync_time_ms'] - t) < .008 for t in seen):
            continue
        seen.append(p['sync_time_ms'])
        entry = {'sync_time_ms': p['sync_time_ms'], 'fcs_ok': p['fcs_ok'], 'rate_mbps': p['rate_mbps'],
                 'has_payload': bool(p['frame'])}
        if p['frame']:
            entry['gaps'] = payload_gaps(runs, round(p['payload_start_time_ms'] * 40000),
                round(p['payload_end_time_ms'] * 40000))
        distinct.append(entry)
    return {'distinct_valid_plcp': len(distinct), 'distinct_dsss_fcs_valid': sum(p['fcs_ok'] for p in distinct),
        'valid_payloads_with_gaps': sum(p['fcs_ok'] and bool(p.get('gaps', {}).get('runs')) for p in distinct),
        'failed_payloads': sum(not p['fcs_ok'] and p['has_payload'] for p in distinct),
        'failed_payloads_with_gaps': sum(not p['fcs_ok'] and bool(p.get('gaps', {}).get('runs')) for p in distinct),
        'packets': sorted(distinct, key=lambda p: p['sync_time_ms'])}


def assert_idle():
    for path in Path('/proc').iterdir():
        if not path.name.isdigit():
            continue
        try:
            name = (path / 'comm').read_text().strip().lower()
        except (OSError, ProcessLookupError):
            continue
        if name in ('birria_rx', 'capture_tap_pro', 'cyberether', 'test_hardware', 'test_stability'):
            raise RuntimeError(f'Hardware owner may be active: {path.name} {name}')


def main():
    root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--reference', type=Path, required=True,
                    help='Windows scan comparison.json used as the AP reference')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    reference = reference_rows(args.reference)
    results = {'started_at': datetime.now().astimezone().isoformat(),
        'design': 'Paired identical I/Q, alternating decoder arm order; 2 observations per channel per arm',
        'settings': {'sample_rate': 40000000, 'bandwidth': 20000000, 'gain': [3, 8, 0], 'tap': 4},
        'reference': str(args.reference),
        'captures': [], 'status': 'running'}
    def save():
        (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    save()
    try:
        for repeat in (1, 2):
            for channel in (2, 11, 4):
                stem = args.output / f'ch{channel}_repeat{repeat}'
                path = stem.with_suffix('.cs16')
                count = 20000000 // 2048 * 2048
                entry = {'channel': channel, 'repeat': repeat, 'attempts': [], 'arms': {},
                    'started_at': datetime.now().astimezone().isoformat()}
                results['captures'].append(entry)
                for attempt in range(1, 4):
                    assert_idle()
                    command = [str(root / 'build/birria_rx'), '--frequency', str(channel_mhz(channel) * 1000000),
                        '--buffered', '--bandwidth', '20000000', '--gain', '3', '8', '0', str(path), str(count), 'manual']
                    acquisition = subprocess.run(command, capture_output=True, text=True, timeout=45)
                    log = acquisition.stdout + acquisition.stderr
                    stem.with_suffix(f'.capture{attempt}.log').write_text(log)
                    status = {'attempt': attempt, 'command': command, 'returncode': acquisition.returncode,
                        'bytes': path.stat().st_size if path.exists() else 0, 'log': log}
                    entry['attempts'].append(status)
                    good = (acquisition.returncode == 0 and status['bytes'] == count * 4 and
                            'stream=success' in log and 'reset=success' in log and 'firmware state=2' in log)
                    status['accepted'] = good
                    save()
                    if good:
                        break
                    status['excluded'] = 'Failed, incomplete or nonterminal acquisition'
                    if path.exists():
                        path.unlink()
                    if 'reset=success' not in log:
                        raise RuntimeError('Reset status not successful; stop hardware work')
                else:
                    raise RuntimeError(f'Channel {channel} exhausted bounded capture retries')
                entry['iq_metrics'] = iq_metrics(path)
                runs = zero_runs(path)
                entry['gap_structure'] = gap_structure(runs, count)
                entry['samples'] = count
                # Streaming digest avoids reading the recording as one bytes object.
                with path.open('rb') as f:
                    entry['iq_sha256'] = hashlib.file_digest(f, 'sha256').hexdigest()
                order = ('baseline', 'late_preamble') if repeat == 1 else ('late_preamble', 'baseline')
                entry['decoder_order'] = order
                for arm in order:
                    if arm == 'baseline':
                        report, packets = wifi.analyze(path, channel_mhz(channel), [channel], cck=True)
                    else:
                        report, packets, trials = analyze_refined(path, channel)
                        stem.with_suffix('.estimator_trials.json').write_text(json.dumps(trials, indent=2) + '\n')
                    stem.with_suffix(f'.{arm}.json').write_text(json.dumps(report, indent=2) + '\n')
                    wifi.write_pcap(stem.with_suffix(f'.{arm}.pcap'), packets)
                    entry['arms'][arm] = {'valid_frames': report['valid_frames'], 'networks': report['networks'],
                        'score': score(report['networks'], reference, [channel]),
                        'decoding_metrics': packet_metrics(packets, runs),
                        'valid_frame_sha256': sorted(hashlib.sha256(p['frame']).hexdigest() for p in packets if p['fcs_ok'])}
                    save()
                entry['raw_removed_after_evidence_saved'] = True
                save()
                path.unlink()
                print(json.dumps({'channel': channel, 'repeat': repeat,
                    'valid_frames': {arm: entry['arms'][arm]['valid_frames'] for arm in order},
                    'gap_fraction': entry['gap_structure']['zero_run_fraction']}), flush=True)
        results['status'] = 'complete'
    except Exception as exc:
        results['status'], results['error'] = 'failed', str(exc)
        raise
    finally:
        results['finished_at'] = datetime.now().astimezone().isoformat()
        save()


if __name__ == '__main__':
    main()
