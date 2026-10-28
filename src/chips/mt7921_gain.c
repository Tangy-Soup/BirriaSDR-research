/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * MT7961/MT7972 receive gain overrides, restoration and the measured dB table
 *
 * Manual gain is a coordinated analog/PHY override, not one amplifier setting.
 * Each receive chain has three digital MMIO words and one analog word. Chain 1's
 * PHY words sit 0x10000 above chain 0's; both analog words are RF 0x768, on the
 * chain's own SPI port (1 = chain 0, 0 = chain 1). Only the fields in gain_masks
 * belong to this module; read-modify-write preserves every other field.
 *
 *   PHY 0x830000c4: A/B/C force bits [2:0]; LNA codes [10:8], [18:16],
 *                   [26:24]; LPF codes [15:12], [23:20], [31:28].
 *   PHY 0x83000108: digital A [29:24] and C [21:16].
 *   PHY 0x83000150: force D [31], LNA [22:20]; clear owned [15:12].
 *   RF  0x768:      analog force [31], LNA [30:27], LPF gain [26:23].
 *
 * A/B/C/D name the decoded PHY field groups, not four receive antennas. Their
 * individual contributions are not all characterized. The validated control
 * is the combined recipe below, using LNA 0..5, LPF 0..8 and digital 0..12.
 * These are raw indices, not dB. LPF gain changes the filter-stage gain, not
 * its bandwidth; writing a wider numeric code is not a wider RF passband.
 * LNA 6 and 7 fit the fields but measured the same as 5, so they are rejected.
 *
 * Only the streamed chains are forced: chain 0, chain 1, or both for
 * BIRRIASDR_CHAIN_BOTH. Mtk79ICAP found that also holding a chain the stream
 * does not carry nearly doubles blanking on the streamed one.
 *
 * Initialize this recipe after RF initialization, before capture starts. Snapshot
 * each streamed chain's baseline and reject pre-existing forces, including
 * chain 0's coarse force 0x830000d0[27], rather than saving a forced state as
 * an automatic baseline. Clamp the analog path first, then program digital and
 * PHY overrides, checking masked readback after every write. On failure,
 * restore digital/PHY fields before releasing the analog clamp. Attempt every
 * restore even if one fails.
 *
 * WFSYS reset does not reliably clear the analog overrides. mt_reset() must
 * call mt_restore_gain() BEFORE resetting; otherwise the next capture can
 * inherit forced gain and fail baseline validation. Keep the saved baseline
 * after a restore failure so later cleanup can retry it. A start only checks
 * the chains it streams, so a chain-1 override left by a killed process is not
 * detected by a later chain-0 capture; a replug clears it.
 *
 * Automatic/released mode means leaving the initialized baseline unforced.
 * It does not prove full AGC operation or that manual mode freezes every gain
 * loop: released chain 0 changes its gain per frame, while released chain 1
 * held a fixed analog 5/7 plus about 16.6 dB of digital gain. AGC target,
 * attack/decay and complete AGC disable remain unsupported. The private update
 * helper can reuse the saved baselines while capturing, provided the reader
 * owns all RF/register access. The writes are not simultaneous and buffered
 * samples precede their acknowledgment; callers must not present a host
 * callback offset as an exact RF boundary.
 *
 * The dB table is relative: tenths of a dB above the lowest code (0/0/0), not
 * an absolute RF gain. Each stage was measured on one chain at a time at
 * 2437 MHz and 40 MSps, as the level change of one AP's beacons against
 * reference dwells in one live-sweep stream. The stages add within 0.3 dB rms
 * (chain 0) and 0.44 dB rms (chain 1). Digital gain measured 1.0 dB per even
 * code; odd codes assume the same step. The both-chain taps come before the
 * digital stage, so there digital adds nothing and chain 0's analog values
 * apply. Chain 1's stages were measured before its receive word gained bit 17
 * (mt7921_chain.c); the table is relative, but those steps were not remeasured.
 * The overall-gain steps raise LNA first, then LPF, then digital,
 * because SNR followed LNA, saturated in LPF and never changed with digital.
 * See docs/mt7921.md for the scope of these controls.
 */
#include "mt7921.h"
#include <string.h>

static const uint32_t gain_addresses[] = {0x830000c4, 0x83000108, 0x83000150, 0x768};
static const uint32_t gain_masks[] = {0xf7f7f707, 0x3f3f0000, 0x8070f000, 0xff800000};

/* Measured stage gains in tenths of a dB above code 0, per receive chain. */
static const int16_t lna_tenths[2][MT_GAIN_LNA_MAX + 1] = {
    {0, 81, 138, 197, 258, 318},
    {0, 77, 144, 200, 268, 330},
};
/* Chain 1's LPF codes 1, 3 and 5 are interpolated between measured codes. */
static const int16_t lpf_tenths[2][MT_GAIN_LPF_MAX + 1] = {
    {0, 21, 40, 59, 80, 97, 117, 139, 156},
    {0, 17, 33, 49, 64, 76, 89, 96, 110},
};
#define MT_DIGITAL_TENTHS 10

