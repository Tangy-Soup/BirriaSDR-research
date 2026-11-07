/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 * A deliberately small recorder: no UI, DSP, device power cycling or hidden driver.
 */
#include "birriasdr.h"
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared by the signal handler, watcher and callback. Never use an atomic
 * implementation that could take a lock from a signal handler. */
static atomic_bool interrupted;
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "the interrupt flag must be lock-free");

/* Streamed recordings keep file I/O off the read thread. The read callback only
 * copies into this preallocated ring; one writer thread converts and writes.
 * When the ring is full the callback waits: the disk has fallen too far behind,
 * and the library's own overrun handling then reports the loss (a stream error,
 * or a logged gap with --recover), so the file never silently skips samples.
 * Positions are complex samples since the start, protected by the lock; only
 * the slots between tail and head belong to the writer.
 */
struct ring {
    int16_t *iq;
    uint64_t capacity, batch, head, tail, peak, waits;
    bool finished, failed;
    pthread_mutex_t lock;
    pthread_cond_t changed;
};

struct recorder {
    birriasdr_dev_t *dev;
    FILE *file, *gaps;
    /* count: samples accepted for the file, its position for the gap log.
     * written: samples actually in the file. */
    uint64_t limit, count, written, skip, gap_count;
    int error;
    struct ring ring;
};

/* Largest single write, in complex samples (4 MiB). */
#define RING_WRITE_SAMPLES (1u << 20)
/* Default ring: this many seconds of the stream, to ride out writeback stalls. */
#define RING_DEFAULT_SECONDS 4u

/* Full restarts allowed per read with --recover, as in the SoapySDR module;
 * clean overruns are recovered without spending this budget.
 */
#define RECOVER_MAX_RESTARTS 16u

/* Signal handlers only set a flag; library cancellation runs in normal context. */
static void on_signal(int signal_number)
{
    (void)signal_number;
    atomic_store_explicit(&interrupted, true, memory_order_relaxed);
}

struct interrupt_watch {
    birriasdr_dev_t *dev;
    atomic_bool done;
};

/* STATE before read startup is expected. Keep retrying until the read returns,
 * including when setup/recovery has not yet made cancellation available. */
static void *watch_interrupt(void *context)
{
    struct interrupt_watch *watch = context;
    const struct timespec period = {.tv_nsec = 10000000};
    while (!atomic_load(&watch->done)) {
        if (atomic_load_explicit(&interrupted, memory_order_relaxed))
            (void)birriasdr_cancel_async(watch->dev);
        nanosleep(&period, NULL);
    }
    return NULL;
}

/* Write conventional little-endian signed I,Q in place, preserving all 14 source
 * bits. The caller's buffer is scratch: nothing reads these samples afterwards.
 */
static int store_iq(int16_t *iq, size_t count, struct recorder *recorder)
{
    uint8_t *bytes = (uint8_t *)iq;
    size_t stored;
    for (size_t i = 0; i < count * 2; ++i) {
        uint16_t value = (uint16_t)iq[i];
        bytes[2 * i] = (uint8_t)value;
        bytes[2 * i + 1] = (uint8_t)(value >> 8);
    }
    stored = fwrite(iq, 4, count, recorder->file);
    recorder->written += stored;
    return stored == count ? 0 : -1;
}

/* Drain the ring until the reader has finished and everything is written, or
 * until a write fails. A failure releases a callback waiting for space. */
static void *write_ring(void *context)
{
    struct recorder *recorder = context;
    struct ring *ring = &recorder->ring;
    pthread_mutex_lock(&ring->lock);
    for (;;) {
        uint64_t used = ring->head - ring->tail;
        if (used < ring->batch && !ring->finished) {
            pthread_cond_wait(&ring->changed, &ring->lock);
            continue;
        }
        if (!used)
            break;
        uint64_t at = ring->tail % ring->capacity, n = used;
        if (n > ring->capacity - at)
            n = ring->capacity - at;
        if (n > RING_WRITE_SAMPLES)
            n = RING_WRITE_SAMPLES;
        pthread_mutex_unlock(&ring->lock);
        int failed = store_iq(ring->iq + at * 2, (size_t)n, recorder);
        pthread_mutex_lock(&ring->lock);
        if (failed) {
            ring->failed = true;
            pthread_cond_broadcast(&ring->changed);
            break;
        }
        ring->tail += n;
        pthread_cond_broadcast(&ring->changed);
    }
    pthread_mutex_unlock(&ring->lock);
    return NULL;
}

