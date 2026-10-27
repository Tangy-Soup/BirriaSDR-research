"""Bounded offline source-gap, intact-payload and advertisement inventory.

Diagnostic failed-frame bytes never contribute identities. No reference input.
"""
import argparse
from collections import Counter
import json
from pathlib import Path

import numpy as np
from scipy import signal

import wifi
import wifi_b
from analyze_blanking import zero_runs, payload_gaps
from cs16 import load


def histogram(values, limit=20):
    return [{'value': int(k), 'count': int(v)} for k, v in Counter(map(int, values)).most_common(limit)]


def gap_structure(runs, total):
    starts, ends = runs.T if len(runs) else (np.array([], dtype=int),) * 2
    widths = ends - starts
    phases = {}
    for period in (4, 16, 40, 128, 512, 2048, 16384, 65536):
        counts = np.bincount(starts % period, minlength=period)
        phases[str(period)] = {'occupied_phases': int(np.count_nonzero(counts)),
            'max_phase_count': int(counts.max()), 'top_phases': histogram(starts % period, 8),
            'starts_at_boundary': int(np.sum(starts % period == 0)),
            'runs_crossing_boundary': int(np.sum(starts // period != (ends - 1) // period))}
    return {'samples': total, 'runs': len(runs), 'zero_run_samples': int(widths.sum()),
        'zero_run_fraction': float(widths.sum() / total), 'widths': histogram(widths),
        'start_spacing_samples': histogram(np.diff(starts)),
        'start_spacing_modulo_40_samples': histogram(np.diff(starts) % 40, 40),
        'spacing_percentiles_samples': np.percentile(np.diff(starts), [0, 10, 50, 90, 100]).tolist() if len(starts) > 1 else [],
        'boundary_phases': phases, 'first_intervals': runs[:20].tolist()}


def elements(data):
    at, result = 0, []
    while at + 2 <= len(data):
        kind, length = data[at:at + 2]
        if at + 2 + length > len(data):
            return result, False
        value = data[at + 2:at + 2 + length]
        result.append((kind, value))
        at += 2 + length
    return result, at == len(data)


def packet_quality(path, packet):
    # Analyze only this packet. Barker correlation is not a calibrated SNR.
    sfd = round(packet['sync_time_ms'] * 40000)
    start = max(0, sfd - 100 * 40)
    end = round(packet['payload_end_time_ms'] * 40000) + 100 * 40
    x = load(path, start=start, count=end - start)
    y = signal.resample_poly(x, 11, 10).astype(np.complex64)
    y = signal.fftconvolve(y, signal.firwin(101, 10e6, fs=44e6), 'same')
    c = signal.fftconvolve(y, np.repeat(wifi_b.BARKER, 4)[::-1], 'full')[43:]
    anchor = round((packet['sync_time_ms'] * 40000 - start) * 1.1)
    pre = np.arange(max(2, anchor - 48 * 44), anchor - 4 * 44, 44)
    if len(pre) < 16:
        return {}
    offsets = np.arange(-2, 3)
    strengths = np.mean(np.abs(c[pre[:, None] + offsets]) ** 2, axis=0)
    step = int(np.argmax(strengths)) - 2
    sym = c[pre + step]
    diff = sym[1:] * np.conj(sym[:-1])
    phase = .5 * np.angle(np.sum(diff ** 2 / np.maximum(np.abs(diff), 1e-20)))
    error = np.angle(diff * np.exp(-1j * phase))
    error = (error + np.pi / 2) % np.pi - np.pi / 2
    payload = round((packet['payload_start_time_ms'] * 40000 - start) * 1.1)
    centers = np.arange(payload, min(len(c) - 3, payload + round(packet['length_us'] * 44)), 44)
    tracked, positions, pos = [], [], wifi_b.acquire_timing(c, max(2, anchor - 48 * 44))
    # Same bounded timing rule as symbols(), with positions retained for diagnosis.
    for index in range(min(66000, max(0, (len(c) - pos - 3) // 44))):
        if pos + 2 >= len(c):
            break
        if index % 16 == 0:
            train = pos + 44 * np.arange(min(16, (len(c) - pos - 3) // 44))
            if len(train):
                energy = np.abs(c[train[:, None] + offsets]).sum(axis=0)
                pos += int(np.clip(np.argmax(energy) - 2, -1, 1))
        positions.append(pos)
        tracked.append(c[pos])
        pos += 44
    tracked, positions = np.asarray(tracked), np.asarray(positions)
    inside = (positions >= payload) & (positions < payload + packet['length_us'] * 44)
    body = tracked[inside]
    differential = body[1:] * np.conj(body[:-1]) * np.exp(-1j * phase)
    margin = np.abs(np.real(differential)) / np.maximum(np.abs(differential), 1e-20)
    drift = positions[inside] - 44 * np.arange(np.count_nonzero(inside))
    return {'preamble_differential_phase_rms_rad': float(np.sqrt(np.mean(error ** 2))),
        'preamble_cfo_hz_from_sfd': float(phase * 1e6 / (2 * np.pi)),
        'decoder_cfo_hz': packet['cfo_hz'],
        'preamble_barker_power': float(np.mean(np.abs(sym) ** 2)),
        'payload_barker_power_percentiles': np.percentile(np.abs(c[centers]) ** 2, [10, 50, 90]).tolist() if len(centers) else [],
        'payload_dbpsk_absolute_decision_margin_percentiles': np.percentile(margin, [10, 50, 90]).tolist() if len(margin) else [],
        'payload_dbpsk_margin_below_0p25_fraction': float(np.mean(margin < .25)) if len(margin) else None,
        'tracked_timing_excursion_samples44': int(np.ptp(drift)) if len(drift) else None,
        'warning': 'Diagnostic resampling and preamble anchor differ slightly from original candidate; margins are not true bit error rates'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--channel', type=int, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    runs = zero_runs(args.capture)
    gaps = gap_structure(runs, args.capture.stat().st_size // 4)
    (args.output / 'gaps.json').write_text(json.dumps(gaps, indent=2) + '\n')
    report, packets = wifi.analyze(args.capture, wifi_b.channel_mhz(args.channel), [args.channel], cck=True)
    (args.output / 'decode.json').write_text(json.dumps(report, indent=2) + '\n')
    wifi.write_pcap(args.output / 'valid.pcap', packets)
    payloads, seen, adverts = [], [], {}
    for p in sorted(packets, key=lambda p: (not p['fcs_ok'], p.get('sync_time_ms', p['time_ms']))):
        if p['fcs_ok'] and wifi.network(p['frame']):
            frame = p['frame']
            ies, complete = elements(frame[36:-4])
            key = frame[16:22].hex(':')
            entry = adverts.setdefault(key, {'network': wifi.network(frame), 'frames': 0, 'ie_ids': Counter(), 'special_elements': []})
            entry['frames'] += 1
            entry['ie_ids'].update(str(kind) for kind, value in ies)
            # Multiple BSSID, RNR, extension IEs: inventory only until verified.
            for kind, value in ies:
                item = {'id': kind, 'hex': value.hex(), 'complete_outer_ies': complete}
                if kind in (71, 201, 255) and item not in entry['special_elements']:
                    entry['special_elements'].append(item)
        if p['phy'] != 'DSSS' or not p['frame']:
            continue
        if any(abs(p['sync_time_ms'] - previous) < .008 for previous in seen):
            continue
        seen.append(p['sync_time_ms'])
        start, end = round(p['payload_start_time_ms'] * 40000), round(p['payload_end_time_ms'] * 40000)
        overlap = payload_gaps(runs, start, end)
        entry = {k: p[k] for k in ('sync_time_ms', 'payload_start_time_ms', 'payload_end_time_ms', 'rate_mbps', 'length_us', 'fcs_ok', 'cfo_hz', 'preamble')}
        entry['gaps'] = overlap
        entry['quality'] = packet_quality(args.capture, p)
        if not p['fcs_ok'] and not overlap['runs']:
            entry['failed_frame_hex_unverified'] = p['frame'].hex()
        payloads.append(entry)
    result = {'capture': str(args.capture), 'gap_structure': gaps,
        'deduplication': 'SFD within 8 us, prefer FCS-valid decode',
        'payloads': sorted(payloads, key=lambda p: p['sync_time_ms']),
        'valid_advertisement_inventory': adverts,
        'direct_networks': report['networks'], 'valid_frames': report['valid_frames'],
        'advertised_neighbors': report['advertised_neighbors'],
        'profile_derived_identities': [], 'profile_note': 'Inventory only; no identity inferred from unparsed elements'}
    (args.output / 'diagnosis.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'channel': args.channel, 'gaps': gaps['runs'], 'fraction': gaps['zero_run_fraction'],
        'valid_frames': report['valid_frames'], 'networks': report['networks'],
        'intact_failed': [p for p in payloads if not p['fcs_ok'] and not p['gaps']['runs']]}), flush=True)


if __name__ == '__main__':
    main()
