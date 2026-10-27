"""Relate exact-zero runs to RF bursts (round-four Phase 1 gating diagnosis).

Bursts are found from the sample envelope alone, so OFDM/HT and undecodable
traffic count as well as DSSS. Each burst gets a spectral centroid (on-channel
or adjacent) and, where a DSSS payload from `diagnose_capture.py` falls inside
it, a valid/failed label. Zero-run starts are then timed against burst onset
and end. If the zeros were created by USB/DMA transport, their rate could not
depend on whether a burst is present or where its carrier lies.

Envelope blocks are 1 microsecond. Onset/end are accurate to a few blocks, so
latencies below about 5 microseconds are not resolved. Failed-payload labels
remain diagnostic and never identify an AP.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from analyze_blanking import zero_runs

FS = 40e6
BLOCK = 40                 # samples per 1 us envelope block
SMOOTH = 8                 # envelope smoothing, blocks
START_DB, HOLD_DB = 4.0, 2.0
MIN_BURST_US = 16
MERGE_US = 4
NFFT = 256
SNR_BINS = [(-99, 6), (6, 12), (12, 20), (20, 99)]


def envelope(raw, chunk=100_000):
    """Mean power of the non-zero samples in each block; NaN if mostly zero."""
    n = len(raw) // BLOCK
    power = np.full(n, np.nan, np.float32)
    for b in range(0, n, chunk):
        e = min(n, b + chunk)
        x = np.asarray(raw[b * BLOCK:e * BLOCK], np.float32).reshape(-1, BLOCK, 2)
        p = x[..., 0] ** 2 + x[..., 1] ** 2
        live = np.any(x != 0, axis=-1)
        count = live.sum(1)
        ok = count >= 8
        power[b:e][ok] = p.sum(1)[ok] / count[ok]
    # Carry the previous level across blanked blocks so gaps never split bursts.
    idx = np.where(np.isnan(power), 0, np.arange(n))
    np.maximum.accumulate(idx, out=idx)
    return power[idx]


def find_bursts(power):
    smooth = np.convolve(power, np.ones(SMOOTH) / SMOOTH, mode='same')
    floor = float(np.percentile(smooth, 20))
    hold = smooth > floor * 10 ** (HOLD_DB / 10)
    start = smooth > floor * 10 ** (START_DB / 10)
    edges = np.flatnonzero(np.diff(np.r_[False, hold, False]))
    regions = []
    for a, b in zip(edges[::2], edges[1::2]):
        if not start[a:b].any():
            continue
        if regions and a - regions[-1][1] < MERGE_US:
            regions[-1][1] = b
        else:
            regions.append([a, b])
    bursts = [(a, b) for a, b in regions if b - a >= MIN_BURST_US]
    return bursts, floor, smooth


def noise_spectrum(raw, smooth, floor, rng, count=400):
    quiet = np.flatnonzero(smooth < floor * 1.05)
    quiet = quiet[(quiet > 16) & (quiet < len(smooth) - 16)]
    acc, used = np.zeros(NFFT), 0
    for blk in rng.choice(quiet, size=min(len(quiet), count * 3), replace=False):
        x = np.asarray(raw[blk * BLOCK:blk * BLOCK + NFFT], np.float32)
        if len(x) < NFFT or np.all(x == 0, axis=1).any():
            continue
        z = x[:, 0] + 1j * x[:, 1]  # .cs16 files are host-converted I, Q
        acc += np.abs(np.fft.fftshift(np.fft.fft(z * np.hanning(NFFT)))) ** 2
        used += 1
        if used >= count:
            break
    return acc / max(used, 1)


def burst_spectrum(raw, a, b, noise):
    """Return (centroid, 5% edge, 95% edge) of the burst's excess power, in Hz.

    The centroid alone misleads for clipped OFDM: a channel-1 OFDM frame seen
    from 2417 MHz spans about -7.5..+3 MHz after the analog filter. The
    occupied-band midpoint separates on-channel from adjacent traffic.
    """
    lo, hi = (a + 2) * BLOCK, min(b, a + 1002) * BLOCK
    acc, used = np.zeros(NFFT), 0
    for s in range(lo, hi - NFFT, NFFT):
        x = np.asarray(raw[s:s + NFFT], np.float32)
        if np.all(x == 0, axis=1).any():
            continue
        z = x[:, 0] + 1j * x[:, 1]
        acc += np.abs(np.fft.fftshift(np.fft.fft(z * np.hanning(NFFT)))) ** 2
        used += 1
        if used >= 64:
            break
    if not used:
        return None
    excess = np.maximum(acc / used - noise, 0)
    if excess.sum() <= 0:
        return None
    freqs = (np.arange(NFFT) - NFFT // 2) * FS / NFFT
    cum = np.cumsum(excess) / excess.sum()
    return (float((freqs * excess).sum() / excess.sum()),
            float(freqs[np.searchsorted(cum, 0.05)]), float(freqs[np.searchsorted(cum, 0.95)]))


def load_payloads(path):
    if not path:
        return []
    data = json.loads(Path(path).read_text())
    return [p for p in data.get('payloads', []) if 'sync_time_ms' in p]


def load_ofdm(path):
    """OFDM frames with a valid L-SIG from a wifi.py result's `frames` list."""
    if not path:
        return []
    frames = json.loads(Path(path).read_text()).get('frames', [])
    return [f for f in frames if f.get('phy') == 'OFDM' and 'time_ms' in f]


