/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */
#ifndef BIRRIASDR_H
#define BIRRIASDR_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#pragma GCC visibility push(default)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque receiver. Each handle owns its USB context and one Wi-Fi interface. */
typedef struct birriasdr_dev birriasdr_dev_t;

enum birriasdr_error {
    BIRRIASDR_OK = 0,
    BIRRIASDR_ERROR_INVALID = -1,
    BIRRIASDR_ERROR_NO_DEVICE = -2,
    BIRRIASDR_ERROR_ACCESS = -3,
    BIRRIASDR_ERROR_BUSY = -4,
    BIRRIASDR_ERROR_IO = -5,
    BIRRIASDR_ERROR_TIMEOUT = -6,
    BIRRIASDR_ERROR_NO_MEMORY = -7,
    BIRRIASDR_ERROR_UNSUPPORTED = -8,
    BIRRIASDR_ERROR_COLD_BOOT = -9,
    BIRRIASDR_ERROR_PROTOCOL = -10,
    BIRRIASDR_ERROR_OVERRUN = -11,
    BIRRIASDR_ERROR_STATE = -12
};

enum birriasdr_chipset {
    BIRRIASDR_CHIP_UNKNOWN,
    BIRRIASDR_CHIP_MT7921,
    BIRRIASDR_CHIP_MT7925
};

enum birriasdr_capability {
    BIRRIASDR_CAP_RX = 1u << 0,
    BIRRIASDR_CAP_RAW_GAIN = 1u << 1,
    BIRRIASDR_CAP_GAIN_DB = 1u << 2,
    BIRRIASDR_CAP_AGC = 1u << 3,
    BIRRIASDR_CAP_FILTER = 1u << 4,
    BIRRIASDR_CAP_ANTENNA = 1u << 5,
    BIRRIASDR_CAP_CALIBRATION = 1u << 6,
    BIRRIASDR_CAP_LIVE_GAIN = 1u << 7,
    BIRRIASDR_CAP_TUNING = 1u << 8,
    BIRRIASDR_CAP_LIVE_TUNING = 1u << 9,
    BIRRIASDR_CAP_PD_SUPPRESS = 1u << 10,
    BIRRIASDR_CAP_CHAIN_SELECT = 1u << 11,
    BIRRIASDR_CAP_CHANNEL_FILTER = 1u << 12
};

/** Receive chains for birriasdr_set_receive_chain(). */
enum birriasdr_receive_chain {
    BIRRIASDR_CHAIN_0 = 0,
    BIRRIASDR_CHAIN_1 = 1,
    BIRRIASDR_CHAIN_BOTH = 2
};

/** Discovery includes recognized but unimplemented devices (capabilities=0).
 * Indices are enumeration snapshots: unplugging a device can change them.
 * No firmware is loaded and no interface is claimed during enumeration.
 */
struct birriasdr_device_info {
    uint16_t vid, pid;
    uint8_t bus, address;
    enum birriasdr_chipset chipset;
    uint32_t capabilities;
    char name[96];
};

/** Raw hardware indices, deliberately distinct from calibrated gain in dB. */
struct birriasdr_gain {
    uint8_t lna;       /**< MT7972 range 0..5; 5 GHz needs 4..5 for full sensitivity. */
    uint8_t lpf;       /**< Filter-stage gain 0..8; does not select bandwidth. */
    uint8_t digital;   /**< Digital gain code 0..12. */
};

/** Last hardware operation reached; useful when a read fails before samples arrive. */
enum birriasdr_stage {
    BIRRIASDR_STAGE_IDLE,
    BIRRIASDR_STAGE_RESET,
    BIRRIASDR_STAGE_ROM,
    BIRRIASDR_STAGE_DOWNLOAD,
    BIRRIASDR_STAGE_CAPTURE,
    BIRRIASDR_STAGE_RF,
    BIRRIASDR_STAGE_GAIN,
    BIRRIASDR_STAGE_STREAM,
    BIRRIASDR_STAGE_VERIFY
};

/** Summary of the most recent read, available after read_async/read_sync returns.
 * A nonzero read result invalidates continuity of that capture, even if some
 * samples were already delivered. firmware_state 9 means the ICAP ring overran.
 * An automatically recovered read can return zero despite notified gaps.
 * Samples/packets/bytes accumulate across attempts; the remaining fields describe
 * the final attempt. Each recovery event snapshots the current acquisition.
 * Packets count received data events, including the final packet discarded at
 * a device gap, so packets times payload size can exceed delivered samples.
 */
