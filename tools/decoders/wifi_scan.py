"""Capture and decode each Wi-Fi channel at its center; discard I/Q by default.

Receive only. Uses the existing birria_rx recorder and wifi.py offline decoder.
Each channel gets independent stream verification, logs, JSON and PCAP output.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import subprocess
import sys

from wifi_b import CHANNELS, channel_mhz
from wifi_benchmark import reference_rows, score, iq_metrics


def main():
    root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--channels', type=int, nargs='+', choices=CHANNELS, metavar='N', default=list(range(1, 12)))
    ap.add_argument('--seconds', type=float, default=.5)
    ap.add_argument('--passes', type=int, default=1, help='Repeat the channel sweep and merge verified BSSIDs')
    ap.add_argument('--keep-iq', action='store_true', help='Retain successful raw recordings (160 MB/second/channel)')
    ap.add_argument('--buffered', action='store_true', help='Capture <=2 seconds in RAM before writing raw samples')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--recorder', type=Path, default=root / 'build/birria_rx')
    ap.add_argument('--phy', choices=['both', 'ofdm', 'dsss'], default='both')
    gain_options = ap.add_mutually_exclusive_group()
    gain_options.add_argument('--gain', type=int, nargs=3, metavar=('LNA', 'FILTER', 'DIGITAL'))
    gain_options.add_argument('--released', action='store_true', help='Release overrides; full AGC is unqualified')
    ap.add_argument('--reference', type=Path, help='Windows comparison.json or scan.json for BSSID recall scoring')
    ap.add_argument('--bandwidth', type=int, choices=[20000000, 40000000], default=20000000,
                    help='Nominal analog/digital filter profile in Hz; sample rate stays 40 MSps')
    args = ap.parse_args()
    if not .05 <= args.seconds <= 30:
        ap.error('--seconds must be between 0.05 and 30')
    if not 1 <= args.passes <= 10:
        ap.error('--passes must be between 1 and 10')
    if args.buffered and args.seconds > 2:
        ap.error('--buffered supports at most two seconds per capture')
    if len(set(args.channels)) != len(args.channels):
        ap.error('Channels must be unique')
    if args.phy == 'dsss' and max(args.channels) > 14:
        ap.error('--phy dsss takes 2.4 GHz channels only; 802.11b exists only at 2.4 GHz')
    if args.gain and any(n < 0 or n > m for n, m in zip(args.gain, (3, 8, 12))):
        ap.error('Gain codes require LNA 0..3, filter 0..8, digital 0..12')
    reference = reference_rows(args.reference) if args.reference else None
    out = args.output or root / 'captures' / datetime.now().astimezone().strftime('wifi_scan_%Y%m%d_%H%M%S')
    # A new directory protects existing recordings and previous reports.
    try:
        out.mkdir(parents=True, exist_ok=False)
    except OSError as exc:
        ap.error(str(exc))
    out = out.resolve()
    count = round(args.seconds * 40000000)
    if args.buffered:
        count = count // 2048 * 2048
    manifest = {'started_at': datetime.now().astimezone().isoformat(),
                'channels': args.channels, 'passes': args.passes, 'samples_per_channel': count, 'keep_iq': args.keep_iq,
                'gain': 'released' if args.released else args.gain or [2, 8, 0], 'buffered': args.buffered,
                'bandwidth_hz': args.bandwidth,
                'reference': str(args.reference) if args.reference else None,
                'captures': [], 'networks': {}, 'status': 'running'}
    def save():
        (out / 'survey.json').write_text(json.dumps(manifest, indent=2) + '\n')
    save()
    try:
        for pass_number, ch in ((p, c) for p in range(1, args.passes + 1) for c in args.channels):
            suffix = f'_pass{pass_number}' if args.passes > 1 else ''
            stem = out / f'ch{ch}_{channel_mhz(ch)}MHz{suffix}'
            iq = stem.with_suffix('.cs16')
            entry = {'channel': ch, 'pass': pass_number, 'center_mhz': channel_mhz(ch), 'attempts': [],
                     'started_at': datetime.now().astimezone().isoformat()}
            manifest['captures'].append(entry)
            try:
                for attempt in range(1, 4):
                    command = [str(args.recorder), '--frequency', str(channel_mhz(ch) * 1000000)]
                    if args.buffered:
                        command.append('--buffered')
                    command += ['--bandwidth', str(args.bandwidth)]
                    if args.gain:
                        command += ['--gain', *map(str, args.gain)]
                    command += [str(iq), str(count)]
                    if not args.released:
                        command.append('manual')
                    result = subprocess.run(command, capture_output=True, text=True, timeout=args.seconds + 40)
                    text = result.stdout + result.stderr
                    stem.with_suffix(f'.capture{attempt}.log').write_text(text)
                    entry['attempts'].append({'returncode': result.returncode, 'log': text})
                    save()
                    if result.returncode == 0 and iq.exists() and iq.stat().st_size == count * 4:
                        break
                    if iq.exists():
                        iq.unlink()
                    if attempt == 3:
                        raise RuntimeError(f'Channel {ch} capture failed after three attempts; see logs')
                print(f'Channel {ch}: captured {count / 40000000:g} s; decoding', flush=True)
                entry['iq_metrics'] = iq_metrics(iq)
                command = [sys.executable, str(root / 'tools/decoders/wifi.py'), str(iq),
                           '--center', str(channel_mhz(ch)), '--channel', str(ch), '--phy', args.phy,
                           '--json', str(stem.with_suffix('.json')), '--pcap', str(stem.with_suffix('.pcap'))]
                with stem.with_suffix('.decode.log').open('w') as log:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=600)
                if result.returncode:
                    raise RuntimeError(f'Channel {ch} decoding failed; see logs')
                report = json.loads(stem.with_suffix('.json').read_text())
                entry['valid_frames'] = report['valid_frames']
                entry['networks'] = report['networks']
                if reference is not None:
                    entry['benchmark'] = score(report['networks'], reference, [ch])
                for net in report['networks']:
                    previous = manifest['networks'].get(net['bssid'])
                    if previous is None:
                        manifest['networks'][net['bssid']] = net | {'seen_passes': [pass_number]}
                    else:
                        previous['valid_frames'] += net['valid_frames']
                        previous['observed_channels'] = sorted(set(previous['observed_channels'] + net['observed_channels']))
                        previous['phys'] = sorted(set(previous['phys'] + net['phys']))
                        previous['seen_passes'] = sorted(set(previous['seen_passes'] + [pass_number]))
                        if previous['ssid'] == '<hidden>' and net['ssid'] != '<hidden>':
                            previous['ssid'], previous['ssid_hex'] = net['ssid'], net['ssid_hex']
                print(f"Channel {ch}: {entry['valid_frames']} valid frames; {len(entry['networks'])} BSSIDs", flush=True)
                save()
            finally:
                if iq.exists() and not args.keep_iq:
                    iq.unlink()
        manifest['status'] = 'complete'
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        manifest['status'] = 'failed'
        manifest['error'] = str(exc)
        print(str(exc), file=sys.stderr)
    except KeyboardInterrupt:
        manifest['status'] = 'interrupted'
        print('Scan interrupted; completed channel results are saved.', file=sys.stderr)
    finally:
        if reference is not None:
            manifest['benchmark'] = score(list(manifest['networks'].values()), reference, args.channels)
        manifest['finished_at'] = datetime.now().astimezone().isoformat()
        save()
    print(f"{len(manifest['networks'])} advertised BSSIDs; results: {out}", flush=True)
    return 0 if manifest['status'] == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
