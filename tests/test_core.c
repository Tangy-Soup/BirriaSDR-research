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

struct gap_observation { unsigned count; uint32_t state; uint64_t packets; };

/* Markers carry no samples and must precede the next packet at this sequence. */
static int observe_gap(void *context, uint32_t state, uint64_t packets)
{
    struct gap_observation *o = context;
    ++o->count;
    o->state = state;
    o->packets = packets;
    return 0;
}

/* Exercise every marker split, strict readers, duplicates, missing markers,
 * invalid reasons and both wrapping counters. A marker never advances the
 * data packet count or bypasses the acquisition nonce check.
 */
static void test_gap_events(void)
{
    uint8_t marker[48] = {0};
    bs_put_le32(marker, 0x3800002c);
    bs_put_le32(marker + 8, 9);
    bs_put_le32(marker + 12, 1);
    bs_put_le32(marker + 24, 0x38000014);
    marker[28] = 0xed;
    marker[32] = 0xf6;
    bs_put_le32(marker + 36, 3);
    bs_put_le32(marker + 40, 0x12345678);
    for (size_t split = 0; split <= sizeof(marker); ++split) {
        struct gap_observation o = {0};
        struct bs_framer f = {.nonce = 0x12345678, .packets = 3,
                              .gap = observe_gap, .context = &o};
        CHECK(bs_framer_feed(&f, marker, split) == 0);
        CHECK(bs_framer_feed(&f, marker + split, sizeof(marker) - split) == 0);
        CHECK(o.count == 1 && o.state == 9 && o.packets == 3);
        CHECK(f.packets == 3 && f.gaps == 1 && bs_framer_finish(&f, 3) == 0);
        CHECK(bs_framer_feed(&f, marker, sizeof(marker)) == BIRRIASDR_ERROR_PROTOCOL);
    }
    const unsigned corrupt[] = {8, 12, 24, 28, 32, 36, 40};
    for (unsigned n = 0; n < sizeof(corrupt) / sizeof(corrupt[0]); ++n) {
        struct gap_observation o = {0};
        struct bs_framer f = {.nonce = 0x12345678, .packets = 3,
                              .gap = observe_gap, .context = &o};
        marker[corrupt[n]] ^= 2;
        CHECK(bs_framer_feed(&f, marker, sizeof(marker)) == BIRRIASDR_ERROR_PROTOCOL);
        CHECK(!o.count);
        marker[corrupt[n]] ^= 2;
    }
    struct bs_framer strict = {.nonce = 0x12345678, .packets = 3};
    CHECK(bs_framer_feed(&strict, marker, sizeof(marker)) == BIRRIASDR_ERROR_PROTOCOL);
    struct gap_observation o = {0};
    struct bs_framer f = {.nonce = 0x12345678, .packets = UINT64_C(1) << 32,
                          .gaps = UINT32_MAX, .gap = observe_gap, .context = &o};
    bs_put_le32(marker + 8, 8);
    bs_put_le32(marker + 12, 0);
    bs_put_le32(marker + 36, 0);
    CHECK(bs_framer_feed(&f, marker, sizeof(marker)) == 0);
    CHECK(o.count == 1 && o.state == 8 && o.packets == (UINT64_C(1) << 32));
    CHECK(f.gaps == 0);
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

/* A start acknowledgment may arrive fragmented beside sample events. Reject
 * duplicate/nonzero ACKs and carry packet sequence through its wire rollover.
 */
static void test_events(void)
{
    uint8_t ack[40] = {0}, frame[BS_FRAME_BYTES];
    unsigned count = 0;
    struct bs_framer framer = {.nonce = 0x12345678, .payload = payload, .context = &count};
    bs_put_le32(ack, 0x38000024);
    ack[28] = 1;
    CHECK(bs_framer_feed(&framer, ack, 17) == 0);
    CHECK(bs_framer_feed(&framer, ack + 17, 23) == 0);
    make_frame(frame, 0);
    CHECK(bs_framer_feed(&framer, frame, sizeof(frame)) == 0);
    CHECK(framer.acknowledgements == 1 && count == 1);
    CHECK(bs_framer_feed(&framer, ack, sizeof(ack)) == BIRRIASDR_ERROR_PROTOCOL);
    framer = (struct bs_framer){.nonce = 0x12345678, .payload = payload, .context = &count};
    ack[32] = 1;
    CHECK(bs_framer_feed(&framer, ack, sizeof(ack)) == BIRRIASDR_ERROR_PROTOCOL);
    framer = (struct bs_framer){.nonce = 0x12345678, .payload = payload, .context = &count,
                              .packets = UINT32_MAX};
    make_frame(frame, UINT32_MAX);
    CHECK(bs_framer_feed(&framer, frame, sizeof(frame)) == 0);
    make_frame(frame, 0);
    CHECK(bs_framer_feed(&framer, frame, sizeof(frame)) == 0);
    CHECK(bs_framer_finish(&framer, UINT64_C(4294967297)) == 0);
}

/* Every signed 14-bit component must survive decode, including both extrema.
 * Complement Q so reversing the two components cannot accidentally pass.
 */
static void test_all_sample_values(void)
{
    uint8_t raw[4];
    int16_t iq[2];
    for (uint32_t value = 0; value < 16384; ++value) {
        uint32_t q = 16383 - value;
        int expected_i = value < 8192 ? (int)value : (int)value - 16384;
        int expected_q = q < 8192 ? (int)q : (int)q - 16384;
        bs_put_le32(raw, value << 16 | q);
        CHECK(bs_decode_iq(raw, iq, 1) == 0);
        CHECK(iq[0] == expected_i && iq[1] == expected_q);
    }
}

/* Verify the embedded original image's PDA trailer, independently of USB ROM. */
static void test_image(void)
{
    uint32_t state = 0;
    CHECK(sizeof(mt7921_image) == 1664);
    for (size_t i = 0; i < sizeof(mt7921_image); i += 4) {
        uint32_t bits = state & 0x10921111, parity = 0;
        while (bits) { parity ^= bits & 1; bits >>= 1; }
        state = (state >> 1) ^ parity << 31 ^ bs_get_le32(mt7921_image + i);
    }
    CHECK(state == 0);
}

/* The overall-gain table is the measured one, ascending, per receive chain; a
 * request commits the nearest step's codes (the lower step on a tie) and only
 * in manual mode, like set_gain_codes.
 */
static void test_gain_db(void)
{
    birriasdr_dev_t dev = {0};
    struct birriasdr_gain gain;
    int gains[32], manual, tenths;
    atomic_init(&dev.run_state, BS_IDLE);
    CHECK(pthread_mutex_init(&dev.control_mutex, NULL) == 0);
    dev.backend = &bs_mt7921_backend;
    dev.frequency = dev.backend->default_frequency;
    dev.rate = dev.backend->default_rate;
    atomic_init(&dev.receive_chain, BIRRIASDR_CHAIN_0);
    CHECK(birriasdr_get_capabilities(&dev) & BIRRIASDR_CAP_GAIN_DB);
    CHECK(birriasdr_get_tuner_gains(NULL, NULL, 0) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_tuner_gains(&dev, NULL, 4) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_tuner_gains(&dev, NULL, 0) == 26);
    CHECK(birriasdr_get_tuner_gains(&dev, gains, 3) == 26 && gains[0] == 0 && gains[2] == 138);
    for (int chain = BIRRIASDR_CHAIN_0; chain <= BIRRIASDR_CHAIN_BOTH; ++chain) {
        int count;
        atomic_store(&dev.receive_chain, (unsigned)chain);
        count = birriasdr_get_tuner_gains(&dev, gains, 32);
        CHECK(count == (chain == BIRRIASDR_CHAIN_BOTH ? 14 : 26) && gains[0] == 0);
        for (int i = 1; i < count; ++i)
            CHECK(gains[i] > gains[i - 1]);
        CHECK(gains[count - 1] == (chain == BIRRIASDR_CHAIN_0 ? 594 : chain == BIRRIASDR_CHAIN_1 ? 560 : 474));
    }
    atomic_store(&dev.receive_chain, BIRRIASDR_CHAIN_0);
    CHECK(birriasdr_set_tuner_gain(NULL, 0) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_set_tuner_gain(&dev, 200) == BIRRIASDR_ERROR_STATE);
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 1) == 0);
    CHECK(birriasdr_set_tuner_gain(&dev, 200) == 0);
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0 && manual);
    CHECK(gain.lna == 3 && gain.lpf == 0 && gain.digital == 0);
    CHECK(birriasdr_get_tuner_gain(&dev, &tenths) == 0 && tenths == 197);
    CHECK(birriasdr_set_tuner_gain(&dev, 288) == 0);   /* tie between 258 and 318 */
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0 && gain.lna == 4);
    CHECK(birriasdr_set_tuner_gain(&dev, -1000) == 0);
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0 && !gain.lna && !gain.lpf && !gain.digital);
    CHECK(birriasdr_set_tuner_gain(&dev, 10000) == 0);
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0);
    CHECK(gain.lna == 5 && gain.lpf == 8 && gain.digital == 12);
    CHECK(birriasdr_get_tuner_gain(&dev, &tenths) == 0 && tenths == 594);
    /* Arbitrary codes use the separable stage table; both-chain skips digital. */
    gain = (struct birriasdr_gain){2, 8, 6};
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == 0);
    CHECK(birriasdr_get_tuner_gain(&dev, &tenths) == 0 && tenths == 138 + 156 + 60);
    atomic_store(&dev.receive_chain, BIRRIASDR_CHAIN_1);
    CHECK(birriasdr_get_tuner_gain(&dev, &tenths) == 0 && tenths == 144 + 110 + 60);
    atomic_store(&dev.receive_chain, BIRRIASDR_CHAIN_BOTH);
    CHECK(birriasdr_get_tuner_gain(&dev, &tenths) == 0 && tenths == 138 + 156);
    CHECK(birriasdr_set_tuner_gain(&dev, 10000) == 0);
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0 && gain.lna == 5 && gain.lpf == 8 && !gain.digital);
    CHECK(birriasdr_get_tuner_gain(NULL, &tenths) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_tuner_gain(&dev, NULL) == BIRRIASDR_ERROR_INVALID);
    gain = (struct birriasdr_gain){6, 0, 0};
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_INVALID);
    atomic_store(&dev.run_state, BS_RUNNING);
    CHECK(birriasdr_set_tuner_gain(&dev, 0) == BIRRIASDR_ERROR_BUSY);   /* no control window */
    atomic_store(&dev.run_state, BS_IDLE);
    pthread_mutex_destroy(&dev.control_mutex);
}