struct birriasdr_stream_stats {
    uint64_t samples;
    uint64_t packets;
    uint64_t usb_bytes;
    uint32_t firmware_state;
    uint32_t dma_errors;
    enum birriasdr_stage stage;
    int reset_error;
};

/** Samples are interleaved native-endian int16_t I,Q in [-8192,8191].
 * count and first_sample count COMPLEX samples, not scalar components/bytes.
 * The buffer is borrowed until callback return; copy it to retain it. There
 * is no averaging, decimation, left shifting, or floating-point conversion.
 * Run callbacks promptly: a stalled consumer can overrun the device's SRAM.
 * The center-frequency setter/getter, tuning-range getter, raw-gain mode/code
 * setters, gain-code getter and cancel_async may be called from the callback;
 * other operations must wait until the read returns.
 * Prefer a control thread for updates so callbacks keep draining data.
 */
typedef void (*birriasdr_read_async_cb_t)(const int16_t *iq, uint32_t count,
                                        uint64_t first_sample, void *context);

/** Notification at a source gap or before a full acquisition restart.
 * A local source gap keeps firmware, USB and receiver settings running. A full
 * restart drains the preceding acquisition first. Fresh samples follow this
 * event and are NOT continuous with preceding samples. The number of missing
 * samples is unknown; no zeros or invented timestamps are inserted.
 */
struct birriasdr_recovery_event {
    uint64_t next_sample; /**< Total samples delivered in this read; not elapsed RF time. */
    uint32_t attempt;     /**< Recovery number starting at one, saturating at UINT32_MAX. */
    int error;           /**< Error that caused this discontinuity. */
    struct birriasdr_stream_stats previous; /**< Acquisition snapshot at the gap, with its fault state. */
};

/** Called on the read thread, with no library lock held and no samples in flight
 * to application callbacks. May call cancel_async or getters allowed during a
 * read. Live setters are also allowed, but return BUSY during full restart or
 * cancellation. A local gap retains the live control window. A notification
 * is not a guarantee of further samples; read_async reports final failure.
 */
typedef void (*birriasdr_recovery_cb_t)(const struct birriasdr_recovery_event *event,
                                      void *context);

/** Return a static library version string; never NULL. */
const char *birriasdr_get_version(void);

/** Return a static explanation for a library error; never NULL. */
const char *birriasdr_strerror(int error);

/** Return the number of recognized USB devices, or a negative error. */
int birriasdr_get_device_count(void);

/** Describe a recognized device without claiming it. Returns 0 or an error. */
int birriasdr_get_device_info(uint32_t index, struct birriasdr_device_info *info);

/** Claim a supported Wi-Fi USB interface without loading firmware yet.
 * The interface must be free of a kernel Wi-Fi driver. The library never
 * unloads drivers, power-cycles a hub, downloads vendor firmware, or writes
 * nonvolatile memory. Startup may reset USB to recover a previous failure;
 * a reset that invalidates the handle returns NO_DEVICE. A retained vendor
 * patch is rejected when reading.
 * On failure *dev is NULL. Currently only COMFAST 3574:6211 is qualified.
 */
int birriasdr_open(birriasdr_dev_t **dev, uint32_t index);

/** Release the handle. Returns BUSY during a read; cancel and join it first.
 * The Wi-Fi kernel driver is not rebound (that could load vendor firmware).
 */
int birriasdr_close(birriasdr_dev_t *dev);

/** Return this backend's implemented capability bits, or 0 for NULL. */
uint32_t birriasdr_get_capabilities(const birriasdr_dev_t *dev);

/** Return inclusive command limits in Hz. Returns UNSUPPORTED if unknown.
 * These are programming limits, not guaranteed RF sensitivity or passband
 * coverage. The interval may contain gaps; use get_tuning_band for each
 * contiguous band. Safe during a read.
 */
int birriasdr_get_tuning_range(const birriasdr_dev_t *dev, uint64_t *minimum_hz,
                              uint64_t *maximum_hz);

/** Return contiguous band `index` in Hz, starting at 0. Returns INVALID past the
 * last band and UNSUPPORTED for a band this board cannot reach. Live retunes
 * may cross bands. Safe during a read.
 */
int birriasdr_get_tuning_band(const birriasdr_dev_t *dev, uint32_t index,
                              uint64_t *minimum_hz, uint64_t *maximum_hz);

