# Decoders for BirriaSDR captures

Offline decoders for `.cs16` files recorded by `birria_rx` (40 MSps, interleaved int16 I/Q).

| Script | What it does |
|---|---|
| `wifi.py` | Combined DSSS/OFDM decoder: FCS-verified AP list, per-frame JSON diagnostics, and Wireshark PCAP export. |
| `wifi_ofdm.py` | 20 MHz OFDM: legacy 6–54 Mbps and HT-mixed (802.11n) single-stream MCS 0–7, with short guard interval, STBC and A-MPDU. STF/LTF synchronization, frequency correction, channel estimation, pilot tracking, soft decisions, deinterleaving, depuncturing, Viterbi decoding, descrambling, and FCS validation. |
| `wifi_b.py` | 802.11b DSSS at 1 and 2 Mbps with long/short preambles: beacons (SSID, BSSID, security), management/control frames and data-frame headers. FCS-checked. Configurable burst threshold and minimum duration. Unrecognized bursts are not identified as OFDM. |
| `wifi_scan.py` | Records each requested channel at its own center, verifies capture success, runs both decoders, and saves a combined survey. Discards raw I/Q by default. |
| `wifi_benchmark.py` | Measures gain/filter configurations against a saved Windows BSSID list, including missing APs, clipping, signal statistics and capture errors. |
| `diagnose_capture.py` | Bounded gap spacing/boundary analysis, intact-payload quality diagnostics, and valid advertisement inventory. |
| `probe_preamble_carrier.py` | Offline research comparison of late-preamble carrier estimates; not enabled in the normal decoder. |
| `run_carrier_comparison.py` | Six short receive-only captures with paired baseline/research decoding and saved acquisition evidence. |
| `windows_wifi_scan.ps1` | Requests a fresh Windows managed-mode scan, waits for scan completion and saves AP channels and signal quality. |
| `ble.py` | Bluetooth LE legacy advertising on channels 37/38/39 (2402/2426/2480 MHz) inside the capture. CRC-checked. |
| `ble_report.py` | Per-address summary of a streaming `birria_ble` packet log (`.jsonl`), with `ble.py`'s advertising-data parser; `--packets` lists every packet. |
| `spectrum.py` | Average spectrum plus spectrogram PNG. |
| `survey.py` | Stitches several captures into one band panorama and lists the emissions found. The centre frequency comes from the file name (`s2174.cs16`). |
| `cs16.py` | Shared loader and frequency shift. |

## Setup

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

## Usage

Always pass the centre frequency used for the capture, in MHz.

Use the combined decoder for discovery. It reports advertised networks only from
checksum-valid beacons and probe responses; it counts BSSIDs separately even
when they advertise the same SSID. Packet data remains encrypted when transmitted
encrypted; this is a PHY decoder, not a decryption tool.

```bash
cd tools/decoders
.venv/bin/python wifi.py capture_ch1.cs16 \
  --center 2412 --json ch1.json --pcap ch1.pcap
.venv/bin/python wifi.py recording.cs16 --center 2437 --phy ofdm --packets
```

5 GHz captures take their real centre and channel numbers (32–68, 96–144 and
149–177 in steps of 4), e.g. `wifi.py rec.cs16 --center 5180
--channel 36`; without `--channel` the channel nearest the centre is decoded.
5 GHz channels decode OFDM only (802.11b exists only at 2.4 GHz). A channel must
lie within 9 MHz of the capture centre in both bands.

`--phy both` is the default. DSSS uses a 6 dB threshold above the lower-fifth
power estimate and accepts bursts of at least 60 microseconds. Change these
with `--threshold-db` and `--min-us`. OFDM uses normalized short-training
correlation (`--stf-threshold 0.5`; 0.65 until 2026-10-29) instead of the
DSSS power/duration gate.
Lower thresholds can produce more candidates; valid MAC FCS remains mandatory
for AP identities and PCAP output. A valid legacy SIGNAL header alone is not a
decoded MAC frame. Diagnostics retain failed payloads without interpreting their
addresses or SSIDs.

