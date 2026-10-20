/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * MT7961/MT7972 receive-chain selection (research setting, opt-in)
 *
 * The die has two receivers. Chain 0 (WF0) is physical RF SPI port 1. Chain 1
 * (WF1) is physical SPI port 0, the companion bank whose secondary synthesizer
 * already supplies chain 0's LO. The normal bring-up leaves that receiver at
 * 0x1e4 = 0x23c1. Writing 0x1fbc3 instead (bits 1, 11, 12, 14, 15 and 16 added)
 * turns WF1's receive path on into ICAP; leaving out any of bits 1, 11, 14, 15
 * or 16 leaves it RF-blind, and bit 12 improved the packet rise. Bit 17 would
 * add ~17.5 dB to signal and noise alike but stops the filtered dense taps
 * carrying packets, so it is not set. Chain 0's borrowed LO keeps working
 * because the synthesizer fields are untouched; both chains receive the tuned
 * channel, and paired captures found no measurable chain-0 cost.
 *
 * ICAP's dense tap number is stage * 4 + kind, where kind 0 is WF0 and kind 1
 * is WF1. Kind 1 outputs an envelope-like placeholder (I = Q) while WF1 is off.
 * So chain 1 alone needs only the tap nibble 0x830ad448[11:8] moved from 4 to
 * 5, the same stage as the shipped path; rate and sample format are unchanged.
 *
 * Both chains use source mode 0x11 (0x830ad440[7:0]) with node 0x0c and
 * 0x830ad448[21] and [17:16] = 3 (mask 0x033f00ff, value 0x0023000c). Each
 * sample time then carries two raw14 words: slot 0 is tap 12 (WF0 after the
 * digital channel filter) and slot 1 is tap 13 (WF1, same stage). Each slot
 * runs at 40 MSps, so the producer and USB word rate double. 16M-word reads
 * completed 9 of 11 times in the first study (both overruns had WF1 off) and
 * 6 of 6, up to 80M words, in the validation of this module.
 *
 * Unlike the dense taps (12-bit data, low two bits zero, DC-cancelled), mode
 * 0x11 slots carry all 14 bits and a large static DC offset: about (-165, +152)
 * LSB on slot 0 and (-49, +68) on slot 1 at channel 6, different on channel
 * 11, stable to ~2 LSB within a capture. It dominates the slot's floor, and
 * left in place it cost slot 0 about 37% of its frames. The library does
 * not filter samples, so applications must remove DC (a ~1.6 ms block mean
 * sufficed); with it removed, both slots decoded alike at channel 6.
 *
 * WF1's gain is not under the manual-gain overrides (those target chain 0) and
 * steps by ~6 dB within a capture, as AGC would. Evidence and limits are in
 * docs/mt7921.md and captures/research_20261019_tenth_round in the workspace.
 *
 * Apply once per preparation, after packet-detect suppression and before GO.
 * Save each word first, change only the selected fields, verify by readback,
 * and roll back on partial failure. mt_reset() restores the fields first,
 * before the other restores and the WFSYS reset. A failed restoration keeps
 * the saved state for retry. Receive-only and volatile: no flash, eFuse or
 * transmit state is touched. Chain 0 performs no register access at all.
 */
#include "mt7921.h"

#define MT_CHAIN_FIELDS 3u

/* Field 0 is RF SPI port 0 (WF1's receiver); fields 1 and 2 are MMIO. */
static const uint32_t chain_addresses[MT_CHAIN_FIELDS] = {0x1e4, 0x830ad440, 0x830ad448};
/* Indexed by chain - 1: chain 1 alone, then both chains. Zero masks are unused. */
static const uint32_t chain_masks[2][MT_CHAIN_FIELDS] = {
    {0xffffffff, 0, 0x00000f00},
    {0xffffffff, 0xff, 0x033f00ff}
};
static const uint32_t chain_values[2][MT_CHAIN_FIELDS] = {
    {0x0001fbc3, 0, 0x00000500},
    {0x0001fbc3, 0x11, 0x0023000c}
};

static int chain_read(birriasdr_dev_t *dev, unsigned field, uint32_t *value)
{
    if (!field)
        return mt_rf_read(dev, MT_SYNTH_PORT, chain_addresses[0], value);
    return bs_read32(dev, BS_MMIO, chain_addresses[field], value);
}

/* Change only the selected bits; masked verification accepts unrelated changes. */
static int chain_write(birriasdr_dev_t *dev, unsigned field, uint32_t mask, uint32_t value)
{
    uint32_t actual;
    if (!field)
        BS_TRY(mt_rf_update(dev, MT_SYNTH_PORT, chain_addresses[0], mask, value & mask));
    else
        BS_TRY(bs_update32(dev, BS_MMIO, chain_addresses[field], mask, value & mask));
    BS_TRY(chain_read(dev, field, &actual));
    return ((actual ^ value) & mask) ? BIRRIASDR_ERROR_STATE : 0;
}

/* Restore in reverse order: ICAP selection first, then WF1's receiver.
 * Attempt every field even if one fails; retain the baseline until all pass.
 */
int mt_restore_chain(birriasdr_dev_t *dev)
{
    unsigned chain = dev->mt7921.chain_saved;
    int first_error = 0;
    if (!chain)
        return 0;
    for (unsigned field = MT_CHAIN_FIELDS; field-- > 0;) {
        uint32_t mask = chain_masks[chain - 1][field];
        int result = mask ? chain_write(dev, field, mask, dev->mt7921.chain_baseline[field]) : 0;
        if (result < 0 && !first_error)
            first_error = result;
    }
    if (!first_error)
        dev->mt7921.chain_saved = 0;
    return first_error;
}

int mt_configure_chain(birriasdr_dev_t *dev)
{
    uint32_t before[MT_CHAIN_FIELDS] = {0};
    unsigned chain = atomic_load(&dev->receive_chain);
    if (chain == BIRRIASDR_CHAIN_0)
        return 0;
    if (chain > BIRRIASDR_CHAIN_BOTH)
        return BIRRIASDR_ERROR_INVALID;
    if (dev->mt7921.chain_saved)
        return BIRRIASDR_ERROR_STATE;
    for (unsigned field = 0; field < MT_CHAIN_FIELDS; ++field)
        if (chain_masks[chain - 1][field])
            BS_TRY(chain_read(dev, field, &before[field]));
    for (unsigned field = 0; field < MT_CHAIN_FIELDS; ++field)
        dev->mt7921.chain_baseline[field] = before[field];
    dev->mt7921.chain_saved = chain;
    for (unsigned field = 0; field < MT_CHAIN_FIELDS; ++field) {
        uint32_t mask = chain_masks[chain - 1][field];
        int result = mask ? chain_write(dev, field, mask, chain_values[chain - 1][field]) : 0;
        if (result < 0)
            return mt_restore_chain(dev) < 0 ? BIRRIASDR_ERROR_STATE : result;
    }
    return 0;
}
