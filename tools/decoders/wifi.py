"""Decode legacy DSSS + OFDM Wi-Fi and list only FCS-verified AP identities."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct

from cs16 import load
import combine as combine_mod
import wifi_b
import wifi_ofdm
from wifi_advertisements import advertised_neighbors


def network(frame):
    """Only beacons and probe responses establish an advertised network."""
    if len(frame) < 40 or frame[0] & 0x0F or frame[0] >> 4 not in (5, 8):
        return None
    ies, at = {}, 36
    while at + 2 <= len(frame) - 4:
        kind, size = frame[at:at + 2]
        if at + 2 + size > len(frame) - 4:
            break
        ies.setdefault(kind, frame[at + 2:at + 2 + size])
        at += 2 + size
    ssid = ies.get(0)
    return {'bssid': frame[16:22].hex(':'),
            'ssid': ssid.decode('utf-8', 'replace') if ssid and any(ssid) else '<hidden>',
            'ssid_hex': ssid.hex() if ssid is not None else None,
            'advertised_channel': ies[3][0] if ies.get(3) else None}


def write_pcap(path, packets):
    """DLT_IEEE802_11 (105); timestamps relative to recording start, FCS retained."""
    with Path(path).open('wb') as f:
        f.write(struct.pack('<IHHIIII', 0xA1B2C3D4, 2, 4, 0, 0, 65535, 105))
        for p in sorted(packets, key=lambda item: item['time_ms']):
            if not p['fcs_ok']:
                continue
            micros = max(0, round(p['time_ms'] * 1000))
            frame = p['frame']
            f.write(struct.pack('<IIII', micros // 1000000, micros % 1000000, len(frame), len(frame)))
            f.write(frame)


def erasure_supported(packets, min_check_bits=16):
    """Separate tier: BSSIDs supported by repeated FCS-constrained recoveries.

    Recovered frames (`dsss_recover`) never enter `networks`. A BSSID is listed
    here only with at least two independent recoveries (distinct SFD times and
    sequence numbers), each keeping at least `min_check_bits` unused FCS bits,
    and a consistent SSID and advertised channel. `verified_unaided` says
    whether normal FCS-valid frames also established it.
    """
    valid_sfd = {(p['channel'], round(p['sync_time_ms'] / .008)) for p in packets
                 if p.get('fcs_ok') and 'sync_time_ms' in p}
    seen, per = set(), {}
    for p in packets:
        info = p.get('recovery', {})
        if 'recovered_frame' not in p or info.get('check_bits_left', 0) < min_check_bits:
            continue
        key = (p['channel'], round(p['sync_time_ms'] / .008))
        if key in seen or key in valid_sfd:
            continue
        seen.add(key)
        frame = bytes.fromhex(p['recovered_frame'])
        net = network(frame)
        if net is None:
            continue
        seq = int.from_bytes(frame[22:24], 'little') >> 4
        per.setdefault(net['bssid'], []).append((net, seq, p, info))
    out = []
    for bssid, items in per.items():
        identities = {(n['ssid_hex'], n['advertised_channel']) for n, _, _, _ in items}
        sequences = {seq for _, seq, _, _ in items}
        out.append(items[0][0] | {
            'recovered_frames': len(items), 'distinct_sequence_numbers': len(sequences),
            'min_check_bits_left': min(i['check_bits_left'] for _, _, _, i in items),
            'consistent_identity': len(identities) == 1,
            'listed': len(items) >= 2 and len(sequences) >= 2 and len(identities) == 1,
            'observed_channels': sorted({p['channel'] for _, _, p, _ in items})})
    return out


def decode_window(x, center, channels, phy, threshold_db, min_us, stf_threshold, dsss_sync_search, cck,
                  recover=False):
    packets, diagnostics = [], []
    for ch in channels:
        if abs(wifi_b.channel_mhz(ch) - center) > 9:
            raise ValueError(f'Channel {ch} is outside the reliable interior of this capture; retune to its center')
        if phy == 'dsss' and ch > 14:
            raise ValueError(f'Channel {ch} is a 5 GHz channel; 802.11b DSSS exists only at 2.4 GHz')
        if phy in ('both', 'dsss') and ch <= 14:
            before = len(packets)
            wifi_b.decode_channel(x, center, ch, threshold_db, min_us, packets, quiet=True,
                                  sync_search=dsss_sync_search, cck=cck, recover=recover)
            diagnostics.append({'channel': ch, 'phy': 'DSSS', 'decoded': len(packets) - before,
                                'valid_frames': sum(p['fcs_ok'] for p in packets[before:]),
                                'sync_search': dsss_sync_search})
        if phy in ('both', 'ofdm'):
            found, candidates = wifi_ofdm.decode_channel(x, center, ch, stf_threshold)
            for p in found:
                p['phy'] = 'OFDM'
            packets.extend(found)
            diagnostics.append({'channel': ch, 'phy': 'OFDM', 'stf_candidates': candidates,
                                'valid_lsig_headers': sum(p.get('mpdu_index', 0) == 0 for p in found),
                                'statuses': dict(Counter(p['status'] for p in found))})
    return packets, diagnostics


BRANCHES = ('wf0', 'wf1', 'mrc')   # --combine: chain 0, chain 1, whitened combination


def merge_branches(packets):
    """One record per frame decoded by several branches (same channel, PHY, bytes, time < 20 us).

    FCS-valid frames keep the first branch's record (BRANCHES order) and list
    every branch that decoded them in `branches`. Records without a valid FCS
    are kept from chain 0 only, as a single-chain decode would report them.
    """
    seen, out = {}, []
    for p in sorted(packets, key=lambda p: (BRANCHES.index(p['branch']), p['channel'], p['time_ms'])):
        if not p['fcs_ok']:
            if p['branch'] == 'wf0':
                out.append(p)
            continue
        t = p.get('sync_time_ms', p['time_ms'])
        twins = seen.setdefault((p['channel'], p['phy'], p['frame']), [])
        twin = next((q for q in twins if abs(q.get('sync_time_ms', q['time_ms']) - t) < .02), None)
        if twin is not None:
            twin['branches'].append(p['branch'])
            continue
        p['branches'] = [p['branch']]
        twins.append(p)
        out.append(p)
    return out


def analyze(path, center, channels, phy='both', threshold_db=6, min_us=60, stf_threshold=.65,
            dsss_sync_search=True, window_samples=20_000_000, cck=False, recover=False, dc_block=0,
            combine=False, slot=0, of=1):
    """Decode bounded windows with ownership by preamble time, retaining boundaries.

    Seventy milliseconds covers even the maximum 16-bit DSSS duration field.
    A packet belongs to one core window by SFD/STF time, so overlap cannot inflate
    discovery counts. Memory use stays independent of recording duration.
    dc_block > 0 removes a static DC offset per block of that many samples
    (cs16.load); 0 decodes the samples as recorded.

    combine=True reads a two-slot interleaved recording (birria_rx --chain both)
    and decodes three branches: chain 0, chain 1, and their noise-whitened
    combination (combine.Combiner, one per channel). FCS-valid frames from any
    branch count once (merge_branches); times and durations count per-slot
    samples (40 MSps). It needs dc_block, since both-chain slots carry DC.

    of > 1 decodes one slot (`slot`, 0-based) of a recording whose I,Q pairs
    interleave `of` streams, without a split copy: birria_rx --chain both is
    of=2 (slot 0 = chain 0, slot 1 = chain 1). Times count that slot's samples.
    combine implies of=2 and decodes both slots.
    """
    size = Path(path).stat().st_size
    if size % 4 or not size:
        raise ValueError('Capture must contain complete I,Q pairs and at least one sample')
    if window_samples < 4096:
        raise ValueError('Decode windows must contain at least 4096 samples')
    if dc_block and dc_block < 4096:
        raise ValueError('DC blocks must contain at least 4096 samples')
    if combine and (of not in (1, 2) or size % 8 or not dc_block):
        raise ValueError('Combining needs a two-slot interleaved recording (whole slot pairs) and a DC block')
    if combine:
        of, slot = 2, 0
    if not (isinstance(of, int) and of >= 1 and isinstance(slot, int) and 0 <= slot < of):
        raise ValueError('Slots must satisfy 0 <= slot < of')
    if size % (4 * of):
        raise ValueError(f'A capture with {of} interleaved slots must hold whole groups of {of} I,Q pairs')
    total, overlap = size // (4 * of), 2_800_000
    combiners = {}
    if combine:
        combiners = {ch: combine_mod.Combiner(path, (wifi_b.channel_mhz(ch) - center) * 1e6, dc_block)
                     for ch in channels}
    packets, diagnostics = [], []
    for core in range(0, total, window_samples):
        core_end = min(total, core + window_samples)
        start, end = max(0, core - overlap), min(total, core_end + overlap)
        if combine:
            branches = [('wf0', channels, lambda: load(path, start, end - start, dc_block, slot=0, of=2)),
                        ('wf1', channels, lambda: load(path, start, end - start, dc_block, slot=1, of=2))]
            branches += [('mrc', [ch], lambda ch=ch: combiners[ch].samples(start, end - start))
                         for ch in channels if combiners[ch].usable]
        else:
            branches = [(None, channels, lambda: load(path, start=start, count=end - start, dc_block=dc_block,
                                                       slot=slot, of=of))]
        for branch, decoded, read in branches:
            x = read()
            found, details = decode_window(x, center, decoded, phy, threshold_db, min_us,
                                           stf_threshold, dsss_sync_search, cck, recover)
            for p in found:
                anchor = p.get('sync_time_ms', p['time_ms']) * 40000 + start
                if not core <= anchor < core_end:
                    continue
                p['time_ms'] += start / 40000
                for field in ('sync_time_ms', 'payload_start_time_ms', 'payload_end_time_ms'):
                    if field in p:
                        p[field] += start / 40000
                if 'sample' in p:
                    p['sample'] += start // 2  # OFDM's native 20 MSps index.
                if branch:
                    p['branch'] = branch
                packets.append(p)
            for detail in details:
                detail['window_start_seconds'] = start / 40000000
                detail['window_core_start_seconds'] = core / 40000000
                if branch:
                    detail['branch'] = branch
            diagnostics.extend(details)
            del x
    branch_valid = {}
    if combine:
        for b in BRANCHES:
            branch_valid[b] = sum(p['fcs_ok'] and p['branch'] == b for p in packets)
        packets = merge_branches(packets)
        branch_valid['mrc_only'] = sum(p['fcs_ok'] and p['branches'] == ['mrc'] for p in packets)
    packets.sort(key=lambda p: (p['channel'], p['time_ms']))
    networks, serialized, neighbors, advertisement_errors = {}, [], {}, []
    for p in packets:
        record = {k: v for k, v in p.items() if k != 'frame'}
        record['fields'] = wifi_b.describe(p['frame']) if p['fcs_ok'] else []
        serialized.append(record)
        if not p['fcs_ok']:
            continue
        net = network(p['frame'])
        if net is None:
            continue
        reported, errors = advertised_neighbors(p['frame'])
        advertisement_errors.extend({'time_ms': p['time_ms'], 'error': error} for error in errors)
        reported_keys = set()
        for neighbor in reported:
            key = (neighbor['reporter_bssid'], neighbor['bssid'], neighbor['operating_class'], neighbor['reported_channel'])
            if key in reported_keys:
                continue
            reported_keys.add(key)
            if key not in neighbors:
                neighbors[key] = neighbor | {'parent_valid_frames': 0}
            neighbors[key]['parent_valid_frames'] += 1
        bssid = net['bssid']
        if bssid not in networks:
            networks[bssid] = net | {'valid_frames': 0, 'phys': [], 'observed_channels': []}
        entry = networks[bssid]
        entry['valid_frames'] += 1
        if p['phy'] not in entry['phys']:
            entry['phys'].append(p['phy'])
        if p['channel'] not in entry['observed_channels']:
            entry['observed_channels'].append(p['channel'])
    header_rates, header_times = Counter(), {}
    for p in sorted((p for p in packets if p['phy'] == 'DSSS'), key=lambda p: p['sync_time_ms']):
        previous = header_times.get(p['channel'], -1e9)
        if p['sync_time_ms'] - previous >= .008:
            header_rates[str(p['rate_mbps'])] += 1
            header_times[p['channel']] = p['sync_time_ms']
    result = {'capture': str(path), 'center_mhz': center, 'decoded_channels': list(channels),
              'duration_seconds': total / 40000000,
              'decode_window_samples': window_samples, 'decode_overlap_samples': overlap,
              'cck_enabled': cck, 'dc_block_samples': dc_block, 'interleaved_slots': of,
              'slot': None if combine else slot,
              'combine': {'enabled': True, 'branch_valid_frames': branch_valid,
                          'channels': {str(ch): c.info for ch, c in combiners.items()}}
              if combine else {'enabled': False},
              'valid_frames': sum(bool(p['fcs_ok']) for p in packets),
              'networks': list(networks.values()), 'diagnostics': diagnostics, 'frames': serialized,
              'advertised_neighbors': list(neighbors.values()),
              'advertisement_parse_errors': advertisement_errors,
              'dsss_plcp_rates': dict(header_rates),
              'recovery_enabled': recover,
              'erasure_supported': erasure_supported(packets) if recover else [],
              'limitations': ['Legacy DSSS 1/2 Mbps and legacy OFDM; opt-in experimental CCK; no HT/VHT/HE or PBCC',
                              'Unverified frames never establish AP identities',
                              'Signal levels and frequency are uncalibrated; receiver blanking and aliasing remain']}
    return result, packets


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--center', type=float, required=True, help='Capture center in MHz')
    ap.add_argument('--channel', type=int, nargs='+', choices=wifi_b.CHANNELS, metavar='N',
                    help='2.4 or 5 GHz channel number(s) (default: the one at the centre); '
                         '5 GHz channels decode OFDM only')
    ap.add_argument('--phy', choices=['both', 'dsss', 'ofdm'], default='both')
    ap.add_argument('--threshold-db', type=float, default=6)
    ap.add_argument('--min-us', type=float, default=60)
    ap.add_argument('--stf-threshold', type=float, default=.65)
    ap.add_argument('--no-dsss-sync-search', action='store_true', help='Disable despread sync search for comparisons')
    ap.add_argument('--cck', action='store_true', help='Enable experimental 5.5/11 Mbps CCK payload decoding')
    ap.add_argument('--recover', action='store_true',
                    help='Research tier: FCS-constrained recovery of failed 1 Mbps frames, reported separately')
    ap.add_argument('--dc-block', type=int, nargs='?', const=65536, default=0, metavar='SAMPLES',
                    help='Subtract the mean of each block of SAMPLES (default 65536, 1.6 ms); '
                         'for BirriaSDR --chain both slots, which carry a static DC offset')
    ap.add_argument('--of', type=int, default=1, metavar='N',
                    help='The capture interleaves N streams pair by pair (birria_rx --chain both: 2); decode --slot')
    ap.add_argument('--slot', type=int, default=0, metavar='K',
                    help='With --of N: the 0-based slot to decode (--chain both: 0 = chain 0, 1 = chain 1)')
    ap.add_argument('--combine', action='store_true',
                    help='The capture is a birria_rx --chain both recording: decode chain 0, chain 1 and their '
                         'noise-whitened combination, and count each FCS-valid frame once '
                         '(implies --dc-block 65536 unless given)')
    ap.add_argument('--json', type=Path)
    ap.add_argument('--pcap', type=Path, help='Valid frames for Wireshark; relative timestamps, FCS retained')
    ap.add_argument('--packets', action='store_true', help='Also print each verified frame')
    args = ap.parse_args()
    if not 0 < args.stf_threshold < 1 or args.min_us <= 0:
        ap.error('Require 0 < --stf-threshold < 1 and --min-us > 0')
    if args.capture.stat().st_size % 4:
        ap.error('Capture size must be a multiple of four bytes (I,Q pairs)')
    if args.combine and (args.of not in (1, 2) or args.slot):
        ap.error('--combine decodes both slots of a two-slot recording; drop --slot and use --of 2 or omit it')
    channels = args.channel or [wifi_b.nearest_channel(args.center)]
    if args.combine and not args.dc_block:
        args.dc_block = 65536
    try:
        result, packets = analyze(args.capture, args.center, channels, args.phy,
                                  args.threshold_db, args.min_us, args.stf_threshold, not args.no_dsss_sync_search,
                                  cck=args.cck, recover=args.recover, dc_block=args.dc_block, combine=args.combine,
                                  slot=args.slot, of=args.of)
    except ValueError as exc:
        ap.error(str(exc))
    print(f"{result['duration_seconds']:g} seconds; {result['valid_frames']} valid frames; "
          f"{len(result['networks'])} advertised BSSIDs")
    if args.combine:
        counts = result['combine']['branch_valid_frames']
        print('branches: ' + ', '.join(f'{k} {v}' for k, v in counts.items()))
        for ch, info in result['combine']['channels'].items():
            print(f"channel {ch}: {info['status']}; slot-1 delay {info['delay_samples']:+g} samples "
                  f"({info['delay_bursts']} bursts); noise correlation {info.get('noise_correlation', float('nan')):.2f}")
    for net in result['networks']:
        print(f"{net['bssid']} SSID={net['ssid']!r} channel={net['advertised_channel']} "
              f"frames={net['valid_frames']} PHY={','.join(net['phys'])}")
    for net in result['erasure_supported']:
        print(f"[erasure tier{' listed' if net['listed'] else ''}] {net['bssid']} SSID={net['ssid']!r} "
              f"channel={net['advertised_channel']} recovered={net['recovered_frames']} "
              f"min_check_bits_left={net['min_check_bits_left']}")
    for diagnostic in result['diagnostics']:
        print(json.dumps(diagnostic))
    if args.packets:
        for p in packets:
            if p['fcs_ok']:
                print(f"{p['time_ms']:10.3f} ms {p['phy']} {p['rate_mbps']} Mbps " + ' '.join(wifi_b.describe(p['frame'])))
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + '\n')
    if args.pcap:
        write_pcap(args.pcap, packets)


if __name__ == '__main__':
    main()