/** Set a requested center in Hz, without channel-number rounding or host mixing.
 * The MT7921 bands are 2156000000..2610000000 and 4500000000..6200000000 Hz,
 * inclusive, with nominal 28.6102294921875 and 47.6837158203125 Hz hardware
 * steps; frequency accuracy is uncalibrated. Requests in the gap are UNSUPPORTED.
 * While idle, stage the center for the next read. With CAP_LIVE_TUNING, apply it
 * during capture without restarting ICAP/DMA or downloading firmware. Calls
 * serialize with gain and lifecycle controls; startup/shutdown may return BUSY.
 * During capture, success means calibration completed, known diagnostic rails
 * were absent and frequency readback passed. It does not prove PLL lock or settling.
 * Samples during a retune are disturbed; phase is not continuous. Queued data
 * can still belong to the old center after return. No exact sample boundary is
 * reported. A failed hardware retune attempts rollback and stops the acquisition;
 * enabled recovery can then restart with the preceding committed frequency.
 * An out-of-range request returns UNSUPPORTED without disrupting a live stream.
 */
int birriasdr_set_center_freq(birriasdr_dev_t *dev, uint64_t frequency_hz);

/** Return the last committed requested center in Hz, including during capture;
 * not a calibrated RF measurement or the center of buffered samples. Zero if NULL.
 */
uint64_t birriasdr_get_center_freq(const birriasdr_dev_t *dev);

/** Stage complex samples/second, while idle only. MT7921 captures at 40000000.
 * The sampled span is 40 MHz, with a selectable matched nominal 20 or 40 MHz
 * analog/digital filter profile. This is not a calibrated passband or an
 * alias-rejection guarantee. Half the capture rate (20000000 on MT7921) keeps
 * the hardware at its rate and decimates 2:1 on the host with a halfband
 * filter: flat to +/-8.5 MHz, -6 dB at +/-10 MHz and at least 57 dB down from
 * +/-11.5 MHz, with a fixed 575 ns delay and a 23-sample start transient after
 * each start or continuity gap. USB traffic is unchanged. Decimation is not
 * available with BIRRIASDR_CHAIN_BOTH (reads return UNSUPPORTED).
 */
int birriasdr_set_sample_rate(birriasdr_dev_t *dev, uint32_t samples_per_second);

/** Return the nominal configured complex output rate, or 0 for NULL. */
uint32_t birriasdr_get_sample_rate(const birriasdr_dev_t *dev);

/** Set gain mode: 1 forces the documented gain indices, 0 releases overrides.
 * Releasing overrides is not a claim of complete closed-loop AGC control.
 * While idle, settings apply at the next read. With CAP_LIVE_GAIN, a running
 * capture changes registers synchronously without rebooting or stopping ICAP.
 * Calls serialize with startup/cleanup; BUSY means no live control window is
 * available. Backends without live support return UNSUPPORTED during capture.
 * The update can take milliseconds and spans multiple register writes. Success
 * confirms readback, not an exact sample boundary: buffered samples may precede
 * the change and samples during the transition are not gain-calibrated.
 * A hardware update failure stops that acquisition and leaves cleanup to the
 * reader, which may automatically restart if set_recovery was enabled.
 */
int birriasdr_set_tuner_gain_mode(birriasdr_dev_t *dev, int manual);

/** Set manual LNA/LPF/digital indices. Requires manual mode. The same idle/live
 * semantics as set_tuner_gain_mode apply. The backend preserves unrelated fields,
 * verifies readbacks and restores the preceding settings if an update fails.
 */
int birriasdr_set_gain_codes(birriasdr_dev_t *dev, const struct birriasdr_gain *gain);

/** Read committed gain codes and mode, including during capture. These are
 * requested values while idle, not an instantaneous AGC or RF gain readback.
 */
int birriasdr_get_gain_codes(birriasdr_dev_t *dev, struct birriasdr_gain *gain,
                            int *manual);

/** Reserved: calibrated tuner gain in tenths of a dB. UNSUPPORTED until the
 * measured stage-gain mapping is known; raw indices must not be labeled dB.
 */
int birriasdr_set_tuner_gain(birriasdr_dev_t *dev, int tenths_db);

/** Reserved: enumerate calibrated gains. UNSUPPORTED; does not fill gains. */
int birriasdr_get_tuner_gains(birriasdr_dev_t *dev, int *gains, size_t capacity);

/** Reserved: global AGC enable/freeze. UNSUPPORTED because only individual
 * force fields are established; use tuner_gain_mode for those overrides.
 */
