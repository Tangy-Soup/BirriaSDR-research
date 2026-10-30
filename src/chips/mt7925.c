/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * MT7925 backend placeholder - discovery is not receiver support
 *
 * The device registry can name this family, but it is not qualified for SDR
 * operation. Capabilities stay zero, open rejects unqualified devices before
 * claiming/writing hardware, and every operation here returns UNSUPPORTED.
 * Keep these explicit failures until the corresponding mechanism is known;
 * successful no-ops would let callers mistake missing setup for valid I/Q.
 *
 * MT7925 uses Connac3 command framing rather than MT7921's Connac2 framing.
 * Its ROM calls, RAM layout, analog die, clock/calibration sequence and ICAP
 * routing have not been qualified by this library. Do not reuse the MT7921
 * image or register recipes merely by adding a USB ID to the qualified list.
 *
 * Bring-up needs, in order: read-only die identification; a bounded reset/ROM
 * transport; an original program with verified execution and DMA ownership;
 * RF initialization and external-signal reception; then a validated sample
 * format, gain ranges and terminal/error reporting. Register readback and
 * USB throughput alone cannot establish a working receiver.
 *
 * Public enumeration, read/cancel handling and USB transfer management can be
 * shared. The existing framer and raw14 Q,I decoder describe our original
 * firmware's ABI, not a universal chipset protocol. A new backend must produce
 * that ABI or provide an explicit adapter before sharing the receive path.
 * Add backend limits/capabilities with evidence and offline protocol tests;
 * keep uncharacterized controls unsupported. See docs/architecture.md for
 * the backend boundary and test strategy.
 */
#include "internal.h"

/* TODO: reverse-engineer MT7925 boot/PHY/ICAP and validate an original image.
 * Its Connac3 command framing is not interchangeable with MT7921 Connac2.
 */
static int prepare(birriasdr_dev_t *dev, uint32_t packets)
{
    (void)dev;
    (void)packets;
    return BIRRIASDR_ERROR_UNSUPPORTED;
}

/* TODO: identify start, stop and reset sequences before issuing any USB write. */
static int control(birriasdr_dev_t *dev)
{
    (void)dev;
    return BIRRIASDR_ERROR_UNSUPPORTED;
}

/* TODO: establish mailbox/status semantics; do not fabricate successful status. */
static int status(birriasdr_dev_t *dev, uint32_t words[16])
{
    (void)dev;
    (void)words;
    return BIRRIASDR_ERROR_UNSUPPORTED;
}

/* TODO: qualify sample clocks, RF bands and gain-code ranges on the new die. */
static int validate(uint64_t frequency, uint32_t rate, unsigned chain, const struct birriasdr_gain *gain)
{
    (void)frequency;
    (void)rate;
    (void)chain;
    (void)gain;
    return BIRRIASDR_ERROR_UNSUPPORTED;
}

/* TODO: identify owned gain fields and validate live application and rollback. */
static int change_gain(birriasdr_dev_t *dev, bool manual, const struct birriasdr_gain *gain)
{
    (void)dev;
    (void)manual;
    (void)gain;
    return BIRRIASDR_ERROR_UNSUPPORTED;
}

/* TODO: decode and validate this die's synthesizer before exposing live tuning. */
static int change_frequency(birriasdr_dev_t *dev, uint64_t frequency)
{
    (void)dev;
    (void)frequency;
    return BIRRIASDR_ERROR_UNSUPPORTED;
}

const struct bs_backend bs_mt7925_backend = {
    .chipset = BIRRIASDR_CHIP_MT7925,
    .name = "MT7925 (unimplemented)",
    .validate = validate,
    .identify = control,
    .prepare = prepare, .start = control, .stop = control,
    .status = status, .reset = control, .change_gain = change_gain,
    .change_frequency = change_frequency
};
