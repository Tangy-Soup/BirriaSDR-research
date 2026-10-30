/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */
#include "chips/mt7921.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)
struct word { unsigned space; uint32_t address, value; };
struct model {
    struct word values[2048], writes[2048];
    size_t count, written;
    uint32_t command[2];
    bool stuck_standby;
    uint32_t fail_address;
    bool fail_after_write;
    /* Bridge traffic: busy polls, command-word reads, launched SPI accesses. */
    unsigned polls, command_reads, accesses;
};

/* A sparse map models register readbacks, not the production implementation. */
static uint32_t get(struct model *model, unsigned space, uint32_t address)
{
    for (size_t i = 0; i < model->count; ++i)
        if (model->values[i].space == space && model->values[i].address == address)
            return model->values[i].value;
    return 0;
}

/* Store fake state separately from the write trace, including self-clearing LOAD. */
static void put(struct model *model, unsigned space, uint32_t address, uint32_t value)
{
    for (size_t i = 0; i < model->count; ++i) {
        if (model->values[i].space == space && model->values[i].address == address) {
            model->values[i].value = value;
            return;
        }
    }
    CHECK(model->count < 2048);
    model->values[model->count++] = (struct word){space, address, value};
}

/* Decode the bridge result register into a physical bank read. */
static int read32(void *context, enum bs_space space, uint32_t address, uint32_t *value)
{
    struct model *model = context;
    if (space == BS_BRIDGE) {
        uint32_t command = model->command[address >= 0x7c004050];
        model->polls += address == 0x7c004000;
        model->command_reads += address == 0x7c004010 || address == 0x7c004050;
        *value = address == 0x7c004000 ? 0 :
            address == 0x7c004018 || address == 0x7c004058 ?
            get(model, 16 + ((command >> 13) & 7), command & 0xfff) : command;
    } else {
        *value = get(model, (unsigned)space, address);
    }
    return 0;
}

/* Model the SPI command/data launch and record the actual RF write once. This
 * lets a trace from Python compare against C despite their different transports.
 */
static int write32(void *context, enum bs_space space, uint32_t address, uint32_t value)
{
    struct model *model = context;
    unsigned target = (unsigned)space;
    bool fail;
    if (space == BS_BRIDGE) {
        unsigned index = address >= 0x7c004050;
        uint32_t command;
        if (address == 0x7c004010 || address == 0x7c004050) {
            model->command[index] = value;
            return 0;
        }
        command = model->command[index];
        ++model->accesses;
        if (command & 0x1000)
            return 0;
        target = 16 + ((command >> 13) & 7);
        address = command & 0xfff;
    }
    fail = address == model->fail_address && model->fail_address;
    if (fail) {
        model->fail_address = 0;
        if (!model->fail_after_write)
            return BIRRIASDR_ERROR_IO;
    }
    CHECK(model->written < 2048);
    model->writes[model->written++] = (struct word){target, address, value};
    if (target >= 16 && (address == 0xc68 || address == 0xca8))
        value &= 0x7fffffff;
    if (target == 21 && address == 0xb00 && model->stuck_standby)
        value |= 0x10000;
    put(model, target, address, value);
    return fail ? BIRRIASDR_ERROR_IO : 0;
}

/* Seed only independently specified identity, strap, completion and trim values. */
static void initialize(struct model *model, birriasdr_dev_t *dev)
{
    memset(model, 0, sizeof(*model));
    memset(dev, 0, sizeof(*dev));
    dev->io = (struct bs_io){model, read32, write32};
    dev->frequency = UINT64_C(2437000000);
    dev->rate = 40000000;
    dev->manual = true;
    dev->gain = (struct birriasdr_gain){2, 8, 0};
    put(model, 0, 0x02004600, 40000000);
    put(model, 0, 0x70000148, 0x80);
    put(model, 0, 0x70011094, 4);
    put(model, 21, 0x2c, 0x79728a10);
    put(model, 21, 0xb00, 0x28);
    for (unsigned p = 16; p < 18; ++p) {
        put(model, p, 0xd7c, 1u << 21);
        put(model, p, 0xd78, 0x1b);
    }
}

/* Compare bootstrap write/order against the established Python receiver. This
 * catches missing wake steps, bank swaps, constants and ordering regressions.
 * The later matched-filter setup is covered independently by test_filter.
 */
