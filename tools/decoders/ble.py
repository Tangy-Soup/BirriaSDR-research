"""Decode Bluetooth LE legacy advertising packets from a BirriaSDR capture.

Usage: ble.py capture.cs16 --center 2437 [--channels 37 38 39]
Advertising channels outside the capture bandwidth are skipped. Every packet is CRC-checked.
"""
import argparse
import sys
import numpy as np
from scipy import signal

from cs16 import FS, blanking_mask, load, shift

ADV_FREQ = {37: 2402e6, 38: 2426e6, 39: 2480e6}
AA = 0x8E89BED6
SPS = 4  # samples per symbol after decimation to 4 MSps

PDU_TYPES = {0: "ADV_IND", 1: "ADV_DIRECT_IND", 2: "ADV_NONCONN_IND", 3: "SCAN_REQ",
             4: "SCAN_RSP", 5: "CONNECT_IND", 6: "ADV_SCAN_IND", 7: "ADV_EXT_IND"}
COMPANIES = {0x004C: "Apple", 0x0006: "Microsoft", 0x0075: "Samsung", 0x00E0: "Google",
             0x0087: "Garmin", 0x0157: "Huami", 0x038F: "Xiaomi", 0x02E5: "Espressif",
             0x0059: "Nordic", 0x0171: "Amazon", 0x0499: "Ruuvi", 0x000F: "Broadcom",
             0x05A7: "Sonos", 0x02FF: "Silicon Labs", 0x0002: "Intel"}
UUID16 = {0xFEAF: "Nest", 0xFE9F: "Google", 0xFEF3: "Google", 0xFE2C: "Google Fast Pair",
          0xFD6F: "Exposure Notification", 0xFEAA: "Eddystone", 0xFEED: "Tile",
          0xFE03: "Amazon", 0xFD5A: "Samsung", 0x180F: "Battery"}


def bits_lsb(value, n):
    return np.array([(value >> i) & 1 for i in range(n)], dtype=np.int8)


def whitening(chan, nbits):
    lfsr = int(f"{chan:08b}"[::-1], 2) | 2
    out = np.zeros(nbits, dtype=np.int8)
    for i in range(nbits):
        if lfsr & 0x80:
            lfsr ^= 0x11
            out[i] = 1
        lfsr = (lfsr << 1) & 0xFF
    return out


def crc24(bits, init=0x555555):
    crc = init
    for b in bits:
        fb = ((crc >> 23) & 1) ^ int(b)
        crc = (crc << 1) & 0xFFFFFF
        if fb:
            crc ^= 0x00065B
    return crc


def to_bytes(bits):
    return bytes(int(sum(int(b) << i for i, b in enumerate(bits[k:k + 8]))) for k in range(0, len(bits) - 7, 8))


def parse_ad(data):
    out, i = [], 0
    while i < len(data):
        ln = data[i]
        if ln == 0 or i + 1 + ln > len(data):
            break
        t, v = data[i + 1], data[i + 2:i + 1 + ln]
        if t == 0x01 and v:
            out.append(f"flags=0x{v[0]:02x}")
        elif t in (0x08, 0x09):
            out.append(f"name={v.decode('utf-8', 'replace')!r}")
        elif t == 0x0A and v:
            out.append(f"tx_power={int.from_bytes(v[:1], 'little', signed=True)}dBm")
        elif t in (0x02, 0x03):
            ids = [int.from_bytes(v[k:k + 2], "little") for k in range(0, len(v) - 1, 2)]
            out.append("uuid16=" + ",".join(f"{u:04x}" + (f"({UUID16[u]})" if u in UUID16 else "") for u in ids))
        elif t == 0x16 and len(v) >= 2:
            u = int.from_bytes(v[:2], "little")
            out.append(f"svc_data[{u:04x}{'/' + UUID16[u] if u in UUID16 else ''}]={v[2:].hex()}")
        elif t == 0xFF and len(v) >= 2:
            c = int.from_bytes(v[:2], "little")
            out.append(f"mfr[{COMPANIES.get(c, hex(c))}]={v[2:].hex()}")
        else:
            out.append(f"ad{t:02x}={v.hex()}")
        i += 1 + ln
    return out


