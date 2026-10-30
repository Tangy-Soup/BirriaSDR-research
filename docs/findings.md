# Research findings: an EDUP EP-AX1672 as a raw-I/Q receiver

These notes collect what we measured while running BirriaSDR on an
[EDUP EP-AX1672](https://www.szedup.com/usb-adaptersBr3/EP1672.html), an AX3000
Wi-Fi 6E USB 3 adapter built on the MediaTek MT7921AU (`0e8d:7961`, MT7961
digital die and MT7972 analog die, 40 MHz crystal), in October 2026. The aim was to decode 2.4 GHz Wi-Fi and BLE
straight from the I/Q, compare the result with a stock Wi-Fi adapter, and
find out what limits it.

The code changes that came out of this work are in this branch and documented
in [mt7921.md](mt7921.md) and the [README](../README.md). This page records
the measurements behind them, including what didn't work.

**Limits of the evidence.** Everything here comes from one adapter, at one
location, on one host. Results are labelled:

- **Established:** repeated, or measured against a control.
- **Single run:** measured once, or on little data.
- **Negative:** tried and rejected.

The access points in these measurements are neighbours' networks, so this page
refers to them by role ("a weak channel-6 AP") rather than by name or address.

## Contents

1. [Setup and method](#1-setup-and-method)
2. [Summary](#2-summary)
3. [Sample blanking is the PHY packet detector](#3-sample-blanking-is-the-phy-packet-detector)
4. [Antenna placement, USB 3 self-noise and shielding](#4-antenna-placement-usb-3-self-noise-and-shielding)
5. [ICAP taps and the channel filter](#5-icap-taps-and-the-channel-filter)
6. [The second receive chain](#6-the-second-receive-chain)
7. [Gain](#7-gain)
8. [Sample rates and the transport](#8-sample-rates-and-the-transport)
9. [5 GHz observations](#9-5-ghz-observations)
10. [Decoder findings](#10-decoder-findings)
11. [Host and platform notes](#11-host-and-platform-notes)
12. [Negative results](#12-negative-results)
13. [Open questions](#13-open-questions)
14. [Credits](#14-credits)

## 1. Setup and method

| Item | Details |
| --- | --- |
| Receiver | EDUP EP-AX1672, two RP-SMA ports with the stock 5 dBi antennas unless noted. Port **A is chain 0 (WF0)**, port **B is chain 1 (WF1)**. |
| Host | Windows with WSL2 (Ubuntu), the adapter passed through with usbipd-win at 5000M. Custom WSL kernel with `rtw88` built in and `mt7921u` left out, so the stock driver can't claim the adapter. |
| Reference receiver | A TP-Link RTL8822BU adapter in monitor mode (rtw88), plus Windows' own scan list. Later, `mt7921u` in monitor mode on the same EDUP (one driver at a time; see §11). |
| Recorder | `birria_rx`, 40 MSps complex int16 (`.cs16`), mostly 1–2 s captures per channel |
| Decoders | Python decoders in [`tools/decoders/`](../tools/decoders/README.md): 802.11b DSSS, legacy 802.11a/g OFDM and BLE advertising. Each frame is counted only if its FCS or CRC is valid. |

**Scoring.** The main metric is FCS-valid frames, matched against frames the
reference receiver logged at the same time. Clocks are aligned with radiotap
TSFT or beacon TSF and sequence control, because USB/IP adds about 60 ms of
jitter to host timestamps, and beacons from one AP repeat every 102.4 ms. A/B
comparisons alternate the two arms (ABBA or paired runs), because received
levels drift between captures.

**Register work.** Every experimental register write was masked, read back,
and restored before the normal reset. Over about 800 screening captures one
restore failed, during a deliberately broad overlay (§12). All of it is
receive-only.

**Tuning range of this unit** (established). The 2.4 GHz synthesizer
calibrates from 2171 to 2652 MHz. 2170 and 2655 MHz fail on the calibration
rail in repeated runs, and above about 2645 MHz the calibration result drifts
between runs. The library's command range is unchanged: 2156 MHz (2190 on
generic IDs) to 2610 MHz. The high VCO range (the 5 GHz setting) also
calibrates on the 2.4 GHz path, from 2600 to at least 3000 MHz, but doesn't
receive. At 2597 MHz both ranges calibrate. There, a nearby n41 cell's sync
block was found on the low range in runs before and after. On the high range
it was found under none of the 16 `0xc00[5:2]` LO-path values with either
front end (32 runs). The only activity on the high range was wideband noise
bursts timed to Wi-Fi beacons. These overrides came from a research build,
and bring-up rewrites both fields on every open. See
[mt7921.md](mt7921.md#5-ghz).

## 2. Summary

| Finding | Status | Section |
| --- | --- | --- |
| The 0.5–1.4% exact-zero samples come from the PHY packet detector; `0x8300011c[19:16]` = −8 stops them | Established | §3 |
| The adapter's own USB 3 link couples into antenna A; antenna straight out plus foil shielding brings the 2.4 GHz floor down 8–12 dB | Established | §4 |
| ICAP tap 12 is after the digital channel filter and helps at the 20 MHz profile | Established | §5 |
| Chain 1 can be captured from the ROM-only bring-up, alone or interleaved with chain 0 | Established | §6 |
| Chain 1 needs `0x1e4` bit 17: it is deaf at 5 GHz without it and level with chain 0 with it | Single run | §6 |
| Two-chain whitened combining adds frames but no new networks | Established | §6 |
| LNA codes 4 and 5 exist and help; measured gain table in dB | Established | §7 |
| 80 MSps works over usbipd with the Mtk79ICAP 2.4 GHz BW80 recipe | Established | §8 |
| Host USB queue depth, not USB aggregation, sets the gap rate | Established | §8 |
| The 2.4 GHz synthesizer calibrates from 2171 to 2652 MHz on this unit. The high VCO range calibrates beyond that but receives nothing under any LO-path value | Established | §1, §12 |
| Most OFDM `bad_service` failures are 802.11n HT-mixed frames, not blanking | Established | §10 |
| Per-symbol pilot slope fitting loses weak OFDM frames | Established | §10 |
| At 5 GHz the capture floor sits after every analog gain stage, and filter-stage codes 9 to 15 add up to 13 dB on either chain (LPF 14: +7 dB SNR on chain 0, +8 dB on chain 1) | Established | §7, §9 |
| Chain 0 has no sample blanking at 5 GHz, so packet-detect suppression has nothing to remove there. Chain 1 does blank at 5 GHz | Established | §9 |
| Weak legacy OFDM frames: a smoothed, L-SIG-aided estimate, smoothed pilot phase, an FCS-gated re-estimate and a lower STF threshold recover about 2.5 dB | Established | §10 |
| At 5 GHz chain 0 now reaches 50% of beacons about 2 dB short of the stock driver per chain, and about 4 dB short of its two-chain receiver (the first estimate was 15 dB) | Single run, same hour | §9 |
| With both chains at LPF 14 and combining, the whole-receiver gap at 5 GHz is about 1.3 dB | Single run, not same hour | §9 |

## 3. Sample blanking is the PHY packet detector

**Symptom.** Out of the box, about 0.5–1.4% of samples are exactly `(0, 0)`,
in runs of 48–49 samples (rarely 55), several thousand runs per second, in both
manual-gain and AGC modes. Every event has the same shape: a 2-sample and an
8-sample lead-in about 22 µs before the main run, the 48-sample main run, and
a 6-sample trailer about 50 samples later, so roughly 64 zeros per event.
Event starts fall on whole microseconds plus a fixed offset, unrelated to the
2,048-sample USB payload.

**It isn't the transport** (established). With the ring stopped, an EP0
readback of capture SRAM before reset matched the stream exactly: 388,313 of
388,325 samples and 178 of 178 zero runs. The zeros are already in SRAM.
Beacons are spaced exactly 102.4 ms apart, so no samples are dropped.

**It is the PHY's receive state machine** (established):

- Frames the PHY locks onto are clean after their first ~31 µs (96 of 96 test
  probes). Blanking resumes 2.6–4.8 µs after the frame ends.
- The event rate follows the noise at antenna A: about 0 per ms with the port
  open, 3.5 per ms with a good antenna placement, 11–13 per ms with USB noise
  coupled in.
- Mid-SNR frames are the ones lost. On channel 11, gapped frames decoded 0 of
  66, and gap-free frames at 0–4 dB SNR decoded 10 of 10. Across all channels,
  gap-free frames above 0 dB decoded 140 of 141.
- Every ICAP tap we tried is gapped, so the blanking is upstream of all of
  them.

Mtk79ICAP reports the same blanking under the vendor firmware (its
`iqstream.c` cites 247 runs/s).

**The fix** (established). `0x8300011c` bits 19:16 behave as a signed 4-bit
packet-detect field. The booted value is +7. Any negative value stops the
triggers after a short adaptation. We use **−8** (mask `0x000f0000`, value
`0x00080000`). We found it by masked screening of the AGC page
(`0x83000000–0x8300040f`) and bisecting, in about 700 short captures.

- **Adaptation:** 0.1–252 ms after GO in 46 one-second captures, and up to
  about 590 ms in a later 33-capture survey. The written value is still there
  at the end of each capture, so this is hardware settling, not firmware
  overwriting the field. `birria_rx --pd-n8` discards the first 600 ms.
- **After adaptation:** main runs fall from about 6–7 per ms to exactly 0 in
  44 of 46 captures.
- **Paired survey of channels 1–11, 44 captures:** zero fraction 0.66% →
  0.03%, FCS-valid frames +12%, noise floor unchanged.
- **Weak-frame cost:** none found. In a 20-pair alternating test on channels
  6 and 11, marginal APs decoded 316 beacons against 315, and frames below
  0 dB SNR decoded 10 of 24 against 6 of 23.

Avoid nibble −1 (`0xf`): it lifts the output about 8 dB and decoding
collapses. `0x83000188` byte 0 = `0x3f` removes only the 48-sample main run,
not the trigger; it adds 4-sample runs and decodes worse.

Implemented as `birriasdr_set_packet_detect_suppression()` and
`birria_rx --pd-n8`; see [mt7921.md](mt7921.md#packet-detect-suppression).

## 4. Antenna placement, USB 3 self-noise and shielding

**Port A is chain 0** (established). Laying antenna A flat changed the
spectrum; laying B flat changed nothing.

**The adapter's own USB 3 link couples into antenna A** (established).
Whenever antenna A runs alongside the adapter (elbow folded, upright, or on a
short RP-SMA pigtail), it picks up continuous broadband noise about 12 dB over
the receiver's own floor.

| Setup (2.4 GHz, chain 0) | Floor | Blanking events |
| --- | --- | --- |
| Antenna port open | −53.7 dBFS | about 0 per ms |
| Antenna straight out from the side port | about −50 dBFS | 3.5 per ms |
| Antenna bent alongside the adapter | about −42 dBFS | 11 per ms |

With the antenna straight out, channel-11 networks decoded rose from 1 to 4–6.
A 3 ft USB extension without a ferrite lowered one AP's beacon SNR from 25 to
11 dB; a ferrite near the adapter brought it back to 17.7 dB. A longer antenna,
a 2 dBi omni, ferrites alone, distance from the PC, and switching off other
devices in the room made no difference.

**Foil shielding** (single run, 28-capture scan before and after). The adapter
and the USB extension connector were wrapped in kitchen aluminum foil, folded
over for strength, and finished with copper tape. The shield was grounded to
the USB shells and the RP-SMA base:

| | Floor before → after |
| --- | --- |
| 2.4 GHz ch 1 / 6 / 11 (LNA 3) | −44.3 / −43.7 / −42.6 → **−48.0 / −49.4 / −49.6 dBFS** |
| 5 GHz, 25 channels (LNA 5) | −52.0 to −52.6 dBFS, unchanged |

At 5 GHz the floor was already the receiver's own, so there was nothing to
remove. No narrowband spurs stood out on any channel.

**Antenna B** (single run). With B pointing straight out, nearly horizontal and
opposite A, the in-band noise correlation between the two chains fell from
|ρ| 0.48–0.55 to 0.10–0.15, and both floors dropped 1–5 dB. Most of the
correlated noise was the same external pickup, not the shared LO.

**How the antenna is seated matters** (single run). Retightening antenna A
alone moved one AP's beacons by +7 dB at 5 GHz and −10 dB at 2.4 GHz, with
floors unchanged. Check levels in both bands before any A/B test that spans a
re-seat.

## 5. ICAP taps and the channel filter

In dense capture mode (`0x830ad440 = 0x13`), the tap select
`0x830ad448[11:8]` reads as stage × 4 + kind:

| Kind | Taps | Under the ROM-only bring-up |
| --- | --- | --- |
| 0 | 0, 4, 8, 12 | Chain 0 I/Q |
| 1 | 1, 5, 9, 13 | Chain 1 I/Q when chain 1 is powered (§6); otherwise a non-negative envelope with I = Q |
| 2, 3 | | Exact zeros |

A nonzero `[15:12]` gives a constant idle word. `0x830ad448[19:18]` is a real
rate field in dense mode: 3 = 40 MSps, and 1 or 2 overrun until the BW80
recipe and a matching firmware loop are in place (§8).

**Tap 12 is the output after the digital channel filter** (established), so
its width follows `0x83081000[27:24]`. Tap 4, BirriaSDR's default, has no such
filter. This matches Mtk79ICAP.

- **20 MHz profile, 18 alternating pairs on channels 1, 6 and 11:** the skirt
  10–15 MHz off-centre is about 13 dB lower on tap 12 (−24 against −11 dB),
  OFDM decodes rose 8%, DSSS was higher in 12 of 16 non-tied pairs, and the
  networks found were the same. A later check gave −25 to −30 dB against −9 to
  −12 dB and +6–14% frames.
- **40 MHz profile:** both taps are flat to ±20 MHz. A channel-1 AP 25 MHz
  below centre still aliases in and decodes at +15 MHz (76 frames on tap 4,
  43 on tap 12), so tap 12 is not an anti-alias filter for the full span.
- **Caveat:** the tap-12 edge at the 20 MHz profile is bimodal across
  captures: −4.3 dB (wide) in 12 of 21 captures and −9.6 dB (narrow) in 9 of
  21, drifting from wide to narrow over a series. A latch pulse didn't change
  this. Treat 20 MHz tap-12 edge numbers with care.

Implemented as `birriasdr_set_channel_filter()` (`--channel-filter`, Soapy
`channel_filter`).

**Other modes.** Mode `0x14` with `0x830ad448` bit 24 gives two interleaved
40 MSps slots at about 320 MB/s. Other mode bytes return a source timeout
(firmware state 10) unless their matching enable bit in `0x830ad448` is set.

## 6. The second receive chain

**Reading the ROM** (method). The boot ROM is readable at `0x800000` with
ordinary EP0 reads (512 KB, mirrored). Its RF wrappers map **chain 0 to RF SPI
port 1 and chain 1 to SPI port 0**, so the "companion" SPI 0 that BirriaSDR
uses as its LO source is chain 1's own receiver. The ROM's path-mode routine
sets SPI 0 `0x1e4` to `0x23c1` in single-chain modes. This repository doesn't
include the ROM image.

**Enabling chain 1** (established). Setting RF SPI 0 `0x1e4` from `0x23c1` to
`0x1fbc3` powers chain 1's receive path. Bits 1, 11, 14, 15 and 16 are needed
and bit 12 is optional. The kind-1 taps then carry chain 1: tap 5 unfiltered,
tap 13 filtered. Removing antenna B confirmed which chain is which. Enabling
chain 1 costs chain 0 nothing.

**Bit 17** (single run; interleaved 1 s A/B at 5785, 5240, 5180 and 2462 MHz).
Chain 0's own receive word, `0x3fb0b` on SPI 1, carries bit 17; `0x1fbc3`
didn't. Without it chain 1 shows no bursts at all at 5 GHz (+0.2 dB over its
floor where chain 0 shows +31 dB), and at 2.4 GHz it runs 10–17 dB below
chain 0. With `0x3fbc3` its median SNR is within 2 dB of chain 0's at
5785 and 5240 MHz, and at 2462 MHz the deficit went from −14.2 to −0.2 dB:
its floor rose 3.8 dB and its signals about 18 dB. On those 2462 MHz
samples chain 0 alone decoded 142 frames, chain 1 alone 217, and all branches
of `--combine` together 258. Chain 0's floor, burst rise and decode rate did
not change. An earlier study, before the foil shielding of §4, had found that
bit 17 raised signal and noise alike and broke the filtered dense taps, and
left it out. On the shielded adapter neither held: `--chain 1` alone at
2462 MHz went from 6 and 6 to 92 and 83 valid frames on tap 13, and from 33
and 4 to 56 and 59 on tap 5 (two 0.5 s runs each). BirriaSDR now writes `0x3fbc3` for `--chain 1` and `--chain both`.

**Both chains at once** (established). Mode `0x11`, node `0x0c` interleaves
tap 12 (chain 0) and tap 13 (chain 1) at 40 MSps each: even samples are
chain 0, odd samples chain 1, twice the USB rate. Unlike the dense taps, which
are 12-bit left-justified with almost no DC, these slots carry all 14 bits and
a large static DC offset (about −165+152j and −49+68j LSB at channel 6). It
changes with tuning and is stable within a capture, so it must be removed
before comparing chains. The 3 samples after each blanking run don't take the
plain correction: the receiver brings the whole raw sample back on a ramp
(about −0.05, 0.61 and 0.09 of full value), so subtracting the full offset
there leaves a spike of about minus the offset (~300 LSB on chain 0) in the
first data symbol of most frames. Zeroing those samples, as `--dc-block`
now does, took seven both-chain recordings from 1528 to 1600 frames over
all `--combine` branches (+113, −41; chain 0 alone 1362 → 1430). An earlier "chain 1 decodes 1.57× chain 0" result was
this DC offset and is withdrawn: with DC removed, chain 0 decoded 277 frames,
chain 1 276, and the union 279.

Implemented as `birriasdr_set_chain()` / `birria_rx --chain 0|1|both`.

**Combining** (established):

- In-band noise correlates between the chains at |ρ| 0.45–0.57 (with the
  original antenna placement), with a phase that changes per capture.
- Slot 1 lags slot 0 by 0–0.9 samples, constant within a capture but
  different between captures.
- Chain 1's SNR was usually 5–14 dB below chain 0's on the same burst. That
  was without bit 17; with it the chains are about level, so the combining
  and survey results below are due a rerun.
- Plain MRC, which ignores the noise correlation, is worse than the better
  chain alone. Whitened combining (w = R⁻¹h) gains 0 to +1.1 dB median.
- Per-STFT-bin combining broke short OFDM frames and used 2.7 GB of RAM;
  rejected.

`wifi.py --combine` decodes chain 0, chain 1 and the whitened combination and
counts each FCS-valid frame once. The combination only ever adds frames: on
17 two-chain recordings, +11 frames over the per-chain union and 0 lost.

**Survey** (established). Over 24 runs on channels 1, 6 and 11 scored against
the reference receiver, chain 0 alone decoded 144 of 175 reference frames, the
union 147, and the combination 157. Frames at −6 to −3 dB went from 2 of 14 to
9 of 14. One AP was decoded for the first time, from two combined-branch
beacons. A 33-run survey of channels 1–11 found no further networks.

## 7. Gain

**LNA codes above 3 exist** (established). Reading back the applied-index
mirror (RF `0x268`: LNA `[23:21]`, LPF `[20:17]`) showed chain 1 sitting at
LNA 5 / LPF 7 when nothing forces it. So "3/8 is the maximum" is wrong.

**Measured gain table, chain 0, 2.4 GHz** (established; one steady AP, live
sweeps inside one stream, 0 overruns). Relative to LNA 0 / LPF 0 / digital 0:

| Stage | Codes → dB |
| --- | --- |
| LNA | 0 → 0, 1 → 8.1, 2 → 13.8, 3 → 19.6, 4 → 25.8, 5 → 31.8 (6 and 7 behave like 5) |
| LPF | 0 → 0, then 2.1, 4.0, 5.9, 8.0, 9.7, 11.7, 13.9, 15.6 for codes 1–8 |
| Digital | 1.0 dB per code |

- The stages are separable to within 0.36 dB rms.
- SNR follows LNA about 1:1, saturates at LNA 5, and LPF stops helping above
  about 6. Digital gain adds no SNR. Fill LNA, then LPF, then digital.
- LNA 5 with LPF 7 or 8 gave +5.2 dB SNR over the shipped 3/4/0 and +2.3 dB
  over 3/8/0. From LNA 2 / LPF 0, LNA 5 / LPF 7 is 31.8 dB, which matches
  Mtk79ICAP's 32 dB 5 GHz model.
- **Chain 1:** LNA and digital steps match chain 0 within about 1 dB; its LPF
  steps are smaller (0 → 8 spans 11.0 dB against 15.6 dB). These were
  measured before chain 1's receive word gained bit 17 (§6) and haven't been
  remeasured.
- **Both-chain mode** taps come before digital gain, so digital codes do
  nothing there.
- **Released (unforced) gain:** chain 1 sits at a fixed LNA 5 / LPF 7 plus
  about 16.6 dB of digital gain, with no per-frame changes. Chain 0 behaves
  like a per-frame AGC, dipping on 40–44% of frames.
- At 2.4 GHz, chain 1's gain table is non-monotonic at high LPF codes: LPF 9,
  10, 12, 13 and 15 collapse it by 9–14 dB. Only LNA 5 / LPF 11 helped (+3–5 dB), and in
  a 16+16 ABBA that bought no extra frames.

The gain to measure against is a steady transmitter's frames, not the noise
floor: the floor is measured after the gain, so it can't measure the gain.

Implemented as per-chain dB tables (`birriasdr_set_tuner_gain()`,
`BIRRIASDR_CAP_GAIN_DB`, Soapy overall gain); see
[mt7921.md](mt7921.md#gain-in-db). The 2.4 GHz tables come from one AP at
2437 MHz. At 5 GHz each chain has its own table from live sweeps at 5240 to
5785 MHz, where filter-stage codes 9 to 15 are allowed on every chain and
2.4 GHz stays at 0 to 8 (§9).

## 8. Sample rates and the transport

**80 MSps** (established). Mtk79ICAP's 2.4 GHz BW80 recipe (RF `0x160`,
`0xb00`, `0xb04`, `0x83081000/0x83091000[27:24]`, then a pulse of
`0x8300c008/0x8301c008[31]` to latch it) works on the ROM-only bring-up, with
`0x830ad448[19:18]` = 2 and the firmware's poll guard scaled for the rate.
Under the ROM bring-up the other recipe words already hold their BW80 values.

- Samples are distinct (adjacent-pair repeats 0.1%, as at 40 MSps) and the
  floor is flat within about 3 dB across ±30 MHz. One 1 s capture at 2437 MHz
  decoded channels 1, 3, 6 and 11.
- Continuous reads run at 320 MB/s over usbipd.
- Applying the recipe at 40 MSps is safe (11 of 11 runs applied and restored)
  but only widens the filter and lets neighbours alias in, so it is useful only
  together with the rate change.
- 80 MSps is single-chain only (both chains would need 640 MB/s), and refused
  above 2610 MHz because the recipe is the 2.4 GHz one.

**Gaps follow host completion stalls; queue depth is the fix** (established).

| Queued USB transfers | 80 MSps gaps/s | 40 MSps gaps/s |
| --- | --- | --- |
| 64 | 0.50 | 0.15 |
| 192 (new default) | 0.033 | |
| 256 | 0.044 | 0 |

Host completion stalls of 15–18 ms are common over usbipd, and 64 transfers
cover only about 6.6 ms at 320 MB/s. USB aggregation made no difference:
32 KiB transfers carry 4 events each, as with Mtk79ICAP's settings, 64 KiB is
neutral, and 128 KiB is worse. Linux's default 16 MB usbfs limit refuses 256 ×
64 KiB; the library now reports that as `BIRRIASDR_ERROR_USB_MEMORY`.

**Recording to disk at 80 MSps** (single run). `birria_rx --recover` writes
across gaps and logs them. 40 MSps for 30 s to disk had 0 gaps. 80 MSps to
disk rose from 0–3 to 50–85 gaps/s after about 20 s as page-cache writeback
began; into RAM it was about 0.9/s. A writer thread with a RAM ring would be
the fix.

**20 MSps.** A 47-tap host halfband (flat to 8.5 MHz, −57.8 dB from 11.5 MHz)
gave no measurable aliasing. The alternative that keeps one ICAP word in four
(upstream PR #14) is flat to Nyquist and let a signal 12 MHz off-centre fold
in at about −10 dB, so we kept the halfband. A repeat on 2026-10-30 added
upstream main `8ddab14`, which since writes every rate-dependent filter field,
pulses the rate latch and reads back the rate bits. Three rounds at a 2390 MHz
centre, back to back with PR #14 and the halfband, used the BLE channel 37
signal at 2402 MHz as the source. It folded into `8ddab14` at −5.9, −6.5 and
−13.2 dB, and into PR #14 at −6.5, −5.8 and −11.1 dB. Both stayed flat to
Nyquist, and the halfband again showed no alias. The antennas were both
horizontal and the shield was on, unlike the first run.

**Sample clock** (established). Measured against beacon TSF and each AP's
carrier offset, the sample clock is within +0.1 ppm at 40 MSps and +0.14 ppm at
80 MSps. The +40 ppm correction used elsewhere doesn't apply to this
bring-up.

**Retune time.** A live retune is 231 RF-bridge control transfers of about
375 µs each over usbipd (about 87 ms). Skipping the pre-poll and command read
when the bridge is already ready cuts it to 147 transfers, about 54 ms. A
Raspberry Pi shows similar times, so the per-transfer cost isn't usbipd.

## 9. 5 GHz observations

These are early and mostly single runs, made after upstream added 5 GHz.

- **Chain 1 needs bit 17 at 5 GHz** (§6). Without it, later runs at 5180,
  5240 and 5785 MHz found chain 1 deaf, though one early run had seen it about
  2 dB weaker than chain 0. With bit 17 it is within 2 dB of chain 0.
- **I/Q balance is fine.** Image rejection measured on FCS-valid beacons
  tuned ±10 MHz off-centre was about 34 dB at 5 GHz and 28–33 dB at 2.4 GHz,
  with a small DC spur in both bands. Missing I/Q calibration doesn't explain
  the weak-AP losses.
- **Sensitivity against the stock driver.** The first comparison, on
  different days and antennas, put BirriaSDR about 15 dB behind `mt7921u`.
  Same-hour runs (BirriaSDR, then the stock driver in passive monitor mode,
  then BirriaSDR again, antennas untouched) refined it:
  - **Before any change** the gap was about 10 dB per chain, the same on
    channels 36 and 48. Expressed on the stock driver's chain-0 dBm scale, our
    floor was about −88 dBm.
  - **The floor is fixed after every analog stage.** At 5 GHz, minimum and
    maximum LNA and filter-stage codes give the same captured floor within
    0.2 dB, with no spurs. The signal tracked LNA and filter codes 1:1, and
    beacons peaked about 30 dB below full scale. So added analog gain is SNR.
    The chain 0 filter-stage codes 9 to 15 supply it (§7): the LPF 14 code
    gave +7.1 dB SNR at 5240 MHz and +6.9 dB at 5180 MHz (6 of 6 ABBA pairs
    each), moving our floor to about −95 dBm on the stock scale. On chain 1
    the same code gave +8.2 dB at 5805 MHz (6 of 6 pairs) and +8.4 dB at
    5240 MHz (3 of 3), with none of the collapse chain 1 shows at 2.4 GHz.
  - **The decoder then lost about 2.5 dB at threshold** (§10). With the
    weak-frame changes in place, a final same-hour run over 21 APs on channels
    36, 48, 149 and 161 fitted the 50% beacon level at −91.3 dBm for us
    (chain 0) against −93.1 dBm for the stock driver at its combined level,
    a gap of 2.0 dB [0.1, 18.1] per chain. The decoder before the change,
    on the same samples, was 3.9 dB short. As a whole receiver, stock reaches
    −95.4 dBm on its chain-0 axis, 4.3 dB [1.9, 11.7] ahead, because it
    combines two chains. The upper CI bounds are loose because few APs sit
    below stock's 50% level.
  - **Two-chain combining decides some APs.** On channel 161 one pair of
    BSSIDs reads −95 dBm on chain 0 and −92 dBm on chain 1. The stock driver
    decodes 94% of their beacons and we, on chain 0 alone, decode none.
  - **Both chains at LPF 14 close most of the rest.** Two-chain capture at
    LPF 14 on both chains, decoded with `--combine`, moved our 50% beacon
    level from −90.7 dBm (chain 0 alone) to −94.6 dBm, against −95.7 dBm for
    the stock driver: about 1.3 dB short as a whole receiver. That comparison
    wasn't made in the same hour, and its upper bounds are loose.
  - **Some APs decode worse than their SNR suggests.** Two APs at about
    4.4 dB L-LTF SNR decode only 9 to 13%, where another AP at 3.4 dB decodes
    76%. A stronger AP whose data symbols don't match its own L-LTF channel
    (its estimate costs 1.45 dB even at 21 dB SNR) behaves the same way.
- **No sample blanking at 5 GHz on chain 0.** Every chain-0 5 GHz capture
  checked (5180, 5240, plain and with packet-detect suppression, across days)
  has no zero runs of 8 or more samples. A 2.4 GHz capture from the same
  session has about one per millisecond. So `--pd-n8` has nothing to remove on
  chain 0 at 5 GHz, and blanking doesn't explain any chain-0 loss there.
  Chain 1 does blank at 5 GHz: its captures carry about 20 to 50 exact-zero
  runs per second at filter stages 8 to 15.
- **Stock band registers.** Comparing read-only PHY snapshots under the stock
  driver at channels 36 and 6 found 78 band-dependent fields. BirriaSDR never
  programs 48 of them, and leaves 15 at their 2.4 GHz values. Writing 13 of
  those 15 raised beacon SNR by only 0.5 dB (16 of 20 ABBA pairs). Of the RF
  front-end fields upstream PR #9 sets, only one trim field accepts a write
  after prepare, and it changed nothing (−0.02 dB).
- LNA 5 is best at 5 GHz too (about +12 dB SNR over LNA 3, matching reports in
  upstream PR #8).
- A 25-channel scan found no APs on channels 52–144 at this location.

## 10. Decoder findings

**Most OFDM `bad_service` failures are 802.11n** (established). With packet
detection suppressed, 99.3–99.7% of legacy OFDM frames that the reference
receiver logged also decode. Of 3,307 `bad_service` results, 3,169 are
HT-mixed frames (QBPSK after L-SIG). Every CRC-valid HT-SIG was one spatial
stream, MCS 0–7, BCC, and 39–50% used STBC, so an HT decoder would work with
one chain. Don't use `bad_service` counts as a blanking or sensitivity
metric.

`tools/decoders` now decodes that subset: 20 MHz, one spatial stream, MCS
0–7, BCC, either guard interval, Alamouti STBC and A-MPDUs (single run). On a
2 s channel-11 capture it added 39 HT MPDUs (141 → 184 valid frames) and lost
no legacy frames. Most HT frames there were MCS 5–7 and failed on SNR.
Smoothing the HT channel estimate over a −12..+16-sample impulse response,
and averaging in the L-LTF when it shows the same channel, then took HT MPDUs
from 29 to 84 and from 66 to 103 on two 2 s channel-11 captures (same
samples, 2 HT frames lost, no legacy frames lost; single run).

**Per-symbol pilot slope fitting loses weak frames** (established). Fitting a
phase offset and slope to the four pilots on every data symbol is too noisy at
about 10 dB LTF SNR: the slope error grows toward the band edges and leaves
burst errors the Viterbi decoder can't fix. On 16 beacons from a hidden AP on
channel 48, about 5 dB weaker than its neighbour:

| Pilot phase tracking | Weak 455-byte beacons | Stronger 275-byte beacons |
| --- | --- | --- |
| Offset and slope per symbol | 0 of 16 | 9 of 9 |
| Common phase only | 12 of 16 | 9 of 9 |
| Common phase + one slope trend per frame | 11 of 16 | 9 of 9 |
| Common phase + slope smoothed over ±8 symbols | 8 of 16 | 9 of 9 |

The decoder now uses |h|²-weighted common phase plus one linear slope trend
per frame, which keeps sampling-clock drift, and retries a failed frame with
the per-symbol fit, keeping that result only if its FCS passes. On the same
samples, a rescan went from 256 to 313 valid frames; 5 GHz channel 149 went
from 2 to 23; a 2.4 GHz channel-11 recording went from 124 to 162 with no
frames lost. Preamble or LTF match tracks channel shape, not SNR, so it is a
poor predictor of decodability.

**Weak legacy frames at 5 GHz** (established). With chain 0 at LPF 14, the
decoder needed about 3.6 dB L-LTF SNR for 50% of 6 Mbps beacons. A receiver
with a perfect channel estimate and correct LLRs needs about 0.1 dB on the
same channels.
- **Where the loss came from.** Re-encoding correctly decoded frames gives
  the exact transmitted symbols. Against that genie, at about 5 dB SNR:
  - The two-symbol L-LTF channel estimate costs about 0.9 dB.
  - Pilot phase tracking costs 0.7 dB.
  - Weighting the LLRs by the noisy |h|² from that estimate costs about
    1.1 dB.

  The genie's per-symbol common-phase jitter equals estimation noise alone,
  so the true phase moves slowly and can be smoothed. Moving the FFT window
  into the cyclic prefix changes nothing.
- **What the decoder does now:**
  - It projects the legacy estimate onto −12..+16 taps, as for HT.
  - It adds L-SIG as a third training symbol once its parity passes.
  - It smooths the pilot common phase over ±4 symbols around its trend.
  - For a 6 Mbps legacy frame that fails FCS, it re-encodes its own decode,
    re-estimates channel and phase from every symbol and decodes again (up to
    twice, kept only if FCS passes).
  - The STF detection threshold dropped from 0.65 to 0.5. Lowering it alone
    gains nothing, but once decoding improves the detector is the limit (it
    found only half of the frames near 3.3 dB).
- **Results.** Noise injection on real 5 GHz frames, paired, moved the 50%
  SNR by −2.47 dB [−2.66, −2.27]. On one AP's channels it went from 4.3 to
  1.6 dB. A held-out 2 s capture at 5240 MHz went from 27 to 52 valid frames,
  with a −90 dBm AP going from 4 to 17 of its 19.5 beacons. At 2.4 GHz,
  two 2 s channel-11 captures gained 28 and 22 frames and lost 0 and 3, with
  runtime up 29 to 36%.

**DSSS timing.** A Barker sidelobe bug in symbol timing failed about 2.6% of
strong, intact frames; fixed. A RAKE retry (chip-spaced lags weighted by their
correlation) recovers frames from APs with a delayed path about one chip late
at −6 to −9 dB.

**Erasure recovery** (established, then shelved). Recovering a DSSS frame with
up to 12 unknown bits, while keeping at least 20 FCS check bits, gave 3 of 3
correct frames with a measured false-accept rate of 1.46 × 10⁻⁶ (theory 1.91 ×
10⁻⁶). Gapped frames have 9–68 erased symbols, far beyond that, and once
blanking was suppressed recovery added no networks. OFDM can't recover whole
erased symbols either.

**Gap-free failures at high SNR** run at about 5% (5 of 101 test probes, 15 of
257 long AP frames); only 4 of 20 had outlier carrier-offset estimates.

**BLE.** `birria_ble` demodulates LE 1M advertising live and keeps only
CRC-valid PDUs. Centred at 2414 MHz it covers channels 37 and 38. A 5-minute
run with packet detection suppressed collected 6,299 valid PDUs with 0 drops,
using 0.81 CPU-seconds per second and 1.6 MB of output.

## 11. Host and platform notes

- **WSL2 and usbipd-win work at 40 and 80 MSps.** The adapter must enumerate
  at 5000M (check `lsusb -t`). After one replug a port's SuperSpeed side
  stopped linking and the adapter fell back to 480M, which BirriaSDR refuses;
  another port fixed it.
- **The stock driver and BirriaSDR exclude each other.** `mt7921u` refuses an
  adapter BirriaSDR has booted ("Failed to get patch semaphore"), and the
  reverse also holds. Every switch needs a physical replug. On Windows, a
  MediaTek driver installed later grabbed the adapter after a replug; `usbipd
  bind --force` keeps it.
- **WSL2 firmware loading:** `firmware_class.path` resolves in the utility
  VM's namespace and doesn't find the files. Build them in with
  `CONFIG_EXTRA_FIRMWARE`.
- **`CLOCK_MONOTONIC` in WSL2 ran 1.6% slow** against Windows time on this
  host, so any host-timed rate test (SoapySDRUtil, GNU Radio `probe_rate`)
  reads about 40.6 MSps for 40. Measure the sample clock against beacon
  timestamps instead.
- **Overrun bursts.** Overruns (firmware state 9) occasionally cluster for
  up to about 30 minutes with the host idle, then clear with nothing changed.
  Wait and retry; it isn't the hardware.
- **Forced gain after an unclean exit.** The analog gain override survives a
  WFSYS reset. If a client is killed mid-stream, the next open fails at stage
  6 with "unexpected hardware or receiver state". Replugging clears it. The
  Soapy module stops live streams at normal process exit for this reason.
- **Reading an unmapped PHY page can hang the adapter.** A read of
  `0x830d0000` timed out, the reset timed out too, and the adapter needed a
  replug. Check the ROM for real accesses before reading a new page.
- **GNU Radio at 80 MSps** needs bigger buffers: gr-osmosdr dropped about 14%
  until `set_min_output_buffer(1 << 20)`. The Soapy source keeps up.

## 12. Negative results

| Tried | Result |
| --- | --- |
| Gain-only changes within LNA 0–3 | No added networks |
| Capture taps 0–3, 5–8 and 12 against blanking | All gapped; blanking is upstream of every tap |
| Forcing the coarse AGC state (`0x830000d0[27]`) | Zero runs unchanged |
| 2 s captures or repeated passes instead of 1 s | Same networks; 2 s ran out of retries |
| The 40 MHz filter profile for Wi-Fi discovery | Missed APs that the 20 MHz profile caught |
| Late-preamble carrier refinement | 0 extra frames on identical I/Q |
| Coherent phase window of ±64 symbols | 180° slips from residual drift; use 16 |
| Overlaying all 321 stock-differing PHY registers at once | 8 writes didn't hold and 2 restores failed; stopped |
| ±12 sweeps of several AGC-page fields; copying 28 stock AGC values | Nothing beyond noise |
| `0x83000188` byte 0 = `0x3f`; packet-detect nibble −1 | Fewer valid frames (§3) |
| Antenna variants and room changes (§4) | No improvement over antenna straight out |
| Higher chain-1 gain (LNA 5 / LPF 11) | +3–5 dB SNR, no extra frames |
| A latch pulse for the bimodal 20 MHz tap-12 edge | Still bimodal |
| Packet-detect suppression (`--pd-n8`) at 5 GHz | Nothing to remove: no blanking at 5 GHz (§9) |
| Per-subcarrier noise weighting of OFDM LLRs at 5 GHz | 0.18 dB worse: the 5 GHz floor is flat, and estimating it adds noise |
| Decision-directed common phase on top of smoothed pilot phase | No further gain |
| Coarse CFO re-estimated from the STF before the found L-LTF | No change at any STF threshold |
| Moving the OFDM FFT window into the cyclic prefix | Flat within 0.1 dB |
| The high VCO range on the 2.4 GHz front end, which calibrates to 3000 MHz | No reception. An n41 sync block at 2597 MHz was found on the low range and not on the high range (2 of 2 pairs) (§1) |
| All 16 `0xc00[5:2]` LO-path values with either front end, high VCO range | None received the n41 sync block. Values with `[5:4]` = 3 only give the 5/6-VCO 5 GHz LO (§1) |

## 13. Open questions

- Why the 20 MHz tap-12 edge alternates between two widths across captures.
- The remaining 5 GHz gap of about 2 dB per chain: APs whose data symbols
  don't match their L-LTF channel, and about 1.3 dB of floor still above
  thermal. Chain 1 at filter-stage codes above 8 plus combining closed most
  of the whole-receiver gap (§9). A same-hour comparison against the stock
  driver is still to do.
- HT frames at MCS 5–7 that still fail on SNR, and whether two-chain
  combining recovers them now that chain 1 is level with chain 0.
- Chain 1's dB gain steps with bit 17 set.
- Two never-decoded 2.4 GHz APs that are still visible at −4 to −8 dB SNR on
  chain 0.
- Whether the packet-detect, chain-select and gain-table results hold on the
  COMFAST board and on other EDUP units.
- A writer thread for gap-free 80 MSps recording to disk.
- Whether VCO trim fields can push the 2.4 GHz edge above 2652 MHz.

## 14. Credits

- [BirriaSDR](https://github.com/luigifcruz/BirriaSDR) by Luigi Cruz, with
  the 5 GHz work by Tim Douglas, is the receiver this all runs on.
- [Mtk79ICAP](https://github.com/MtkWifiRev/Mtk79ICAP) (vendor-firmware mt76
  fork) supplied the 2.4 GHz BW80 recipe, the gain-hold register layout, the
  tap-12/13 interpretation and independent confirmation of the blanking.
- [w1700k-sdr](https://github.com/h0m3us3r/w1700k-sdr) was the original lead
  for the PHY gain-register layout (credited upstream).
- Upstream pull requests and issues on BirriaSDR (5 GHz in #8 and #9, other
  sample-rate approaches in #13–#15, board data in #4 and #6) were useful
  points of comparison.
