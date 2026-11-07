#!/usr/bin/env python3
"""Per-packet legacy CSI from the L-LTF, as versioned JSON Lines.

    csi_export.py capture.cs16 decode.json out.jsonl [--center MHz] [--slot K --of 2]
                  [--dc-block [N]] [--chain C] [--gain L F D] [--epoch TEXT] [--normalize] [--valid-only]

Post-processes a wifi.py --json decode; it never changes what was decoded. For each OFDM
or HT record the 20 MHz channel is cut out as wifi_ofdm.decode_channel does it, the L-LTF
is re-found near the decoder's position, the CFO is removed, and the two repeats A, B give
per subcarrier (52 legacy carriers, -26..-1, +1..+26):

    h_raw    = (A + B) / 2 / LTF, the decoder's raw estimate
    h_smooth = h_raw projected onto impulse responses of -12..+16 taps, the decoder's smoothing
    noise    = |A - B|^2 / 2

The first line is a header describing the capture. Every later line is one PPDU. Complex
vectors are [re, im, re, im, ...] in FFT-bin units of samples scaled to +/-1 full scale.

**What this isn't:**
- Not calibrated. Receiver gain and filter shape stay in the values; compare shapes across
  packets only at the same settings and input rate, or use --normalize (unit RMS, which
  removes one amplitude scale and nothing else).
- Phase includes packet timing, residual CFO and hardware response.
- Selected by the decoder: FCS-valid-only CSI (--valid-only) is biased toward easy packets
  and can't estimate what failed frames saw. FCS-failed records are kept by default. Their
  decoded identity can't be trusted, so they carry no address and fcs_ok is false.

**Two chains:** run once per slot of a --chain both recording (--slot K --of 2 --dc-block)
on the same decode JSON. `packet` is the same on both, and `ltf_offset` gives each chain's
delay in 20 MSps samples.

**Privacy:** `source` holds a neighbour's MAC address when the FCS is valid, so treat the
output like any other capture product: nothing leaves the machine without publish_scan.py.
"""
import argparse
import json
import subprocess
from pathlib import Path

import numpy as np
from scipy import signal

import cs16
import wifi_ofdm as W

FORMAT, VERSION = 'birria-csi', 1
MARGIN = 2000  # 40 MSps samples either side of the frame start
K = W.ACTIVE % 64


def flat(z):
    """Complex vector as [re, im, ...] with float32 precision."""
    return np.round(np.column_stack((z.real, z.imag)).ravel().astype(float), 8).tolist()


def packet_csi(path, rec, center, total, slot=0, of=1, dc_block=0):
    """CSI of one record, or None if the frame is too close to the end of the file."""
    off = (W.channel_mhz(rec['channel']) - center) * 1e6
    start = max(0, 2 * rec['sample'] - MARGIN)
    count = min(total - start, 2 * MARGIN + 2 * (192 + 128 + 400))
    x = cs16.load(path, start, count, dc_block=dc_block, slot=slot, of=of)
    y = signal.resample_poly(W.shift(x, off), 1, 2).astype(np.complex64)
    guess = rec['sample'] + 192 - start // 2
    lo, hi = max(0, guess - 24), min(len(y) - 128, guess + 24)
    if hi <= lo:
        return None
    corr = np.abs(signal.correlate(y[lo:hi + 128], W.LTF, mode='valid')) ** 2
    pair = np.minimum(corr[:-64], corr[64:])
    ltf = lo + int(np.argmax(pair))
    t = y[ltf:ltf + 128].astype(np.complex128)
    omega = np.angle(np.vdot(t[:64], t[64:])) / 64
    t = t * np.exp(-1j * omega * np.arange(128))
    a, b = np.fft.fft(t[:64]), np.fft.fft(t[64:])
    h_raw = (a[K] + b[K]) / 2 / W.LTF_BINS[K]
    noise = np.abs(a[K] - b[K]) ** 2 / 2
    sig = np.abs((a[K] + b[K]) / 2) ** 2 - noise / 2
    return dict(ltf_offset=int(ltf - guess), cfo_residual_hz=float(omega * 20e6 / (2 * np.pi)),
                snr_db=float(10 * np.log10(max(sig.sum(), 1e-30) / max(noise.sum(), 1e-30))),
                h_raw=h_raw, h_smooth=W.LEGACY_SMOOTH @ h_raw, noise=noise)


