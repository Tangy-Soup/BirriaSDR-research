/* Receive-only research tool. NOT a public API and NOT a capture profile.
 *
 * Page-level bisection of the stock-versus-BirriaSDR PHY register diff
 * (see docs/findings.md, negative results). After the normal
 * prepare (MCU waiting for GO, ICAP stopped) each overlay entry
 *   0xADDRESS 0xMASK 0xVALUE
 * is applied as a masked read/modify/write and read back. Before the normal
 * gain/filter restore and WFSYS reset, every applied entry is restored to its
 * saved bits and read back again. PHY fields can survive WFSYS reset, so the
 * restore must not rely on it. A readback mismatch on apply aborts the capture
 * (restoration still runs); a restoration mismatch is reported loudly and
 * the caller must stop register work. An empty overlay ("-") runs the same
 * path as an interleaved baseline. No transmit features; RF setup unchanged.
 */
#include "internal.h"
#include "chips/mt7921.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define MAX_ENTRIES 1024

static const struct bs_backend *original;
static uint32_t addr[MAX_ENTRIES], mask[MAX_ENTRIES], want[MAX_ENTRIES], saved[MAX_ENTRIES];
static unsigned entries, applied, apply_mismatch, restore_fail, restored;
static uint32_t first_bad_apply, first_bad_restore;
#define MAX_REPORTED 64
static uint32_t bad_addr[MAX_REPORTED], bad_got[MAX_REPORTED];
static uint32_t end_value[MAX_ENTRIES];   /* value just before restore: did the overlay survive the capture? */
static unsigned end_changed;
static bool restore_attempted;

static int prepare_probe(birriasdr_dev_t *dev, uint32_t packets)
{
    BS_TRY(original->prepare(dev, packets));
    for (unsigned i = 0; i < entries; ++i) {
        uint32_t value;
        BS_TRY(bs_read32(dev, BS_MMIO, addr[i], &saved[i]));
        applied = i + 1;
        BS_TRY(bs_write32(dev, BS_MMIO, addr[i], (saved[i] & ~mask[i]) | (want[i] & mask[i])));
        BS_TRY(bs_read32(dev, BS_MMIO, addr[i], &value));
        if ((value ^ want[i]) & mask[i]) {
            if (apply_mismatch < MAX_REPORTED) {
                bad_addr[apply_mismatch] = addr[i];
                bad_got[apply_mismatch] = value;
            }
            if (!apply_mismatch++)
                first_bad_apply = addr[i];
        }
    }
    return apply_mismatch ? BIRRIASDR_ERROR_STATE : 0;
}

static int restore(birriasdr_dev_t *dev)
{
    restore_attempted = true;
    for (unsigned n = applied; n-- > 0;) {
        uint32_t value;
        int result = bs_read32(dev, BS_MMIO, addr[n], &value);
        if (!result) {
            end_value[n] = value;
            if ((value ^ want[n]) & mask[n])
                ++end_changed;
        }
        if (!result)
            result = bs_write32(dev, BS_MMIO, addr[n], (value & ~mask[n]) | (saved[n] & mask[n]));
        if (!result)
            result = bs_read32(dev, BS_MMIO, addr[n], &value);
        if (result || ((value ^ saved[n]) & mask[n])) {
            if (!restore_fail++)
                first_bad_restore = addr[n];
        } else {
            ++restored;
        }
    }
    return restore_fail ? BIRRIASDR_ERROR_STATE : 0;
}

static int reset_probe(birriasdr_dev_t *dev)
{
    int restore_result = restore_attempted ? 0 : restore(dev);
    int reset = original->reset(dev);
    return restore_result ? restore_result : reset;
}

static int load_overlay(const char *path)
{
    if (!strcmp(path, "-"))
        return 0;
    FILE *file = fopen(path, "r");
    if (!file) { perror("Overlay"); return -1; }
    unsigned a, m, v;
    while (fscanf(file, "%x %x %x", &a, &m, &v) == 3) {
        if (entries == MAX_ENTRIES || a % 4 || !m) { fclose(file); return -1; }
        /* Only the PHY blocks covered by the diff; never MCU, MAC or RF/top. */
        if (a < 0x83000000u || a >= 0x830b0000u) { fclose(file); return -1; }
        addr[entries] = a; mask[entries] = m; want[entries++] = v;
    }
    fclose(file);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "Usage: %s new-output.cs16 center-Hz samples overlay.txt|-\n", argv[0]);
        return 2;
    }
    char *end;
    uint64_t frequency = strtoull(argv[2], &end, 10);
    if (*end || frequency < 2156000000ULL || frequency > 2610000000ULL) return 2;
    unsigned long requested = strtoul(argv[3], &end, 10);
    if (*end || requested < 2048 || requested > 40000000 || requested % 2048) return 2;
    uint32_t count = (uint32_t)requested;
    if (load_overlay(argv[4])) return 2;
    FILE *out = fopen(argv[1], "wbx");
    if (!out) { perror("Output"); return 2; }
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
    if (!result && received == count && fwrite(iq, 4, count, out) != count) result = 1;
    if (fclose(out)) result = 1;
    free(iq);
    printf("stream=%s samples=%u/%u state=%u stage=%d reset=%s close=%d entries=%u applied=%u apply_mismatch=%u",
           birriasdr_strerror(result), received, count, stats.firmware_state, stats.stage,
           birriasdr_strerror(stats.reset_error), closed, entries, applied, apply_mismatch);
    if (apply_mismatch) {
        printf(" first_bad_apply=0x%08x mismatches=", first_bad_apply);
        for (unsigned i = 0; i < apply_mismatch && i < MAX_REPORTED; ++i)
            printf("%s0x%08x:0x%08x", i ? "," : "", bad_addr[i], bad_got[i]);
    }
    printf(" restored=%u restore_fail=%u end_changed=%u", restored, restore_fail, end_changed);
    if (applied && !apply_mismatch) {
        printf(" end_values=");
        for (unsigned i = 0; i < applied && i < 8; ++i)
            printf("%s0x%08x:0x%08x", i ? "," : "", addr[i], end_value[i]);
    }
    if (restore_fail) printf(" first_bad_restore=0x%08x RESTORE_FAILED", first_bad_restore);
    printf("\n");
    return result || closed || received != count || apply_mismatch || restore_fail ? 1 : 0;
}
