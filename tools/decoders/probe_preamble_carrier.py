"""Offline controlled test of late-preamble DSSS carrier refinement.

Research only. Estimates use received DBPSK preamble symbols, never expected
MAC bytes. Existing good decodes are preserved; FCS is the only acceptance test.
The temporary estimator override is synchronous and restored in finally.
"""
import argparse
import json
from pathlib import Path

import numpy as np

import wifi
import wifi_b


def analyze_refined(path, channel):
    original_decode = wifi_b.decode_ppdu
    original_carrier = wifi_b.correct_dsss_carrier
    attempts = []

    def refined(c, a, b, phase_hint=None, cck_samples=None):
        before = original_decode(c, a, b, phase_hint, cck_samples)
        if not before or not before.get('plcp_ok') or before.get('fcs_ok') or before.get('rate') not in (1, 2):
            return before
        phase = (before['sfd_sample'] - a) % wifi_b.SPS
        k = (before['sfd_sample'] - a) // wifi_b.SPS
        symbols = wifi_b.symbols(c, a + phase, min(320, (b - a) // wifi_b.SPS))
        entry = {'candidate_start_ms': a / 44000, 'sfd_local_ms': before['sfd_sample'] / 44000,
            'sync_symbols_available': int(k), 'initial_cfo_hz': before['cfo_hz'], 'trials': []}
        for span in (32, 64):
            preamble = symbols[max(0, k - span):k + 1]
            if len(preamble) < 17:
                continue
            _, estimate = original_carrier(preamble)
            try:
                def carrier(sym, phase=None):
                    return original_carrier(sym, estimate if phase is None else phase)
                wifi_b.correct_dsss_carrier = carrier
                after = original_decode(c, a, b, phase_hint, cck_samples)
            finally:
                wifi_b.correct_dsss_carrier = original_carrier
            accepted = bool(after and after.get('fcs_ok') and after.get('plcp_ok') and
                            abs(after['sfd_sample'] - before['sfd_sample']) < 8 * wifi_b.SPS)
            entry['trials'].append({'span_symbols': span, 'estimate_hz': float(estimate * 1e6 / (2 * np.pi)),
                'plcp_ok': bool(after and after.get('plcp_ok')), 'fcs_ok': accepted})
            if accepted:
                attempts.append(entry)
                return after
        attempts.append(entry)
        return before

    try:
        wifi_b.decode_ppdu = refined
        report, packets = wifi.analyze(path, wifi_b.channel_mhz(channel), [channel], cck=True)
    finally:
        wifi_b.decode_ppdu = original_decode
        wifi_b.correct_dsss_carrier = original_carrier
    return report, packets, attempts


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--channel', type=int, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report, packets, attempts = analyze_refined(args.capture, args.channel)
    (args.output / 'decode.json').write_text(json.dumps(report, indent=2) + '\n')
    (args.output / 'estimator_trials.json').write_text(json.dumps(attempts, indent=2) + '\n')
    wifi.write_pcap(args.output / 'valid.pcap', packets)
    print(json.dumps({'channel': args.channel, 'valid_frames': report['valid_frames'],
        'networks': report['networks'], 'candidates_tested': len(attempts),
        'successful_refinements': sum(any(t['fcs_ok'] for t in a['trials']) for a in attempts)}), flush=True)


if __name__ == '__main__':
    main()