/* Chain 0 at 5 GHz takes LPF 9..15; every other band and chain keeps 0..8, and
 * a retune or chain change that would strand staged codes is refused unchanged.
 */
static void test_gain_5g_codes(void)
{
    birriasdr_dev_t dev = {0};
    struct birriasdr_gain gain, maximum;
    int gains[64], manual, count;   /* BS_MAX_GAIN_STEPS (device.c) */
    atomic_init(&dev.run_state, BS_IDLE);
    CHECK(pthread_mutex_init(&dev.control_mutex, NULL) == 0);
    dev.backend = &bs_mt7921_backend;
    dev.frequency = UINT64_C(5240000000);
    dev.rate = dev.backend->default_rate;
    atomic_init(&dev.receive_chain, BIRRIASDR_CHAIN_0);
    CHECK(birriasdr_get_gain_code_limits(NULL, &maximum) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_gain_code_limits(&dev, NULL) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_gain_code_limits(&dev, &maximum) == 0);
    CHECK(maximum.lna == 5 && maximum.lpf == 15 && maximum.digital == 12);
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 1) == 0);
    gain = (struct birriasdr_gain){5, 14, 0};
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == 0);
    gain.lpf = 16;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_INVALID);
    /* The 5 GHz overall table extends past LPF 8 and stays ascending. */
    count = birriasdr_get_tuner_gains(&dev, gains, 64);
    CHECK(count > 18 && count <= 64 && gains[0] == 0);
    for (int i = 1; i < count; ++i)
        CHECK(gains[i] > gains[i - 1]);
    CHECK(birriasdr_set_tuner_gain(&dev, 10000) == 0);
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0 && gain.lna == 5 && gain.lpf > 8 && gain.digital == 12);
    gain = (struct birriasdr_gain){5, 14, 0};
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == 0);
    /* LPF 14 is valid only on chain 0 at 5 GHz. While idle, leaving that band
     * or chain lowers it to 8; a live retune that would need it is refused.
     */
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(5785000000)) == 0);   /* same band */
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0 && gain.lpf == 14);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2437000000)) == 0);
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0 && gain.lna == 5 && gain.lpf == 8);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(5240000000)) == 0);
    gain.lpf = 14;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == 0);
    CHECK(birriasdr_set_receive_chain(&dev, BIRRIASDR_CHAIN_BOTH) == 0);
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0 && gain.lpf == 8);
    CHECK(birriasdr_set_receive_chain(&dev, BIRRIASDR_CHAIN_0) == 0);
    gain.lpf = 14;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == 0);
    /* Released mode keeps staged codes; a retune still fits them. */
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 0) == 0);
    CHECK(birriasdr_set_receive_chain(&dev, BIRRIASDR_CHAIN_1) == 0);
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0 && !manual && gain.lpf == 8);
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 1) == 0);
    /* During capture the staged codes are live: no silent change, INVALID. */
    CHECK(birriasdr_set_receive_chain(&dev, BIRRIASDR_CHAIN_0) == 0);
    gain.lpf = 14;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == 0);
    atomic_store(&dev.run_state, BS_RUNNING);
    dev.control_ready = true;
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2437000000)) == BIRRIASDR_ERROR_INVALID);
    CHECK(dev.frequency == UINT64_C(5240000000));
    CHECK(birriasdr_get_gain_codes(&dev, &gain, &manual) == 0 && gain.lpf == 14);
    atomic_store(&dev.run_state, BS_IDLE);
    dev.control_ready = false;
    gain.lpf = 8;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == 0);
    CHECK(birriasdr_set_receive_chain(&dev, BIRRIASDR_CHAIN_1) == 0);
    CHECK(birriasdr_get_gain_code_limits(&dev, &maximum) == 0 && maximum.lpf == 8);
    gain.lpf = 9;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_tuner_gains(&dev, NULL, 0) == 26);
    CHECK(birriasdr_set_receive_chain(&dev, BIRRIASDR_CHAIN_0) == 0);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2437000000)) == 0);
    CHECK(birriasdr_get_gain_code_limits(&dev, &maximum) == 0 && maximum.lpf == 8);
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_tuner_gains(&dev, NULL, 0) == 26);
    pthread_mutex_destroy(&dev.control_mutex);
}