static void test_trace(const char *path)
{
    struct model model;
    birriasdr_dev_t dev;
    char line[200];
    size_t index = 0;
    FILE *file = fopen(path, "r");
    CHECK(file);
    initialize(&model, &dev);
    CHECK(mt_configure_capture(&dev) == 0);
    CHECK(mt_configure_rf(&dev) == 0);
    CHECK(mt_apply_gain(&dev) == 0);
    while (fgets(line, sizeof(line), file)) {
        unsigned space, address, value;
        if (line[0] == '#') continue;
        CHECK(sscanf(line, "%x %x %x", &space, &address, &value) == 3);
        CHECK(index < model.written);
        if (model.writes[index].space != space || model.writes[index].address != address ||
            model.writes[index].value != value) {
            fprintf(stderr, "Write %zu: Python %x %08x %08x, C %x %08x %08x\n", index,
                     space, address, value, model.writes[index].space,
                     model.writes[index].address, model.writes[index].value);
            exit(1);
        }
        ++index;
    }
    fclose(file);
    CHECK(index == model.written);
    CHECK(get(&model, 21, 0xb00) == 0x28);
    CHECK(get(&model, 16, 0x1e4) == 0x23c1);
    CHECK(get(&model, 17, 0x1e4) == 0x3fb0b);
    printf("%zu ordered writes match the Python receiver\n", index);
}

/* Inject identity, wake-readback and gain-write failures. Failure must propagate;
 * a partial manual-gain update must restore only the previous owned fields.
 */
