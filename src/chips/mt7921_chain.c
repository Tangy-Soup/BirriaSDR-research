/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * MT7961/MT7972 receive-chain and capture-stage selection (opt-in)
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
 * Manual gain forces every streamed chain (mt7921_gain.c). Released, WF1 holds
 * a fixed analog 5/7 plus digital gain. The both-chain taps precede the digital
 * gain stage. Evidence and limits are in docs/mt7921.md and, in the workspace,
 * captures/research_20261019_tenth_round and research_20261023_gain_db.
 *
 * The optional channel filter (birriasdr_set_channel_filter()) moves a
 * single-chain capture from stage 1 to stage 3 of the same tap map: tap 12 for
 * chain 0, 13 for chain 1. Stage 3 follows the PHY's digital channel filter,
 * whose width tracks the bandwidth class 0x83081000[27:24]. In 18 paired
 * captures at the 20 MHz profile, tap 12 lowered the floor 10-15 MHz off
 * centre by about 13 dB against tap 4, with 8% more FCS-valid OFDM frames and
 * the same sample format. At the 40 MHz profile both taps are flat to +/-20 MHz
 * and tap 12 only reduces aliasing; it does not remove it. Mtk79ICAP streams
 * tap 12/13 by default under the vendor firmware. Both-chain capture already
 * reads stage 3 (node 0x0c), so the setting changes nothing there.
 *
 * Apply once per preparation, after packet-detect suppression and before GO.
 * Save each word first, change only the selected fields, verify by readback,
 * and roll back on partial failure. mt_reset() restores the fields first,
 * before the other restores and the WFSYS reset. A failed restoration keeps
 * the saved state for retry. Receive-only and volatile: no flash, eFuse or
 * transmit state is touched. Chain 0 without the channel filter performs no
 * register access at all.
 */
#include "mt7921.h"

#define MT_CHAIN_FIELDS 3u

/* Field 0 is RF SPI port 0 (WF1's receiver); fields 1 and 2 are MMIO. */
static const uint32_t chain_addresses[MT_CHAIN_FIELDS] = {0x1e4, 0x830ad440, 0x830ad448};
#define MT_WF1_RECEIVE 0x0001fbc3u
#define MT_TAP_MASK 0x00000f00u
#define MT_TAP(stage, kind) ((uint32_t)((stage) * 4u + (kind)) << 8)

/* Fill the fields one selection changes; zero masks are left untouched. */
static void chain_selection(unsigned chain, bool filtered, uint32_t masks[MT_CHAIN_FIELDS],
                            uint32_t values[MT_CHAIN_FIELDS])
{
    unsigned stage = filtered ? 3 : 1;
    for (unsigned field = 0; field < MT_CHAIN_FIELDS; ++field)
        masks[field] = values[field] = 0;
    if (chain != BIRRIASDR_CHAIN_0) {
        masks[0] = 0xffffffff;
        values[0] = MT_WF1_RECEIVE;
    }
    if (chain == BIRRIASDR_CHAIN_BOTH) {
        masks[1] = 0xff;
        values[1] = 0x11;
        masks[2] = 0x033f00ff;
        values[2] = 0x0023000c;
    } else if (chain == BIRRIASDR_CHAIN_1 || filtered) {
        masks[2] = MT_TAP_MASK;
        values[2] = MT_TAP(stage, chain);
    }
}

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
    int first_error = 0;
    if (!dev->mt7921.chain_saved)
        return 0;
    for (unsigned field = MT_CHAIN_FIELDS; field-- > 0;) {
        uint32_t mask = dev->mt7921.chain_mask[field];
        int result = mask ? chain_write(dev, field, mask, dev->mt7921.chain_baseline[field]) : 0;
        if (result < 0 && !first_error)
            first_error = result;
    }
    if (!first_error)
        dev->mt7921.chain_saved = false;
    return first_error;
}

int mt_configure_chain(birriasdr_dev_t *dev)
{
    uint32_t before[MT_CHAIN_FIELDS] = {0}, masks[MT_CHAIN_FIELDS], values[MT_CHAIN_FIELDS];
    unsigned chain = atomic_load(&dev->receive_chain);
    bool filtered = atomic_load(&dev->channel_filter) != 0;
    if (chain > BIRRIASDR_CHAIN_BOTH)
        return BIRRIASDR_ERROR_INVALID;
    if (chain == BIRRIASDR_CHAIN_0 && !filtered)
        return 0;
    if (dev->mt7921.chain_saved)
        return BIRRIASDR_ERROR_STATE;
    chain_selection(chain, filtered, masks, values);
    for (unsigned field = 0; field < MT_CHAIN_FIELDS; ++field)
        if (masks[field])
            BS_TRY(chain_read(dev, field, &before[field]));
    for (unsigned field = 0; field < MT_CHAIN_FIELDS; ++field) {
        dev->mt7921.chain_baseline[field] = before[field];
        dev->mt7921.chain_mask[field] = masks[field];
    }
    dev->mt7921.chain_saved = true;
    for (unsigned field = 0; field < MT_CHAIN_FIELDS; ++field) {
        int result = masks[field] ? chain_write(dev, field, masks[field], values[field]) : 0;
        if (result < 0)
            return mt_restore_chain(dev) < 0 ? BIRRIASDR_ERROR_STATE : result;
    }
    return 0;
}