OFDM pilot tracking takes a common phase per data symbol from the four pilots,
weighted by channel power, and a single linear trend of the phase slope over the
whole frame. The slope follows sampling-clock drift, but a slope fitted to four
pilots on each symbol is too noisy for weak frames: it tilts the edge
subcarriers and leaves burst errors the Viterbi decoder can't fix. On
2026-10-27 captures this decoded 2 → 23 frames on 5 GHz channel 149, 9 → 24 on
a channel 48 fixture and 221 → 233 on 2.4 GHz channel 11 (+24, −12, all short
control frames at marginal SNR). A frame that fails FCS is decoded again with
offset and slope fitted per symbol, and that result is kept only if its FCS
passes (the packet then carries `tracking: per_symbol`); this wins back the
frames the trend loses, such as those with a timing step mid-frame. On the
ch 11 fixture it recovers all 12. See
[findings §10](../../docs/findings.md#10-decoder-findings).

Weak legacy frames (2026-10-29, aimed at 5 GHz beacons at 3–5 dB SNR):
- **Channel estimate:** the legacy L-LTF estimate is smoothed onto −12..+16
  taps like the HT one. Once L-SIG passes parity, that symbol is used as a
  third training symbol (1:2 with the L-LTF). The LLR weights come from the
  smoothed estimate.
- **Pilot tracking:** the common phase is smoothed over ±`CPE_SMOOTH` (4)
  symbols around its linear trend.
- **Second pass:** a 6 Mbps legacy frame that still fails FCS is re-encoded
  from its own decode. Channel and phase are re-estimated from every symbol,
  and it is decoded again, up to `SECOND_PASSES` (2) times. A pass counts
  only if its FCS passes (`tracking: second_pass_N`).
- **Detection:** the STF threshold dropped to 0.5. Lowering it only pays off
  together with the changes above.

Results:
- **Noise injection on real 5 GHz frames:** the SNR needed for 50% of
  beacons fell by 2.5 dB (4.3 → 1.6 dB L-LTF SNR on one AP's channels,
  against 0.1 dB for a genie receiver).
- **A held-out 2 s capture at 5240 MHz:** 27 → 52 valid frames, with a
  −90 dBm AP going from 4 to 17 of its ~19.5 beacons.
- **A same-hour comparison with the stock driver:** the 50% beacon level
  per chain improved by about 1.9 dB on identical samples.
- **2.4 GHz channel 11:** +28/−0 and +22/−3 frames on two 2 s captures,
  with runtime up 29–36% there.

See [findings §10](../../docs/findings.md#10-decoder-findings).

HT-mixed (802.11n) frames are recognised after a 6 Mbps L-SIG by two QBPSK
HT-SIG symbols whose CRC-8 and tail check out; a frame whose HT-SIG fails is
still tried as legacy and reported `ht_bad_sig` only if that fails too (a
noisy legacy symbol can look rotated). Supported: 20 MHz, one spatial stream,
MCS 0–7, BCC, long or short guard interval, and Alamouti STBC (two HT-LTFs,
paired symbols combined over both space-time streams). 40 MHz, LDPC, two or
more spatial streams and extension streams are reported `ht_unsupported`
with the HT-SIG fields. A frame with a QBPSK second symbol only is labelled
`vht` (VHT-SIG-A) if legacy decoding fails. HT records carry
`ofdm_format: ht`, `mcs`, `stbc`, `short_gi`, `aggregated`, `smoothing`.
An A-MPDU is split at its delimiters (signature 0x4E, 12-bit length; delimiter
CRCs are not checked) and every MPDU that passes its own FCS becomes a record with
`mpdu_index` and the PPDU's `ampdu_subframes` / `ampdu_valid` counts. On a
2026-10-27 2 s capture of 2.4 GHz channel 11 this decoded 39 HT MPDUs (141 →
184 valid frames, no legacy frame lost); most HT PPDUs there are MCS 5–7 and
fail on SNR (data EVM −12 to −19 dB).

The HT channel estimate (`ht_channel`) is smoothed: each HT-LTF estimate is
projected onto cyclic impulse responses of −12..+16 samples (HT cyclic shifts
reach −600 ns; delay spread is kept to the 800 ns guard). A single-stream
frame also averages in the two L-LTF symbols (2:1) unless their difference
from the HT-LTF is concentrated inside that smooth space (`legacy_match` ≥
`LEGACY_MATCH` = 4), which is what a transmitter with other cyclic shifts on
the legacy fields produces; such a frame keeps its own estimate. The
smoothing bit is recorded but not used: frames with it clear gained too.
On two live 2 s channel 11 captures this took HT MPDUs from 29 to 84 and 66 to
103 (valid frames 148 → 203 and 190 → 227, 2 HT frames lost, no legacy frame
lost).

DSSS also searches for scrambled preamble structure after Barker despreading,
independently of the broadband power gate. This can recover weaker packets below
that gate. It corrects carrier rotation from the DBPSK sync field and uses a
16-symbol timing average to avoid chasing noise. Disable the additional search
with `--no-dsss-sync-search` for an offline comparison. Neither detector repairs
payload bytes to force a matching checksum.

When a DSSS frame fails its FCS, `wifi_b` demodulates it again with a RAKE
despreader (`wifi_b.ALTERNATIVES`): chip-spaced Barker correlation lags
weighted by their per-frame cross-correlation with the tracked lag, first with
the block tracker's timing, then with smoothed fractional timing. Some APs
arrive with a delayed path about a chip late at -6 to -9 dB, energy a single
lag discards. The first FCS-valid result is kept and its packet records
`demod` (`track`, `track_rake` or `smooth_rake`); a frame the primary
demodulator decodes is unchanged. Each fallback is an independent full
demodulation checked by the PLCP CRC and FCS, never a bit correction. Over the
repeated-survey benchmark it added 153 FCS-valid frames (+19%) and lost none, at
about 50% more decode time.

PCAP exports contain only FCS-valid 802.11 frames (link type 105), including their
four-byte FCS. Times are relative to capture start, not wall-clock UTC. Configure
Wireshark to assume an FCS is present if it does not infer that automatically.

JSON also lists `advertised_neighbors` from Reduced Neighbor Reports (IE 201)
inside FCS-valid beacons/probe responses. Each entry records the reporter,
operating class and reported channel, and has `directly_observed: false`.
These entries never enter `networks` or AP recall. A short SSID is retained as
raw hash bytes, without guessing an SSID. The parser handles type-0 TBTT layouts
with a BSSID (lengths 7–9 or at least 11), skips other layouts, and reports
malformed elements. Multiple BSSID profile expansion remains unimplemented;
the retained diagnostic inputs contain no IE 71. Layouts were cross-checked
against [Linux's RNR definitions](https://github.com/torvalds/linux/blob/master/include/linux/ieee80211.h)
and [RNR iteration](https://github.com/torvalds/linux/blob/master/net/wireless/scan.c).

For a complete channel survey, tune the receiver to each channel instead of
relying on overlapping capture edges:

```bash
# Run from tools/decoders; requires an attached, available adapter and built birria_rx.
.venv/bin/python wifi_scan.py --seconds 0.5
.venv/bin/python wifi_scan.py --channels 1 6 11 --seconds 1 --keep-iq
# Narrow filter + measured gain profile, scored against the Windows scan:
.venv/bin/python wifi_scan.py --seconds 0.5 --gain 3 8 0 \
  --reference windows_scan/comparison.json
# Repeat intermittent target channels and merge their verified BSSIDs:
.venv/bin/python wifi_scan.py --channels 4 9 11 --seconds 0.8 --passes 3 --gain 3 8 0
# Longer, repeated survey with disk writes deferred until each acquisition ends:
.venv/bin/python wifi_scan.py --seconds 1 --passes 3 --buffered --gain 3 8 0
```

The default channels are 1–11; request other supported 2.4 GHz channels explicitly
when appropriate for your location. The scanner creates a new timestamped folder
under the repository's `captures/`, with `survey.json` and per-channel JSON, PCAP and logs. It retries
failed recordings up to three times and discards incomplete attempts. Raw I/Q is
deleted after each channel unless `--keep-iq` is specified (160 MB per second per
channel). Existing captures are never deleted by the scanner. Errors stop the
scan, set its status to failed, preserve completed reports and return nonzero.

`--buffered` acquires a finite recording in RAM before conversion and disk writes.
It accepts at most two seconds (320 MB), rounded down to a 2048-sample packet
boundary. It reduces capture-time disk work but cannot prevent every device-ring
overrun. In the local repeated survey, two-second captures exhausted all retries
on one channel; one-second dwell is the more conservative starting point.

The combined decoder processes half-second windows with 70 ms of overlap,
covering the largest possible 16-bit DSSS duration field plus its preamble.
Frames are assigned to one window by their preamble time, retaining packets that
cross a boundary without duplicating counts. Peak sample memory is independent
of recording duration. Window-level detector diagnostics can include overlap;
the final `frames`, `valid_frames` and network counts contain owned packets only.
`dsss_plcp_rates` reports distinct CRC-valid headers by rate, including CCK.
With CCK disabled, its headers contain no decoded frame and never establish an
AP identity. Older reports can count duplicate failed-header candidates.

`wifi.py --cck` enables an experimental noncoherent 5.5/11 Mbps decoder. It
selects CCK codewords from the received chips and resolves differential phase
against the last PLCP header symbol, including odd-symbol rotation and length
extension. It passes generated-waveform tests for both rates, long/short
preambles, carrier offset, and corrupted payload rejection. Real-world multipath,
clock drift and sample blanking can still defeat it; PBCC remains unsupported.

`wifi.py --dc-block` removes a static DC offset before decoding. Use it for
BirriaSDR `--chain both` slots, which carry a large static DC offset, about
(−165, +152) LSB on chain 0 at channel 6. Left in place, it hides weak
frames. The option subtracts the mean of each 65,536-sample (1.6 ms) block;
`--dc-block N` uses N samples (at least 4096). The block grid starts at the
file's (or slot's) first sample, so decode windows don't change the
correction. Exact-zero (blanked) samples stay zero and are left out of the
means. So do the 3 samples after each blanking run (8 or more zeros): the
receiver brings the raw sample, offset included, back on a ramp there, and
subtracting the full offset would leave a spike of about minus the offset
in the first data symbols of most frames. On seven both-chain recordings
that spike cost 72 frames net (1528 → 1600 with it removed). It is off by
default; single-chain captures have almost no DC.

**Both-chain recordings** (`birria_rx --chain both`) interleave two receivers
pair by pair: even I,Q pairs are chain 0 (WF0), odd pairs chain 1 (WF1), each
at 40 MSps. No split copy is needed:

```sh
.venv/bin/python wifi.py both.cs16 --center 2437 --slot 0 --of 2 --dc-block   # chain 0 only
.venv/bin/python wifi.py both.cs16 --center 2437 --slot 1 --of 2 --dc-block   # chain 1 only
.venv/bin/python wifi.py both.cs16 --center 2437 --combine                    # both, plus their combination
```

`--slot K --of N` decodes one slot exactly as a split copy of it would decode
(`cs16.load(..., slot=K, of=N)`; times count that slot's samples). Read
without `--of`, an interleaved file decodes nothing useful.

`--combine` decodes three branches and counts each FCS-valid frame once:
chain 0, chain 1, and a noise-whitened combination of the two
(`combine.py`). Every frame records the branches that decoded it
(`branches`, e.g. `["wf0", "mrc"]`), and the JSON's `combine` section gives
per-branch counts (`mrc_only` = frames only the combination decoded) and,
per channel, the estimated slot-1 delay and noise correlation. It implies
`--dc-block 65536` unless given, and costs about three single-slot decodes.

- The chains' noise is correlated (|ρ| 0.45–0.57 in-band in our
  recordings), and slot 1 is offset from slot 0 by up to 0.9 samples, varying
  per capture. Plain maximal-ratio combining was worse than the better chain
  alone, so the weights whiten the noise first (`w = Rn⁻¹h`). Rn comes from
  the quietest fifth of 10 µs blocks, and h from the signal covariance over
  ±50 µs. The delay is estimated from the strongest bursts both chains hear
  well, and the output keeps chain 0's phase and noise level.
- On 17 two-chain recordings, the combination added
  11 frames that neither chain decoded (+2.2% over the per-slot union) and
  lost none, among them four beacons from a weak AP that neither slot heard.
  Branches keep the union, so the combination can only add frames; on its own
  it misses some short OFDM frames on busy channels.

Wi-Fi scans now select the nominal **20 MHz analog/digital filter profile** by
default; `--bandwidth 40000000` restores the wider profile. Both retain 40 MSps
and 160 MB/s raw I/Q volume. Gain defaults to the existing 2/8/0 codes; `--gain`
selects explicit LNA/filter/digital indices, while `--released` releases overrides.
Index values are not calibrated dB. The local benchmark found that the narrower
profile recovered beacons from two APs that the wide profile missed;
gain changes alone did not recover them. Use clipping and AP recall to assess
settings, rather than assuming a higher RMS means better reception.

`--reference` adds channel-specific and aggregate BSSID recall to `survey.json`.
Only FCS-valid AP advertisements are compared with the Windows list; its SSIDs
and addresses are never used to construct or repair a frame. The reference is a
snapshot and weak APs can appear intermittently, so it is not a guarantee that
every listed AP transmitted during a later recording.

For repeatable gain comparisons (raw files are discarded unless requested):

```bash
.venv/bin/python wifi_benchmark.py \
  --reference windows_scan/comparison.json \
  --channels 11 4 6 --seconds 0.5 --bandwidth 20000000 \
  --profiles 2,8,0 3,8,0 --phy both
```

```bash
.venv/bin/python wifi_b.py capture_ch1.cs16 --center 2412
.venv/bin/python wifi_b.py sweep/w2432.cs16 --center 2432 --channel 4 5 6
.venv/bin/python ble.py capture_ch1.cs16 --center 2412
.venv/bin/python spectrum.py capture_ch1.cs16 --center 2412 --out ch1.png
.venv/bin/python survey.py sweep/*.cs16 --out panorama.png
```

Record with `manual` gain so that levels are real:

```bash
../../build/birria_rx --frequency 2412000000 capture_ch1.cs16 40000000 manual   # no sudo: the udev rule grants access
```

## Known limits

- Exact-zero receiver gaps occur in runs of roughly 48–49 samples (1.2 µs).
  They can overlap Wi-Fi payloads: a two-second channel-2 recording had gaps
  inside 14 of 15 distinct failed DSSS payloads and none of its 29 valid payloads.
  This is an association, not proof that every failure is caused by blanking.
  `analyze_blanking.py capture.cs16 --center 2417 --out gaps.json` measures
  gap intersections using approximate payload intervals. Failed-frame addresses
  remain explicitly unverified and never contribute to discovery counts.
- Strong signals more than 20 MHz from the centre alias into the opposite edge of the capture. Bursts that appear at both edges at the same time are usually one signal.
- Decoding works best when the Wi-Fi channel is at the capture centre. At ±5 MHz about half the frames fail their FCS. Frames from neighbouring channels can also come out garbled, so trust only `fcs=OK` frames.
- `wifi_b.py` cannot decode OFDM. Use `wifi.py` or `wifi_ofdm.py` for APs with legacy OFDM beacons.
- VHT/HE (802.11ac/ax) payloads, HT with 40 MHz, LDPC or two or more spatial
  streams, PBCC, and multi-stream reception remain unsupported. CCK 5.5/11 Mbps
  is experimental and requires `--cck`. A VHT/HE transmission can have a valid
  legacy SIGNAL field but still fail legacy payload decoding. Such failures
  remain unverified.
- Captures are short observations; weak APs, collisions, fading, receiver blanking and interference can still prevent discovery. This is not a calibrated spectrum analyzer or a complete replacement for a normal Wi-Fi scan.

## Validation

```bash
cd tools/decoders
.venv/bin/python -m unittest test_wifi -v
# Include advertisement parsing and indirect-identity isolation tests:
.venv/bin/python -m unittest discover -s . -p 'test*.py' -q
```

The tests generate waveforms rather than storing large captures. They exercise
all eight legacy OFDM rates under noise, multipath and carrier offset; a long
OFDM frame under a 40 ppm sampling-clock offset; a timing step mid-frame
(needs the per-symbol retry); HT MCS 0–7 with and without STBC and short
guard interval, A-MPDUs with a corrupted MPDU, a bad HT-SIG CRC, and a
two-antenna transmitter whose legacy fields use other cyclic shifts (L-LTF
not averaged in); 5 GHz channel numbers (OFDM only); long and
short DSSS preambles; both supported DSSS rates; packet detection across chunk
boundaries; 40 MSps resampling; malformed headers; bad CRC; truncation; rejection
of noise and tones; AP identity parsing; JSON serialization; and PCAP filtering.
Further cases exercise DSSS packets below the broadband power gate, +/-180 kHz
carrier offset, rejection of corrupted payloads, a frame with a delayed second
path that only the RAKE fallback decodes, and benchmark scoring without using
expected SSID bytes in decoding.
Scanner tests also verify repeated-pass merging and preservation of prior files.
Additional tests cover packet ownership across decoder windows, unsupported CCK
header reporting, and raw zero runs spanning file-analysis chunks.
DC-block tests decode OFDM and DSSS frames under a both-chain-sized static
offset, which the decoder misses without `--dc-block`. They also check that
the correction is the same in every window and leaves blanked samples zero,
and that a ramped blanking edge leaves no offset spike.
Slot tests check that `--slot K --of 2` decodes each slot exactly as its split
copy does, and reject invalid slots and partial slot groups. Combining tests
decode a weak DSSS frame that neither chain decodes alone, under correlated
noise and a half-sample slot-1 delay, check the whitened weights against the
analytic optimum, and check that the combined stream is the same in every
window and leaves blanked samples zero.
Real capture results are saved alongside their input recordings.

On the existing one-second channel 1 recording, the original decoder found 27
valid DSSS frames. The combined decoder found 88 valid frames (28 DSSS and 60
OFDM). The subsequent 0.5-second-per-channel sweep of channels 1–11 completed
without recording errors and found 91 valid frames and four advertised BSSIDs
across channels 1 and 6; no additional AP identities were verified in that
short sweep.

PHY parameter cross-checks: the open-source [gr-ieee802-11 receiver](https://github.com/bastibl/gr-ieee802-11)
provides an independent implementation of the legacy OFDM protocol. This receiver
is implemented directly in NumPy/SciPy and does not require GNU Radio.
