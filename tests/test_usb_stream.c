/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 * Fake libusb completions test ownership; this executable never opens USB.
 */
#include "usb_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); abort(); } } while (0)
struct request { struct libusb_transfer *transfer; bool cancelled; };
static struct {
    pthread_mutex_t mutex;
    struct request pending[256];
    unsigned count, allocated, freed, submitted, fail_submit, completions, limit;
} mock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

/* Allocate the same public transfer layout used by libusb_fill_bulk_transfer. */
struct libusb_transfer * LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    CHECK(iso_packets == 0);
    ++mock.allocated;
    return calloc(1, sizeof(struct libusb_transfer));
}

/* Freeing a transfer still owned by the event loop is a test failure. */
void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&mock.mutex);
    for (unsigned i = 0; i < mock.count; ++i)
        CHECK(mock.pending[i].transfer != transfer);
    ++mock.freed;
    pthread_mutex_unlock(&mock.mutex);
    free(transfer);
}

/* Inject either a partial-start failure or a failure during callback resubmission. */
int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&mock.mutex);
    if (++mock.submitted == mock.fail_submit) {
        pthread_mutex_unlock(&mock.mutex);
        return LIBUSB_ERROR_IO;
    }
    CHECK(mock.count < 256);
    mock.pending[mock.count++] = (struct request){transfer, false};
    pthread_mutex_unlock(&mock.mutex);
    return 0;
}

/* Cancellation marks a request; it deliberately does not complete it yet. */
int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&mock.mutex);
    for (unsigned i = 0; i < mock.count; ++i) {
        if (mock.pending[i].transfer == transfer) {
            mock.pending[i].cancelled = true;
            pthread_mutex_unlock(&mock.mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&mock.mutex);
    return LIBUSB_ERROR_NOT_FOUND;
}

/* Deliver data or deferred cancellation on the actual reader thread, exercising
 * callback/main-thread locking. No completion may outlive reader destruction.
 */
int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *context,
                                                       struct timeval *timeout, int *completed)
{
    struct libusb_transfer *transfer = NULL;
    (void)context;
    (void)timeout;
    (void)completed;
    pthread_mutex_lock(&mock.mutex);
    for (unsigned i = 0; i < mock.count; ++i) {
        struct request request = mock.pending[i];
        if (!request.cancelled && mock.completions >= mock.limit)
            continue;
        transfer = request.transfer;
        memmove(&mock.pending[i], &mock.pending[i + 1], (mock.count - i - 1) * sizeof(request));
        --mock.count;
        transfer->status = request.cancelled ? LIBUSB_TRANSFER_CANCELLED : LIBUSB_TRANSFER_COMPLETED;
        transfer->actual_length = request.cancelled ? 0 : 16;
        if (!request.cancelled)
            memset(transfer->buffer, (int)++mock.completions, 16);
        break;
    }
    pthread_mutex_unlock(&mock.mutex);
    if (transfer)
        transfer->callback(transfer);
    else
        bs_sleep_us(1000);
    return 0;
}

/* Reset only after all previous transfers have completed and been freed. */
static void reset_mock(unsigned limit, unsigned fail_submit)
{
    CHECK(mock.count == 0 && mock.allocated == mock.freed);
    mock.allocated = mock.freed = mock.submitted = mock.completions = 0;
    mock.limit = limit;
    mock.fail_submit = fail_submit;
}

/* Clean shutdown, consumer timeout, partial startup and resubmit failure must
 * all leave zero in-flight transfers and exactly one free per allocation.
 */
int main(void)
{
    birriasdr_dev_t dev = {0};
    struct bs_reader *reader;
    uint8_t bytes[BS_USB_BYTES];
    size_t length;
    reset_mock(8, 0);
    CHECK(bs_reader_create(&dev, 4, &reader) == 0);
    CHECK(bs_reader_start(reader) == 0);
    for (unsigned i = 1; i <= 8; ++i) {
        CHECK(bs_reader_take(reader, bytes, &length, 1000) == 0);
        CHECK(length == 16 && bytes[0] == i && bytes[15] == i);
    }
    CHECK(bs_reader_take(reader, bytes, &length, 5) == BIRRIASDR_ERROR_TIMEOUT);
    bs_reader_destroy(reader);
    reset_mock(0, 3);
    CHECK(bs_reader_create(&dev, 4, &reader) == 0);
    CHECK(bs_reader_start(reader) == BIRRIASDR_ERROR_IO);
    bs_reader_destroy(reader);
    reset_mock(8, 5);
    CHECK(bs_reader_create(&dev, 4, &reader) == 0);
    CHECK(bs_reader_start(reader) == 0);
    CHECK(bs_reader_take(reader, bytes, &length, 1000) == BIRRIASDR_ERROR_IO);
    bs_reader_destroy(reader);
    reset_mock(0, 0);
    puts("USB transfer ownership and failure tests passed");
    return 0;
}