/* Unsupported chip/control paths must fail without USB access or pretend values. */
static void test_api(void)
{
    birriasdr_dev_t dev = {0};
    const struct bs_usb_id *id = bs_lookup_id(0x0e8d, 0x7925);
    struct birriasdr_gain gain = {2, 8, 0};
    uint64_t minimum, maximum;
    atomic_init(&dev.run_state, BS_IDLE);
    CHECK(pthread_mutex_init(&dev.control_mutex, NULL) == 0);
    dev.backend = &bs_mt7921_backend;
    dev.frequency = dev.backend->default_frequency;
    dev.rate = dev.backend->default_rate;
    atomic_init(&dev.bandwidth, dev.backend->default_bandwidth);
    atomic_init(&dev.packet_detect_suppression, 0);
    atomic_init(&dev.receive_chain, 0);
    atomic_init(&dev.channel_filter, 0);
    CHECK(id && id->backend->chipset == BIRRIASDR_CHIP_MT7925 && !id->qualified);
    dev.backend = id->backend;
    CHECK(birriasdr_set_packet_detect_suppression(&dev, 1) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_get_packet_detect_suppression(&dev) == 0);
    CHECK(birriasdr_set_receive_chain(&dev, BIRRIASDR_CHAIN_1) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_get_receive_chain(&dev) == BIRRIASDR_CHAIN_0);
    CHECK(birriasdr_set_channel_filter(&dev, 1) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_get_channel_filter(&dev) == 0);
    dev.backend = &bs_mt7921_backend;
    CHECK(id->backend->prepare(&dev, 0) == BIRRIASDR_ERROR_UNSUPPORTED);
    dev.backend = id->backend;
    CHECK(birriasdr_set_tuner_gain(&dev, 100) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_get_tuner_gains(&dev, NULL, 0) == BIRRIASDR_ERROR_UNSUPPORTED);
    dev.backend = &bs_mt7921_backend;
    CHECK(birriasdr_set_agc_mode(&dev, 1) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_get_tuner_bandwidth(NULL) == 0);
    CHECK(birriasdr_set_tuner_bandwidth(NULL, 20000000) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_tuner_bandwidth(&dev) == 40000000);
    CHECK(birriasdr_set_tuner_bandwidth(&dev, 20000000) == 0);
    CHECK(birriasdr_get_tuner_bandwidth(&dev) == 20000000);
    CHECK(birriasdr_set_tuner_bandwidth(&dev, 30000000) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_get_tuner_bandwidth(&dev) == 20000000);
    CHECK(birriasdr_set_tuner_bandwidth(&dev, 40000000) == 0);
    CHECK(birriasdr_get_capabilities(&dev) & BIRRIASDR_CAP_PD_SUPPRESS);
    CHECK(birriasdr_get_packet_detect_suppression(NULL) == 0);
    CHECK(birriasdr_set_packet_detect_suppression(NULL, 1) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_packet_detect_suppression(&dev) == 0);
    CHECK(birriasdr_set_packet_detect_suppression(&dev, 7) == 0);
    CHECK(birriasdr_get_packet_detect_suppression(&dev) == 1);
    CHECK(birriasdr_get_capabilities(&dev) & BIRRIASDR_CAP_CHAIN_SELECT);
    CHECK(birriasdr_get_receive_chain(NULL) == BIRRIASDR_CHAIN_0);
    CHECK(birriasdr_set_receive_chain(NULL, BIRRIASDR_CHAIN_1) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_set_receive_chain(&dev, 3) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_set_receive_chain(&dev, -1) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_receive_chain(&dev) == BIRRIASDR_CHAIN_0);
    CHECK(birriasdr_set_receive_chain(&dev, BIRRIASDR_CHAIN_BOTH) == 0);
    CHECK(birriasdr_get_receive_chain(&dev) == BIRRIASDR_CHAIN_BOTH);
    CHECK(birriasdr_set_receive_chain(&dev, BIRRIASDR_CHAIN_1) == 0);
    CHECK(birriasdr_get_receive_chain(&dev) == BIRRIASDR_CHAIN_1);
    CHECK(birriasdr_get_capabilities(&dev) & BIRRIASDR_CAP_CHANNEL_FILTER);
    CHECK(birriasdr_get_channel_filter(NULL) == 0);
    CHECK(birriasdr_set_channel_filter(NULL, 1) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_set_channel_filter(&dev, 3) == 0);
    CHECK(birriasdr_get_channel_filter(&dev) == 1);
    /* Half the hardware rate is host decimation; nothing else is invented. */
    dev.decimation = 1;
    CHECK(birriasdr_get_sample_rate(&dev) == 40000000);
    CHECK(birriasdr_set_sample_rate(&dev, 20000000) == 0);
    CHECK(birriasdr_get_sample_rate(&dev) == 20000000 && dev.rate == 40000000 && dev.decimation == 2);
    CHECK(birriasdr_set_sample_rate(&dev, 10000000) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_sample_rate(&dev, 0) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_get_sample_rate(&dev) == 20000000);
    CHECK(birriasdr_set_sample_rate(&dev, 40000000) == 0);
    CHECK(birriasdr_get_sample_rate(&dev) == 40000000 && dev.decimation == 1);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(3000000000)) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(4499999999)) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(6200000001)) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(5180000000)) == 0);
    CHECK(birriasdr_get_center_freq(&dev) == UINT64_C(5180000000));
    CHECK(birriasdr_get_tuning_range(&dev, &minimum, &maximum) == 0);
    CHECK(minimum == UINT64_C(2156000000) && maximum == UINT64_C(6200000000));
    CHECK(birriasdr_get_tuning_band(&dev, 1, &minimum, &maximum) == 0);
    CHECK(minimum == UINT64_C(4500000000) && maximum == UINT64_C(6200000000));
    CHECK(birriasdr_set_center_freq(&dev, minimum) == 0);
    CHECK(birriasdr_set_center_freq(&dev, maximum) == 0);
    CHECK(birriasdr_get_tuning_band(&dev, 2, &minimum, &maximum) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_get_tuning_band(&dev, 0, &minimum, &maximum) == 0);
    CHECK(birriasdr_get_capabilities(&dev) & BIRRIASDR_CAP_TUNING);
    CHECK(minimum == UINT64_C(2156000000) && maximum == UINT64_C(2610000000));
    CHECK(birriasdr_set_center_freq(&dev, minimum) == 0);
    CHECK(birriasdr_set_center_freq(&dev, maximum) == 0);
    CHECK(birriasdr_set_center_freq(&dev, minimum - 1) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_center_freq(&dev, maximum + 1) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_get_center_freq(&dev) == maximum);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2437123456)) == 0);
    CHECK(birriasdr_get_center_freq(&dev) == UINT64_C(2437123456));
    id = bs_lookup_id(0x0e8d, 0x7961);
    CHECK(id && id->qualified && id->min_frequency == UINT64_C(2190000000) && !id->max_frequency);
    dev.min_frequency = id->min_frequency;
    CHECK(birriasdr_get_tuning_band(&dev, 0, &minimum, &maximum) == 0);
    CHECK(minimum == UINT64_C(2190000000) && maximum == UINT64_C(2610000000));
    CHECK(birriasdr_get_tuning_band(&dev, 1, &minimum, &maximum) == 0);
    CHECK(minimum == UINT64_C(4500000000) && maximum == UINT64_C(6200000000));
    dev.max_frequency = UINT64_C(2610000000);
    CHECK(birriasdr_get_tuning_band(&dev, 1, &minimum, &maximum) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(5180000000)) == BIRRIASDR_ERROR_UNSUPPORTED);
    dev.max_frequency = 0;
    CHECK(birriasdr_get_tuning_range(&dev, &minimum, &maximum) == 0);
    CHECK(minimum == UINT64_C(2190000000) && maximum == UINT64_C(6200000000));
    CHECK(birriasdr_set_center_freq(&dev, minimum - 1) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_get_center_freq(&dev) == UINT64_C(2437123456));
    CHECK(birriasdr_set_center_freq(&dev, minimum) == 0);
    dev.min_frequency = 0;
    CHECK(birriasdr_get_tuning_range(&dev, &minimum, &maximum) == 0);
    CHECK(minimum == UINT64_C(2156000000));
    CHECK(birriasdr_set_sample_rate(&dev, 0) == BIRRIASDR_ERROR_UNSUPPORTED);
    /* 80 MSps is a native (experimental) rate; nothing above it is invented. */
    CHECK(birriasdr_set_sample_rate(&dev, 80000000) == 0);
    CHECK(birriasdr_get_sample_rate(&dev) == 80000000 && dev.rate == 80000000 && dev.decimation == 1);
    CHECK(birriasdr_set_sample_rate(&dev, 160000000) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_get_sample_rate(&dev) == 80000000);
    /* The BW80 graft is 2.4 GHz only, whichever of rate and center comes first. */
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(5180000000)) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_sample_rate(&dev, 40000000) == 0);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(5180000000)) == 0);
    CHECK(birriasdr_set_sample_rate(&dev, 80000000) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2437000000)) == 0);
    CHECK(birriasdr_set_sample_rate(&dev, 40000000) == 0);
    CHECK(birriasdr_get_sample_rate(&dev) == 40000000);
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_STATE);
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 1) == 0);
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == 0);
    gain.lna = 5;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == 0);
    gain.lna = 6;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_INVALID);
    gain.lna = 9;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_cancel_async(&dev) == BIRRIASDR_ERROR_STATE);
    CHECK(atomic_load(&dev.run_state) == BS_IDLE);
    atomic_store(&dev.run_state, BS_RUNNING);
    CHECK(birriasdr_cancel_async(&dev) == 0);
    CHECK(atomic_load(&dev.run_state) == BS_CANCELLING);
    CHECK(birriasdr_cancel_async(&dev) == 0);
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 0) == BIRRIASDR_ERROR_BUSY);
    CHECK(birriasdr_set_center_freq(&dev, minimum) == BIRRIASDR_ERROR_BUSY);
    CHECK(birriasdr_set_tuner_bandwidth(&dev, 20000000) == BIRRIASDR_ERROR_BUSY);
    CHECK(birriasdr_get_tuner_bandwidth(&dev) == 40000000);
    CHECK(birriasdr_set_packet_detect_suppression(&dev, 0) == BIRRIASDR_ERROR_BUSY);
    CHECK(birriasdr_get_packet_detect_suppression(&dev) == 1);
    CHECK(birriasdr_set_receive_chain(&dev, BIRRIASDR_CHAIN_0) == BIRRIASDR_ERROR_BUSY);
    CHECK(birriasdr_get_receive_chain(&dev) == BIRRIASDR_CHAIN_1);
    CHECK(birriasdr_set_channel_filter(&dev, 0) == BIRRIASDR_ERROR_BUSY);
    CHECK(birriasdr_get_channel_filter(&dev) == 1);
    CHECK(birriasdr_close(&dev) == BIRRIASDR_ERROR_BUSY);
    CHECK(pthread_mutex_destroy(&dev.control_mutex) == 0);
}

/* Tests deliberately run without a dongle, root privileges or vendor firmware. */
int main(void)
{
    test_framing();
    test_gap_events();
    test_samples();
    test_events();
    test_all_sample_values();
    test_image();
    test_api();
    test_gain_db();
    test_gain_5g_codes();
    puts("core tests passed");
    return 0;
}
