/* Receive-only research tool. NOT a public API and NOT a capture profile.
 *
 * BirriaSDR side of the stock-versus-BirriaSDR PHY register diff. Run one
 * normal finite baseline capture; after the firmware publishes finite
 * completion (state 2, MCU parked) and BEFORE the normal gain/filter restore
 * and WFSYS reset, READ the requested MMIO ranges twice through the ordinary
 * endpoint-zero read. Nothing is written. The second pass identifies fields
 * that change by themselves. Output lines match an mt76 debugfs register dump:
 *   0xADDRESS 0xPASS1 0xPASS2
 * mt76's mt792xu_rr issues the same vendor request (0x63, type 0xdf), so the
 * stock-driver snapshot reads the same address space.
 */
#include "internal.h"
#include "chips/mt7921.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define MAX_RANGES 64

static const struct bs_backend *original;
static uint32_t starts[MAX_RANGES], counts[MAX_RANGES], total;
static unsigned ranges;
static uint32_t *values[2];
static uint32_t done[2], state, fail_address;
static int read_error;
static bool attempted;
static uint64_t read_ns;

static int snapshot(birriasdr_dev_t *dev)
{
    uint64_t begin = bs_now_ns();
    attempted = true;
    BS_TRY(bs_read32(dev, BS_MMIO, MT_MAILBOX + 28, &state));
    if (state != 2)
        return BIRRIASDR_ERROR_STATE;
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint32_t n = 0;
        for (unsigned r = 0; r < ranges; ++r)
            for (uint32_t i = 0; i < counts[r]; ++i, ++n) {
                fail_address = starts[r] + 4 * i;
                BS_TRY(bs_read32(dev, BS_MMIO, fail_address, &values[pass][n]));
                done[pass] = n + 1;
            }
    }
    read_ns = bs_now_ns() - begin;
    return 0;
}

static int reset_probe(birriasdr_dev_t *dev)
{
    if (!attempted && atomic_load(&dev->run_state) != BS_CANCELLING)
        read_error = snapshot(dev);
    return original->reset(dev);
}

int main(int argc, char **argv)
{
    if (argc < 5 || argc - 4 > MAX_RANGES) {
        fprintf(stderr, "Usage: %s new-output.txt center-Hz samples 0xADDR:words [...]\n", argv[0]);
        return 2;
    }
    char *end;
    uint64_t frequency = strtoull(argv[2], &end, 10);
    if (*end || frequency < 2156000000ULL || frequency > 2610000000ULL) return 2;
    unsigned long requested = strtoul(argv[3], &end, 10);
    if (*end || requested < 2048 || requested > 40000000 || requested % 2048) return 2;
    uint32_t count = (uint32_t)requested;
    for (int a = 4; a < argc; ++a) {
        unsigned long start = strtoul(argv[a], &end, 16);
        if (*end != ':' || start % 4) return 2;
        unsigned long words = strtoul(end + 1, &end, 10);
        if (*end || !words || words > 65536) return 2;
        starts[ranges] = (uint32_t)start;
        counts[ranges++] = (uint32_t)words;
        total += (uint32_t)words;
    }
    FILE *out = fopen(argv[1], "wx");
    if (!out) { perror("Output"); return 2; }
    int16_t *iq = malloc((size_t)count * 4);
    values[0] = calloc(total, sizeof(uint32_t));
    values[1] = calloc(total, sizeof(uint32_t));
    birriasdr_dev_t *dev = NULL;
    int result = iq && values[0] && values[1] ? birriasdr_open(&dev, 0) : BIRRIASDR_ERROR_NO_MEMORY;
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
    uint32_t n = 0;
    for (unsigned r = 0; r < ranges; ++r)
        for (uint32_t i = 0; i < counts[r]; ++i, ++n)
            if (n < done[1])
                fprintf(out, "0x%08x 0x%08x 0x%08x\n", starts[r] + 4 * i, values[0][n], values[1][n]);
    if (fclose(out)) result = 1;
    free(iq);
    free(values[0]);
    free(values[1]);
    printf("stream=%s samples=%u/%u state=%u stage=%d reset=%s close=%d snapshot=%s words=%u/%u passes=%u,%u in %.3f s%s",
           birriasdr_strerror(result), received, count, stats.firmware_state, stats.stage,
           birriasdr_strerror(stats.reset_error), closed,
           attempted ? birriasdr_strerror(read_error) : "not attempted",
           done[1], total, done[0], done[1], read_ns / 1e9, read_error ? "" : "\n");
    if (read_error)
        printf(" failed_at=0x%08x\n", fail_address);
    return result || closed || received != count || read_error || !attempted ? 1 : 0;
}
