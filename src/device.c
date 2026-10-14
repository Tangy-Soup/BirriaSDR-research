/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */
#include "internal.h"
#include <stdlib.h>
#include <string.h>

/* Centralize the idle-state contract. No control write may race capture or a
 * libusb completion callback; live control needs a future firmware mailbox API.
 */
static int require_idle(birriasdr_dev_t *dev)
{
    if (!dev)
        return BIRRIASDR_ERROR_INVALID;
    return atomic_load(&dev->run_state) != BS_IDLE ? BIRRIASDR_ERROR_BUSY : 0;
}

/* Keep the ABI version independent of individual firmware image revisions. */
const char *birriasdr_get_version(void)
{
    return "0.1.0";
}

/* Errors are library-owned values, not ambiguous errno/libusb mixtures. */
const char *birriasdr_strerror(int error)
{
    switch (error) {
    case 0: return "success";
    case BIRRIASDR_ERROR_INVALID: return "invalid argument";
    case BIRRIASDR_ERROR_NO_DEVICE: return "device absent or disconnected";
    case BIRRIASDR_ERROR_ACCESS: return "USB or device-lock permission denied";
    case BIRRIASDR_ERROR_BUSY: return "device, driver, or stream is busy";
    case BIRRIASDR_ERROR_IO: return "USB/register I/O failed";
    case BIRRIASDR_ERROR_TIMEOUT: return "hardware handshake timed out";
    case BIRRIASDR_ERROR_NO_MEMORY: return "allocation failed";
    case BIRRIASDR_ERROR_UNSUPPORTED: return "chipset, board, or control not implemented";
    case BIRRIASDR_ERROR_COLD_BOOT: return "retained vendor patch: cold power-on required";
    case BIRRIASDR_ERROR_PROTOCOL: return "invalid, missing, or stale stream data";
    case BIRRIASDR_ERROR_OVERRUN: return "capture continuity lost to source overrun";
    case BIRRIASDR_ERROR_STATE: return "unexpected hardware or receiver state";
    default: return "unknown library error";
    }
}

/* Discovery uses descriptors only, so unsupported backends remain visible. */
int birriasdr_get_device_count(void)
{
    return bs_usb_discover(0, NULL, NULL);
}

/* Copy descriptive information; do not hold libusb descriptor pointers. */
int birriasdr_get_device_info(uint32_t index, struct birriasdr_device_info *info)
{
    if (!info)
        return BIRRIASDR_ERROR_INVALID;
    memset(info, 0, sizeof(*info));
    return bs_usb_discover(index, info, NULL);
}

/* Allocate one independent context, then claim a qualified interface. Booting
 * is delayed until read so setters configure the next acquisition atomically.
 */
int birriasdr_open(birriasdr_dev_t **out, uint32_t index)
{
    birriasdr_dev_t *dev;
    int result;
    if (!out)
        return BIRRIASDR_ERROR_INVALID;
    *out = NULL;
    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return BIRRIASDR_ERROR_NO_MEMORY;
    dev->lock_fd = -1;
    dev->interface_number = -1;
    atomic_init(&dev->run_state, BS_IDLE);
    dev->gain = (struct birriasdr_gain){1, 4, 0};
    result = bs_usb_discover(index, NULL, dev);
    if (result < 0) {
        bs_usb_close(dev);
        free(dev);
        return result;
    }
    *out = dev;
    return 0;
}

/* A read owns every outstanding transfer. Requiring it to return before close
 * prevents transfer callbacks from dereferencing a freed device/context.
 */
int birriasdr_close(birriasdr_dev_t *dev)
{
    int result = 0;
    BS_TRY(require_idle(dev));
    if (dev->touched)
        result = dev->backend->reset(dev);
    bs_usb_close(dev);
    free(dev);
    return result;
}

/* Capabilities describe implemented operations, never silicon marketing limits. */
uint32_t birriasdr_get_capabilities(const birriasdr_dev_t *dev)
{
    return dev ? dev->backend->capabilities : 0;
}