static void test_failures(void)
{
    struct model model;
    birriasdr_dev_t dev;
    initialize(&model, &dev);
    put(&model, 21, 0x2c, 0);
    CHECK(mt_configure_rf(&dev) == BIRRIASDR_ERROR_UNSUPPORTED && model.written == 0);
    initialize(&model, &dev);
    model.stuck_standby = true;
    CHECK(mt_configure_rf(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(get(&model, 16, 0xca8) == 0);
    initialize(&model, &dev);
    put(&model, 0, 0x830000c4, 0x08080808);
    put(&model, 17, 0x768, 0x00551234);
    model.fail_address = 0x83000150;
    CHECK(mt_apply_gain(&dev) == BIRRIASDR_ERROR_IO);
    CHECK(get(&model, 0, 0x830000c4) == 0x08080808);
    CHECK(get(&model, 17, 0x768) == 0x00551234);
    CHECK(get(&model, 0, 0x83000108) == 0);
    initialize(&model, &dev);
    put(&model, 0, 0x830000c4, 0x08080808);
    put(&model, 17, 0x768, 0x00551234);
    CHECK(mt_apply_gain(&dev) == 0 && dev.mt7921.gain_saved);
    CHECK(get(&model, 17, 0x768) & 0x80000000);
    CHECK(mt_restore_gain(&dev) == 0 && !dev.mt7921.gain_saved);
    CHECK(get(&model, 0, 0x830000c4) == 0x08080808);
    CHECK(get(&model, 17, 0x768) == 0x00551234);
    CHECK(mt_apply_gain(&dev) == 0); /* A fresh unforced baseline is reusable. */
    model.fail_address = 0x83000108;
    CHECK(mt_restore_gain(&dev) == BIRRIASDR_ERROR_IO && dev.mt7921.gain_saved);
    CHECK(mt_restore_gain(&dev) == 0 && !dev.mt7921.gain_saved);
}

/* Repeated updates retain the boot baseline, preserve unowned fields and roll
 * back to the preceding manual setting when a later register write fails.
 */
static void test_gain_updates(void)
{
    struct model model;
    birriasdr_dev_t dev;
    struct birriasdr_gain gain = {1, 4, 6};
    uint32_t previous[4];
    initialize(&model, &dev);
    dev.manual = false;
    put(&model, 0, 0x830000c4, 0x08080808);
    put(&model, 17, 0x768, 0x00551234);
    CHECK(mt_change_gain(&dev, true, &gain) == BIRRIASDR_ERROR_STATE);
    CHECK(mt_apply_gain(&dev) == 0 && dev.mt7921.gain_saved);
    CHECK(model.written == 0);
    CHECK(mt_change_gain(&dev, true, &gain) == 0);
    CHECK(get(&model, 0, 0x830000c4) == 0x4949490f);
    CHECK(get(&model, 0, 0x83000108) == 0x06060000);
    CHECK(get(&model, 17, 0x768) == 0x8a551234);
    previous[0] = get(&model, 0, 0x830000c4);
    previous[1] = get(&model, 0, 0x83000108);
    previous[2] = get(&model, 0, 0x83000150);
    previous[3] = get(&model, 17, 0x768);
    gain = (struct birriasdr_gain){2, 8, 0};
    model.fail_address = 0x83000150;
    CHECK(mt_change_gain(&dev, true, &gain) == BIRRIASDR_ERROR_IO);
    CHECK(get(&model, 0, 0x830000c4) == previous[0]);
    CHECK(get(&model, 0, 0x83000108) == previous[1]);
    CHECK(get(&model, 0, 0x83000150) == previous[2]);
    CHECK(get(&model, 17, 0x768) == previous[3]);
    CHECK(mt_change_gain(&dev, true, &gain) == 0);
    CHECK(mt_change_gain(&dev, false, &gain) == 0 && dev.mt7921.gain_saved);
    CHECK(get(&model, 0, 0x830000c4) == 0x08080808);
    CHECK(get(&model, 17, 0x768) == 0x00551234);
    CHECK(mt_change_gain(&dev, true, &gain) == 0);
    CHECK(mt_restore_gain(&dev) == 0 && !dev.mt7921.gain_saved);
    CHECK(get(&model, 17, 0x768) == 0x00551234);
    gain.digital = 13;
    CHECK(mt_change_gain(&dev, true, &gain) == BIRRIASDR_ERROR_INVALID);
}

/* Manual gain forces exactly the streamed chains: chain 1's PHY words sit
 * 0x10000 above chain 0's and its analog word is RF 0x768 on SPI 0. Release and
 * restore return every saved chain, and a forced chain-1 baseline is rejected.
 */
static void test_chain_gain(void)
{
    struct model model;
    birriasdr_dev_t dev;
    struct birriasdr_gain gain = {5, 7, 0};
    initialize(&model, &dev);
    atomic_store(&dev.receive_chain, BIRRIASDR_CHAIN_1);
    put(&model, 0, 0x830000c4, 0x08080808);
    put(&model, 0, 0x830100c4, 0x08080808);
    put(&model, 16, 0x768, 0x00551234);
    put(&model, 17, 0x768, 0x00661234);
    CHECK(mt_apply_gain(&dev) == 0 && dev.mt7921.gain_chains == 2);
    CHECK(get(&model, 0, 0x830100c4) == 0x8a8a8a0f);   /* LNA 2, LPF 8, forces A/B/C */
    CHECK(get(&model, 0, 0x83010150) == 0x80200000);
    CHECK(get(&model, 16, 0x768) == 0x94551234);
    CHECK(get(&model, 0, 0x830000c4) == 0x08080808);   /* chain 0 untouched */
    CHECK(get(&model, 17, 0x768) == 0x00661234);
    CHECK(get(&model, 0, 0x83000150) == 0);
    CHECK(mt_change_gain(&dev, true, &gain) == 0);
    CHECK(get(&model, 0, 0x830100c4) == 0x7d7d7d0f && get(&model, 16, 0x768) == 0xabd51234);
    CHECK(mt_change_gain(&dev, false, &gain) == 0);
    CHECK(get(&model, 0, 0x830100c4) == 0x08080808 && get(&model, 16, 0x768) == 0x00551234);
    CHECK(mt_restore_gain(&dev) == 0 && !dev.mt7921.gain_saved && !dev.mt7921.gain_chains);

    initialize(&model, &dev);
    atomic_store(&dev.receive_chain, BIRRIASDR_CHAIN_BOTH);
    put(&model, 16, 0x768, 0x00551234);
    put(&model, 17, 0x768, 0x00661234);
    CHECK(mt_apply_gain(&dev) == 0 && dev.mt7921.gain_chains == 3);
    CHECK(get(&model, 0, 0x830000c4) == 0x82828207 && get(&model, 0, 0x830100c4) == 0x82828207);
    CHECK(get(&model, 16, 0x768) == 0x94551234 && get(&model, 17, 0x768) == 0x94661234);
    CHECK(mt_change_gain(&dev, true, &gain) == 0);
    CHECK(get(&model, 0, 0x830000c4) == 0x75757507 && get(&model, 0, 0x830100c4) == 0x75757507);
    model.fail_address = 0x83010108;   /* chain 1's restore fails after chain 0's */
    CHECK(mt_restore_gain(&dev) == BIRRIASDR_ERROR_IO && dev.mt7921.gain_saved);
    CHECK(get(&model, 0, 0x830000c4) == 0 && get(&model, 17, 0x768) == 0x00661234);
    model.fail_address = 0;
    CHECK(mt_restore_gain(&dev) == 0 && !dev.mt7921.gain_saved);
    CHECK(get(&model, 0, 0x830100c4) == 0 && get(&model, 16, 0x768) == 0x00551234);

    initialize(&model, &dev);
    atomic_store(&dev.receive_chain, BIRRIASDR_CHAIN_1);
    put(&model, 16, 0x768, 0x80000000);   /* left forced by an unclean exit */
    CHECK(mt_apply_gain(&dev) == BIRRIASDR_ERROR_STATE && !dev.mt7921.gain_saved);
    CHECK(model.written == 0);
    atomic_store(&dev.receive_chain, BIRRIASDR_CHAIN_0);   /* chain 0 does not check chain 1 */
    CHECK(mt_apply_gain(&dev) == 0 && dev.mt7921.gain_chains == 1);
}

/* The overall-gain steps are ascending for every chain selection, start at the
 * lowest code, and leave digital out of both-chain capture. Chain 0 at 5 GHz
 * extends the LPF steps past 8 in measured-gain order; other combinations keep
 * the 2.4 GHz layout.
 */
static void test_gain_steps(void)
{
    const uint64_t g24 = MT_RF_ANCHOR_HZ, g5 = UINT64_C(5240000000);
    struct bs_gain_step steps[64];
    struct birriasdr_gain gain = {0, 0, 0}, maximum;
    int tenths;
    for (unsigned chain = BIRRIASDR_CHAIN_0; chain <= BIRRIASDR_CHAIN_BOTH; ++chain) {
        int count = mt_gain_steps(chain, g24, steps, 32);
        CHECK(count == (chain == BIRRIASDR_CHAIN_BOTH ? 14 : 26));
        CHECK(steps[0].tenths_db == 0 && !steps[0].gain.lna && !steps[0].gain.lpf);
        for (int i = 1; i < count; ++i) {
            CHECK(steps[i].tenths_db > steps[i - 1].tenths_db);
            CHECK(mt_gain_tenths_db(chain, g24, &steps[i].gain, &tenths) == 0 && tenths == steps[i].tenths_db);
        }
        CHECK(steps[5].gain.lna == 5 && !steps[5].gain.lpf && steps[13].gain.lpf == 8);
        CHECK(chain == BIRRIASDR_CHAIN_BOTH || steps[25].gain.digital == 12);
        /* Chain 1 and both-chain capture have no 5 GHz codes above 8. */
        CHECK(chain == BIRRIASDR_CHAIN_0 || mt_gain_steps(chain, g5, NULL, 0) == count);
    }
    CHECK(mt_gain_steps(BIRRIASDR_CHAIN_0, g24, NULL, 0) == 26);
    CHECK(mt_gain_steps(3, g24, steps, 32) == BIRRIASDR_ERROR_INVALID);
    gain.lna = 6;
    CHECK(mt_gain_tenths_db(BIRRIASDR_CHAIN_0, g24, &gain, &tenths) == BIRRIASDR_ERROR_INVALID);

    /* Chain 0 at 5 GHz: ascending, each LPF code at most once, the top LPF
     * step the code with the most measured gain, digital last.
     */
    {
        int count = mt_gain_steps(BIRRIASDR_CHAIN_0, g5, steps, 64), best = -1, top_lpf = -1;
        bool seen[MT_GAIN_LPF_MAX_5G + 1] = {false};
        CHECK(count > 18 && count <= 64);
        CHECK(steps[0].tenths_db == 0 && steps[5].gain.lna == 5 && !steps[5].gain.lpf);
        for (int i = 1; i < count; ++i) {
            CHECK(steps[i].tenths_db > steps[i - 1].tenths_db);
            CHECK(mt_gain_tenths_db(BIRRIASDR_CHAIN_0, g5, &steps[i].gain, &tenths) == 0 &&
                  tenths == steps[i].tenths_db);
            if (i > 5 && !steps[i].gain.digital) {
                CHECK(!seen[steps[i].gain.lpf]);
                seen[steps[i].gain.lpf] = true;
                top_lpf = steps[i].gain.lpf;
            }
        }
        for (unsigned code = 0; code <= MT_GAIN_LPF_MAX_5G; ++code) {
            struct birriasdr_gain g = {5, (uint8_t)code, 0};
            CHECK(mt_gain_tenths_db(BIRRIASDR_CHAIN_0, g5, &g, &tenths) == 0);
            if (best < 0 || tenths > best)
                best = tenths;
        }
        gain = (struct birriasdr_gain){5, (uint8_t)top_lpf, 0};
        /* The top step may be any code within the 0.5 dB step of the best one. */
        CHECK(top_lpf > 8 && mt_gain_tenths_db(BIRRIASDR_CHAIN_0, g5, &gain, &tenths) == 0 && tenths > best - 5);
        CHECK(steps[count - 1].gain.lpf == top_lpf && steps[count - 1].gain.digital == 12);
    }

    /* Validation: LPF 9..15 only for chain 0 at 5 GHz. */
    for (unsigned chain = BIRRIASDR_CHAIN_0; chain <= BIRRIASDR_CHAIN_BOTH; ++chain)
        for (unsigned band = 0; band < 2; ++band) {
            uint64_t f = band ? g5 : g24;
            bool extended = band && chain == BIRRIASDR_CHAIN_0;
            CHECK(mt_gain_limits(f, chain, &maximum) == 0 && maximum.lpf == (extended ? 15 : 8));
            for (unsigned code = 8; code <= 16; ++code) {
                struct birriasdr_gain g = {5, (uint8_t)code, 0};
                int expect = code <= 8 || (extended && code <= 15) ? 0 : BIRRIASDR_ERROR_INVALID;
                CHECK(mt_validate_gain(f, chain, &g) == expect);
                CHECK(bs_mt7921_backend.validate(f, MT_RATE_40, chain, &g) == expect);
            }
        }
    CHECK(mt_gain_limits(g5, 3, &maximum) == BIRRIASDR_ERROR_INVALID);
    CHECK(mt_validate_gain(g5, BIRRIASDR_CHAIN_0, NULL) == BIRRIASDR_ERROR_INVALID);
}

/* A parked retune must target SPI 0's secondary oscillator, leave the other
 * oscillator untouched, and restore routing on both success and failure.
 * An active capture/GO must reject the operation without any RF writes.
 */
static void test_retune(void)
{
    struct model model;
    birriasdr_dev_t dev;
    size_t before;
    initialize(&model, &dev);
    atomic_init(&dev.run_state, BS_IDLE);
    put(&model, 0, MT_MAILBOX + 28, 1);
    put(&model, 16, 0x240, 0x00123456);
    put(&model, 16, 0xc68, 0x06f00000);
    CHECK(mt_retune_2g_probe(&dev, UINT64_C(2400000114), 18) == 0);
    CHECK(get(&model, 16, 0xca8) == 0x05000004);
    CHECK(get(&model, 16, 0xc68) == 0x06f00000);
    CHECK(get(&model, 16, 0x240) == 0x00123456);
    CHECK(get(&model, 17, 0xca8) == 0);
    CHECK(dev.frequency == UINT64_C(2437000000));
    model.fail_address = 0xce0;
    CHECK(mt_retune_2g_probe(&dev, UINT64_C(2400000114), 18) == BIRRIASDR_ERROR_IO);
    CHECK(get(&model, 16, 0x240) == 0x00123456);
    CHECK((get(&model, 16, 0xcc4) & 0x60000000) == 0);
    CHECK((get(&model, 16, 0xcc8) & 1) == 0);
    CHECK(get(&model, 16, 0xce0) == 0);
    CHECK((get(&model, 16, 0xd20) & 4) != 0);
    before = model.written;
    put(&model, 0, MT_MAILBOX + 60, 1);
    CHECK(mt_retune_2g_probe(&dev, UINT64_C(2437000000), 18) == BIRRIASDR_ERROR_STATE);
    CHECK(model.written == before);
    put(&model, 0, MT_MAILBOX + 60, 0);
    put(&model, 0, 0x80021090, 0x4f3);
    CHECK(mt_retune_2g_probe(&dev, UINT64_C(2437000000), 18) == BIRRIASDR_ERROR_STATE);
    CHECK(model.written == before);
    put(&model, 0, 0x80021090, 0x4f1);
    atomic_store(&dev.run_state, BS_RUNNING);
    CHECK(mt_retune_2g_probe(&dev, UINT64_C(2437000000), 18) == BIRRIASDR_ERROR_BUSY);
    CHECK(model.written == before);
    atomic_store(&dev.run_state, BS_IDLE);
    put(&model, 16, 0xd7c, 0);
    CHECK(mt_retune_2g_probe(&dev, UINT64_C(2437000000), 18) == BIRRIASDR_ERROR_TIMEOUT);
    CHECK(get(&model, 16, 0x240) == 0x00123456);
    CHECK((get(&model, 16, 0xcc4) & 0x60000000) == 0);
    CHECK((get(&model, 16, 0xcc8) & 1) == 0);
    CHECK(get(&model, 16, 0xce0) == 0);
}

/* Startup must preserve the companion setup before moving the selected LO.
 * Live retuning must leave capture/GO and gain untouched, restore routing and
 * roll back a failed calibration to the previous center without an API commit.
 */
static void test_public_tuning(void)
{
    struct model model;
    birriasdr_dev_t dev;
    size_t before;
    initialize(&model, &dev);
    dev.frequency = UINT64_C(2300000000);
    CHECK(mt_configure_rf(&dev) == 0);
    CHECK(get(&model, 16, 0xca8) == 0x04caaaab);
    CHECK(get(&model, 16, 0xc68) == 0x06f00000);
    CHECK(get(&model, 17, 0xc68) == 0x0513bbbc);
    CHECK(get(&model, 16, 0x240) == 0);
    put(&model, 0, MT_MAILBOX + 60, 1);
    put(&model, 0, 0x80021090, 0x24f3);
    put(&model, 17, 0x768, 0x94123456);
    before = model.written;
    CHECK(mt_change_frequency(&dev, UINT64_C(2437123456)) == 0);
    CHECK(get(&model, 16, 0xca8) == 0x0513cc97);
    CHECK(get(&model, 0, MT_MAILBOX + 60) == 1);
    CHECK(get(&model, 0, 0x80021090) == 0x24f3);
    CHECK(get(&model, 17, 0x768) == 0x94123456);
    for (size_t i = before; i < model.written; ++i)
        CHECK(model.writes[i].space == 16); /* Only the selected analog bank. */
    dev.frequency = UINT64_C(2437123456); /* Simulate the API's successful commit. */
    model.fail_address = 0xce0;
    CHECK(mt_change_frequency(&dev, MT_RF_MIN_HZ) == BIRRIASDR_ERROR_IO);
    CHECK(get(&model, 16, 0xca8) == 0x0513cc97);
    CHECK(dev.frequency == UINT64_C(2437123456));
    CHECK(get(&model, 16, 0x240) == 0);
    CHECK((get(&model, 16, 0xcc4) & 0x60000000) == 0);
    before = model.written;
    CHECK(mt_change_frequency(&dev, MT_RF_MAX_HZ + 1) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(model.written == before);
    put(&model, 16, 0xd74, 0xfffff400);
    CHECK(mt_set_frequency(&dev, MT_RF_MIN_HZ) == BIRRIASDR_ERROR_STATE);
    put(&model, 16, 0xd74, 0x00000400);
    CHECK(mt_set_frequency(&dev, MT_RF_MAX_HZ) == BIRRIASDR_ERROR_STATE);
}

/* Crossing bands moves the VCO range, LO path and front end together, and the
 * way back restores 2.4 GHz. Retunes within a band write no band fields.
 */
static void test_band_switch(void)
{
    struct model model;
    birriasdr_dev_t dev;
    size_t before;
    initialize(&model, &dev);
    dev.frequency = UINT64_C(5735000000);
    CHECK(mt_configure_rf(&dev) == 0);
    CHECK(get(&model, 16, 0xca8) == 0x072b3333);
    CHECK(get(&model, 16, 0xcac) == 27526);
    CHECK(get(&model, 16, 0x160) & (1u << 29));
    CHECK(get(&model, 17, 0x160) & (1u << 29));
    CHECK((get(&model, 16, 0xc00) & 0x3c) == 0x38);
    before = model.written;
    CHECK(mt_change_frequency(&dev, UINT64_C(5815000000)) == 0);
    for (size_t i = before; i < model.written; ++i)
        CHECK(model.writes[i].space == 16 && model.writes[i].address != 0xc00);
    CHECK(mt_change_frequency(&dev, UINT64_C(2437000000)) == 0);
    CHECK(get(&model, 16, 0xca8) == 0x0513bbbc);
    CHECK(!(get(&model, 16, 0x160) & (1u << 29)));
    CHECK(!(get(&model, 17, 0x160) & (1u << 29)));
    CHECK((get(&model, 16, 0xc00) & 0x3c) == 0x28);
    CHECK(mt_change_frequency(&dev, UINT64_C(3000000000)) == BIRRIASDR_ERROR_UNSUPPORTED);
}

/* A failed band write may already have reached the radio. In either direction,
 * rollback must restore both band fields, the VCO and the committed center.
 */
static void test_band_switch_failures(void)
{
    static const uint64_t frequencies[] = {MT_RF_ANCHOR_HZ, UINT64_C(5735000000)};
    static const uint32_t addresses[] = {0xc00, 0x160};
    for (unsigned band = 0; band < 2; ++band) {
        for (unsigned field = 0; field < 2; ++field) {
            for (unsigned applied = 0; applied < 2; ++applied) {
                struct model model;
                birriasdr_dev_t dev;
                uint32_t lo, synth, receiver, word, cycles;
                initialize(&model, &dev);
                dev.backend = &bs_mt7921_backend;
                dev.frequency = frequencies[band];
                atomic_init(&dev.run_state, BS_IDLE);
                CHECK(pthread_mutex_init(&dev.control_mutex, NULL) == 0);
                CHECK(mt_configure_rf(&dev) == 0);
                lo = get(&model, 16, 0xc00);
                synth = get(&model, 16, 0x160);
                receiver = get(&model, 17, 0x160);
                word = get(&model, 16, 0xca8);
                cycles = get(&model, 16, 0xcac);
                atomic_store(&dev.run_state, BS_RUNNING);
                dev.control_ready = true;
                model.fail_address = addresses[field];
                model.fail_after_write = applied != 0;
                CHECK(birriasdr_set_center_freq(&dev, frequencies[1 - band]) == BIRRIASDR_ERROR_IO);
                CHECK(model.fail_address == 0);
                CHECK(get(&model, 16, 0xc00) == lo);
                CHECK(get(&model, 16, 0x160) == synth);
                CHECK(get(&model, 17, 0x160) == receiver);
                CHECK(get(&model, 16, 0xca8) == word);
                CHECK(get(&model, 16, 0xcac) == cycles);
                CHECK(birriasdr_get_center_freq(&dev) == frequencies[band]);
                CHECK(atomic_load(&dev.run_state) == BS_CONTROL_FAILED);
                CHECK(dev.control_error == BIRRIASDR_ERROR_IO);
                CHECK(pthread_mutex_destroy(&dev.control_mutex) == 0);
            }
        }
    }
}

/* Exercise both low fractional bits through normal startup and the live public
 * setter. A failed update must restore the previous fine word and API metadata,
 * not round the rollback to the old 18-bit grid. No physical resolution is tested.
 */
static void test_fine_tuning(void)
{
    struct model model;
    birriasdr_dev_t dev;
    initialize(&model, &dev);
    dev.backend = &bs_mt7921_backend;
    atomic_init(&dev.run_state, BS_IDLE);
    CHECK(pthread_mutex_init(&dev.control_mutex, NULL) == 0);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2400000029)) == 0);
    CHECK(mt_configure_rf(&dev) == 0);
    CHECK(get(&model, 16, 0xca8) == 0x05000001);
    atomic_store(&dev.run_state, BS_RUNNING);
    dev.control_ready = true;
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2400000057)) == 0);
    CHECK(get(&model, 16, 0xca8) == 0x05000002);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2400000086)) == 0);
    CHECK(get(&model, 16, 0xca8) == 0x05000003);
    model.fail_address = 0xce0;
    CHECK(birriasdr_set_center_freq(&dev, MT_RF_ANCHOR_HZ) == BIRRIASDR_ERROR_IO);
    CHECK(get(&model, 16, 0xca8) == 0x05000003);
    CHECK(birriasdr_get_center_freq(&dev) == UINT64_C(2400000086));
    CHECK(atomic_load(&dev.run_state) == BS_CONTROL_FAILED);
    CHECK(dev.control_error == BIRRIASDR_ERROR_IO);
    CHECK(pthread_mutex_destroy(&dev.control_mutex) == 0);
}

