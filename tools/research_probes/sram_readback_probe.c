/* Receive-only research tool. NOT a public API and NOT a capture profile.
 *
 * Stopped-source SRAM versus stream (docs/findings.md, section 3). Run one normal
 * finite capture (birria_rx's baseline profile), then, after the firmware has
 * published finite completion (state 2, ICAP control 0x4f0) and BEFORE the
 * normal gain/filter restore and WFSYS reset, read capture-ring words at
 * 0xe0200000 through the ordinary endpoint-zero MMIO read. Only reads are
 * added; the backend's prepare/start/stop and reset run unchanged.
 *
 * Streamed packet k was DMA'd from ring sample index (k mod 32) * 2048
 * (firmware/iq_stream.S), and the producer counts complex samples from zero,
 * so sample s of the stream lives at ring index s mod 65536 and is still
 * intact iff s >= produced - 65536. The comparison is done offline from the
 * mailbox words saved in the JSON.
 */
#include "internal.h"
#include "chips/mt7921.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define RING_BASE 0xe0200000u
#define RING_WORDS 65536u
#define RECHECK_WORDS 64u

static const struct bs_backend *original;
static uint32_t ring_start, ring_count;
static uint32_t *ring;
static uint32_t mailbox[22], live_control, live_producer, after_producer, after_control;
static uint32_t recheck_mismatch;
static uint64_t read_ns;
static uint32_t words_read;
static int read_error;
static bool attempted;

static int read_ring(birriasdr_dev_t *dev)
{
    uint64_t begin;
    attempted = true;
    for (unsigned i = 0; i < 22; ++i)
        BS_TRY(bs_read32(dev, BS_MMIO, MT_MAILBOX + i * 4, &mailbox[i]));
    /* Only a cleanly finished finite capture has a stopped, well-defined ring. */
    if (mailbox[7] != 2)
        return BIRRIASDR_ERROR_STATE;
    BS_TRY(bs_read32(dev, BS_MMIO, 0x80021090, &live_control));
    BS_TRY(bs_read32(dev, BS_MMIO, 0x800210b4, &live_producer));
    begin = bs_now_ns();
    for (uint32_t i = 0; i < ring_count; ++i) {
        uint32_t index = (ring_start + i) % RING_WORDS;
        BS_TRY(bs_read32(dev, BS_MMIO, RING_BASE + index * 4, &ring[i]));
        words_read = i + 1;
    }
    read_ns = bs_now_ns() - begin;
    /* The source must have stayed stopped while we read: same producer, same
     * leading words on a second pass.
     */
    for (uint32_t i = 0; i < ring_count && i < RECHECK_WORDS; ++i) {
        uint32_t value, index = (ring_start + i) % RING_WORDS;
        BS_TRY(bs_read32(dev, BS_MMIO, RING_BASE + index * 4, &value));
        recheck_mismatch += value != ring[i];
    }
    BS_TRY(bs_read32(dev, BS_MMIO, 0x800210b4, &after_producer));
    return bs_read32(dev, BS_MMIO, 0x80021090, &after_control);
}

/* Reads happen first; normal restore and reset always follow, whatever the
 * readback result. The readback result is reported separately.
 */
