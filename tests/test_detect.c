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
    uint32_t value;
    unsigned operations, fail_at, fail_again;
    bool drop_write;
};
/* Booted value measured on the EDUP board: field 0x8300011c[19:16] is +7. */
static const uint32_t baseline = 0x00f7bcef;

static int access_word(void *context, enum bs_space space, uint32_t address, uint32_t *value, bool write)
{
    struct model *model = context;
    CHECK(space == BS_MMIO && address == 0x8300011c);
    ++model->operations;
    if (model->operations == model->fail_at || model->operations == model->fail_again)
        return BIRRIASDR_ERROR_IO;
    if (write) {
        if (model->drop_write) model->drop_write = false;
        else model->value = *value;
    } else {
        *value = model->value;
    }
    return 0;
}

static int read_word(void *context, enum bs_space space, uint32_t address, uint32_t *value)
{
    return access_word(context, space, address, value, false);
}

static int write_word(void *context, enum bs_space space, uint32_t address, uint32_t value)
{
    return access_word(context, space, address, &value, true);
}

static void initialize(struct model *model, birriasdr_dev_t *dev, bool enabled)
{
    memset(model, 0, sizeof(*model));
    memset(dev, 0, sizeof(*dev));
    model->value = baseline;
    atomic_init(&dev->packet_detect_suppression, enabled ? 1u : 0u);
    dev->io = (struct bs_io){model, read_word, write_word};
}

int main(void)
{
    struct model model;
    birriasdr_dev_t dev;

    /* Disabled (the default) performs no register access at all. */
    initialize(&model, &dev, false);
    CHECK(mt_configure_detect(&dev) == 0 && !dev.mt7921.detect_saved);
    CHECK(mt_restore_detect(&dev) == 0 && model.operations == 0);

    /* Enabled writes -8 into bits 19:16 only, then verifies by readback. */
    initialize(&model, &dev, true);
    CHECK(mt_configure_detect(&dev) == 0 && dev.mt7921.detect_saved);
    unsigned successful_operations = model.operations;
    CHECK(model.value == 0x00f8bcef);
    CHECK(mt_configure_detect(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(model.operations == successful_operations);
    /* Restore the field but preserve unrelated changes made meanwhile. */
    model.value ^= 0x00100001;
    CHECK(mt_restore_detect(&dev) == 0 && !dev.mt7921.detect_saved);
    CHECK(model.value == (baseline ^ 0x00100001));

    for (unsigned failure = 1; failure <= successful_operations; ++failure) {
        initialize(&model, &dev, true);
        model.fail_at = failure;
        CHECK(mt_configure_detect(&dev) == BIRRIASDR_ERROR_IO);
        CHECK(!dev.mt7921.detect_saved && model.value == baseline);
        CHECK(mt_configure_detect(&dev) == 0); /* Failed preparation is reusable. */
        CHECK(mt_restore_detect(&dev) == 0 && model.value == baseline);
    }

    /* A write that does not take effect is rejected and rolled back. */
    initialize(&model, &dev, true);
    model.drop_write = true;
    CHECK(mt_configure_detect(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(!dev.mt7921.detect_saved && model.value == baseline);

    /* If rollback also fails, keep the saved baseline so reset can retry. */
    initialize(&model, &dev, true);
    model.fail_at = successful_operations;
    model.fail_again = successful_operations + 1;
    CHECK(mt_configure_detect(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(dev.mt7921.detect_saved);
    CHECK(mt_restore_detect(&dev) == 0 && !dev.mt7921.detect_saved);
    CHECK(model.value == baseline);
    puts("Packet-detect suppression configuration, rollback and readback tests passed");
    return 0;
}