/* Only the independently verified RF profile is exposed; do not silently map
 * arbitrary frequencies onto the closest Wi-Fi channel.
 */
int birriasdr_set_center_freq(birriasdr_dev_t *dev, uint64_t frequency)
{
    BS_TRY(require_idle(dev));
    BS_TRY(dev->backend->validate(frequency, dev->rate, &dev->gain));
    dev->frequency = frequency;
    return 0;
}

/* Return a request, not a claim about oscillator calibration. */
uint64_t birriasdr_get_center_freq(const birriasdr_dev_t *dev)
{
    return dev ? dev->frequency : 0;
}

/* Capture-clock programming for higher rates is deliberately not inferred from
 * the earlier vendor-firmware experiments. No host decimator is hidden here.
 */
int birriasdr_set_sample_rate(birriasdr_dev_t *dev, uint32_t rate)
{
    BS_TRY(require_idle(dev));
    BS_TRY(dev->backend->validate(dev->frequency, rate, &dev->gain));
    dev->rate = rate;
    return 0;
}

/* The nominal complex rate is configuration metadata, not measured timing. */
uint32_t birriasdr_get_sample_rate(const birriasdr_dev_t *dev)
{
    return dev ? dev->rate : 0;
}

/* Save the mode for the next hardware initialization; automatic means release
 * the known force bits, not promise that every internal AGC loop is understood.
 */
int birriasdr_set_tuner_gain_mode(birriasdr_dev_t *dev, int manual)
{
    BS_TRY(require_idle(dev));
    if (manual != 0 && manual != 1)
        return BIRRIASDR_ERROR_INVALID;
    if (!(dev->backend->capabilities & BIRRIASDR_CAP_RAW_GAIN))
        return BIRRIASDR_ERROR_UNSUPPORTED;
    dev->manual = manual != 0;
    return 0;
}

/* Reject invalid indices before any hardware activity or configuration change. */
int birriasdr_set_gain_codes(birriasdr_dev_t *dev, const struct birriasdr_gain *gain)
{
    BS_TRY(require_idle(dev));
    if (!gain)
        return BIRRIASDR_ERROR_INVALID;
    BS_TRY(dev->backend->validate(dev->frequency, dev->rate, gain));
    if (!dev->manual)
        return BIRRIASDR_ERROR_STATE;
    dev->gain = *gain;
    return 0;
}

/* Return requested indices without performing an RF-SPI transaction. */
int birriasdr_get_gain_codes(birriasdr_dev_t *dev, struct birriasdr_gain *gain, int *manual)
{
    BS_TRY(require_idle(dev));
    if (!gain || !manual)
        return BIRRIASDR_ERROR_INVALID;
    *gain = dev->gain;
    *manual = dev->manual;
    return 0;
}

/* Missing research must be observable to callers, not a successful no-op. */
static int unsupported_control(birriasdr_dev_t *dev)
{
    BS_TRY(require_idle(dev));
    return BIRRIASDR_ERROR_UNSUPPORTED;
}

/* TODO: measure and implement raw-code to calibrated RF dB conversion. */
int birriasdr_set_tuner_gain(birriasdr_dev_t *dev, int tenths_db)
{
    (void)tenths_db;
    return unsupported_control(dev);
}

/* TODO: publish measured stage combinations rather than inventing gain values. */
int birriasdr_get_tuner_gains(birriasdr_dev_t *dev, int *gains, size_t capacity)
{
    (void)gains;
    (void)capacity;
    return unsupported_control(dev);
}

/* TODO: identify every relevant AGC loop and validate a complete freeze/release. */
int birriasdr_set_agc_mode(birriasdr_dev_t *dev, int enabled)
{
    (void)enabled;
    return unsupported_control(dev);
}