/* Copy into the ring from the read callback, waiting for space if it is full.
 * Returns -1 once the writer has failed. */
static int ring_put(struct ring *ring, const int16_t *iq, uint64_t count)
{
    while (count) {
        uint64_t space, at, n;
        pthread_mutex_lock(&ring->lock);
        if (ring->head - ring->tail == ring->capacity && !ring->failed)
            ++ring->waits;
        while (ring->head - ring->tail == ring->capacity && !ring->failed)
            pthread_cond_wait(&ring->changed, &ring->lock);
        if (ring->failed) {
            pthread_mutex_unlock(&ring->lock);
            return -1;
        }
        space = ring->capacity - (ring->head - ring->tail);
        at = ring->head % ring->capacity;
        pthread_mutex_unlock(&ring->lock);
        n = count < space ? count : space;
        if (n > ring->capacity - at)
            n = ring->capacity - at;
        memcpy(ring->iq + at * 2, iq, (size_t)n * 4);
        iq += n * 2;
        count -= n;
        pthread_mutex_lock(&ring->lock);
        ring->head += n;
        if (ring->head - ring->tail > ring->peak)
            ring->peak = ring->head - ring->tail;
        if (ring->head - ring->tail >= ring->batch)
            pthread_cond_broadcast(&ring->changed);
        pthread_mutex_unlock(&ring->lock);
    }
    return 0;
}

