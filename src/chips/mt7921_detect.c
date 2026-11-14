/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * MT7961/MT7972 packet-detect suppression (research setting, opt-in)
 *
 * The Wi-Fi PHY keeps its receive state machine running while ICAP captures.
 * Each packet-detect attempt that fails to lock writes exact-zero runs into
 * capture SRAM: a 2 + 8-sample lead-in, a 48-sample main run, then a 6-sample
 * trailer. Most attempts are triggered by noise, so the rate follows the
 * antenna noise level. Frames the PHY locks onto stay clean; frames it
 * triggers on but cannot lock (about 0-4 dB SNR here) are blanked repeatedly.
 *
 * 0x8300011c[19:16] behaves as a signed 4-bit field; the booted value is +7.
 * Any negative value stops the triggers once the detector has adapted after GO.
 * This module writes -8 (0x8). On the tested EDUP board and channels 1-11,
 * the zero fraction fell from about 0.66% to 0.03%, and no main runs remained
 * after 150 ms in 44 of 46 one-second captures. Adaptation took 0.1-252 ms
 * there, but up to ~587 ms in a later 33-capture survey (4 runs past 300 ms),
 * so applications should discard the first ~600 ms of each read. -1 also
 * lifted the captured level by about 8 dB, and decoding collapsed; do not
 * substitute it. A 20-pair weak-AP test found no weak-frame cost (marginal
 * APs 316 vs 315 beacons; below 0 dB SNR 10/24 vs 6/23 decoded).
 * Evidence and limits are in docs/mt7921.md.
 *
 * Order matters (2026-11-14 probes): written last in preparation, just before
 * GO, the field reads back -8 throughout but the triggers can continue for
 * the whole read. Written before RF tuning, calibration and gain (Flipper's
 * position, the end of mt_configure_capture()), they mostly stop at once:
 * mode 0x14 two-chain reads at 2437 MHz, alternating order, gave a median of
 * 3 zero runs/s against 226 for the late write, though one early read in
 * seven still blanked throughout, so keep discarding. Holding GO back 750 ms
 * after a late write did not do the same (2 of 4 clean), so the order
 * matters, not the time.
 *
 * Both chains at 5 GHz also blank through chain 1's PHY: its copy of the field
 * (0x8301011c) and its two frame detectors (0x83098200[22:21], booted on; the
 * capture setup clears only chain 0's) trigger too. With all three written
 * early, mode 0x14 at 5745 MHz gave a median of 4 zero runs/s over 13 reads,
 * against 20-25 with chain 0's field alone. At 2.4 GHz adding them was no
 * better, so they are written only for both chains at 5 GHz and up, at
 * preparation (a live retune across bands keeps the prepared set).
 *
 * Apply once per preparation, from mt_configure_capture() before the RF
 * configuration and GO. Save each whole word first, change only the field
 * bits, then verify by readback.
 * PHY fields can survive WFSYS reset, so mt_reset() restores the saved fields
 * before resetting. A failed restoration keeps the saved state for retry.
 * Receive-only and volatile: no flash, eFuse or transmit state is touched.
 */
#include "mt7921.h"

/* Field 0 always; fields 1 and 2 only for both chains at 5 GHz and up. */
static const uint32_t detect_address[MT_DETECT_FIELDS] = {0x8300011c, 0x8301011c, 0x83098200};
static const uint32_t detect_mask[MT_DETECT_FIELDS] = {0x000f0000, 0x000f0000, 0x00600000};
static const uint32_t detect_value[MT_DETECT_FIELDS] = {0x00080000, 0x00080000, 0};

/* Write one field and accept unrelated bits that change independently. */
static int detect_write(birriasdr_dev_t *dev, unsigned field, uint32_t value)
{
    uint32_t actual, mask = detect_mask[field];
    BS_TRY(bs_update32(dev, BS_MMIO, detect_address[field], mask, value & mask));
    BS_TRY(bs_read32(dev, BS_MMIO, detect_address[field], &actual));
    return ((actual ^ value) & mask) ? BIRRIASDR_ERROR_STATE : 0;
}

/* Restore in reverse order; attempt every field, keep the baseline until all pass. */
int mt_restore_detect(birriasdr_dev_t *dev)
{
    int first_error = 0;
    if (!dev->mt7921.detect_saved)
        return 0;
    for (unsigned field = dev->mt7921.detect_fields; field-- > 0;) {
        int result = detect_write(dev, field, dev->mt7921.detect_baseline[field]);
        if (result < 0 && !first_error)
            first_error = result;
    }
    if (!first_error)
        dev->mt7921.detect_saved = false;
    return first_error;
}

/* Disabled suppression performs no register access at all. */
int mt_configure_detect(birriasdr_dev_t *dev)
{
    unsigned fields = 1;
    if (!atomic_load(&dev->packet_detect_suppression))
        return 0;
    if (dev->mt7921.detect_saved)
        return BIRRIASDR_ERROR_STATE;
    if (atomic_load(&dev->receive_chain) == BIRRIASDR_CHAIN_BOTH &&
        atomic_load(&dev->frequency) >= MT_RF_5G_MIN_HZ)
        fields = MT_DETECT_FIELDS;
    for (unsigned field = 0; field < fields; ++field)
        BS_TRY(bs_read32(dev, BS_MMIO, detect_address[field], &dev->mt7921.detect_baseline[field]));
    dev->mt7921.detect_fields = fields;
    dev->mt7921.detect_saved = true;
    for (unsigned field = 0; field < fields; ++field) {
        int result = detect_write(dev, field, detect_value[field]);
        if (result < 0)
            return mt_restore_detect(dev) < 0 ? BIRRIASDR_ERROR_STATE : result;
    }
    return 0;
}
