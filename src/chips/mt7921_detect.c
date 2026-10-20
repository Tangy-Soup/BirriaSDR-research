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
 * Apply once per preparation, after the filter profile and before GO. Save the
 * whole word first, change only the four field bits, then verify by readback.
 * PHY fields can survive WFSYS reset, so mt_reset() restores the saved field
 * before resetting. A failed restoration keeps the saved state for retry.
 * Receive-only and volatile: no flash, eFuse or transmit state is touched.
 */
#include "mt7921.h"

#define MT_DETECT_ADDRESS 0x8300011cu
#define MT_DETECT_MASK 0x000f0000u
#define MT_DETECT_SUPPRESS 0x00080000u

/* Write the field and accept unrelated bits that change independently. */
static int detect_write(birriasdr_dev_t *dev, uint32_t value)
{
    uint32_t actual;
    BS_TRY(bs_update32(dev, BS_MMIO, MT_DETECT_ADDRESS, MT_DETECT_MASK, value & MT_DETECT_MASK));
    BS_TRY(bs_read32(dev, BS_MMIO, MT_DETECT_ADDRESS, &actual));
    return ((actual ^ value) & MT_DETECT_MASK) ? BIRRIASDR_ERROR_STATE : 0;
}

int mt_restore_detect(birriasdr_dev_t *dev)
{
    if (!dev->mt7921.detect_saved)
        return 0;
    BS_TRY(detect_write(dev, dev->mt7921.detect_baseline));
    dev->mt7921.detect_saved = false;
    return 0;
}

/* Disabled suppression performs no register access at all. */
int mt_configure_detect(birriasdr_dev_t *dev)
{
    int result;
    if (!atomic_load(&dev->packet_detect_suppression))
        return 0;
    if (dev->mt7921.detect_saved)
        return BIRRIASDR_ERROR_STATE;
    BS_TRY(bs_read32(dev, BS_MMIO, MT_DETECT_ADDRESS, &dev->mt7921.detect_baseline));
    dev->mt7921.detect_saved = true;
    result = detect_write(dev, MT_DETECT_SUPPRESS);
    if (result < 0)
        return mt_restore_detect(dev) < 0 ? BIRRIASDR_ERROR_STATE : result;
    return 0;
}
