/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */
#include "usb_stream.h"
#include <stdlib.h>
#include <string.h>

struct delivery {
    birriasdr_dev_t *dev;
    birriasdr_read_async_cb_t callback;
    void *context;
    int16_t *iq;
    uint32_t capacity, used;
    uint64_t delivered;
};

/* Deliver only complete validated samples. A short final buffer is allowed;
 * sequence offsets are complex-sample counts, independent of callback sizing.
 */
static void flush_samples(struct delivery *delivery)
{
    if (!delivery->used)
        return;
    delivery->callback(delivery->iq, delivery->used, delivery->delivered, delivery->context);
    delivery->delivered += delivery->used;
    delivery->dev->stats.samples = delivery->delivered;
    delivery->used = 0;
}

/* Reorder raw Q,I into the public I,Q representation and batch callbacks. The
 * firmware payload is always 2048 complex samples; buffer capacity is a multiple.
 */
static int receive_payload(void *context, const uint8_t *raw)
{
    struct delivery *delivery = context;
    BS_TRY(bs_decode_iq(raw, delivery->iq + 2u * delivery->used, BS_PACKET_SAMPLES));
    delivery->used += BS_PACKET_SAMPLES;
    if (delivery->used == delivery->capacity)
        flush_samples(delivery);
    return 0;
}

/* Firmware distinguishes finite completion, host stop, ring ambiguity, overrun
 * and timeout. Never convert a source fault into a successful short capture.
 */
static int validate_terminal(birriasdr_dev_t *dev, const uint32_t words[16],
                             const struct bs_framer *framer, uint32_t limit)
{
    dev->stats.firmware_state = words[7];
    dev->stats.stage = BIRRIASDR_STAGE_VERIFY;
    dev->stats.dma_errors = words[13];
    if (words[7] == 8 || words[7] == 9)
        return BIRRIASDR_ERROR_OVERRUN;
    if (words[7] == 4 || words[7] == 5 || words[7] == 10)
        return BIRRIASDR_ERROR_TIMEOUT;
    if (words[7] != 2 && words[7] != 3)
        return BIRRIASDR_ERROR_STATE;
    if (words[13] || words[10] != words[11])
        return BIRRIASDR_ERROR_PROTOCOL;
    if (words[7] == 2 && (!limit || words[10] != limit))
        return BIRRIASDR_ERROR_PROTOCOL;
    if (words[7] == 3 && atomic_load(&dev->run_state) != BS_CANCELLING)
        return BIRRIASDR_ERROR_STATE;
    return bs_framer_finish(framer, words[10]);
}

/* Consume independently of the USB event pump. EP0 status reads are deferred
 * until a finite run finishes, cancellation is requested, or progress pauses;
 * hammering EP0 during capture would itself compete with the data endpoint.
 */
static int receive_loop(birriasdr_dev_t *dev, struct bs_reader *reader,
                        struct bs_framer *framer, uint32_t limit, uint8_t *bytes)
{
    uint64_t last_progress = bs_now_ns(), next_poll = 0;
    uint32_t words[16] = {0};
    bool stop_sent = false, terminal = false;
    for (;;) {
        size_t length;
        uint64_t now;
        int result;
        if (atomic_load(&dev->run_state) == BS_CANCELLING && !stop_sent) {
            BS_TRY(dev->backend->stop(dev));
            stop_sent = true;
        }
        result = bs_reader_take(reader, bytes, &length, 20);
        if (result != BIRRIASDR_ERROR_TIMEOUT) {
            BS_TRY(result);
            dev->stats.usb_bytes += length;
            BS_TRY(bs_framer_feed(framer, bytes, length));
            dev->stats.packets = framer->packets;
            if (length)
                last_progress = bs_now_ns();
        }
        now = bs_now_ns();
        if (!terminal && now >= next_poll && (stop_sent ||
            (limit && framer->packets >= limit) || now - last_progress > 250000000)) {
            BS_TRY(dev->backend->status(dev, words));
            terminal = words[7] != 1;
            next_poll = now + 50000000;
        }
        if (terminal && framer->packets >= words[10]) {
            /* Drain already queued/trailing events: duplicates must fail too. */
            do {
                result = bs_reader_take(reader, bytes, &length, 20);
                if (result == BIRRIASDR_ERROR_TIMEOUT)
                    break;
                BS_TRY(result);
                dev->stats.usb_bytes += length;
                BS_TRY(bs_framer_feed(framer, bytes, length));
            } while (length);
            dev->stats.packets = framer->packets;
            return validate_terminal(dev, words, framer, limit);
        }
        if (now - last_progress > UINT64_C(3000000000))
            return BIRRIASDR_ERROR_TIMEOUT;
    }
}

/* One lifecycle owns prepare, queued reads, GO, validation and reset. Every
 * failure path drains USB transfers before resetting/freeing their storage.
 * No device reset or implicit firmware fallback is hidden behind an error.
 */
int bs_stream(birriasdr_dev_t *dev, birriasdr_read_async_cb_t callback,
               void *context, uint32_t depth, uint32_t buffer_samples, uint32_t packets)
{
    struct bs_reader *reader = NULL;
    struct delivery delivery = {0};
    struct bs_framer framer = {0};
    uint8_t *bytes = NULL;
    int result;
    unsigned idle = BS_IDLE;
    if (!dev || !callback)
        return BIRRIASDR_ERROR_INVALID;
    if (!depth) depth = 64;
    if (!buffer_samples) buffer_samples = 16384;
    if (depth < 4 || depth > 256 || buffer_samples > 262144 || buffer_samples % BS_PACKET_SAMPLES)
        return BIRRIASDR_ERROR_INVALID;
    if (!atomic_compare_exchange_strong(&dev->run_state, &idle, BS_RUNNING))
        return BIRRIASDR_ERROR_BUSY;
    memset(&dev->stats, 0, sizeof(dev->stats));
    delivery = (struct delivery){dev, callback, context, NULL, buffer_samples, 0, 0};
    delivery.iq = calloc((size_t)buffer_samples * 2, sizeof(*delivery.iq));
    bytes = malloc(BS_USB_BYTES);
    result = BIRRIASDR_ERROR_NO_MEMORY;
    if (!delivery.iq || !bytes)
        goto cleanup;
    result = bs_reader_create(dev, depth, &reader);
    if (result < 0)
        goto cleanup;
    result = dev->backend->prepare(dev, packets);
    if (result < 0)
        goto cleanup;
    framer.nonce = dev->nonce;
    framer.payload = receive_payload;
    framer.context = &delivery;
    result = bs_reader_start(reader);
    if (result < 0)
        goto cleanup;
    result = dev->backend->start(dev);
    if (result < 0)
        goto cleanup;
    dev->stats.stage = BIRRIASDR_STAGE_STREAM;
    result = receive_loop(dev, reader, &framer, packets, bytes);
    flush_samples(&delivery);
cleanup:
    bs_reader_destroy(reader);
    if (dev->touched) {
        dev->stats.reset_error = dev->backend->reset(dev);
        dev->touched = dev->stats.reset_error != 0;
        if (!result)
            result = dev->stats.reset_error;
    }
    free(bytes);
    free(delivery.iq);
    atomic_store(&dev->run_state, BS_IDLE);
    return result;
}
