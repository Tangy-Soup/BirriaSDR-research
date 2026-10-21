/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/* Test receive-chain selection against an independent three-register model:
 * RF SPI port 0 0x1e4 and the two ICAP selection words. Inject errors at every
 * successful-path I/O boundary, a lost register write, and a failed rollback,
 * for chain-1, dual-chain and channel-filter selections. No USB or physical access.
 */
#include "chips/mt7921.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(t) do { if (!(t)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #t); exit(1); } } while (0)

struct model {
    uint32_t value[3];
    unsigned operations, fail_at, fail_again, writes;
    bool drop_write;
};
/* Values after BirriaSDR's bring-up on the EDUP board; the top nibble of
 * 0x830ad448 is seeded to catch writes outside the selected fields.
 */
static const uint32_t baseline[] = {0x000023c1, 0x80000013, 0x508c0400};

static int access_word(struct model *model, unsigned field, uint32_t *value, bool write)
{
    CHECK(field < 3);
    ++model->operations;
    if (model->operations == model->fail_at || model->operations == model->fail_again)
        return BIRRIASDR_ERROR_IO;
    if (write) {
        ++model->writes;
        if (model->drop_write) model->drop_write = false;
        else model->value[field] = *value;
    } else {
        *value = model->value[field];
    }
    return 0;
}

static unsigned mmio_field(enum bs_space space, uint32_t address)
{
    CHECK(space == BS_MMIO && (address == 0x830ad440 || address == 0x830ad448));
    return address == 0x830ad440 ? 1 : 2;
}

static int read_word(void *context, enum bs_space space, uint32_t address, uint32_t *value)
{
    return access_word(context, mmio_field(space, address), value, false);
}

static int write_word(void *context, enum bs_space space, uint32_t address, uint32_t value)
{
    return access_word(context, mmio_field(space, address), &value, true);
}

/* Only WF1's receiver (physical port 0) may be touched; chain 0 is port 1. */
int mt_rf_read(birriasdr_dev_t *dev, unsigned port, uint32_t address, uint32_t *value)
{
    CHECK(port == 0 && address == 0x1e4);
    return access_word(dev->io.context, 0, value, false);
}

int mt_rf_update(birriasdr_dev_t *dev, unsigned port, uint32_t address,
                 uint32_t clear, uint32_t set)
{
    uint32_t value;
    BS_TRY(mt_rf_read(dev, port, address, &value));
    value = (value & ~clear) | set;
    return access_word(dev->io.context, 0, &value, true);
}

static void initialize_filtered(struct model *model, birriasdr_dev_t *dev, unsigned chain,
                                bool filtered)
{
    memset(model, 0, sizeof(*model));
    memset(dev, 0, sizeof(*dev));
    memcpy(model->value, baseline, sizeof(baseline));
    atomic_init(&dev->receive_chain, chain);
    atomic_init(&dev->channel_filter, filtered ? 1u : 0u);
    dev->io = (struct bs_io){model, read_word, write_word};
}

static void initialize(struct model *model, birriasdr_dev_t *dev, unsigned chain)
{
    initialize_filtered(model, dev, chain, false);
}

static bool at_baseline(const struct model *model)
{
    return !memcmp(model->value, baseline, sizeof(baseline));
}