def classify(spectrum):
    if spectrum is None:
        return 'unknown'
    mid = abs(spectrum[1] + spectrum[2]) / 2
    return 'on' if mid <= 1.5e6 else 'adjacent' if mid >= 2.0e6 else 'ambiguous'


def rate_curve(events, bursts, align, window, bin_us=5):
    """Zero-run start rate (per microsecond) against time from onset or end."""
    lo, hi = window
    edges = np.arange(lo, hi + bin_us, bin_us)
    hits = np.zeros(len(edges) - 1)
    exposure = np.zeros(len(edges) - 1)
    for a, b in bursts:
        t0 = a if align == 'onset' else b
        # Onset-aligned bins after onset count only while the burst lasts;
        # end-aligned bins before the end count only while it lasted.
        valid_lo = lo if align == 'onset' else max(lo, a - b)
        valid_hi = min(hi, b - a) if align == 'onset' else hi
        bl = np.clip(edges[:-1], valid_lo, valid_hi)
        bh = np.clip(edges[1:], valid_lo, valid_hi)
        exposure += np.maximum(bh - bl, 0)
        rel = events[(events >= t0 + valid_lo) & (events < t0 + valid_hi)] - t0
        hits += np.histogram(rel, edges)[0]
    with np.errstate(invalid='ignore', divide='ignore'):
        rate = np.where(exposure > 0, hits / exposure, np.nan)
    return {'bin_start_us': edges[:-1].tolist(), 'runs': hits.astype(int).tolist(),
            'exposure_us': exposure.round(1).tolist(),
            'rate_per_ms': [None if np.isnan(r) else round(float(r) * 1000, 3) for r in rate]}


def per_burst(events, a, b, skip_us=50):
    inside = events[(events >= a) & (events < b)] - a
    after_skip = inside[inside >= skip_us]
    last = float(inside[-1]) if len(inside) else None
    following = events[events >= b]
    preceding = events[events < a]
    return {'runs_inside': int(len(inside)),
            'runs_after_skip': int(len(after_skip)),
            'first_run_us': float(inside[0]) if len(inside) else None,
            'last_run_us': last,
            'gap_free_tail_us': float(b - a - (last + 1.2 if last is not None else 0)),
            'resume_us': float(following[0] - b) if len(following) else None,
            'prior_us': float(a - preceding[-1]) if len(preceding) else None}


def summarize(rows, idle_rate, skip_us=50):
    out = []
    for cls in ['on', 'adjacent', 'ambiguous', 'unknown']:
        for label in ['dsss_valid', 'dsss_failed', 'ofdm_valid', 'ofdm_failed', 'no_dsss_plcp']:
            for lo, hi in SNR_BINS:
                sel = [r for r in rows if r['class'] == cls and r['label'] == label
                       and lo <= r['snr_db'] < hi and r['duration_us'] >= skip_us + 100]
                if not sel:
                    continue
                exposure = sum(r['duration_us'] - skip_us for r in sel)
                runs = sum(r['runs_after_skip'] for r in sel)
                rate = 1000 * runs / exposure
                out.append({'class': cls, 'label': label, 'snr_db': [lo, hi], 'bursts': len(sel),
                            'gap_free_after_skip': sum(r['runs_after_skip'] == 0 for r in sel),
                            'rate_after_skip_per_ms': round(rate, 3),
                            'rate_vs_idle': round(float(rate / idle_rate), 3) if idle_rate else None})
    return out