int birriasdr_set_agc_mode(birriasdr_dev_t *dev, int enabled);

/** Stage nominal analog/digital filter bandwidth in Hz, while idle only.
 * MT7921 supports 20000000 and 40000000; default is 40000000. Both retain the
 * 40 MSps complex sample clock. The 20 MHz profile helps isolate one Wi-Fi
 * channel; it does not reduce USB traffic. Profiles are not calibrated flat
 * passbands or brick-wall anti-alias filters. Other widths return UNSUPPORTED.
 */
int birriasdr_set_tuner_bandwidth(birriasdr_dev_t *dev, uint32_t bandwidth_hz);

/** Return the committed nominal filter profile, including during capture.
 * Zero for NULL or a backend with no implemented filter profile.
 */
uint32_t birriasdr_get_tuner_bandwidth(const birriasdr_dev_t *dev);

/** Stage packet-detect suppression for the next read, while idle only.
 * Nonzero enables it; zero (the default) leaves the PHY as booted.
 * With CAP_PD_SUPPRESS, the PHY's own packet detector stops triggering on
 * noise. Its retriggers otherwise blank short runs of exact-zero samples into
 * the capture. The detector adapts after GO, so blanking can continue for the
 * first ~250 ms, occasionally ~590 ms, of each read; discard that prefix. This is a research setting:
 * it is not calibrated. A paired weak-AP test found no weak-frame cost.
 * Returns UNSUPPORTED without the capability.
 */
int birriasdr_set_packet_detect_suppression(birriasdr_dev_t *dev, int enabled);

/** Return 1 if packet-detect suppression is staged or active, otherwise 0. */
int birriasdr_get_packet_detect_suppression(const birriasdr_dev_t *dev);

/** Stage the receive chain for the next read, while idle only.
 * BIRRIASDR_CHAIN_0 (the default) leaves the backend's normal path unchanged.
 * With CAP_CHAIN_SELECT, BIRRIASDR_CHAIN_1 captures the second receiver alone
 * at the same tap stage, rate and format. BIRRIASDR_CHAIN_BOTH interleaves the
 * two chains sample by sample (chain 0 first, then chain 1), each at the
 * nominal rate after the channel filter, so a read carries twice as many
 * samples per second. That doubles the USB load, and source overruns are more
 * likely. Both-chain samples also carry a static DC offset per chain (up to a
 * few hundred LSB) that is not removed; subtract it before demodulating.
 * Both are research settings, qualified only on the tested board: a
 * chain is a receiver, not an antenna switch, and connector routing depends on
 * the board. Returns UNSUPPORTED without the capability, INVALID for an
 * unknown chain and BUSY while a read is active.
 */
int birriasdr_set_receive_chain(birriasdr_dev_t *dev, int chain);

/** Return the staged or active receive chain; BIRRIASDR_CHAIN_0 for NULL. */
int birriasdr_get_receive_chain(const birriasdr_dev_t *dev);

/** Stage the capture point after the PHY's digital channel filter, while idle.
 * Nonzero enables it; zero (the default) keeps the backend's unfiltered tap.
 * With CAP_CHANNEL_FILTER, single-chain captures then follow the digital
 * channel filter, whose width tracks birriasdr_set_tuner_bandwidth(). At the
 * 20 MHz profile this lowered the floor 10-15 MHz off centre by about 13 dB on
 * the tested board; at 40 MHz it reduces aliasing but does not remove it.
 * Rate and sample format are unchanged, and both-chain captures already use
 * the filtered point. Returns UNSUPPORTED without the capability and BUSY
 * while a read is active.
 */
int birriasdr_set_channel_filter(birriasdr_dev_t *dev, int enabled);

/** Return 1 if the channel-filter capture point is staged or active, otherwise 0. */
int birriasdr_get_channel_filter(const birriasdr_dev_t *dev);

/** Reserved: oscillator correction in ppm. UNSUPPORTED; no software DDC added. */
int birriasdr_set_freq_correction(birriasdr_dev_t *dev, int ppm);

/** Reserved: physical antenna routing. UNSUPPORTED; chain != antenna switch. */
int birriasdr_set_antenna(birriasdr_dev_t *dev, unsigned antenna);

/** Reserved: apply volatile calibration coefficients. UNSUPPORTED until their
 * format, routing and calibration procedure are decoded. Never programs eFuse.
 */
int birriasdr_set_calibration(birriasdr_dev_t *dev, const void *data, size_t length);