/* Bit c is set when receive chain c is streamed. */
static unsigned streamed_chains(birriasdr_dev_t *dev)
{
    unsigned chain = atomic_load(&dev->receive_chain);
    return chain == BIRRIASDR_CHAIN_BOTH ? 3u : chain == BIRRIASDR_CHAIN_1 ? 2u : 1u;
}

static uint32_t gain_address(unsigned chain, unsigned field)
{
    return field == 3 ? gain_addresses[3] : gain_addresses[field] + (chain << 16);
}

static unsigned gain_port(unsigned chain)
{
    return chain ? MT_SYNTH_PORT : MT_RX_PORT;
}

/* Three PHY words and one analog override together control the measured path.
 * Reading them as a group gives rollback state without copying unknown fields.
 */
static int gain_snapshot(birriasdr_dev_t *dev, unsigned chain, uint32_t values[4])
{
    for (unsigned i = 0; i < 3; ++i)
        BS_TRY(bs_read32(dev, BS_MMIO, gain_address(chain, i), &values[i]));
    return mt_rf_read(dev, gain_port(chain), gain_addresses[3], &values[3]);
}

/* Write only owned fields and verify them. The analog override uses the
 * chain's SPI port; the first three words are that chain's digital PHY MMIO.
 */
static int gain_write(birriasdr_dev_t *dev, unsigned chain, unsigned field, uint32_t value)
{
    uint32_t actual;
    uint32_t mask = gain_masks[field], address = gain_address(chain, field);
    if (field == 3) {
        BS_TRY(mt_rf_update(dev, gain_port(chain), address, mask, value & mask));
        BS_TRY(mt_rf_read(dev, gain_port(chain), address, &actual));
    } else {
        BS_TRY(bs_update32(dev, BS_MMIO, address, mask, value & mask));
        BS_TRY(bs_read32(dev, BS_MMIO, address, &actual));
    }
    return ((actual ^ value) & mask) ? BIRRIASDR_ERROR_STATE : 0;
}

/* Apply one chain's four-field transaction, preserving unrelated fields. A
 * failed update restores the immediately preceding settings, not the boot
 * baseline, which remains saved separately for release and final cleanup.
 */
static int gain_transaction(birriasdr_dev_t *dev, unsigned chain, const uint32_t before[4],
                            const uint32_t values[4], bool manual)
{
    static const unsigned order[] = {3, 1, 2, 0};
    static const unsigned restore[] = {1, 2, 0, 3};
    const unsigned *sequence = manual ? order : restore;
    int result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        result = gain_write(dev, chain, sequence[i], values[sequence[i]]);
        if (result < 0)
            break;
    }
    if (result < 0) {
        bool rollback_failed = false;
        for (unsigned i = 0; i < 4; ++i)
            if (gain_write(dev, chain, restore[i], before[restore[i]]) < 0)
                rollback_failed = true;
        if (rollback_failed)
            return BIRRIASDR_ERROR_STATE;
    }
    return result;
}

/* Build raw forced indices; the same words serve either chain. */
static int gain_values(const struct birriasdr_gain *gain, uint32_t values[4])
{
    uint32_t lna = gain->lna, lpf = gain->lpf, digital = gain->digital;
    if (lna > 5 || lpf > 8 || digital > 12)
        return BIRRIASDR_ERROR_INVALID;
    values[0] = 7 | lna << 8 | lpf << 12 | lna << 16 | lpf << 20 | lna << 24 | lpf << 28;
    values[1] = digital << 24 | digital << 16;
    values[2] = 0x80000000u | lna << 20;
    values[3] = 0x80000000u | lna << 27 | lpf << 23;
    return 0;
}

/* Each read boots into an unforced baseline. Save every streamed chain's,
 * even in released mode, so a later manual update cannot mistake its own force bits
 * for the baseline. Only the capture owner may call this, before releasing GO.
 */
int mt_apply_gain(birriasdr_dev_t *dev)
{
    uint32_t before[2][4] = {{0}}, coarse, values[4];
    unsigned chains = streamed_chains(dev);
    BS_TRY(gain_values(&dev->gain, values));
    for (unsigned c = 0; c < 2; ++c) {
        if (!(chains & 1u << c))
            continue;
        BS_TRY(gain_snapshot(dev, c, before[c]));
        if ((before[c][0] & 7) || (before[c][2] & 0x80000000) || (before[c][3] & 0x80000000))
            return BIRRIASDR_ERROR_STATE;
    }
    if (chains & 1u) {
        BS_TRY(bs_read32(dev, BS_MMIO, 0x830000d0, &coarse));
        if (coarse & (1u << 27))
            return BIRRIASDR_ERROR_STATE;
    }
    memcpy(dev->mt7921.gain_baseline, before, sizeof(before));
    dev->mt7921.gain_chains = (uint8_t)chains;
    dev->mt7921.gain_saved = true;
    if (!dev->manual)
        return 0;
    for (unsigned c = 0; c < 2; ++c)
        if (chains & 1u << c)
            BS_TRY(gain_transaction(dev, c, before[c], values, true));
    return 0;
}

