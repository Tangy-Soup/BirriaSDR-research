/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */
#include "internal.h"
#include "chips/mt7921.h"
#include "chips/mt7921_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)

/* Count parser deliveries without depending on DSP or external USB hardware. */
static int payload(void *context, const uint8_t *raw)
{
    unsigned *count = context;
    int16_t iq[BS_PACKET_SAMPLES * 2];
    CHECK(bs_decode_iq(raw, iq, BS_PACKET_SAMPLES) == 0);
    ++*count;
    return 0;
}

/* Construct a protocol fixture independently of any USB transfer boundaries. */
static void make_frame(uint8_t frame[BS_FRAME_BYTES], uint32_t sequence)
{
    memset(frame, 0, BS_FRAME_BYTES);
    bs_put_le32(frame, 0x38000000u | (BS_FRAME_BYTES - 4));
    bs_put_le32(frame + 24, 0x38000014u + BS_PAYLOAD_BYTES);
    frame[28] = 0xed;
    frame[32] = 0xf5;
    bs_put_le32(frame + 36, sequence);
    bs_put_le32(frame + 40, 0x12345678);
    bs_put_le32(frame + BS_FRAME_BYTES - 4, 0xffffffff); /* Trailer is not I/Q. */
}

/* Every split point must produce exactly one packet, including bytewise headers.
 * Then verify gaps, stale nonces, duplicates, truncation and malformed lengths.
 */
static void test_framing(void)
{
    uint8_t frame[BS_FRAME_BYTES];
    make_frame(frame, 0);
    for (size_t split = 0; split <= sizeof(frame); ++split) {
        unsigned count = 0;
        struct bs_framer f = {.nonce = 0x12345678, .payload = payload, .context = &count};
        CHECK(bs_framer_feed(&f, frame, split) == 0);
        CHECK(bs_framer_feed(&f, frame + split, sizeof(frame) - split) == 0);
        CHECK(bs_framer_finish(&f, 1) == 0 && count == 1);
        CHECK(bs_framer_feed(&f, frame, sizeof(frame)) == BIRRIASDR_ERROR_PROTOCOL);
    }
    for (unsigned kind = 0; kind < 4; ++kind) {
        unsigned count = 0;
        struct bs_framer f = {.nonce = 0x12345678, .payload = payload, .context = &count};
        make_frame(frame, 0);
        if (kind == 0) frame[36] = 1;
        if (kind == 1) frame[40] ^= 1;
        if (kind == 2) frame[0] = 0;
        if (kind == 3) frame[32] = 0xf4;
        CHECK(bs_framer_feed(&f, frame, sizeof(frame)) == BIRRIASDR_ERROR_PROTOCOL);
        CHECK(count == 0);
    }
    {
        unsigned count = 0;
        struct bs_framer f = {.nonce = 0x12345678, .payload = payload, .context = &count};
        make_frame(frame, 0);
        CHECK(bs_framer_feed(&f, frame, sizeof(frame) - 1) == 0);
        CHECK(bs_framer_finish(&f, 1) == BIRRIASDR_ERROR_PROTOCOL);
    }
}

/* Known signed extrema catch I/Q reversal, sign-extension and accidental scaling. */
static void test_samples(void)
{
    uint8_t raw[8] = {0xff, 0x1f, 0x00, 0x20, 0xff, 0x3f, 0x01, 0x00};
    int16_t iq[4];
    CHECK(bs_decode_iq(raw, iq, 2) == 0);
    CHECK(iq[0] == -8192 && iq[1] == 8191 && iq[2] == 1 && iq[3] == -1);
    raw[7] = 0x40;
    CHECK(bs_decode_iq(raw, iq, 2) == BIRRIASDR_ERROR_PROTOCOL);
}

/* Verify the embedded original image's PDA trailer, independently of USB ROM. */
static void test_image(void)
{
    uint32_t state = 0;
    CHECK(sizeof(mt7921_image) == 1120);
    for (size_t i = 0; i < sizeof(mt7921_image); i += 4) {
        uint32_t bits = state & 0x10921111, parity = 0;
        while (bits) { parity ^= bits & 1; bits >>= 1; }
        state = (state >> 1) ^ parity << 31 ^ bs_get_le32(mt7921_image + i);
    }
    CHECK(state == 0);
}

/* Unsupported chip/control paths must fail without USB access or pretend values. */
static void test_api(void)
{
    birriasdr_dev_t dev = {0};
    const struct bs_usb_id *id = bs_lookup_id(0x0e8d, 0x7925);
    struct birriasdr_gain gain = {2, 8, 0};
    atomic_init(&dev.run_state, BS_IDLE);
    dev.backend = &bs_mt7921_backend;
    dev.frequency = dev.backend->default_frequency;
    dev.rate = dev.backend->default_rate;
    CHECK(id && id->backend->chipset == BIRRIASDR_CHIP_MT7925 && !id->qualified);
    CHECK(id->backend->prepare(&dev, 0) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_tuner_gain(&dev, 100) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_agc_mode(&dev, 1) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_tuner_bandwidth(&dev, 40000000) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(5180000000)) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_STATE);
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 1) == 0);
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == 0);
    gain.lna = 9;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_cancel_async(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(atomic_load(&dev.run_state) == BS_IDLE);
    atomic_store(&dev.run_state, BS_RUNNING);
    CHECK(birriasdr_cancel_async(&dev) == 0);
    CHECK(atomic_load(&dev.run_state) == BS_CANCELLING);
    CHECK(birriasdr_cancel_async(&dev) == 0);
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 0) == BIRRIASDR_ERROR_BUSY);
    CHECK(birriasdr_close(&dev) == BIRRIASDR_ERROR_BUSY);
}

/* Tests deliberately run without a dongle, root privileges or vendor firmware. */
int main(void)
{
    test_framing();
    test_samples();
    test_image();
    test_api();
    puts("core tests passed");
    return 0;
}
