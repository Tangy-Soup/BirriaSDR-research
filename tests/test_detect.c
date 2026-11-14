/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/* Test packet-detect suppression against a one-register transport model.
 * Inject errors at every successful-path I/O boundary, a lost register write,
 * and a failed rollback. No USB discovery or physical access is performed.
 */
#include "chips/mt7921.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(t) do { if (!(t)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #t); exit(1); } } while (0)

struct model {
    uint32_t value[3];
    unsigned operations, fail_at, fail_again;
    bool drop_write;
};
/* Booted values measured on the EDUP board: 0x8300011c and 0x8301011c read
 * 0x00f7bcef (field [19:16] = +7), 0x83098200 reads 0x036d45d7 ([22:21] = 3). */
static const uint32_t address[3] = {0x8300011c, 0x8301011c, 0x83098200};
static const uint32_t baseline[3] = {0x00f7bcef, 0x00f7bcef, 0x036d45d7};

static int access_word(void *context, enum bs_space space, uint32_t addr, uint32_t *value, bool write)
{
    struct model *model = context;
    int index = -1;
    for (int i = 0; i < 3; ++i)
        if (addr == address[i]) index = i;
    CHECK(space == BS_MMIO && index >= 0);
    ++model->operations;
    if (model->operations == model->fail_at || model->operations == model->fail_again)
        return BIRRIASDR_ERROR_IO;
    if (write) {
        if (model->drop_write) model->drop_write = false;
        else model->value[index] = *value;
    } else {
        *value = model->value[index];
    }
    return 0;
}

static int read_word(void *context, enum bs_space space, uint32_t addr, uint32_t *value)
{
    return access_word(context, space, addr, value, false);
}

static int write_word(void *context, enum bs_space space, uint32_t addr, uint32_t value)
{
    return access_word(context, space, addr, &value, true);
}

static void initialize(struct model *model, birriasdr_dev_t *dev, bool enabled, unsigned chain,
                       uint64_t frequency)
{
    memset(model, 0, sizeof(*model));
    memset(dev, 0, sizeof(*dev));
    memcpy(model->value, baseline, sizeof(baseline));
    atomic_init(&dev->packet_detect_suppression, enabled ? 1u : 0u);
    atomic_init(&dev->receive_chain, chain);
    atomic_init(&dev->frequency, frequency);
    dev->io = (struct bs_io){.context = model, .read32 = read_word, .write32 = write_word};
}

static bool at_baseline(const struct model *model)
{
    return !memcmp(model->value, baseline, sizeof(baseline));
}

/* Every failure path must leave the hardware as booted or keep state for retry. */
static void test_faults(unsigned chain, uint64_t frequency)
{
    struct model model;
    birriasdr_dev_t dev;
    initialize(&model, &dev, true, chain, frequency);
    CHECK(mt_configure_detect(&dev) == 0);
    unsigned successful_operations = model.operations;
    for (unsigned failure = 1; failure <= successful_operations; ++failure) {
        initialize(&model, &dev, true, chain, frequency);
        model.fail_at = failure;
        CHECK(mt_configure_detect(&dev) == BIRRIASDR_ERROR_IO);
        CHECK(!dev.mt7921.detect_saved && at_baseline(&model));
        CHECK(mt_configure_detect(&dev) == 0); /* Failed preparation is reusable. */
        CHECK(mt_restore_detect(&dev) == 0 && at_baseline(&model));
    }
    /* A write that does not take effect is rejected and rolled back. */
    initialize(&model, &dev, true, chain, frequency);
    model.drop_write = true;
    CHECK(mt_configure_detect(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(!dev.mt7921.detect_saved && at_baseline(&model));
    /* If rollback also fails, keep the saved baselines so reset can retry. */
    initialize(&model, &dev, true, chain, frequency);
    model.fail_at = successful_operations;
    model.fail_again = successful_operations + 1;
    CHECK(mt_configure_detect(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(dev.mt7921.detect_saved);
    CHECK(mt_restore_detect(&dev) == 0 && !dev.mt7921.detect_saved);
    CHECK(at_baseline(&model));
}

int main(void)
{
    struct model model;
    birriasdr_dev_t dev;

    /* Disabled (the default) performs no register access at all. */
    initialize(&model, &dev, false, BIRRIASDR_CHAIN_BOTH, 5745000000u);
    CHECK(mt_configure_detect(&dev) == 0 && !dev.mt7921.detect_saved);
    CHECK(mt_restore_detect(&dev) == 0 && model.operations == 0);

    /* Chain 0, and both chains at 2.4 GHz: only chain 0's field, -8 in [19:16]. */
    const unsigned one_field_chains[2] = {BIRRIASDR_CHAIN_0, BIRRIASDR_CHAIN_BOTH};
    for (unsigned k = 0; k < 2; ++k) {
        initialize(&model, &dev, true, one_field_chains[k], 2437000000u);
        CHECK(mt_configure_detect(&dev) == 0 && dev.mt7921.detect_saved);
        unsigned successful_operations = model.operations;
        CHECK(model.value[0] == 0x00f8bcef && model.value[1] == baseline[1] && model.value[2] == baseline[2]);
        CHECK(mt_configure_detect(&dev) == BIRRIASDR_ERROR_STATE);
        CHECK(model.operations == successful_operations);
        /* Restore the field but preserve unrelated changes made meanwhile. */
        model.value[0] ^= 0x00100001;
        CHECK(mt_restore_detect(&dev) == 0 && !dev.mt7921.detect_saved);
        CHECK(model.value[0] == (baseline[0] ^ 0x00100001));
        test_faults(one_field_chains[k], 2437000000u);
    }

    /* Chain 0 alone at 5 GHz: still one field. */
    initialize(&model, &dev, true, BIRRIASDR_CHAIN_0, 5745000000u);
    CHECK(mt_configure_detect(&dev) == 0);
    CHECK(model.value[0] == 0x00f8bcef && model.value[1] == baseline[1] && model.value[2] == baseline[2]);
    CHECK(mt_restore_detect(&dev) == 0 && at_baseline(&model));

    /* Both chains at 5 GHz: chain 1's copy -8 and its frame detectors [22:21] off. */
    initialize(&model, &dev, true, BIRRIASDR_CHAIN_BOTH, 5745000000u);
    CHECK(mt_configure_detect(&dev) == 0 && dev.mt7921.detect_saved);
    CHECK(model.value[0] == 0x00f8bcef && model.value[1] == 0x00f8bcef && model.value[2] == 0x030d45d7);
    model.value[2] ^= 0x00000100;
    CHECK(mt_restore_detect(&dev) == 0 && !dev.mt7921.detect_saved);
    CHECK(model.value[0] == baseline[0] && model.value[1] == baseline[1]);
    CHECK(model.value[2] == (baseline[2] ^ 0x00000100));
    test_faults(BIRRIASDR_CHAIN_BOTH, 5745000000u);
    puts("Packet-detect suppression configuration, rollback and readback tests passed");
    return 0;
}