static int reset_probe(birriasdr_dev_t *dev)
{
    if (!attempted && atomic_load(&dev->run_state) != BS_CANCELLING)
        read_error = read_ring(dev);
    return original->reset(dev);
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "Usage: %s new-stream.cs16 new-ring.u32 new-meta.json center-Hz samples ring-start:count\n", argv[0]);
        return 2;
    }
    char *end;
    uint64_t frequency = strtoull(argv[4], &end, 10);
    if (*end || frequency < 2156000000ULL || frequency > 2610000000ULL) return 2;
    unsigned long requested = strtoul(argv[5], &end, 10);
    if (*end || requested < 2048 || requested > 40000000 || requested % 2048) return 2;
    uint32_t count = (uint32_t)requested;
    ring_start = (uint32_t)strtoul(argv[6], &end, 10);
    if (*end != ':' || ring_start >= RING_WORDS) return 2;
    ring_count = (uint32_t)strtoul(end + 1, &end, 10);
    if (*end || !ring_count || ring_count > RING_WORDS) return 2;

    FILE *stream_file = fopen(argv[1], "wbx");
    if (!stream_file) { perror("Stream output"); return 2; }
    FILE *ring_file = fopen(argv[2], "wbx");
    if (!ring_file) { perror("Ring output"); fclose(stream_file); return 2; }
    FILE *meta = fopen(argv[3], "wx");
    if (!meta) { perror("Metadata output"); fclose(stream_file); fclose(ring_file); return 2; }

    int16_t *iq = malloc((size_t)count * 4);
    ring = calloc(ring_count, sizeof(*ring));
    birriasdr_dev_t *dev = NULL;
    int result = iq && ring ? birriasdr_open(&dev, 0) : BIRRIASDR_ERROR_NO_MEMORY;
    struct bs_backend backend;
    struct birriasdr_gain gain = {3, 8, 0};
    uint32_t received = 0;
    struct birriasdr_stream_stats stats = {0};
    if (!result) {
        original = dev->backend;
        backend = *original;
        backend.reset = reset_probe;
        dev->backend = &backend;
        /* Same order and settings as birria_rx --buffered --bandwidth 20000000
         * --frequency F --gain 3 8 0 ... manual.
         */
        result = birriasdr_set_tuner_bandwidth(dev, 20000000);
        if (!result) result = birriasdr_set_center_freq(dev, frequency);
        if (!result) result = birriasdr_set_tuner_gain_mode(dev, 1);
        if (!result) result = birriasdr_set_gain_codes(dev, &gain);
        if (!result) result = birriasdr_read_sync(dev, iq, count, &received);
        birriasdr_get_stream_stats(dev, &stats);
    }
    int closed = dev ? birriasdr_close(dev) : 0;
    bool stream_ok = !result && received == count;
    if (stream_ok && fwrite(iq, 4, count, stream_file) != count) result = 1;
    if (words_read && fwrite(ring, 4, words_read, ring_file) != words_read) result = 1;
    fprintf(meta, "{\n  \"center_hz\": %llu,\n  \"samples\": %u,\n  \"received\": %u,\n",
            (unsigned long long)frequency, count, received);
    fprintf(meta, "  \"stream\": \"%s\",\n  \"firmware_state\": %u,\n  \"stage\": %d,\n  \"reset\": \"%s\",\n  \"close\": %d,\n",
            birriasdr_strerror(result), stats.firmware_state, stats.stage,
            birriasdr_strerror(stats.reset_error), closed);
    fprintf(meta, "  \"readback\": \"%s\",\n  \"ring_start\": %u,\n  \"ring_requested\": %u,\n  \"ring_read\": %u,\n",
            attempted ? birriasdr_strerror(read_error) : "not attempted",
            ring_start, ring_count, words_read);
    fprintf(meta, "  \"read_seconds\": %.6f,\n  \"recheck_words\": %u,\n  \"recheck_mismatch\": %u,\n",
            read_ns / 1e9, ring_count < RECHECK_WORDS ? ring_count : RECHECK_WORDS, recheck_mismatch);
    fprintf(meta, "  \"live_control\": %u,\n  \"live_producer\": %u,\n  \"after_control\": %u,\n  \"after_producer\": %u,\n",
            live_control, live_producer, after_control, after_producer);
    fprintf(meta, "  \"mailbox\": [");
    for (unsigned i = 0; i < 22; ++i)
        fprintf(meta, "%s%u", i ? ", " : "", mailbox[i]);
    fprintf(meta, "]\n}\n");
    if (fclose(stream_file) || fclose(ring_file) || fclose(meta)) result = 1;
    free(iq);
    free(ring);
    printf("stream=%s samples=%u/%u state=%u stage=%d reset=%s close=%d readback=%s words=%u in %.3f s recheck_mismatch=%u control=0x%x producer=%u->%u\n",
           birriasdr_strerror(result), received, count, stats.firmware_state, stats.stage,
           birriasdr_strerror(stats.reset_error), closed,
           attempted ? birriasdr_strerror(read_error) : "not attempted",
           words_read, read_ns / 1e9, recheck_mismatch, live_control, live_producer, after_producer);
    return result || closed || !stream_ok || read_error || !attempted ? 1 : 0;
}
