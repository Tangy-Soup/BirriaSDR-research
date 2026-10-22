/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/* Test bandwidth setup against an independent register transport model: the
 * four 20/40 MHz profile words, plus the BW80 graft's chain-bank RF 0xb00/0xb04
 * and the two latch words. Inject errors at every successful-path I/O
 * boundary, a lost register write, and a failed rollback, for both profiles.
 * No USB discovery or physical access is performed.
 */
#include "chips/mt7921.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(t) do { if (!(t)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #t); exit(1); } } while (0)

/* Words 0-3: RF 0x160 (SPI 0, 1), 0x83081000, 0x83091000. 4-7: RF 0xb00 (SPI
 * 0, 1), RF 0xb04 (SPI 0, 1). 8-9: latch words 0x8300c008, 0x8301c008.
 */
#define WORDS 10
struct model {
    uint32_t value[WORDS];
    unsigned operations, fail_at, fail_again, writes, latch_sets;
    bool drop_write;
};
static const uint32_t baseline[WORDS] = {0x2a123456, 0x17654321, 0xa0abcdef, 0xd0765432,
                                         0x90402000, 0x50412001, 0x0f0f8740, 0xaf2f8701,
                                         0x00000002, 0x00000006};

/* Faults fail once at the requested access; all later cleanup is observable. */
static int access_word(struct model *model, unsigned field, uint32_t *value, bool write)
{
    CHECK(field < WORDS);
    ++model->operations;
    if (model->operations == model->fail_at || model->operations == model->fail_again)
        return BIRRIASDR_ERROR_IO;
    if (write) {
        ++model->writes;
        if (field >= 8 && (*value & 0x80000000u)) ++model->latch_sets;
        if (model->drop_write) model->drop_write = false;
        else model->value[field] = *value;
    } else {
        *value = model->value[field];
    }
    return 0;
}

/* The model exposes exactly the PHY words owned by this module. */
static unsigned mmio_word(enum bs_space space, uint32_t address)
{
    CHECK(space == BS_MMIO);
    switch (address) {
    case 0x83081000: return 2;
    case 0x83091000: return 3;
    case 0x8300c008: return 8;
    case 0x8301c008: return 9;
    }
    CHECK(!"unexpected MMIO address");
    return 0;
}

static int read_word(void *context, enum bs_space space, uint32_t address, uint32_t *value)
{
    return access_word(context, mmio_word(space, address), value, false);
}

static int write_word(void *context, enum bs_space space, uint32_t address, uint32_t value)
{
    return access_word(context, mmio_word(space, address), &value, true);
}

/* Analog access is modeled as a separate register space, retaining bank IDs. */
static unsigned rf_word(unsigned port, uint32_t address)
{
    CHECK(port < 2 && (address == 0x160 || address == 0xb00 || address == 0xb04));
    return address == 0x160 ? port : address == 0xb00 ? 4 + port : 6 + port;
}

int mt_rf_read(birriasdr_dev_t *dev, unsigned port, uint32_t address, uint32_t *value)
{
    return access_word(dev->io.context, rf_word(port, address), value, false);
}

int mt_rf_update(birriasdr_dev_t *dev, unsigned port, uint32_t address,
                 uint32_t clear, uint32_t set)
{
    uint32_t value;
    BS_TRY(mt_rf_read(dev, port, address, &value));
    value = (value & ~clear) | set;
    return access_word(dev->io.context, rf_word(port, address), &value, true);
}

/* Seed unrelated bits differently in both banks to catch accidental clobbers. */
static void initialize(struct model *model, birriasdr_dev_t *dev)
{
    memset(model, 0, sizeof(*model));
    memset(dev, 0, sizeof(*dev));
    memcpy(model->value, baseline, sizeof(baseline));
    dev->rate = 40000000;
    atomic_init(&dev->bandwidth, 40000000);
    dev->io = (struct bs_io){model, read_word, write_word};
}

