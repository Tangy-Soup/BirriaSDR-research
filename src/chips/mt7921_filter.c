/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * MT7961/MT7972 matched 20/40 MHz receive profiles at 40 MSps, BW80 at 80 MSps
 *
 * The analog filter and the digital PHY bandwidth are independent controls.
 * RF 0x160[31:30] selects the analog profile in each physical SPI bank;
 * 0x83081000[27:24] and 0x83091000[27:24] select the digital bandwidth class.
 * The original channel-6 bootstrap leaves both at code 0. Merely widening the
 * analog filter retains the digital rolloff: noise is about 22 dB down at
 * +/-18 MHz with analog code 1 and digital code 0.
 *
 * Set both fields to code 1 after the established RF/gain bootstrap, while
 * capture is stopped and the original MCU program waits for GO. This is the
 * matched 40 MHz profile. The capture tap and divider remain unchanged at
 * nominal 40 MSps complex; no host equalizer, averaging or format conversion
 * is introduced. The paired configuration reduced the measured noise rolloff
 * to about 2.8 dB at +/-18 MHz on the tested board. Alternatively, retain both
 * fields at code 0 for the nominal 20 MHz profile. Channel-centered comparisons
 * on the local EDUP board recovered additional FCS-valid AP beacons with that
 * narrower profile. The public default remains 40 MHz; neither profile changes
 * the sample clock or transport format.
 *
 * This is a nominal channel profile, not a brick-wall anti-alias filter. The
 * receive-only Wi-Fi comparison measured roughly 19-20 dB suppression at
 * +/-30 MHz and still decoded some aliased headers from strong signals there.
 * The wider analog codes admit more out-of-span energy and are research-only.
 * Calibrated passband, stopband guarantees and arbitrary filter widths remain
 * unsupported; see docs/mt7921.md for the configuration and its limitations.
 *
 * The experimental 80 MSps rate uses selector 2, the 2.4 GHz BW80 graft from
 * Mtk79ICAP (driver/mt7921/iqstream.c, after the vendor RAM firmware's BW
 * routine): code 2 in both fields above, plus the chain-bank RF 0xb00[29:24] =
 * 0x20 and 0xb04[28:24] = 0x1f, [7:6] = 2 on both SPI banks, then a pulse of
 * 0x8300c008[31] and 0x8301c008[31] to latch the digital class. Under this
 * ROM bring-up the remaining graft words already hold their BW80 values
 * (0x830a0080[7:4] = 6, 0x8300d06c & 0x78 = 0x40, 0x83080000[12] = 0), and
 * the BW80 AGC table is left alone because the AGC does not run. The graft was
 * applied and restored cleanly at 40 MSps (captures/research_20261020_w24_graft
 * in the workspace); at 40 MSps it only lets neighbouring channels alias in, so
 * it is used only with the 80 MSps rate field, regardless of the 20/40 setting.
 *
 * Save every baseline field before the first write. Preserve unrelated
 * bits, verify each application, and restore every field on partial failure.
 * Analog state can survive WFSYS reset, so normal reset restores this baseline
 * too. A failed restoration keeps the saved state for retry. Every automatic
 * restart follows the same bootstrap and reapplies this profile before GO.
 */
#include "mt7921.h"
#include <string.h>

/* Fields 0-3 form the 20/40 MHz profiles; BW80 adds 4-7. RF fields use the
 * physical SPI bank in port, MMIO fields have port MT_FILTER_MMIO.
 */
#define MT_FILTER_MMIO 0xffu
#define MT_FILTER_FIELDS 8u
static const struct {
    unsigned port;
    uint32_t address, mask;
} fields[MT_FILTER_FIELDS] = {
    {0, 0x160, 0xc0000000}, {1, 0x160, 0xc0000000},
    {MT_FILTER_MMIO, 0x83081000, 0x0f000000}, {MT_FILTER_MMIO, 0x83091000, 0x0f000000},
    {0, 0xb00, 0x3f000000}, {1, 0xb00, 0x3f000000},
    {0, 0xb04, 0x1f0000c0}, {1, 0xb04, 0x1f0000c0},
};
/* Analog before digital, as the vendor firmware; restore digital first. */
static const unsigned apply_narrow[] = {0, 1, 2, 3};
static const unsigned apply_wide[] = {0, 1, 4, 5, 6, 7, 2, 3};
static const unsigned restore_order[] = {2, 3, 7, 6, 5, 4, 0, 1};
static const uint32_t latch_addresses[] = {0x8300c008, 0x8301c008};