def processor_version():
    """The decoder commit, marked when the working tree differs from it."""
    here = Path(__file__).resolve().parent
    try:
        return subprocess.run(['git', 'describe', '--always', '--dirty'], cwd=here, capture_output=True,
                              text=True, check=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return 'unknown'


def export(capture, decode, out, center=None, slot=0, of=1, dc_block=0, chain=None, gain=None, epoch=None,
           normalize=False, valid_only=False):
    """Write header and records; returns (records written, records skipped)."""
    d = json.loads(Path(decode).read_text())
    center = center if center is not None else d.get('center_mhz')
    if center is None:
        raise ValueError('The decode JSON has no center_mhz; pass --center')
    total = Path(capture).stat().st_size // 4 // of
    header = dict(format=FORMAT, version=VERSION, capture=str(capture), decode=str(decode), center_mhz=center,
                  input_rate_hz=cs16.FS, slot=slot, of=of, dc_block=dc_block, chain=chain, gain=gain, epoch=epoch,
                  processor=processor_version(), subcarriers=W.ACTIVE.tolist(), normalized=normalize,
                  fields=dict(h_raw='complex, raw L-LTF estimate', h_smooth='complex, -12..+16 tap projection',
                              noise='real, |A-B|^2/2 per carrier', h_norm='complex, h_raw at unit RMS (--normalize)'))
    written = skipped = 0
    with open(out, 'w') as f:
        f.write(json.dumps(header) + '\n')
        for rec in d['frames']:
            if rec.get('phy') not in ('OFDM', 'HT') or 'sample' not in rec or rec.get('mpdu_index', 0):
                continue
            ok = bool(rec.get('fcs_ok'))
            if valid_only and not ok:
                continue
            csi = packet_csi(capture, rec, center, total, slot, of, dc_block)
            if csi is None:
                skipped += 1
                continue
            row = dict(packet=f"{rec['channel']}:{rec['sample']}", sample=rec['sample'], channel=rec['channel'],
                       phy=rec['phy'], fcs_ok=ok, rate_mbps=rec.get('rate_mbps'), length=rec.get('length'),
                       ltf_match=rec.get('ltf_match'), snr_db=round(csi['snr_db'], 3), ltf_offset=csi['ltf_offset'],
                       cfo_residual_hz=round(csi['cfo_residual_hz'], 1),
                       h_raw=flat(csi['h_raw']), h_smooth=flat(csi['h_smooth']),
                       noise=np.round(csi['noise'], 10).tolist())
            if ok:  # A failed FCS means the decoded addresses are unverified: leave them out.
                row['source'] = next((x[5:] for x in rec.get('fields') or [] if x.startswith('from=')), None)
            if normalize:
                rms = np.sqrt(np.mean(np.abs(csi['h_raw']) ** 2))
                row['h_norm'] = flat(csi['h_raw'] / max(rms, 1e-30))
            f.write(json.dumps(row) + '\n')
            written += 1
    return written, skipped


def read(path):
    """Header and records of an export, with vectors back as complex numpy arrays."""
    lines = Path(path).read_text().splitlines()
    header = json.loads(lines[0])
    if header.get('format') != FORMAT or header.get('version') != VERSION:
        raise ValueError(f"Unsupported CSI file: {header.get('format')} version {header.get('version')}")
    rows = []
    for line in lines[1:]:
        row = json.loads(line)
        for key in ('h_raw', 'h_smooth', 'h_norm'):
            if key in row:
                v = np.array(row[key])
                row[key] = v[0::2] + 1j * v[1::2]
        row['noise'] = np.array(row['noise'])
        rows.append(row)
    return header, rows


def main():
    ap = argparse.ArgumentParser(description='Per-packet legacy CSI from a wifi.py decode')
    ap.add_argument('capture', type=Path)
    ap.add_argument('decode', type=Path, help='wifi.py --json output')
    ap.add_argument('out', type=Path, help='JSON Lines output (replaced)')
    ap.add_argument('--center', type=float, help='Capture center in MHz (default: from the decode)')
    ap.add_argument('--slot', type=int, default=0)
    ap.add_argument('--of', type=int, default=1, help='2 for birria_rx --chain both')
    ap.add_argument('--dc-block', type=int, nargs='?', const=65536, default=0, metavar='SAMPLES')
    ap.add_argument('--chain', type=int, help='Receive chain, recorded in the header')
    ap.add_argument('--gain', type=int, nargs=3, metavar=('LNA', 'LPF', 'DIG'), help='Codes, recorded in the header')
    ap.add_argument('--epoch', help='Acquisition or configuration label, recorded in the header')
    ap.add_argument('--normalize', action='store_true', help='Also write h_norm (h_raw at unit RMS)')
    ap.add_argument('--valid-only', action='store_true', help='FCS-valid packets only (biased; see the module docstring)')
    a = ap.parse_args()
    written, skipped = export(a.capture, a.decode, a.out, a.center, a.slot, a.of, a.dc_block, a.chain, a.gain, a.epoch,
                              a.normalize, a.valid_only)
    print(f'{written} packets written to {a.out}; {skipped} too close to the file end')


if __name__ == '__main__':
    main()