int main(void)
{
    struct model model;
    birriasdr_dev_t dev;
    initialize(&model, &dev);
    CHECK(mt_restore_filter(&dev) == 0 && model.operations == 0);
    CHECK(mt_configure_filter(&dev) == 0 && dev.mt7921.filter_saved);
    unsigned successful_operations = model.operations;
    CHECK(model.value[0] == 0x6a123456 && model.value[1] == 0x57654321);
    CHECK(model.value[2] == 0xa1abcdef && model.value[3] == 0xd1765432);
    CHECK(mt_configure_filter(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(model.operations == successful_operations);
    /* Preserve an unrelated change made after configuration during cleanup. */
    for (unsigned i = 0; i < 4; ++i) model.value[i] ^= 1;
    CHECK(mt_restore_filter(&dev) == 0 && !dev.mt7921.filter_saved);
    for (unsigned i = 0; i < 4; ++i) CHECK(model.value[i] == (baseline[i] ^ 1));

    for (unsigned failure = 1; failure <= successful_operations; ++failure) {
        initialize(&model, &dev);
        model.fail_at = failure;
        CHECK(mt_configure_filter(&dev) == BIRRIASDR_ERROR_IO);
        CHECK(!dev.mt7921.filter_saved);
        CHECK(memcmp(model.value, baseline, sizeof(baseline)) == 0);
        CHECK(mt_configure_filter(&dev) == 0); /* Failed preparation is reusable. */
        CHECK(mt_restore_filter(&dev) == 0);
    }
    initialize(&model, &dev);
    model.drop_write = true;
    CHECK(mt_configure_filter(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(!dev.mt7921.filter_saved && !memcmp(model.value, baseline, sizeof(baseline)));

    initialize(&model, &dev);
    model.fail_at = successful_operations;
    model.fail_again = successful_operations + 1;
    CHECK(mt_configure_filter(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(dev.mt7921.filter_saved);
    CHECK(mt_restore_filter(&dev) == 0 && !dev.mt7921.filter_saved);
    CHECK(!memcmp(model.value, baseline, sizeof(baseline)));

    initialize(&model, &dev);
    atomic_store(&dev.bandwidth, 20000000);
    /* Seed class-one fields so both downward transitions must actually write. */
    model.value[0] |= 0x40000000;
    model.value[1] |= 0x40000000;
    model.value[2] |= 0x01000000;
    model.value[3] |= 0x01000000;
    uint32_t narrow_baseline[4];
    memcpy(narrow_baseline, model.value, sizeof(narrow_baseline));
    CHECK(mt_configure_filter(&dev) == 0);
    CHECK(!(model.value[0] & 0xc0000000) && !(model.value[1] & 0xc0000000));
    CHECK(!(model.value[2] & 0x0f000000) && !(model.value[3] & 0x0f000000));
    CHECK(mt_restore_filter(&dev) == 0);
    CHECK(!memcmp(model.value, narrow_baseline, sizeof(narrow_baseline)));

    initialize(&model, &dev);
    atomic_store(&dev.bandwidth, 30000000);
    CHECK(mt_configure_filter(&dev) == BIRRIASDR_ERROR_UNSUPPORTED && !model.operations);

    initialize(&model, &dev);
    dev.rate = 20000000;
    CHECK(mt_configure_filter(&dev) == BIRRIASDR_ERROR_UNSUPPORTED && !model.operations);

    /* 80 MSps: the BW80 graft, whatever the 20/40 setting, latched once. */
    initialize(&model, &dev);
    dev.rate = 80000000;
    atomic_store(&dev.bandwidth, 20000000);
    CHECK(mt_configure_filter(&dev) == 0 && dev.mt7921.filter_saved && dev.mt7921.filter_wide);
    successful_operations = model.operations;
    CHECK(model.value[0] == 0xaa123456 && model.value[1] == 0x97654321);
    CHECK(model.value[2] == 0xa2abcdef && model.value[3] == 0xd2765432);
    CHECK(model.value[4] == 0xa0402000 && model.value[5] == 0x60412001);
    CHECK(model.value[6] == 0x1f0f8780 && model.value[7] == 0xbf2f8781);
    CHECK(model.value[8] == baseline[8] && model.value[9] == baseline[9] && model.latch_sets == 2);
    CHECK(mt_restore_filter(&dev) == 0 && !dev.mt7921.filter_saved && !dev.mt7921.filter_wide);
    CHECK(!memcmp(model.value, baseline, sizeof(baseline)) && model.latch_sets == 4);
    for (unsigned failure = 1; failure <= successful_operations; ++failure) {
        initialize(&model, &dev);
        dev.rate = 80000000;
        model.fail_at = failure;
        CHECK(mt_configure_filter(&dev) == BIRRIASDR_ERROR_IO);
        CHECK(!dev.mt7921.filter_saved);
        CHECK(!memcmp(model.value, baseline, sizeof(baseline)));
    }
    initialize(&model, &dev);
    dev.rate = 80000000;
    model.drop_write = true;
    CHECK(mt_configure_filter(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(!dev.mt7921.filter_saved && !memcmp(model.value, baseline, sizeof(baseline)));
    initialize(&model, &dev);
    dev.rate = 80000000;
    model.fail_at = successful_operations;
    model.fail_again = successful_operations + 1;
    CHECK(mt_configure_filter(&dev) == BIRRIASDR_ERROR_STATE && dev.mt7921.filter_saved);
    CHECK(mt_restore_filter(&dev) == 0 && !memcmp(model.value, baseline, sizeof(baseline)));
    puts("Filter configuration, preservation, rollback and readback tests passed");
    return 0;
}
