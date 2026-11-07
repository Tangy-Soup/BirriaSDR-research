"""csi_export: known channels through the real loader, header and privacy rules."""
import json
import tempfile
from pathlib import Path

import numpy as np

import csi_export as C
import wifi_ofdm as W

CENTER, CHANNEL, SAMPLE = 2437.0, 6, 3000  # the record's 20 MSps sample; L-LTF starts 192 later


def write_capture(path, taps, snr_db=None, slots=1, dc=0, seed=1):
    """A 20 MSps L-LTF pair through `taps`, resampled to 40 MSps, in a .cs16 file."""
    rng = np.random.default_rng(seed)
    ltf = W.LTF
    wave = np.r_[ltf[-32:], ltf, ltf]
    y = np.zeros(16000, np.complex128)
    y[SAMPLE + 192 - 32:SAMPLE + 192 + 128] = wave * 0.2 / np.sqrt(np.mean(np.abs(ltf) ** 2))
    y = np.convolve(y, taps)[:len(y)]
    # Ideal 2x interpolation, so the exporter's 2:1 resample returns y: FFT zero padding.
    Y = np.fft.fft(y)
    n = len(y)
    x = np.fft.ifft(np.r_[Y[:n // 2], np.zeros(n, complex), Y[n // 2:]]) * 2
    if snr_db is not None:
        x = x + (rng.normal(size=len(x)) + 1j * rng.normal(size=len(x))) * 0.2 * 10 ** (-snr_db / 20) / np.sqrt(2)
    raw = np.empty((len(x), 2), np.int16)
    raw[:, 0] = np.rint(x.real * 8192)
    raw[:, 1] = np.rint(x.imag * 8192)
    if slots == 2:  # slot 0 is empty apart from a large static offset.
        other = np.full_like(raw, dc)
        raw = np.stack((other, raw), axis=1).reshape(-1, 2)
    raw.astype('<i2').tofile(path)


def write_decode(path, frames):
    Path(path).write_text(json.dumps(dict(center_mhz=CENTER, frames=frames)))


def frame(fcs_ok=True, **extra):
    return dict(dict(sample=SAMPLE, channel=CHANNEL, phy='OFDM', fcs_ok=fcs_ok, rate_mbps=6, length=100, ltf_match=.9,
                     fields=['from=aa:bb:cc:dd:ee:ff', 'to=11:22:33:44:55:66']), **extra)


def expected_h(taps, row):
    """Frequency response at the 52 carriers, up to the exporter's timing reference and scale."""
    k = W.ACTIVE
    h = sum(t * np.exp(-2j * np.pi * k * n / 64) for n, t in enumerate(taps))
    h = h / np.sqrt(np.mean(np.abs(h) ** 2))
    got = row['h_raw'] / np.sqrt(np.mean(np.abs(row['h_raw']) ** 2))
    # Remove the arbitrary common phase; the delay of re-finding stays in the taps' position.
    return h * np.exp(1j * np.angle(np.vdot(h, got))), got


def test_known_channel_and_header():
    taps = np.array([1, 0, 0.5j, 0, 0, -0.3])
    with tempfile.TemporaryDirectory() as d:
        cap, dec, out = f'{d}/c.cs16', f'{d}/d.json', f'{d}/o.jsonl'
        write_capture(cap, taps)
        write_decode(dec, [frame()])
        assert C.export(cap, dec, out, chain=0, gain=[5, 8, 0], epoch='e1') == (1, 0)
        header, rows = C.read(out)
    assert header['format'] == 'birria-csi' and header['version'] == 1
    assert header['subcarriers'] == W.ACTIVE.tolist() and len(header['subcarriers']) == 52
    assert header['chain'] == 0 and header['gain'] == [5, 8, 0] and header['epoch'] == 'e1'
    assert header['center_mhz'] == CENTER and header['input_rate_hz'] == 40e6 and header['processor']
    row = rows[0]
    assert len(row['h_raw']) == len(row['h_smooth']) == len(row['noise']) == 52
    # Timing can legitimately pick a neighbouring tap, so match up to a delay of a few samples.
    best = 0
    for delay in range(-6, 7):
        h, got = expected_h(taps, row)
        h = h * np.exp(-2j * np.pi * W.ACTIVE * delay / 64)
        h = h * np.exp(1j * np.angle(np.vdot(h, got)))
        best = max(best, abs(np.vdot(h, got)) / 52)
    assert best > 0.999, best
    assert row['snr_db'] > 40 and row['packet'] == f'{CHANNEL}:{SAMPLE}'


def test_noise_matches_snr_definition_and_smoothing():
    taps = np.array([1, 0.4])
    with tempfile.TemporaryDirectory() as d:
        cap, dec, out = f'{d}/c.cs16', f'{d}/d.json', f'{d}/o.jsonl'
        write_capture(cap, taps, snr_db=20)
        write_decode(dec, [frame()])
        C.export(cap, dec, out)
        _, rows = C.read(out)
    row = rows[0]
    assert 12 < row['snr_db'] < 30, row['snr_db']
    # A short channel lives inside the smoother's taps, so smoothing only removes noise.
    assert np.allclose(W.LEGACY_SMOOTH @ row['h_raw'], row['h_smooth'], atol=1e-6)
    h = sum(t * np.exp(-2j * np.pi * W.ACTIVE * n / 64) for n, t in enumerate(taps))
    scale = lambda z: z / np.sqrt(np.mean(np.abs(z) ** 2))
    def error(z):
        best = 9
        for delay in range(-6, 7):
            e = h * np.exp(-2j * np.pi * W.ACTIVE * delay / 64)
            e = e * np.exp(1j * np.angle(np.vdot(e, z)))
            best = min(best, np.mean(np.abs(scale(z) - scale(e)) ** 2))
        return best
    assert error(row['h_smooth']) < error(row['h_raw'])


def test_failed_fcs_hides_identity_and_filters():
    with tempfile.TemporaryDirectory() as d:
        cap, dec, out = f'{d}/c.cs16', f'{d}/d.json', f'{d}/o.jsonl'
        write_capture(cap, [1])
        frames = [frame(), frame(fcs_ok=False, sample=SAMPLE + 2000),
                  frame(mpdu_index=1), dict(frame(), phy='DSSS')]
        write_decode(dec, frames)
        assert C.export(cap, dec, out)[0] == 2
        text = Path(out).read_text()
        _, rows = C.read(out)
        assert rows[0]['source'] == 'aa:bb:cc:dd:ee:ff' and 'source' not in rows[1]
        assert '11:22:33' not in text and text.count('aa:bb') == 1
        assert C.export(cap, dec, out, valid_only=True)[0] == 1


def test_normalize_is_unit_rms():
    with tempfile.TemporaryDirectory() as d:
        cap, dec, out = f'{d}/c.cs16', f'{d}/d.json', f'{d}/o.jsonl'
        write_capture(cap, [1, .2])
        write_decode(dec, [frame()])
        C.export(cap, dec, out, normalize=True)
        header, rows = C.read(out)
    assert header['normalized']
    assert abs(np.mean(np.abs(rows[0]['h_norm']) ** 2) - 1) < 1e-6


def test_slot_selection_with_dc_block():
    with tempfile.TemporaryDirectory() as d:
        cap, dec, out = f'{d}/c.cs16', f'{d}/d.json', f'{d}/o.jsonl'
        write_capture(cap, [1, .3], slots=2, dc=300)
        write_decode(dec, [frame()])
        C.export(cap, dec, out, slot=1, of=2, dc_block=65536, chain=1)
        header, rows = C.read(out)
    assert header['slot'] == 1 and header['of'] == 2 and rows[0]['snr_db'] > 30


def test_rejects_unknown_format():
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / 'x.jsonl'
        p.write_text(json.dumps(dict(format='birria-csi', version=2)) + '\n')
        try:
            C.read(p)
        except ValueError as e:
            assert 'version 2' in str(e)
        else:
            raise AssertionError('accepted a future version')


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_'):
            fn()
            print('PASS', name)