/** Clear host statistics while idle. Each read starts a fresh hardware capture;
 * this operation does not pretend to flush an active ICAP stream.
 */
int birriasdr_reset_buffer(birriasdr_dev_t *dev);

/** Enable automatic recovery for read_async; configure only while idle.
 * max_restarts is the device/transport fault budget per read call. A nonzero
 * budget requires a callback so discontinuities cannot be hidden. Zero disables
 * recovery and removes the callback (default).
 * Overruns, transient transport errors and failed live configuration updates
 * restart with the last committed settings. Explicit cancellation always wins.
 * With recovery enabled, clean device/host queue overruns report gaps and keep
 * restarting without spending this budget. An overrun with a failed reset,
 * DMA error or live-control failure still spends a bounded retry. The attempt
 * counter includes all recovery notifications, including clean overruns.
 * The MT7921 firmware recovers source overruns without reloading, resetting USB
 * or reapplying RF settings. The host retains one raw packet until the next
 * data event or clean terminal state confirms it. A gap discards that packet
 * and flushes the validated prefix before notifying, so buffers never span gaps.
 * A full host queue pauses resubmission and preserves completed USB bytes in
 * existing transfer buffers. The resulting device backpressure may cause a
 * notified source gap, but does not discard host framing or grow the queue.
 * Device removal, permissions, retained vendor patches and invalid/unsupported
 * requests are not retried. Fault-budget exhaustion returns the last capture error.
 * Finite read_sync calls remain strict and never splice separate acquisitions.
 */
int birriasdr_set_recovery(birriasdr_dev_t *dev, uint32_t max_restarts,
                          birriasdr_recovery_cb_t callback, void *context);

/** Capture an exact finite number of complex samples into I,Q int16_t pairs.
 * count must be a positive multiple of 2048, with room for 2*count int16_t's.
 * count is in output samples, after any host decimation.
 * *n_read is initialized to zero and reports any prefix even on error. Samples
 * are continuous only within one successful call; consecutive calls restart
 * the acquisition program and must not be concatenated as a continuous stream.
 */
int birriasdr_read_sync(birriasdr_dev_t *dev, int16_t *iq, uint32_t count,
                       uint32_t *n_read);

/** Stream until cancel_async, device loss, or an unrecovered hardware/protocol error.
 * Like librtlsdr, this blocks the calling thread while asynchronous USB reads
 * run. transfer_count=0 selects 64 queued transfers, or 256 on macOS.
 * Allowed values are 4..256.
 * buffer_samples=0 selects 16384; otherwise use a multiple of 2048, <=262144.
 * A final callback may be shorter. Cancellation drains submitted device
 * packets before returning; callbacks can still arrive after cancellation.
 * Returns 0 for a clean cancel, otherwise a negative error. Recovery is opt-in
 * through set_recovery and always reports a gap before another acquisition.
 * With recovery enabled, cancellation at a clean overrun also returns 0.
 * first_sample remains a cumulative delivered count across those gaps, not an
 * RF timestamp. Final statistics accumulate samples/packets/bytes over attempts;
 * firmware state, stage, DMA and reset errors describe the final attempt.
 * Without recovery enabled, no samples are silently skipped; check the return
 * value before treating a capture as valid.
 */
int birriasdr_read_async(birriasdr_dev_t *dev, birriasdr_read_async_cb_t callback,
                        void *context, uint32_t transfer_count,
                        uint32_t buffer_samples);

/** Request cancellation concurrently with read_async/read_sync or its callback.
 * Live gain/tuning operations are also allowed as documented above. Not async-signal-safe:
 * signal handlers should set their own flag, then cancel from a normal thread.
 * Repeated cancellation is harmless; returns STATE if no read is active.
 */
int birriasdr_cancel_async(birriasdr_dev_t *dev);

/** Copy final statistics while idle; returns BUSY until the read has returned. */
int birriasdr_get_stream_stats(birriasdr_dev_t *dev,
                             struct birriasdr_stream_stats *stats);

/* All functions return 0 on success unless their comments say otherwise.
 * The center-frequency setter/getter, tuning-range getter, raw-gain mode/code
 * setters, gain-code getter and cancel_async may run concurrently with a read
 * and with each other. Callers must serialize all other operations on a handle,
 * including close after joining BOTH the reader and any control threads.
 * Different handles may be used independently from different threads.
 */
#ifdef __cplusplus
}
#endif
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif
#endif
