/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 * A deliberately small recorder: no UI, DSP, device power cycling or hidden driver.
 */
#include "birriasdr.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t interrupted;
struct recorder {
    birriasdr_dev_t *dev;
    FILE *file, *gaps;
    uint64_t limit, count, skip, gap_count;
    int error;
};

/* Full restarts allowed per read with --recover, as in the SoapySDR module;
 * clean overruns are recovered without spending this budget.
 */
#define RECOVER_MAX_RESTARTS 16u

/* Signal handlers only set a flag; library cancellation runs in normal context. */
static void on_signal(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

/* Write conventional little-endian signed I,Q, preserving all 14 source bits.
 * A requested sample limit is a file limit; cancellation still drains hardware.
 */
static void write_iq(const int16_t *iq, uint32_t count, struct recorder *recorder)
{
    uint8_t bytes[65536];
    if (recorder->limit && count > recorder->limit - recorder->count)
        count = (uint32_t)(recorder->limit - recorder->count);
    for (uint32_t done = 0; done < count && !recorder->error;) {
        uint32_t n = count - done;
        if (n > sizeof(bytes) / 4)
            n = sizeof(bytes) / 4;
        for (uint32_t i = 0; i < n * 2; ++i) {
            uint16_t value = (uint16_t)iq[(size_t)done * 2 + i];
            bytes[2 * i] = (uint8_t)value;
            bytes[2 * i + 1] = (uint8_t)(value >> 8);
        }
        if (fwrite(bytes, 4, n, recorder->file) != n)
            recorder->error = 1;
        else
            recorder->count += n;
        done += n;
    }
}

static void record(const int16_t *iq, uint32_t count, uint64_t first, void *context)
{
    struct recorder *recorder = context;
    (void)first;
    if (recorder->skip) { /* Settling prefix: received but never written. */
        uint32_t n = count < recorder->skip ? count : (uint32_t)recorder->skip;
        recorder->skip -= n;
        iq += (size_t)n * 2;
        count -= n;
    }
    write_iq(iq, count, recorder);
    if (interrupted || recorder->error || (recorder->limit && recorder->count >= recorder->limit))
        birriasdr_cancel_async(recorder->dev);
}

/* --recover: the library has flushed every sample before the gap, so the
 * recorder's count is the file position where continuity breaks. Samples from
 * that index on follow a gap of unknown length; nothing is inserted. A gap
 * inside the --discard prefix is logged at file sample 0. The log is flushed
 * per line so it stays usable if the recording is killed.
 */
static void on_gap(const struct birriasdr_recovery_event *event, void *context)
{
    struct recorder *recorder = context;
    ++recorder->gap_count;
    if (fprintf(recorder->gaps, "%llu %llu %u %u %s\n", (unsigned long long)recorder->count,
                (unsigned long long)event->next_sample, event->attempt,
                event->previous.firmware_state, birriasdr_strerror(event->error)) < 0 ||
        fflush(recorder->gaps))
        recorder->error = 1;
    if (recorder->error)
        birriasdr_cancel_async(recorder->dev);
}

/* List recognized hardware including unsupported boards, without opening them. */
static int list_devices(void)
{
    int count = birriasdr_get_device_count();
    if (count < 0) {
        fprintf(stderr, "%s\n", birriasdr_strerror(count));
        return 1;
    }
    for (int i = 0; i < count; ++i) {
        struct birriasdr_device_info info;
        if (birriasdr_get_device_info((uint32_t)i, &info))
            return 1;
        printf("%d: %04x:%04x %s [%s]\n", i, info.vid, info.pid, info.name,
               info.capabilities & BIRRIASDR_CAP_RX ? "RX implemented" : "unsupported");
    }
    return 0;
}

/* Parse decimal counts/Hz without accepting negative wrap or ignored suffixes. */
static int parse_number(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long number;
    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    number = strtoull(text, &end, 10);
    if (errno || *end || !number || number > UINT64_MAX)
        return -1;
    *value = (uint64_t)number;
    return 0;
}

/* Like parse_number, but zero is a valid discard count. */
static int parse_count(const char *text, uint64_t *value)
{
    if (!strcmp(text, "0")) {
        *value = 0;
        return 0;
    }
    return parse_number(text, value);
}

/* Gain codes include zero; accept only decimal codes within each raw range. */
static int parse_gain_code(const char *text, unsigned maximum, uint8_t *value)
{
    char *end;
    unsigned long code;
    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    code = strtoul(text, &end, 10);
    if (errno || *end || code > maximum)
        return -1;
    *value = (uint8_t)code;
    return 0;
}

/* Record device 0, with optional center, finite count and raw manual gain codes.
 * Keep the example's flags small; applications can use the full public API.
 */
int main(int argc, char **argv)
{
    birriasdr_dev_t *dev = NULL;
    struct birriasdr_stream_stats stats;
    struct recorder recorder = {0};
    uint64_t frequency = 0;
    uint64_t bandwidth = 0, rate = 0;
    struct birriasdr_gain gain = {2, 8, 0};
    /* About 600 ms at 40 MSps, in whole 2048-sample packets: past the detector's
     * measured adaptation after packet-detect suppression starts (0.1-252 ms
     * over 46 captures, then up to ~587 ms in a 33-capture survey).
     */
    uint64_t discard = 11719 * 2048;
    int result, close_result, argument = 1, explicit_gain = 0, buffered = 0;
    int pd_n8 = 0, explicit_discard = 0, chain = BIRRIASDR_CHAIN_0, channel_filter = 0;
    int recover = 0;
    char gaps_path[4096];
    if (argc == 2 && !strcmp(argv[1], "--list"))
        return list_devices();
    while (argument < argc && !strncmp(argv[argument], "--", 2)) {
        if (!strcmp(argv[argument], "--buffered")) {
            buffered = 1;
            ++argument;
        } else if (!strcmp(argv[argument], "--recover")) {
            recover = 1;
            ++argument;
        } else if (!strcmp(argv[argument], "--frequency") && argument + 1 < argc) {
            if (parse_number(argv[argument + 1], &frequency))
                return 1;
            argument += 2;
        } else if (!strcmp(argv[argument], "--pd-n8")) {
            pd_n8 = 1;
            ++argument;
        } else if (!strcmp(argv[argument], "--channel-filter")) {
            channel_filter = 1;
            ++argument;
        } else if (!strcmp(argv[argument], "--chain") && argument + 1 < argc) {
            if (!strcmp(argv[argument + 1], "0")) chain = BIRRIASDR_CHAIN_0;
            else if (!strcmp(argv[argument + 1], "1")) chain = BIRRIASDR_CHAIN_1;
            else if (!strcmp(argv[argument + 1], "both")) chain = BIRRIASDR_CHAIN_BOTH;
            else {
                fprintf(stderr, "--chain accepts 0, 1 or both\n");
                return 1;
            }
            argument += 2;
        } else if (!strcmp(argv[argument], "--discard") && argument + 1 < argc) {
            if (parse_count(argv[argument + 1], &discard))
                return 1;
            explicit_discard = 1;
            argument += 2;
        } else if (!strcmp(argv[argument], "--rate") && argument + 1 < argc) {
            if (parse_number(argv[argument + 1], &rate) || rate > UINT32_MAX)
                return 1;
            argument += 2;
        } else if (!strcmp(argv[argument], "--bandwidth") && argument + 1 < argc) {
            if (parse_number(argv[argument + 1], &bandwidth) || bandwidth > UINT32_MAX)
                return 1;
            argument += 2;
        } else if (!strcmp(argv[argument], "--gain") && argument + 3 < argc) {
            if (parse_gain_code(argv[argument + 1], 5, &gain.lna) ||
                parse_gain_code(argv[argument + 2], 15, &gain.lpf) ||
                parse_gain_code(argv[argument + 3], 12, &gain.digital)) {
                fprintf(stderr, "Gain codes require LNA 0..5, filter 0..8 (0..15 at 5 GHz), "
                                "digital 0..12\n");
                return 1;
            }
            explicit_gain = 1;
            argument += 4;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[argument]);
            return 1;
        }
    }
    if (argc - argument < 1 || argc - argument > 3) {
        fprintf(stderr, "Usage: %s --list | [--buffered | --recover] [--frequency Hz] [--rate Hz] [--bandwidth Hz] [--gain LNA FILTER DIGITAL] [--pd-n8] [--channel-filter] [--chain 0|1|both] [--discard SAMPLES] output.cs16 [complex-samples [manual]]\n", argv[0]);
        return 1;
    }
    if (argc - argument >= 2 && parse_number(argv[argument + 1], &recorder.limit))
        return 1;
    if (argc - argument == 3 && strcmp(argv[argument + 2], "manual"))
        return 1;
    /* --pd-n8 discards the detector's settling prefix by default; --discard
     * alone skips that many leading samples without changing the PHY.
     */
    if (!pd_n8 && !explicit_discard)
        discard = 0;
    /* The settling prefix is time: half as many samples at 20 MSps, twice as
     * many at the experimental 80 MSps. */
    if (rate == 20000000 && !explicit_discard)
        discard = discard ? 5860 * 2048 : 0;
    if (rate == 80000000 && !explicit_discard)
        discard = discard ? 23438 * 2048 : 0;
    /* Both chains interleave two samples per sample time (chain 0 first), so
     * counts are interleaved samples and the default settling prefix doubles.
     */
    if (chain == BIRRIASDR_CHAIN_BOTH && pd_n8 && !explicit_discard)
        discard *= 2;
    if (chain == BIRRIASDR_CHAIN_BOTH && (discard % 2 || recorder.limit % 2)) {
        fprintf(stderr, "--chain both requires even sample and discard counts to keep chain order\n");
        return 1;
    }
    recorder.skip = discard;
    /* Finite buffered reads stay strict; recovery belongs to streamed reads. */
    if (recover && buffered) {
        fprintf(stderr, "--recover streams to the file and cannot be combined with --buffered\n");
        return 1;
    }
    if (recover && snprintf(gaps_path, sizeof(gaps_path), "%s.gaps", argv[argument]) >= (int)sizeof(gaps_path)) {
        fprintf(stderr, "Output path too long for the gap log\n");
        return 1;
    }
    /* Buffered reads hold up to two seconds at 40 MSps (320 MB), or at 80 MSps. */
    uint64_t buffer_limit = rate == 80000000 ? 160000000 : 80000000;
    if (buffered && (!recorder.limit || recorder.limit + discard > buffer_limit ||
                     recorder.limit % 2048 || discard % 2048)) {
        fprintf(stderr, "Buffered capture requires 2048..%llu samples, including any discard, in multiples of 2048\n",
                (unsigned long long)buffer_limit);
        return 1;
    }
    result = birriasdr_open(&dev, 0);
    if (result < 0) {
        fprintf(stderr, "Open: %s\n", birriasdr_strerror(result));
        return 1;
    }
    recorder.dev = dev;
    if (rate) {
        result = birriasdr_set_sample_rate(dev, (uint32_t)rate);
        if (result < 0) {
            fprintf(stderr, "Sample rate: %s\n", birriasdr_strerror(result));
            birriasdr_close(dev);
            return 1;
        }
    }
    if (bandwidth) {
        result = birriasdr_set_tuner_bandwidth(dev, (uint32_t)bandwidth);
        if (result < 0) {
            fprintf(stderr, "Bandwidth: %s\n", birriasdr_strerror(result));
            birriasdr_close(dev);
            return 1;
        }
    }
    if (pd_n8) {
        result = birriasdr_set_packet_detect_suppression(dev, 1);
        if (result < 0) {
            fprintf(stderr, "Packet-detect suppression: %s\n", birriasdr_strerror(result));
            birriasdr_close(dev);
            return 1;
        }
    }
    if (channel_filter) {
        result = birriasdr_set_channel_filter(dev, 1);
        if (result < 0) {
            fprintf(stderr, "Channel filter: %s\n", birriasdr_strerror(result));
            birriasdr_close(dev);
            return 1;
        }
    }
    if (chain != BIRRIASDR_CHAIN_0) {
        result = birriasdr_set_receive_chain(dev, chain);
        if (result < 0) {
            fprintf(stderr, "Receive chain: %s\n", birriasdr_strerror(result));
            birriasdr_close(dev);
            return 1;
        }
    }
    if (frequency) {
        result = birriasdr_set_center_freq(dev, frequency);
        if (result < 0) {
            fprintf(stderr, "Frequency: %s\n", birriasdr_strerror(result));
            birriasdr_close(dev);
            return 1;
        }
    }
    recorder.file = fopen(argv[argument], "wbx");
    if (!recorder.file) {
        perror("Output (must not already exist)");
        birriasdr_close(dev);
        return 1;
    }
    if (recover) {
        recorder.gaps = fopen(gaps_path, "wx");
        if (!recorder.gaps) {
            perror("Gap log (must not already exist)");
            fclose(recorder.file);
            birriasdr_close(dev);
            return 1;
        }
        if (fprintf(recorder.gaps, "# birria_rx gap log for %s\n"
                                   "# file_sample next_sample attempt firmware_state error\n"
                                   "# file_sample: first sample in the file after a gap of unknown length\n",
                    argv[argument]) < 0 || fflush(recorder.gaps)) {
            perror("Gap log");
            fclose(recorder.gaps);
            fclose(recorder.file);
            birriasdr_close(dev);
            return 1;
        }
        result = birriasdr_set_recovery(dev, RECOVER_MAX_RESTARTS, on_gap, &recorder);
        if (result < 0)
            fprintf(stderr, "Recovery: %s\n", birriasdr_strerror(result));
    }
    if (!result && (explicit_gain || argc - argument == 3)) {
        result = birriasdr_set_tuner_gain_mode(dev, 1);
        if (!result) {
            result = birriasdr_set_gain_codes(dev, &gain);
            if (result == BIRRIASDR_ERROR_INVALID) {
                struct birriasdr_gain maximum;
                if (!birriasdr_get_gain_code_limits(dev, &maximum))
                    fprintf(stderr, "Gain codes exceed this band and chain's limits: LNA 0..%u, "
                                    "filter 0..%u, digital 0..%u\n", maximum.lna, maximum.lpf, maximum.digital);
            }
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (!result && buffered) {
        /* Keep disk conversion/I/O out of the finite acquisition. Check signal
         * cancellation after initialization and at most two seconds of RF.
         * Do not save failed or interrupted buffered data.
         */
        uint32_t received = 0;
        uint32_t total = (uint32_t)(recorder.limit + discard);
        int16_t *iq = malloc((size_t)total * 4);
        result = iq ? birriasdr_read_sync(dev, iq, total, &received)
                    : BIRRIASDR_ERROR_NO_MEMORY;
        if (!result && received != total) result = BIRRIASDR_ERROR_PROTOCOL;
        if (!result && interrupted) {
            fprintf(stderr, "Interrupted; buffered recording discarded\n");
            recorder.error = 1;
        }
        recorder.skip = 0;
        if (!result && !recorder.error)
            write_iq(iq + (size_t)discard * 2, received - (uint32_t)discard, &recorder);
        free(iq);
    } else if (!result) {
        result = birriasdr_read_async(dev, record, &recorder, 0, 0);
    }
    birriasdr_get_stream_stats(dev, &stats);
    close_result = birriasdr_close(dev);
    if (fclose(recorder.file))
        recorder.error = 1;
    if (recorder.gaps && fclose(recorder.gaps))
        recorder.error = 1;
    if (discard && !result)
        fprintf(stderr, "Discarded the first %llu complex samples%s\n", (unsigned long long)discard,
                pd_n8 ? " (packet-detect suppression settling)" : "");
    if (chain == BIRRIASDR_CHAIN_BOTH)
        fprintf(stderr, "Both chains: samples alternate chain 0, chain 1 at 40 MSps each; "
                        "each chain carries a DC offset to remove before decoding\n");
    if (recover)
        fprintf(stderr, "Recovered %llu gaps, logged in %s\n", (unsigned long long)recorder.gap_count, gaps_path);
    fprintf(stderr, "Saved %llu complex samples; stream=%s; firmware state=%u; stage=%d; reset=%s\n",
             (unsigned long long)recorder.count, birriasdr_strerror(result),
             stats.firmware_state, stats.stage, birriasdr_strerror(stats.reset_error));
    return result || close_result || recorder.error ? 1 : 0;
}
