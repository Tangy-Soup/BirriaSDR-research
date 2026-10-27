"""Summarize a monitor-mode radiotap pcap as a passive AP reference.

Used for the stock-hardware reference (round four, Phase 3): a second adapter in
monitor mode records the same channel. Only beacons and probe responses
establish networks, as in `wifi.py`. When radiotap says the frame carries its
FCS, the FCS is recomputed and bad frames are excluded; otherwise the FCS
check is the receiving hardware's, and the summary says so.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import statistics
import struct
import zlib

from wifi import network

# radiotap field: (alignment, size) for the fields up to dBm antenna signal.
FIELDS = {0: (8, 8), 1: (1, 1), 2: (1, 1), 3: (2, 4), 4: (1, 2), 5: (1, 1)}
FLAG_FCS, FLAG_BADFCS = 0x10, 0x40


def packets(path):
    data = Path(path).read_bytes()
    magic, = struct.unpack_from('<I', data)
    if magic != 0xA1B2C3D4:
        raise ValueError('expected a little-endian microsecond pcap')
    linktype, = struct.unpack_from('<I', data, 20)
    if linktype != 127:
        raise ValueError(f'expected radiotap linktype 127, got {linktype}')
    at = 24
    while at + 16 <= len(data):
        sec, usec, incl, _ = struct.unpack_from('<IIII', data, at)
        at += 16
        yield sec + usec / 1e6, data[at:at + incl]
        at += incl


def radiotap(pkt):
    """Return (fields, 802.11 frame) for the fields named in FIELDS."""
    length, = struct.unpack_from('<H', pkt, 2)
    words, off = [], 4
    while True:
        word, = struct.unpack_from('<I', pkt, off)
        words.append(word)
        off += 4
        if not word >> 31:
            break
    fields, pos = {}, off
    present = words[0]
    for bit in range(6):
        if not present >> bit & 1:
            continue
        align, size = FIELDS[bit]
        pos += -pos % align
        raw = pkt[pos:pos + size]
        pos += size
        if bit == 0:
            fields['tsft_us'] = struct.unpack('<Q', raw)[0]
        elif bit == 1:
            fields['flags'] = raw[0]
        elif bit == 2:
            fields['rate_mbps'] = raw[0] / 2
        elif bit == 3:
            fields['freq_mhz'], fields['chan_flags'] = struct.unpack('<HH', raw)
        elif bit == 5:
            fields['dbm'] = struct.unpack('<b', raw)[0]
    return fields, pkt[length:]


def summarize(path):
    kinds, nets = Counter(), {}
    per = defaultdict(lambda: {'frames': 0, 'dbm': [], 'rates': Counter(), 'subtypes': Counter()})
    fcs_present = fcs_bad = 0
    first = last = None
    for t, pkt in packets(path):
        first = t if first is None else first
        last = t
        fields, frame = radiotap(pkt)
        flags = fields.get('flags', 0)
        if flags & FLAG_FCS:
            fcs_present += 1
            ok = len(frame) >= 4 and zlib.crc32(frame[:-4]) == int.from_bytes(frame[-4:], 'little')
            if not ok or flags & FLAG_BADFCS:
                fcs_bad += 1
                continue
        else:
            frame = frame + zlib.crc32(frame).to_bytes(4, 'little')   # network() expects an FCS
        if len(frame) < 4:
            continue
        kinds[(frame[0] >> 2 & 3, frame[0] >> 4)] += 1
        net = network(frame)
        if not net:
            continue
        nets[net['bssid']] = net
        entry = per[net['bssid']]
        entry['frames'] += 1
        entry['subtypes']['beacon' if frame[0] >> 4 == 8 else 'probe_response'] += 1
        if 'dbm' in fields:
            entry['dbm'].append(fields['dbm'])
        if 'rate_mbps' in fields:
            entry['rates'][fields['rate_mbps']] += 1
    networks = []
    for bssid, net in sorted(nets.items()):
        e = per[bssid]
        networks.append(net | {'frames': e['frames'], 'subtypes': dict(e['subtypes']),
                               'median_dbm': statistics.median(e['dbm']) if e['dbm'] else None,
                               'rates_mbps': {str(k): v for k, v in sorted(e['rates'].items())}})
    return {'capture': str(path),
            'duration_seconds': round(last - first, 3) if first is not None else 0,
            'fcs_check': ('recomputed in software for frames carrying FCS'
                          if fcs_present else 'hardware only: radiotap reported no FCS'),
            'frames_with_fcs': fcs_present, 'frames_bad_fcs_excluded': fcs_bad,
            'frame_types': {f'type{t}_subtype{s}': n for (t, s), n in sorted(kinds.items())},
            'networks': networks}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('pcap')
    ap.add_argument('--output', help='write JSON here')
    args = ap.parse_args()
    result = summarize(args.pcap)
    text = json.dumps(result, indent=1)
    if args.output:
        Path(args.output).write_text(text)
    for n in result['networks']:
        print(f"{n['bssid']}  ch {n['advertised_channel']!s:>3}  {n['frames']:4d} frames  "
              f"{n['median_dbm']!s:>5} dBm  rates {n['rates_mbps']}  {n['ssid']}")
    print({k: result[k] for k in ['duration_seconds', 'fcs_check', 'frames_with_fcs', 'frames_bad_fcs_excluded']})


if __name__ == '__main__':
    main()