/* Live control (control_ready) skips the busy poll before an access and the
 * command-word read, and nothing else: the RF writes, their order and the
 * command words are identical to the full handshake, the upper half of the
 * command word survives, and any failure restores the full handshake.
 */
static void test_live_handshake(void)
{
    struct model model;
    birriasdr_dev_t dev;
    struct word full[256];
    size_t before, count;
    unsigned accesses;
    initialize(&model, &dev);
    model.command[0] = 0xabcd0000;
    CHECK(mt_configure_rf(&dev) == 0);
    /* Bring-up: two polls and one command read per access. */
    CHECK(model.accesses && model.polls == 2 * model.accesses);
    CHECK(model.command_reads == model.accesses);
    before = model.written;
    model.polls = model.command_reads = model.accesses = 0;
    CHECK(mt_change_frequency(&dev, UINT64_C(2412000000)) == 0);
    CHECK(model.polls == 2 * model.accesses && model.command_reads == model.accesses);
    count = model.written - before;
    CHECK(count <= 256);
    memcpy(full, &model.writes[before], count * sizeof(*full));
    CHECK(mt_change_frequency(&dev, UINT64_C(2437000000)) == 0);
    /* The same retune, live: only the post-access polls remain. */
    dev.control_ready = true;
    before = model.written;
    model.polls = model.command_reads = model.accesses = 0;
    CHECK(mt_change_frequency(&dev, UINT64_C(2412000000)) == 0);
    accesses = model.accesses;
    CHECK(accesses == 42 && model.polls == accesses && model.command_reads == 0);
    CHECK(model.written - before == count);
    CHECK(!memcmp(full, &model.writes[before], count * sizeof(*full)));
    CHECK((model.command[0] & 0xffff0000u) == 0xabcd0000);
    /* A failed live access: the next access polls first again. A failed live
     * retune still rolls back (covered above) and leaves the bridge idle. */
    model.fail_address = 0xce0;
    CHECK(mt_rf_write(&dev, MT_SYNTH_PORT, 0xce0, 0) == BIRRIASDR_ERROR_IO);
    CHECK(!dev.mt7921.rf_idle[0]);
    model.polls = model.accesses = 0;
    uint32_t value;
    CHECK(mt_rf_read(&dev, MT_SYNTH_PORT, 0xca8, &value) == 0);
    CHECK(model.accesses == 1 && model.polls == 2 && dev.mt7921.rf_idle[0]);
    CHECK(mt_rf_read(&dev, MT_SYNTH_PORT, 0xca8, &value) == 0);
    CHECK(model.accesses == 2 && model.polls == 3);
    /* Leaving live control restores the full handshake. */
    dev.control_ready = false;
    model.polls = model.command_reads = model.accesses = 0;
    CHECK(mt_rf_read(&dev, MT_SYNTH_PORT, 0xca8, &value) == 0);
    CHECK(model.polls == 2 && model.command_reads == 1);
}

/* Entire RF regression suite is read/write-model based and never opens USB. */
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    test_trace(argv[1]);
    test_failures();
    test_gain_updates();
    test_chain_gain();
    test_gain_steps();
    test_retune();
    test_public_tuning();
    test_fine_tuning();
    test_band_switch();
    test_band_switch_failures();
    test_live_handshake();
    return 0;
}
