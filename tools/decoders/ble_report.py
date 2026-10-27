"""Summarize a birria_ble packet log (.jsonl) with the advertising-data parser from ble.py.

Usage: ble_report.py run.jsonl [--packets] [--top N]
One line per address: packets per channel, best SNR, PDU types and the
names / manufacturers / service UUIDs it advertised. --packets also prints
every packet. Reads the log line by line, so memory stays small.
"""
import argparse
import json
from collections import defaultdict

from ble import parse_ad

AD_TYPES = {"ADV_IND", "ADV_NONCONN_IND", "SCAN_RSP", "ADV_SCAN_IND"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--packets", action="store_true", help="print every packet too")
    ap.add_argument("--top", type=int, default=0, help="only the N most frequent addresses")
    args = ap.parse_args()
    devices = defaultdict(lambda: {"n": 0, "ch": defaultdict(int), "snr": -99.0, "types": set(), "info": set(),
                                   "first": None, "last": None, "random": False})
    first_t = last_t = None
    total = 0
    with open(args.log) as f:
        for line in f:
            p = json.loads(line)
            total += 1
            first_t = p["t"] if first_t is None else first_t
            last_t = p["t"]
            raw = bytes.fromhex(p["pdu"])
            ad = parse_ad(raw[8:]) if p["type"] in AD_TYPES else []
            if args.packets:
                print(f"{p['t']:.6f}  ch{p['ch']}  {p['snr_db']:5.1f} dB  {p['cfo_khz']:+6.1f} kHz  "
                      f"{p['type']:15s} {p['addr'] or '-':17s}  {' '.join(ad)}")
            if not p["addr"]:
                continue
            d = devices[p["addr"]]
            d["n"] += 1
            d["ch"][p["ch"]] += 1
            d["snr"] = max(d["snr"], p["snr_db"])
            d["types"].add(p["type"])
            d["random"] = bool(p["txadd"])
            d["first"] = p["t"] if d["first"] is None else d["first"]
            d["last"] = p["t"]
            d["info"].update(a for a in ad if a.startswith(("name=", "mfr[", "uuid16=")))
    span = (last_t - first_t) if total else 0
    print(f"{total} packets over {span:.1f} s, {len(devices)} addresses")
    ranked = sorted(devices.items(), key=lambda kv: -kv[1]["n"])
    if args.top:
        ranked = ranked[:args.top]
    for addr, d in ranked:
        chans = " ".join(f"{c}:{n}" for c, n in sorted(d["ch"].items()))
        info = " ".join(sorted(i.split("=")[0] + "=" + i.split("=", 1)[1][:24] for i in d["info"]))
        print(f"  {addr} {'rnd' if d['random'] else 'pub'} {d['n']:6d}  [{chans}]  {d['snr']:5.1f} dB  "
              f"{d['last'] - d['first']:6.1f} s  {','.join(sorted(d['types']))}  {info}")


if __name__ == "__main__":
    main()
