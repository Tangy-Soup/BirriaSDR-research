/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/* Test the 2:1 halfband against a direct double-precision convolution with the
 * same taps: exact DC gain, passband and stopband tones, packet-by-packet
 * continuity, saturation, and reset. No USB or physical access.
 */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(t) do { if (!(t)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #t); exit(1); } } while (0)
#define PACKETS 8u
#define TOTAL (PACKETS * BS_PACKET_SAMPLES)

static const double pi = 3.14159265358979323846;
static int16_t input[2 * TOTAL], output[TOTAL];


/* Fill a complex tone at frequency_hz (40 MSps) with the given amplitude. */
static void tone(double frequency_hz, double amplitude)
{
    for (unsigned n = 0; n < TOTAL; ++n) {
        double phase = 2 * pi * frequency_hz * n / 40e6;
        input[2 * n] = (int16_t)lround(amplitude * cos(phase));
        input[2 * n + 1] = (int16_t)lround(amplitude * sin(phase));
    }
}

static void run(struct bs_halfband *filter)
{
    bs_halfband_reset(filter);
    for (unsigned p = 0; p < PACKETS; ++p)
        bs_halfband_run(filter, input + 2 * p * BS_PACKET_SAMPLES, BS_PACKET_SAMPLES,
                        output + p * BS_PACKET_SAMPLES);
}

/* RMS output magnitude after the start transient, relative to amplitude. */
static double gain_db(double amplitude)
{
    double power = 0;
    unsigned count = 0;
    for (unsigned j = 64; j < TOTAL / 2; ++j, ++count)
        power += (double)output[2 * j] * output[2 * j] + (double)output[2 * j + 1] * output[2 * j + 1];
    return 10 * log10(power / count / (amplitude * amplitude));
}

int main(void)
{
    struct bs_halfband *filter = malloc(sizeof(*filter));
    CHECK(filter);

    /* DC passes exactly once the history is full. */
    for (unsigned n = 0; n < TOTAL; ++n) {
        input[2 * n] = 4000;
        input[2 * n + 1] = -8192;
    }
    run(filter);
    for (unsigned j = 23; j < TOTAL / 2; ++j)
        CHECK(output[2 * j] == 4000 && output[2 * j + 1] == -8192);
    /* The first output only sees one tap side: the transient is visible. */
    CHECK(output[0] != 4000);

    /* Passband flat to 8.5 MHz, -6 dB at 10 MHz, stopband from 11.5 MHz. */
    tone(1e6, 6000);
    run(filter);
    CHECK(fabs(gain_db(6000)) < 0.05);
    tone(-8.5e6, 6000);
    run(filter);
    CHECK(fabs(gain_db(6000)) < 0.05);
    tone(10e6, 6000);
    run(filter);
    CHECK(fabs(gain_db(6000) + 6.0) < 0.3);
    tone(12e6, 8000);
    run(filter);
    CHECK(gain_db(8000) < -55);
    tone(-17e6, 8000);
    run(filter);
    CHECK(gain_db(8000) < -55);

    /* Packet-by-packet output equals a direct convolution over the whole run:
     * output j is centered on input 2j - 23, with zeros before the start.
     */
    srand(7);
    for (unsigned n = 0; n < 2 * TOTAL; ++n)
        input[n] = (int16_t)(rand() % 16384 - 8192);
    run(filter);
    {
        static const int taps[12] = {10399, -3351, 1878, -1209, 817, -557, 376, -247, 156, -92, 49, -27};
        for (unsigned j = 0; j < TOTAL / 2; ++j)
            for (unsigned c = 0; c < 2; ++c) {
                long center = 2 * (long)j - 23;
                long sum = center >= 0 ? 16384L * input[2 * center + c] : 0;
                for (unsigned t = 0; t < 12; ++t) {
                    long k = 2 * t + 1;
                    if (center - k >= 0) sum += (long)taps[t] * input[2 * (center - k) + c];
                    if (center + k >= 0) sum += (long)taps[t] * input[2 * (center + k) + c];
                }
                CHECK(output[2 * j + c] == (int16_t)((sum + 16384) >> 15));
            }
    }

    /* Inputs matching the tap signs sum to 1.67x full scale: saturate, never wrap. */
    memset(input, 0, sizeof(input));
    {
        static const int taps[12] = {10399, -3351, 1878, -1209, 817, -557, 376, -247, 156, -92, 49, -27};
        unsigned center = 2 * 100 - 23;
        input[2 * center] = INT16_MAX;
        input[2 * center + 1] = INT16_MIN;
        for (unsigned t = 0; t < 12; ++t)
            for (int side = -1; side <= 1; side += 2) {
                unsigned n = center + side * (int)(2 * t + 1);
                input[2 * n] = taps[t] > 0 ? INT16_MAX : INT16_MIN;
                input[2 * n + 1] = taps[t] > 0 ? INT16_MIN : INT16_MAX;
            }
        run(filter);
        CHECK(output[200] == INT16_MAX && output[201] == INT16_MIN);
    }

    /* Reset clears history: a second run matches the first exactly. */
    tone(3e6, 5000);
    run(filter);
    {
        static int16_t first[TOTAL];
        memcpy(first, output, sizeof(first));
        bs_halfband_run(filter, input, BS_PACKET_SAMPLES, output);
        run(filter);
        CHECK(!memcmp(first, output, sizeof(first)));
    }
    free(filter);
    puts("Halfband decimator tests passed");
    return 0;
}