def decode_channel(x, zero, center, chan):
    taps = signal.firwin(161, 700e3, fs=FS).astype(np.float32)
    y = signal.upfirdn(taps, shift(x, ADV_FREQ[chan] - center), down=10).astype(np.complex64)
    zero_ds = np.convolve(zero.astype(np.float32), np.ones(170), "same")[::10][:len(y)] > 0
    d = np.angle(y[1:] * np.conj(y[:-1])).astype(np.float32)
    pwr = np.abs(y) ** 2
    floor = np.median(pwr)
    soft = np.convolve(d, np.ones(SPS, dtype=np.float32), "full")[SPS - 1:]
    aa_bits = bits_lsb(AA, 32)
    kern = (np.repeat(2 * aa_bits - 1, SPS) * np.tile(np.r_[1, np.zeros(SPS - 1)], 32))[::-1].astype(np.float32)
    corr = signal.fftconvolve(np.sign(soft), kern, "full")[len(kern) - 1:]
    found, last = [], -10**9
    w = whitening(chan, 8 * 40)
    for c in np.where(corr >= 30)[0]:
        if c - last < 32 * SPS or pwr[c:c + 32 * SPS].mean() < 8 * floor:
            continue
        syms = soft[c::SPS][:32 + 8 * 40]
        bits = (syms - np.mean(syms[:32]) > 0).astype(np.int8)
        if np.sum(bits[:32] != aa_bits) > 2:
            continue
        last = c
        ln = int(sum(int(b) << i for i, b in enumerate(bits[40:48] ^ w[8:16])))
        if not 6 <= ln <= 37:
            continue
        nb = 16 + 8 * ln + 24
        pdu = bits[32:32 + nb] ^ w[:nb]
        if len(pdu) < nb:
            continue
        ok = crc24(pdu[:-24]) == int("".join(map(str, pdu[-24:])), 2)
        blanked = bool(zero_ds[c:c + (32 + nb) * SPS].any())
        snr = 10 * np.log10(pwr[c:c + 32 * SPS].mean() / floor)
        found.append((c / 4e6, chan, ok, blanked, to_bytes(pdu[:-24]), snr))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--center", type=float, required=True, help="capture centre frequency in MHz")
    ap.add_argument("--channels", type=int, nargs="+", default=[37, 38, 39])
    args = ap.parse_args()
    center = args.center * 1e6
    chans = [c for c in args.channels if abs(ADV_FREQ[c] - center) <= 18e6]
    if not chans:
        sys.exit(f"no advertising channel within the capture (37=2402, 38=2426, 39=2480 MHz)")
    x = load(args.capture)
    zero = blanking_mask(x)
    pk = sorted(p for c in chans for p in decode_channel(x, zero, center, c))
    bad = [p for p in pk if not p[2]]
    print(f"channels {chans}: {len(pk)} packets, {len(pk) - len(bad)} CRC-valid, "
          f"{sum(p[3] for p in bad)} failed with receiver blanking inside, {sum(not p[3] for p in bad)} failed otherwise")
    devices = {}
    for t, chan, ok, _, raw, snr in pk:
        if not ok:
            continue
        h0 = raw[0]
        kind = h0 & 0xF
        adva = raw[2:8][::-1].hex(":")
        ad = parse_ad(raw[8:]) if kind in (0, 2, 4, 6) else [raw[8:].hex()]
        print(f"{t * 1e3:8.3f} ms  ch{chan}  {snr:4.1f} dB  {PDU_TYPES.get(kind, kind):15s} {adva} "
              f"({'random' if (h0 >> 6) & 1 else 'public'})  {' '.join(ad)}")
        devices.setdefault(adva, set()).update(a for a in ad if a.startswith(("name=", "mfr[", "uuid16=")))
    print(f"\n{len(devices)} distinct advertisers")
    for a, info in devices.items():
        print(f"  {a}  {' '.join(sorted(i.split('=')[0] + '=' + i.split('=')[1][:24] for i in info))}")


if __name__ == "__main__":
    main()
