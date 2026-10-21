/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * Host 2:1 halfband decimator for the 20 MSps output rate
 *
 * The capture always runs at the backend's 40 MSps; birriasdr_set_sample_rate()
 * with half that rate keeps the hardware unchanged and filters on the host.
 * The 47-tap equiripple halfband (Q15, unity DC gain) is flat to 8.5 MHz
 * (0.013 dB ripple), -6 dB at 10 MHz and at least 57.8 dB down from 11.5 MHz,
 * so content from 10 to 11.5 MHz folds into 8.5-10 MHz attenuated and the
 * usable span is about +/-8.5 MHz. Pair it with the 20 MHz bandwidth profile
 * and the channel filter to keep strong out-of-span signals out of the input.
 *
 * Output sample j is centered on input sample 2j - 23 (a fixed 23-sample,
 * 575 ns delay). History starts as zeros, so the first 23 outputs after a
 * reset carry the filter's start transient. The stream resets the filter at
 * every acquisition start and continuity gap, so no output mixes samples from
 * either side of a gap. Accumulation is 32-bit: the sum of tap magnitudes is
 * 54,700 in Q15, so a full-scale int16 input stays below 2^31.
 */
#include "internal.h"
#include <string.h>

/* Odd-offset taps k = 1, 3, ..., 23 (symmetric); the center tap is 1/2. */
#define HALFBAND_PAIRS 12u
_Static_assert(4 * HALFBAND_PAIRS - 2 == BS_HALFBAND_HISTORY, "history spans the filter");
static const int32_t halfband_taps[HALFBAND_PAIRS] = {
    10399, -3351, 1878, -1209, 817, -557, 376, -247, 156, -92, 49, -27
};

void bs_halfband_reset(struct bs_halfband *filter)
{
    memset(filter->x, 0, sizeof(filter->x[0]) * 2 * BS_HALFBAND_HISTORY);
}

static int16_t saturate(int32_t accumulator)
{
    accumulator = (accumulator + (1 << 14)) >> 15;
    if (accumulator > INT16_MAX) return INT16_MAX;
    if (accumulator < INT16_MIN) return INT16_MIN;
    return (int16_t)accumulator;
}

/* Filter count interleaved I,Q input samples (count even, at most one packet)
 * into count / 2 output samples, then keep the last 46 inputs as history.
 */
void bs_halfband_run(struct bs_halfband *filter, const int16_t *iq, size_t count, int16_t *out)
{
    int16_t *x = filter->x;
    memcpy(x + 2 * BS_HALFBAND_HISTORY, iq, count * 2 * sizeof(*iq));
    for (size_t j = 0; j < count / 2; ++j) {
        const int16_t *center = x + 2 * (2 * j + BS_HALFBAND_HISTORY / 2);
        int32_t i_sum = (int32_t)center[0] << 14, q_sum = (int32_t)center[1] << 14;
        for (unsigned t = 0; t < HALFBAND_PAIRS; ++t) {
            ptrdiff_t k = 2 * (2 * (ptrdiff_t)t + 1);
            i_sum += halfband_taps[t] * ((int32_t)center[-k] + center[k]);
            q_sum += halfband_taps[t] * ((int32_t)center[1 - k] + center[1 + k]);
        }
        out[2 * j] = saturate(i_sum);
        out[2 * j + 1] = saturate(q_sum);
    }
    memmove(x, x + 2 * count, sizeof(*x) * 2 * BS_HALFBAND_HISTORY);
}
