"""Bounded receive-only comparison of the initialized coarse state hold.

Research only: no guessed state code, firmware replacement, or persistent write.
The C wrapper restores its single force bit before normal receiver teardown.
Run only when no other program is using the adapter.
"""
import argparse
import json
from pathlib import Path
import subprocess

import wifi
from wifi_b import channel_mhz
from wifi_benchmark import iq_metrics
from analyze_blanking import zero_runs, payload_gaps


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--probe', default='/tmp/capture_tap_probe')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    for channel in [2, 11, 4]:
        for hold in [0, 1, 0, 1]:
            path = args.output / f'ch{channel}_hold{hold}_{len(results)}.cs16'
            result = subprocess.run([args.probe, str(path), str(channel_mhz(channel) * 1000000),
                '20000000', '4', '0', str(hold)], capture_output=True, text=True, timeout=40)
            entry = {'channel': channel, 'hold_initialized_state': bool(hold),
                'capture_status': result.returncode, 'file': path.name, 'log': result.stdout + result.stderr}
            results.append(entry)
            retain = False
            try:
                if not result.returncode and path.stat().st_size == 20000000 // 2048 * 2048 * 4:
                    entry['iq_metrics'] = iq_metrics(path)
                    runs = zero_runs(path)
                    entry['zero_run_count'] = len(runs)
                    entry['zero_run_samples'] = int((runs[:, 1] - runs[:, 0]).sum())
                    report, packets = wifi.analyze(path, channel_mhz(channel), [channel], cck=True)
                    path.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
                    entry.update(valid_frames=report['valid_frames'], networks=report['networks'],
                                 dsss_plcp_rates=report['dsss_plcp_rates'])
                    retain = any(p['fcs_ok'] and p['phy'] == 'DSSS' and p['rate_mbps'] in (5.5, 11)
                                 for p in packets)
                    entry['retained_iq_for_verified_cck'] = retain
                    if retain:
                        wifi.write_pcap(path.with_suffix('.pcap'), packets)
                    for valid in [True, False]:
                        group = [p for p in packets if p['phy'] == 'DSSS' and p['frame'] and p['fcs_ok'] == valid]
                        key = 'valid_payloads' if valid else 'failed_payloads'
                        entry[key] = {'count': len(group), 'with_zero_runs': sum(
                            payload_gaps(runs, round(p['payload_start_time_ms'] * 40000),
                                         round(p['payload_end_time_ms'] * 40000))['runs'] > 0 for p in group)}
                else:
                    entry['excluded'] = 'Failed or incomplete acquisition'
            finally:
                if path.exists() and not retain:
                    path.unlink()
                (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(json.dumps(entry), flush=True)


if __name__ == '__main__':
    main()