/* A requested sample limit is a file limit; cancellation still drains hardware. */
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
    if (recorder->limit && count > recorder->limit - recorder->count)
        count = (uint32_t)(recorder->limit - recorder->count);
    if (!recorder->error) {
        if (ring_put(&recorder->ring, iq, count))
            recorder->error = 1;
        else
            recorder->count += count;
    }
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
    struct birriasdr_usb_stats usb_stats = {0};
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
    int recover = 0, watching = 0, writing = 0;
    uint64_t ring_mib = 0;
    struct interrupt_watch watch;
    pthread_t watcher, writer;
    struct sigaction action = {0};
    char gaps_path[4096];
    action.sa_handler = on_signal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        perror("Signal handler");
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "--list"))
        return list_devices();
    while (argument < argc && !strncmp(argv[argument], "--", 2)) {
        if (!strcmp(argv[argument], "--buffered")) {
            buffered = 1;
            ++argument;
        } else if (!strcmp(argv[argument], "--recover")) {
            recover = 1;
            ++argument;
        } else if (!strcmp(argv[argument], "--ring") && argument + 1 < argc) {
            if (parse_number(argv[argument + 1], &ring_mib) || ring_mib > 1048576) {
                fprintf(stderr, "--ring takes a size in MiB, 1 to 1048576\n");
                return 1;
            }
            argument += 2;
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
        fprintf(stderr, "Usage: %s --list | [--buffered | --recover] [--ring MiB] [--frequency Hz] [--rate Hz] [--bandwidth Hz] [--gain LNA FILTER DIGITAL] [--pd-n8] [--channel-filter] [--chain 0|1|both] [--discard SAMPLES] output.cs16 [complex-samples [manual]]\n", argv[0]);
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
    if (ring_mib && buffered) {
        fprintf(stderr, "--ring applies to streamed recording and cannot be combined with --buffered\n");
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
    if (atomic_load(&interrupted))
        return birriasdr_close(dev) ? 1 : 0;
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
    if (atomic_load(&interrupted))
        return birriasdr_close(dev) ? 1 : 0;
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
    if (!result && !atomic_load(&interrupted)) {
        watch.dev = dev;
        atomic_init(&watch.done, false);
        int error = pthread_create(&watcher, NULL, watch_interrupt, &watch);
        if (error) {
            fprintf(stderr, "Interrupt watcher: %s\n", strerror(error));
            result = BIRRIASDR_ERROR_NO_MEMORY;
        } else {
            watching = 1;
        }
    }
    if (watching && !buffered) {
        /* Preallocate and touch the whole ring, so the read thread never takes
         * page faults. A finite recording needs no more than its own length.
         * Both chains carry twice the samples per sample time. */
        struct ring *ring = &recorder.ring;
        uint64_t stream_rate = (rate ? rate : 40000000) * (chain == BIRRIASDR_CHAIN_BOTH ? 2 : 1);
        int error;
        ring->capacity = ring_mib ? ring_mib * 1048576 / 4 : stream_rate * RING_DEFAULT_SECONDS;
        if (recorder.limit && recorder.limit < ring->capacity)
            ring->capacity = recorder.limit;
        ring->batch = ring->capacity / 2 < RING_WRITE_SAMPLES ? ring->capacity / 2 + 1 : RING_WRITE_SAMPLES;
        ring->iq = ring->capacity <= SIZE_MAX / 4 ? malloc((size_t)ring->capacity * 4) : NULL;
        if (!ring->iq) {
            fprintf(stderr, "Ring: cannot allocate %llu MiB; choose a smaller --ring\n",
                    (unsigned long long)(ring->capacity / 262144));
            result = BIRRIASDR_ERROR_NO_MEMORY;
        } else {
            memset(ring->iq, 0, (size_t)ring->capacity * 4);
            pthread_mutex_init(&ring->lock, NULL);
            pthread_cond_init(&ring->changed, NULL);
            error = pthread_create(&writer, NULL, write_ring, &recorder);
            if (error) {
                fprintf(stderr, "Writer thread: %s\n", strerror(error));
                result = BIRRIASDR_ERROR_NO_MEMORY;
            } else {
                writing = 1;
            }
        }
    }
    if (watching && buffered) {
        /* Keep disk conversion/I/O out of the finite acquisition. The watcher
         * also cancels callback-free finite reads. Never save failed or
         * interrupted buffered data.
         */
        uint32_t received = 0;
        uint32_t total = (uint32_t)(recorder.limit + discard);
        int16_t *iq = malloc((size_t)total * 4);
        result = iq ? birriasdr_read_sync(dev, iq, total, &received)
                    : BIRRIASDR_ERROR_NO_MEMORY;
        if (!result && received != total) result = BIRRIASDR_ERROR_PROTOCOL;
        /* A cancelled finite read stops short, which read_sync reports as
         * PROTOCOL. After a signal that is the expected end, not bad data. */
        if (interrupted && (!result || result == BIRRIASDR_ERROR_PROTOCOL)) {
            fprintf(stderr, "Interrupted; buffered recording discarded\n");
            result = 0;
            recorder.error = 1;
        }
        recorder.skip = 0;
        if (!result && !recorder.error &&
            store_iq(iq + (size_t)discard * 2, received - (uint32_t)discard, &recorder))
            recorder.error = 1;
        free(iq);
    } else if (writing) {
        result = birriasdr_read_async(dev, record, &recorder, 0, 0);
    }
    /* The watcher owns a borrowed handle: stop and join it before close. */
    if (watching) {
        atomic_store(&watch.done, true);
        pthread_join(watcher, NULL);
    }
    /* Everything accepted so far still reaches the file, also after a signal. */
    if (writing) {
        pthread_mutex_lock(&recorder.ring.lock);
        recorder.ring.finished = true;
        pthread_cond_broadcast(&recorder.ring.changed);
        pthread_mutex_unlock(&recorder.ring.lock);
        pthread_join(writer, NULL);
        if (recorder.ring.failed)
            recorder.error = 1;
    }
    free(recorder.ring.iq);
    birriasdr_get_stream_stats(dev, &stats);
    birriasdr_get_usb_stats(dev, &usb_stats);
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
    if (writing)
        fprintf(stderr, "Ring: peak %llu of %llu MiB; waited for the disk %llu times\n",
                (unsigned long long)(recorder.ring.peak / 262144),
                (unsigned long long)(recorder.ring.capacity / 262144),
                (unsigned long long)recorder.ring.waits);
    if (usb_stats.completions)
        fprintf(stderr, "USB: %llu transfers; queue peak %u of %u; parked peak %u; longest gap %.1f ms\n",
                (unsigned long long)usb_stats.completions, usb_stats.queue_peak, usb_stats.queue_capacity,
                usb_stats.parked_peak, usb_stats.longest_gap_us / 1000.0);
    fprintf(stderr, "Saved %llu complex samples; stream=%s; firmware state=%u; stage=%d; reset=%s\n",
             (unsigned long long)recorder.written, birriasdr_strerror(result),
             stats.firmware_state, stats.stage, birriasdr_strerror(stats.reset_error));
    return result || close_result || recorder.error ? 1 : 0;
}