def idle_spacing(starts_us, bursts, guard_us=50):
    """Inter-run spacing for consecutive runs that both lie in quiet time."""
    busy = np.zeros(int(starts_us[-1]) + 2, bool) if len(starts_us) else np.zeros(1, bool)
    for a, b in bursts:
        busy[max(0, a - guard_us):min(len(busy), b + guard_us)] = True
    quiet = ~busy[np.clip(starts_us.astype(int), 0, len(busy) - 1)]
    pair = quiet[:-1] & quiet[1:]
    d = np.diff(starts_us)[pair]
    # Reject pairs that straddle a burst.
    a_idx = np.flatnonzero(pair)
    ok = np.array([not busy[int(starts_us[i]):int(starts_us[i + 1]) + 1].any() for i in a_idx], bool)
    return d[ok] if len(ok) else d


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('capture')
    ap.add_argument('--diagnosis', help='diagnosis.json from diagnose_capture.py (DSSS labels)')
    ap.add_argument('--frames', help='wifi.py result JSON whose OFDM frames label bursts')
    ap.add_argument('--output', required=True, help='new output JSON path')
    ap.add_argument('--plot', help='optional PNG path for rate curves')
    args = ap.parse_args()

    raw = np.memmap(args.capture, dtype='<i2', mode='r').reshape(-1, 2)
    runs = zero_runs(args.capture)
    starts_us = runs[:, 0] / BLOCK                       # 1 block == 1 us
    power = envelope(raw)
    bursts, floor, smooth = find_bursts(power)
    rng = np.random.default_rng(0)
    noise = noise_spectrum(raw, smooth, floor, rng)
    payloads = load_payloads(args.diagnosis)
    sync_us = np.array([p['sync_time_ms'] * 1000 for p in payloads])
    ofdm = load_ofdm(args.frames)
    ofdm_us = np.array([f['time_ms'] * 1000 for f in ofdm])

    rows = []
    for a, b in bursts:
        spectrum = burst_spectrum(raw, a, b, noise)
        hit = [p for p, t in zip(payloads, sync_us) if a - 10 <= t < b] if len(sync_us) else []
        label = ('dsss_valid' if any(p['fcs_ok'] for p in hit) else
                 'dsss_failed' if hit else 'no_dsss_plcp')
        if label == 'no_dsss_plcp' and len(ofdm_us):
            ohit = [f for f, t in zip(ofdm, ofdm_us) if a - 10 <= t < b]
            if ohit:
                label = 'ofdm_valid' if any(f.get('fcs_ok') for f in ohit) else 'ofdm_failed'
        snr = 10 * np.log10(float(np.nanmean(power[a:b])) / floor)
        row = {'onset_us': int(a), 'end_us': int(b), 'duration_us': int(b - a),
               'snr_db': round(float(snr), 2),
               'centroid_mhz': None if spectrum is None else round(spectrum[0] / 1e6, 3),
               'band_mhz': None if spectrum is None else [round(spectrum[1] / 1e6, 2), round(spectrum[2] / 1e6, 2)],
               'class': classify(spectrum), 'label': label}
        row.update(per_burst(starts_us, a, b))
        rows.append(row)

    busy_us = sum(b - a for a, b in bursts)
    total_us = len(power)
    in_busy = sum(((starts_us >= a) & (starts_us < b)).sum() for a, b in bursts)
    idle_rate = 1000 * (len(starts_us) - in_busy) / max(total_us - busy_us, 1)

    groups = {
        'on_valid': [(r['onset_us'], r['end_us']) for r in rows
                     if r['class'] == 'on' and r['label'] == 'dsss_valid'],
        'on_failed': [(r['onset_us'], r['end_us']) for r in rows
                      if r['class'] == 'on' and r['label'] == 'dsss_failed'],
        'on_other_strong': [(r['onset_us'], r['end_us']) for r in rows
                            if r['class'] == 'on' and r['label'] == 'no_dsss_plcp' and r['snr_db'] >= 12],
        'adjacent_valid': [(r['onset_us'], r['end_us']) for r in rows
                           if r['class'] == 'adjacent' and r['label'] == 'dsss_valid'],
        'adjacent_failed': [(r['onset_us'], r['end_us']) for r in rows
                            if r['class'] == 'adjacent' and r['label'] == 'dsss_failed'],
        'adjacent_other_strong': [(r['onset_us'], r['end_us']) for r in rows
                                  if r['class'] == 'adjacent' and r['label'] == 'no_dsss_plcp' and r['snr_db'] >= 12],
        'ofdm_valid': [(r['onset_us'], r['end_us']) for r in rows if r['label'] == 'ofdm_valid'],
        'ofdm_failed': [(r['onset_us'], r['end_us']) for r in rows if r['label'] == 'ofdm_failed'],
        'weak_any': [(r['onset_us'], r['end_us']) for r in rows if r['snr_db'] < 6],
    }
    curves = {name: {'bursts': len(g),
                     'onset': rate_curve(starts_us, g, 'onset', (-200, 400)),
                     'end': rate_curve(starts_us, g, 'end', (-200, 300))}
              for name, g in groups.items() if g}

    spacing = idle_spacing(starts_us, bursts)
    spacing_samples = np.round(spacing * BLOCK).astype(int)
    vals, counts = np.unique(spacing_samples, return_counts=True)
    top = np.argsort(counts)[::-1][:15]
    mod40 = np.bincount(spacing_samples % 40, minlength=40)

    valid_on = [r for r in rows if r['class'] == 'on' and r['label'] == 'dsss_valid']
    gate = {
        'on_valid_bursts': len(valid_on),
        'on_valid_gap_free_after_50us': sum(r['runs_after_skip'] == 0 for r in valid_on),
        'on_valid_last_run_us': sorted(r['last_run_us'] for r in valid_on if r['last_run_us'] is not None),
    }

    result = {
        'capture': args.capture,
        'diagnosis': args.diagnosis,
        'method': {'block_us': 1, 'smooth_blocks': SMOOTH, 'start_db': START_DB, 'hold_db': HOLD_DB,
                   'min_burst_us': MIN_BURST_US, 'merge_us': MERGE_US, 'skip_us': 50,
                   'on_channel_max_band_midpoint_mhz': 1.5, 'adjacent_min_band_midpoint_mhz': 2.0,
                   'note': 'Envelope excludes zero samples; onset/end resolution is a few us.'},
        'noise_floor_power': floor,
        'zero_runs': int(len(runs)),
        'bursts': len(rows),
        'busy_fraction': round(busy_us / total_us, 4),
        'idle_run_rate_per_ms': round(idle_rate, 3),
        'gate': gate,
        'class_summary': summarize(rows, idle_rate),
        'rate_curves': curves,
        'idle_spacing': {'pairs': int(len(spacing)),
                         'median_us': float(np.median(spacing)) if len(spacing) else None,
                         'top_samples': [{'samples': int(vals[i]), 'count': int(counts[i])} for i in top],
                         'mod40_counts': mod40.tolist()},
        'burst_rows': rows,
    }
    Path(args.output).write_text(json.dumps(result, indent=1))

    if args.plot:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        fig, axes = plt.subplots(1, 2, figsize=(12, 4.5), sharey=True)
        for name, c in curves.items():
            for ax, key in zip(axes, ['onset', 'end']):
                x = np.array(c[key]['bin_start_us']) + 2.5
                y = np.array([np.nan if v is None else v for v in c[key]['rate_per_ms']], float)
                ax.plot(x, y, label=f"{name} (n={c['bursts']})")
        for ax, key in zip(axes, ['burst onset', 'burst end']):
            ax.axvline(0, color='k', lw=0.8)
            ax.axhline(idle_rate, color='gray', ls='--', lw=0.8, label='idle rate')
            ax.set_xlabel(f'time from {key} (µs)')
        axes[0].set_ylabel('zero-run starts per ms')
        axes[0].legend(fontsize=8)
        fig.suptitle(Path(args.capture).name)
        fig.tight_layout()
        fig.savefig(args.plot, dpi=110)

    print(json.dumps({k: result[k] for k in ['bursts', 'busy_fraction', 'idle_run_rate_per_ms', 'gate']}, indent=1))
    for row in result['class_summary']:
        print(row)


if __name__ == '__main__':
    main()
