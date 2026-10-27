"""Conservative RNR inventory, separate from directly received AP identities.

Layouts: Linux include/linux/ieee80211.h ieee80211_neighbor_ap_info,
ieee80211_tbtt_info_7_8_9 and ieee80211_tbtt_info_ge_11 (IEEE 802.11 RNR).
Unknown field types/layouts are skipped; truncated elements fail atomically.
This does not expand Multiple BSSID profiles or infer SSIDs from short hashes.
"""
import zlib


def elements(data):
    result, at = [], 0
    while at < len(data):
        if at + 2 > len(data):
            raise ValueError('truncated IE header')
        kind, length = data[at:at + 2]
        end = at + 2 + length
        if end > len(data):
            raise ValueError('truncated IE value')
        result.append((kind, data[at + 2:end]))
        at = end
    return result


def reduced_neighbors(value):
    result, at = [], 0
    while at < len(value):
        if at + 4 > len(value):
            raise ValueError('truncated RNR header')
        header, length, op_class, channel = value[at:at + 4]
        count = (header >> 4) + 1
        at += 4
        end = at + count * length
        if not length or end > len(value):
            raise ValueError('truncated or zero-length RNR information')
        # Type 0 layouts only. Type 1 and reserved types have other semantics.
        if header & 3 == 0 and (length in (7, 8, 9) or length >= 11):
            for offset in range(at, end, length):
                info = value[offset:offset + length]
                bssid = info[1:7]
                if bssid[0] & 1 or not any(bssid):
                    continue
                entry = {'bssid': bssid.hex(':'), 'operating_class': op_class,
                    'reported_channel': channel, 'tbtt_offset': info[0],
                    'tbtt_information_length': length,
                    'identity_source': 'reduced_neighbor_report', 'directly_observed': False}
                if length >= 11:
                    entry['short_ssid_hex'] = info[7:11].hex()
                result.append(entry)
        at = end
    return result


def advertised_neighbors(frame):
    """Require an independently valid parent beacon/probe-response FCS."""
    if (len(frame) < 40 or frame[0] & 15 or frame[0] >> 4 not in (5, 8) or frame[1] & 0x40 or
            zlib.crc32(frame[:-4]) != int.from_bytes(frame[-4:], 'little')):
        return [], []
    try:
        ies = elements(frame[36:-4])
    except ValueError as exc:
        return [], [str(exc)]
    result, errors = [], []
    for kind, value in ies:
        if kind != 201:
            continue
        try:
            neighbors = reduced_neighbors(value)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        for neighbor in neighbors:
            neighbor['reporter_bssid'] = frame[16:22].hex(':')
            if neighbor not in result:
                result.append(neighbor)
    return result, errors
