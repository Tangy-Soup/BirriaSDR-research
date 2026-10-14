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
    if (space == BS_BRIDGE) {
        unsigned index = address >= 0x7c004050;
        uint32_t command;
        if (address == 0x7c004010 || address == 0x7c004050) {
            model->command[index] = value;
            return 0;
        }
        command = model->command[index];
        if (command & 0x1000)
            return 0;
        target = 16 + ((command >> 13) & 7);
        address = command & 0xfff;
    }
    if (address == model->fail_address && model->fail_address) {
        model->fail_address = 0;
        return BIRRIASDR_ERROR_IO;
    }
    CHECK(model->written < 2048);
    model->writes[model->written++] = (struct word){target, address, value};
    if (target >= 16 && (address == 0xc68 || address == 0xca8))
        value &= 0x7fffffff;
    if (target == 21 && address == 0xb00 && model->stuck_standby)
        value |= 0x10000;
    put(model, target, address, value);
    return 0;
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

/* Compare every write/order against the established Python receiver. This
 * catches missing wake steps, bank swaps, constants and ordering regressions.
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

/* Entire RF regression suite is read/write-model based and never opens USB. */
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    test_trace(argv[1]);
    test_failures();
    return 0;
}