/* Every failure path must leave the hardware as booted or keep state for retry. */
static void test_faults(unsigned chain, bool filtered, unsigned successful_operations)
{
    struct model model;
    birriasdr_dev_t dev;
    for (unsigned failure = 1; failure <= successful_operations; ++failure) {
        initialize_filtered(&model, &dev, chain, filtered);
        model.fail_at = failure;
        CHECK(mt_configure_chain(&dev) == BIRRIASDR_ERROR_IO);
        CHECK(!dev.mt7921.chain_saved && at_baseline(&model));
        CHECK(mt_configure_chain(&dev) == 0); /* Failed preparation is reusable. */
        CHECK(mt_restore_chain(&dev) == 0 && at_baseline(&model));
    }

    /* A write that does not take effect is rejected and rolled back. */
    initialize_filtered(&model, &dev, chain, filtered);
    model.drop_write = true;
    CHECK(mt_configure_chain(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(!dev.mt7921.chain_saved && at_baseline(&model));

    /* If rollback also fails, keep the saved baseline so reset can retry. */
    initialize_filtered(&model, &dev, chain, filtered);
    model.fail_at = successful_operations;
    model.fail_again = successful_operations + 1;
    CHECK(mt_configure_chain(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(dev.mt7921.chain_saved);
    CHECK(mt_restore_chain(&dev) == 0 && !dev.mt7921.chain_saved);
    CHECK(at_baseline(&model));
}

int main(void)
{
    struct model model;
    birriasdr_dev_t dev;
    unsigned successful_operations;

    /* Chain 0 (the default) performs no register access at all. */
    initialize(&model, &dev, BIRRIASDR_CHAIN_0);
    CHECK(mt_configure_chain(&dev) == 0 && !dev.mt7921.chain_saved);
    CHECK(mt_restore_chain(&dev) == 0 && model.operations == 0);

    /* An out-of-range selection is rejected before any access. */
    initialize(&model, &dev, 3);
    CHECK(mt_configure_chain(&dev) == BIRRIASDR_ERROR_INVALID && model.operations == 0);

    /* Chain 1: WF1 receiver on, dense tap 5; the source mode is not touched. */
    initialize(&model, &dev, BIRRIASDR_CHAIN_1);
    CHECK(mt_configure_chain(&dev) == 0 && dev.mt7921.chain_saved);
    successful_operations = model.operations;
    CHECK(model.value[0] == 0x0001fbc3);
    CHECK(model.value[1] == baseline[1]);
    CHECK(model.value[2] == 0x508c0500);
    CHECK(mt_configure_chain(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(model.operations == successful_operations);
    /* Restore the tap field but preserve unrelated changes made meanwhile. */
    model.value[2] ^= 0x01000001;
    CHECK(mt_restore_chain(&dev) == 0 && !dev.mt7921.chain_saved);
    CHECK(model.value[0] == baseline[0] && model.value[1] == baseline[1]);
    CHECK(model.value[2] == (baseline[2] ^ 0x01000001));
    test_faults(BIRRIASDR_CHAIN_1, false, successful_operations);

    /* Both chains: mode 0x11, node 0x0c, bit 21 and [17:16]=3, as measured
     * (readback 0x00a3040c from 0x008c0400 on the EDUP board).
     */
    initialize(&model, &dev, BIRRIASDR_CHAIN_BOTH);
    CHECK(mt_configure_chain(&dev) == 0 && dev.mt7921.chain_saved);
    successful_operations = model.operations;
    CHECK(model.value[0] == 0x0001fbc3);
    CHECK(model.value[1] == 0x80000011);
    CHECK(model.value[2] == 0x50a3040c);
    model.value[1] ^= 0x00000100;
    CHECK(mt_restore_chain(&dev) == 0 && !dev.mt7921.chain_saved);
    CHECK(model.value[0] == baseline[0] && model.value[2] == baseline[2]);
    CHECK(model.value[1] == (baseline[1] ^ 0x00000100));
    test_faults(BIRRIASDR_CHAIN_BOTH, false, successful_operations);

    /* Channel filter on chain 0: only the tap nibble moves, 4 -> 12. */
    initialize_filtered(&model, &dev, BIRRIASDR_CHAIN_0, true);
    CHECK(mt_configure_chain(&dev) == 0 && dev.mt7921.chain_saved);
    successful_operations = model.operations;
    CHECK(model.value[0] == baseline[0] && model.value[1] == baseline[1]);
    CHECK(model.value[2] == 0x508c0c00);
    CHECK(mt_restore_chain(&dev) == 0 && at_baseline(&model));
    test_faults(BIRRIASDR_CHAIN_0, true, successful_operations);

    /* Channel filter on chain 1: WF1 receiver on, tap 13. */
    initialize_filtered(&model, &dev, BIRRIASDR_CHAIN_1, true);
    CHECK(mt_configure_chain(&dev) == 0);
    successful_operations = model.operations;
    CHECK(model.value[0] == 0x0001fbc3 && model.value[1] == baseline[1]);
    CHECK(model.value[2] == 0x508c0d00);
    CHECK(mt_restore_chain(&dev) == 0 && at_baseline(&model));
    test_faults(BIRRIASDR_CHAIN_1, true, successful_operations);

    /* Both chains already read the filtered stage; the filter changes nothing. */
    initialize_filtered(&model, &dev, BIRRIASDR_CHAIN_BOTH, true);
    CHECK(mt_configure_chain(&dev) == 0);
    CHECK(model.value[1] == 0x80000011 && model.value[2] == 0x50a3040c);
    CHECK(mt_restore_chain(&dev) == 0 && at_baseline(&model));
    puts("Receive-chain selection, rollback and readback tests passed");
    return 0;
}