static int filter_read(birriasdr_dev_t *dev, unsigned field, uint32_t *value)
{
    if (fields[field].port != MT_FILTER_MMIO)
        return mt_rf_read(dev, fields[field].port, fields[field].address, value);
    return bs_read32(dev, BS_MMIO, fields[field].address, value);
}

/* Change only the owned field, retaining band selection and other modes.
 * Masked verification accepts independent changes to unrelated register bits.
 */
static int filter_write(birriasdr_dev_t *dev, unsigned field, uint32_t value)
{
    uint32_t mask = fields[field].mask;
    uint32_t actual;
    if (fields[field].port != MT_FILTER_MMIO)
        BS_TRY(mt_rf_update(dev, fields[field].port, fields[field].address, mask, value & mask));
    else
        BS_TRY(bs_update32(dev, BS_MMIO, fields[field].address, mask, value & mask));
    BS_TRY(filter_read(dev, field, &actual));
    return ((actual ^ value) & mask) ? BIRRIASDR_ERROR_STATE : 0;
}

/* Profile value of each field for selector 0 (20 MHz), 1 (40 MHz) or 2 (BW80). */
static uint32_t profile_value(unsigned field, uint32_t selector)
{
    switch (fields[field].address) {
    case 0x160: return selector << 30;
    case 0xb00: return 0x20u << 24;
    case 0xb04: return 0x1fu << 24 | 2u << 6;
    default: return selector << 24;
    }
}

/* Latch the digital class: set [31] in both chains' word, then clear both. */
static int pulse_latch(birriasdr_dev_t *dev)
{
    for (unsigned c = 0; c < 2; ++c)
        BS_TRY(bs_update32(dev, BS_MMIO, latch_addresses[c], 0, 1u << 31));
    for (unsigned c = 0; c < 2; ++c)
        BS_TRY(bs_update32(dev, BS_MMIO, latch_addresses[c], 1u << 31, 0));
    return 0;
}

/* Restore digital class first, then analog width, before the subsystem reset.
 * Attempt every field even if one fails; retain the baseline until all pass.
 */
int mt_restore_filter(birriasdr_dev_t *dev)
{
    int first_error = 0;
    if (!dev->mt7921.filter_saved)
        return 0;
    for (unsigned i = 0; i < MT_FILTER_FIELDS; ++i) {
        unsigned field = restore_order[i];
        int result;
        if (field >= 4 && !dev->mt7921.filter_wide)
            continue;
        result = filter_write(dev, field, dev->mt7921.filter_baseline[field]);
        if (result < 0 && !first_error)
            first_error = result;
        if (field == 3 && dev->mt7921.filter_wide) {
            result = pulse_latch(dev);
            if (result < 0 && !first_error)
                first_error = result;
        }
    }
    if (!first_error)
        dev->mt7921.filter_saved = dev->mt7921.filter_wide = false;
    return first_error;
}

int mt_validate_bandwidth(uint32_t bandwidth)
{
    return bandwidth == 20000000 || bandwidth == 40000000 ? 0 : BIRRIASDR_ERROR_UNSUPPORTED;
}

/* Apply the selected profile once per preparation; this is not a live control.
 * The prepare owner handles any remaining cleanup and whole-startup retry.
 */
int mt_configure_filter(birriasdr_dev_t *dev)
{
    uint32_t before[MT_FILTER_FIELDS] = {0};
    uint32_t bandwidth = atomic_load(&dev->bandwidth);
    bool wide = dev->rate == MT_RATE_80;
    uint32_t selector = wide ? 2 : bandwidth == 20000000 ? 0 : 1;
    const unsigned *order = wide ? apply_wide : apply_narrow;
    unsigned count = wide ? MT_FILTER_FIELDS : 4;
    if ((dev->rate != MT_RATE_40 && !wide) || mt_validate_bandwidth(bandwidth))
        return BIRRIASDR_ERROR_UNSUPPORTED;
    if (dev->mt7921.filter_saved)
        return BIRRIASDR_ERROR_STATE;
    for (unsigned i = 0; i < count; ++i)
        BS_TRY(filter_read(dev, order[i], &before[order[i]]));
    memcpy(dev->mt7921.filter_baseline, before, sizeof(before));
    dev->mt7921.filter_saved = true;
    dev->mt7921.filter_wide = wide;
    for (unsigned i = 0; i < count; ++i) {
        int result = filter_write(dev, order[i], profile_value(order[i], selector));
        if (result < 0)
            return mt_restore_filter(dev) < 0 ? BIRRIASDR_ERROR_STATE : result;
    }
    if (wide) {
        int result = pulse_latch(dev);
        if (result < 0)
            return mt_restore_filter(dev) < 0 ? BIRRIASDR_ERROR_STATE : result;
    }
    bs_sleep_us(20000);
    return 0;
}
