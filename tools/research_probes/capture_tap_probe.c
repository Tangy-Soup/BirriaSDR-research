/* Receive-only research tool. NOT a qualified alternate capture profile.
 * Wrap normal preparation, change only ICAP's tap mux before GO, and restore
 * that field before normal teardown. Stream checking and RF setup stay intact.
 */
#include "internal.h"
#include "chips/mt7921.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static unsigned tap;
static uint32_t saved_tap;
static bool tap_saved;
static bool coarse_hold, coarse_saved;
static uint32_t saved_coarse;
static unsigned bandwidth_class = 1;
static const struct bs_backend *original;

static int prepare_probe(birriasdr_dev_t *dev, uint32_t packets)
{
    uint32_t value;
    BS_TRY(original->prepare(dev, packets));
    BS_TRY(bs_read32(dev, BS_MMIO, 0x830ad448, &value));
    saved_tap = value & 0xf00;
    tap_saved = true;
    BS_TRY(bs_update32(dev, BS_MMIO, 0x830ad448, 0xf00, tap << 8));
    BS_TRY(bs_read32(dev, BS_MMIO, 0x830ad448, &value));
    if ((value & 0xf00) != (tap << 8)) return BIRRIASDR_ERROR_STATE;
    if (bandwidth_class == 0) {
        /* Established 20 MHz bootstrap profile, still sampled at 40 MSps.
         * Normal backend teardown restores all four saved filter fields.
         */
        for (unsigned port = 0; port < 2; ++port) {
            BS_TRY(mt_rf_update(dev, port, 0x160, 0xc0000000u, 0));
            BS_TRY(mt_rf_read(dev, port, 0x160, &value));
            if (value & 0xc0000000u) return BIRRIASDR_ERROR_STATE;
        }
        for (unsigned bank = 0; bank < 2; ++bank) {
            uint32_t address = 0x83081000 + bank * 0x10000;
            BS_TRY(bs_update32(dev, BS_MMIO, address, 0x0f000000, 0));
            BS_TRY(bs_read32(dev, BS_MMIO, address, &value));
            if (value & 0x0f000000) return BIRRIASDR_ERROR_STATE;
        }
        bs_sleep_us(20000);
    }
    if (coarse_hold) {
        /* Experimental: hold the initialized coarse state without selecting a
         * guessed state code. This does not establish global AGC disable.
         * Restore before normal reset so its baseline guard remains useful.
         */
        BS_TRY(bs_read32(dev, BS_MMIO, 0x830000d0, &value));
        saved_coarse = value & (1u << 27);
        coarse_saved = true;
        printf("coarse initialized=0x%08x state=%u\n", value, (value >> 24) & 7);
        BS_TRY(bs_update32(dev, BS_MMIO, 0x830000d0, 1u << 27, 1u << 27));
        BS_TRY(bs_read32(dev, BS_MMIO, 0x830000d0, &value));
        if (!(value & (1u << 27))) return BIRRIASDR_ERROR_STATE;
    }
    return 0;
}

static int reset_probe(birriasdr_dev_t *dev)
{
    int restore = 0;
    if (coarse_saved) {
        restore = bs_update32(dev, BS_MMIO, 0x830000d0, 1u << 27, saved_coarse);
        if (!restore) {
            uint32_t value;
            restore = bs_read32(dev, BS_MMIO, 0x830000d0, &value);
            if (!restore && ((value ^ saved_coarse) & (1u << 27)))
                restore = BIRRIASDR_ERROR_STATE;
        }
        if (!restore) coarse_saved = false;
    }
    if (tap_saved) {
        int result = bs_update32(dev, BS_MMIO, 0x830ad448, 0xf00, saved_tap);
        if (!result) tap_saved = false;
        if (!restore) restore = result;
    }
    int reset = original->reset(dev);
    return restore ? restore : reset;
}

int main(int argc, char **argv)
{
    if (argc < 5 || argc > 7) {
        fprintf(stderr, "Usage: %s new-output.cs16 center-Hz samples tap-0..15 [bandwidth-class-0-or-1 [hold-coarse-0-or-1]]\n", argv[0]);
        return 2;
    }
    char *end;
    uint64_t frequency = strtoull(argv[2], &end, 10);
    if (*end || frequency < 2156000000ULL || frequency > 2610000000ULL) return 2;
    unsigned long requested = strtoul(argv[3], &end, 10);
    if (*end || requested < 2048 || requested > 40000000) return 2;
    uint32_t count = (uint32_t)requested / 2048 * 2048;
    unsigned long requested_tap = strtoul(argv[4], &end, 10);
    if (*end || requested_tap > 15) return 2;
    tap = (unsigned)requested_tap;
    if (argc >= 6) {
        unsigned long requested_class = strtoul(argv[5], &end, 10);
        if (*end || requested_class > 1) return 2;
        bandwidth_class = (unsigned)requested_class;
    }
    if (argc == 7) {
        unsigned long requested_hold = strtoul(argv[6], &end, 10);
        if (*end || requested_hold > 1) return 2;
        coarse_hold = requested_hold != 0;
    }
    FILE *file = fopen(argv[1], "wbx");
    if (!file) { perror("Output"); return 2; }
    int16_t *iq = malloc((size_t)count * 4);
    birriasdr_dev_t *dev = NULL;
    int result = iq ? birriasdr_open(&dev, 0) : BIRRIASDR_ERROR_NO_MEMORY;
    struct bs_backend backend;
    struct birriasdr_gain gain = {3, 8, 0};
    uint32_t received = 0;
    struct birriasdr_stream_stats stats = {0};
    if (!result) {
        original = dev->backend;
        backend = *original;
        backend.prepare = prepare_probe;
        backend.reset = reset_probe;
        dev->backend = &backend;
        result = birriasdr_set_center_freq(dev, frequency);
        if (!result) result = birriasdr_set_tuner_gain_mode(dev, 1);
        if (!result) result = birriasdr_set_gain_codes(dev, &gain);
        if (!result) result = birriasdr_read_sync(dev, iq, count, &received);
        birriasdr_get_stream_stats(dev, &stats);
    }
    int closed = dev ? birriasdr_close(dev) : 0;
    if (!result && received == count && fwrite(iq, 4, count, file) != count) result = 1;
    if (fclose(file)) result = 1;
    free(iq);
    printf("experimental tap=%u bandwidth-class=%u result=%d samples=%u expected=%u state=%u stage=%d reset=%d close=%d\n",
           tap, bandwidth_class, result, received, count, stats.firmware_state, stats.stage, stats.reset_error, closed);
    return result || closed || received != count ? 1 : 0;
}