/* Change gain on an initialized receiver without stopping ICAP or rebooting.
 * The caller must serialize the complete operation with other register access.
 * No firmware command is needed: the streaming MCU never touches these fields.
 * Readback confirms register application, not analog settling or a sample index.
 * Leave requested API settings to the caller, which commits only on success.
 */
int mt_change_gain(birriasdr_dev_t *dev, bool manual, const struct birriasdr_gain *gain)
{
    uint32_t before[4], values[4];
    if (!gain)
        return BIRRIASDR_ERROR_INVALID;
    BS_TRY(gain_values(gain, values));
    if (!dev->mt7921.gain_saved)
        return BIRRIASDR_ERROR_STATE;
    for (unsigned c = 0; c < 2; ++c) {
        if (!(dev->mt7921.gain_chains & 1u << c))
            continue;
        BS_TRY(gain_snapshot(dev, c, before));
        BS_TRY(gain_transaction(dev, c, before, manual ? values : dev->mt7921.gain_baseline[c], manual));
    }
    return 0;
}

/* WFSYS reset does not reliably erase analog gain overrides. Restore this
 * capture's saved baselines BEFORE reset, otherwise the next read finds forced
 * gain and cannot establish an honest automatic baseline. Always attempt all
 * fields; retain the baselines after a failure so close can retry cleanup.
 */
int mt_restore_gain(birriasdr_dev_t *dev)
{
    static const unsigned order[] = {1, 2, 0, 3};
    int first_error = 0;
    if (!dev->mt7921.gain_saved)
        return 0;
    for (unsigned c = 0; c < 2; ++c) {
        if (!(dev->mt7921.gain_chains & 1u << c))
            continue;
        for (unsigned i = 0; i < 4; ++i) {
            unsigned field = order[i];
            int result = gain_write(dev, c, field, dev->mt7921.gain_baseline[c][field]);
            if (result < 0 && !first_error)
                first_error = result;
        }
    }
    if (!first_error) {
        dev->mt7921.gain_saved = false;
        dev->mt7921.gain_chains = 0;
    }
    return first_error;
}

/* Relative gain of arbitrary codes from the separable stage table. In
 * both-chain capture the digital stage is bypassed and chain 0's values apply.
 */
int mt_gain_tenths_db(unsigned chain, const struct birriasdr_gain *gain, int *tenths_db)
{
    unsigned column = chain == BIRRIASDR_CHAIN_1 ? 1 : 0;
    if (!gain || !tenths_db || chain > BIRRIASDR_CHAIN_BOTH || gain->lna > MT_GAIN_LNA_MAX ||
        gain->lpf > MT_GAIN_LPF_MAX || gain->digital > MT_GAIN_DIGITAL_MAX)
        return BIRRIASDR_ERROR_INVALID;
    *tenths_db = lna_tenths[column][gain->lna] + lpf_tenths[column][gain->lpf] +
                 (chain == BIRRIASDR_CHAIN_BOTH ? 0 : MT_DIGITAL_TENTHS * gain->digital);
    return 0;
}

/* Overall-gain steps in ascending order: LNA 0..5 at LPF 0, then LPF 1..8 at
 * LNA 5, then (single-chain only) digital 1..12 at 5/8. Fills at most
 * `capacity` entries and returns the total count.
 */
int mt_gain_steps(unsigned chain, struct bs_gain_step *steps, size_t capacity)
{
    struct birriasdr_gain gain = {0, 0, 0};
    unsigned digital_max = chain == BIRRIASDR_CHAIN_BOTH ? 0 : MT_GAIN_DIGITAL_MAX;
    int count = 0;
    if (chain > BIRRIASDR_CHAIN_BOTH)
        return BIRRIASDR_ERROR_INVALID;
    for (unsigned i = 0; i <= MT_GAIN_LNA_MAX + MT_GAIN_LPF_MAX + digital_max; ++i) {
        if (i <= MT_GAIN_LNA_MAX)
            gain = (struct birriasdr_gain){(uint8_t)i, 0, 0};
        else if (i <= MT_GAIN_LNA_MAX + MT_GAIN_LPF_MAX)
            gain.lpf = (uint8_t)(i - MT_GAIN_LNA_MAX);
        else
            gain.digital = (uint8_t)(i - MT_GAIN_LNA_MAX - MT_GAIN_LPF_MAX);
        if ((size_t)count < capacity) {
            steps[count].gain = gain;
            BS_TRY(mt_gain_tenths_db(chain, &gain, &steps[count].tenths_db));
        }
        ++count;
    }
    return count;
}