/* TODO: complete filter initialization and measure bandwidth against RF input. */
int birriasdr_set_tuner_bandwidth(birriasdr_dev_t *dev, uint32_t bandwidth)
{
    (void)bandwidth;
    return unsupported_control(dev);
}

/* TODO: establish crystal/PLL correction without changing sample interpretation. */
int birriasdr_set_freq_correction(birriasdr_dev_t *dev, int ppm)
{
    (void)ppm;
    return unsupported_control(dev);
}

/* TODO: decode board routing; the ICAP chain selector is a different control. */
int birriasdr_set_antenna(birriasdr_dev_t *dev, unsigned antenna)
{
    (void)antenna;
    return unsupported_control(dev);
}

/* TODO: identify writable coefficient fields and their ordering/units. This API
 * reserves volatile application only; it must never become an eFuse writer.
 */
int birriasdr_set_calibration(birriasdr_dev_t *dev, const void *data, size_t length)
{
    (void)data;
    (void)length;
    return unsupported_control(dev);
}

/* There is no retained host queue between reads. Clear diagnostics only. */
int birriasdr_reset_buffer(birriasdr_dev_t *dev)
{
    BS_TRY(require_idle(dev));
    memset(&dev->stats, 0, sizeof(dev->stats));
    return 0;
}

/* Cancellation and running/idle share one atomic state, so startup cannot erase
 * a concurrent cancellation and idle cancellation cannot poison the next read.
 * USB operations remain on the read thread, outside application callbacks.
 */
int birriasdr_cancel_async(birriasdr_dev_t *dev)
{
    unsigned expected = BS_RUNNING;
    if (!dev)
        return BIRRIASDR_ERROR_INVALID;
    if (atomic_compare_exchange_strong(&dev->run_state, &expected, BS_CANCELLING))
        return 0;
    return expected == BS_CANCELLING ? 0 : BIRRIASDR_ERROR_STATE;
}

/* Statistics are stable once the read thread has returned. */
int birriasdr_get_stream_stats(birriasdr_dev_t *dev, struct birriasdr_stream_stats *stats)
{
    BS_TRY(require_idle(dev));
    if (!stats)
        return BIRRIASDR_ERROR_INVALID;
    *stats = dev->stats;
    return 0;
}

/* The shared read engine owns startup, asynchronous transfers and cleanup. */
int birriasdr_read_async(birriasdr_dev_t *dev, birriasdr_read_async_cb_t callback,
                        void *context, uint32_t depth, uint32_t buffer_samples)
{
    return bs_stream(dev, callback, context, depth, buffer_samples, 0);
}

struct sync_buffer { int16_t *iq; uint32_t count; uint32_t received; };

/* The finite firmware packet count bounds this copy; keep a defensive bound
 * so malformed backend output cannot overflow the caller's allocation.
 */
static void copy_sync(const int16_t *iq, uint32_t count, uint64_t first, void *context)
{
    struct sync_buffer *buffer = context;
    (void)first;
    if (count > buffer->count - buffer->received)
        count = buffer->count - buffer->received;
    memcpy(buffer->iq + 2u * (size_t)buffer->received, iq, 4u * (size_t)count);
    buffer->received += count;
}

/* Finite reads use the same queued USB engine and packet validation as async.
 * Requiring whole payloads avoids discarding unseen samples at the last packet.
 */
int birriasdr_read_sync(birriasdr_dev_t *dev, int16_t *iq, uint32_t count, uint32_t *n_read)
{
    struct sync_buffer buffer = {iq, count, 0};
    int result;
    if (n_read)
        *n_read = 0;
    if (!dev || !iq || !n_read || !count || count % BS_PACKET_SAMPLES)
        return BIRRIASDR_ERROR_INVALID;
    result = bs_stream(dev, copy_sync, &buffer, 64, 16384, count / BS_PACKET_SAMPLES);
    *n_read = buffer.received;
    if (!result && buffer.received != count)
        return BIRRIASDR_ERROR_PROTOCOL;
    return result;
}
