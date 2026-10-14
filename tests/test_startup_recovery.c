/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * Explicit Linux hardware regression for a broken ROM bulk transport.
 *
 * The tested MT7961 can report EPROTO after clearing healthy endpoint halts.
 * Reproduce that condition immediately before the first ROM bulk transaction,
 * then require the public finite read to recover through its own USB reset.
 * Linker wrappers count resets without changing their result. This experiment
 * deliberately disturbs a physical dongle; it is never registered with CTest.
 * The production driver must not proactively clear healthy endpoint halts.
 */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static birriasdr_dev_t *target;
static unsigned resets, faults;
static bool armed;

int LIBUSB_CALL __real_libusb_reset_device(libusb_device_handle *handle);
int LIBUSB_CALL __real_libusb_bulk_transfer(libusb_device_handle *handle,
    unsigned char endpoint, unsigned char *data, int length, int *actual,
    unsigned timeout);

/* Count the real reset requested by startup, without repairing it in the test. */
int LIBUSB_CALL __wrap_libusb_reset_device(libusb_device_handle *handle)
{
    ++resets;
    return __real_libusb_reset_device(handle);
}

/* Inject once, after the normal WFSYS/DMA setup. Subsequent transfers, including
 * the retry, use libusb unchanged. No asynchronous reads are submitted yet.
 */
int LIBUSB_CALL __wrap_libusb_bulk_transfer(libusb_device_handle *handle,
    unsigned char endpoint, unsigned char *data, int length, int *actual,
    unsigned timeout)
{
    if (armed) {
        armed = false;
        uint8_t endpoints[] = {target->ep_in, target->ep_command, target->ep_download};
        for (size_t i = 0; i < sizeof(endpoints); ++i) {
            int result = libusb_clear_halt(handle, endpoints[i]);
            printf("clear_endpoint=%02x result=%d\n", endpoints[i], result);
            if (result) return result;
        }
    }
    int result = __real_libusb_bulk_transfer(handle, endpoint, data, length, actual, timeout);
    if (result != 0 && result != LIBUSB_ERROR_TIMEOUT) {
        ++faults;
        printf("bulk_endpoint=%02x result=%d\n", endpoint, result);
    }
    return result;
}

int main(int argc, char **argv)
{
    struct birriasdr_stream_stats stats = {0};
    uint32_t count = 0;
    if (argc != 2 || strcmp(argv[1], "--run")) {
        fprintf(stderr, "Usage: %s --run (disrupts a physical dongle's endpoints)\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int16_t *iq = malloc(65536 * 2 * sizeof(*iq));
    if (!iq) return 1;
    int result = birriasdr_open(&target, 0);
    if (!result) {
        armed = true;
        result = birriasdr_read_sync(target, iq, 65536, &count);
        birriasdr_get_stream_stats(target, &stats);
    }
    int closed = target ? birriasdr_close(target) : 0;
    printf("read=%d samples=%u stage=%u state=%u reset=%d close=%d usb_resets=%u bulk_faults=%u\n",
           result, count, stats.stage, stats.firmware_state, stats.reset_error, closed, resets, faults);
    free(iq);
    return result || closed || count != 65536 || resets != 1 || !faults;
}
