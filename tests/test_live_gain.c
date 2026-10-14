/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)

struct model { atomic_uint active, calls; int failure; };
struct worker { birriasdr_dev_t *dev; uint8_t code; bool tuning; };

/* Simulate a slow multi-register transaction and detect any overlapping call.
 * This tests operation ownership, independently of the chipset write recipe.
 */
static int change_gain(birriasdr_dev_t *dev, bool manual, const struct birriasdr_gain *gain)
{
    struct model *model = dev->io.context;
    (void)manual;
    (void)gain;
    CHECK(atomic_fetch_add(&model->active, 1) == 0);
    atomic_fetch_add(&model->calls, 1);
    bs_sleep_us(100);
    CHECK(atomic_fetch_sub(&model->active, 1) == 1);
    return model->failure;
}

/* Retuning and gain share the same simulated register resource. */
static int change_frequency(birriasdr_dev_t *dev, uint64_t frequency)
{
    (void)frequency;
    return change_gain(dev, true, NULL);
}

/* Concurrent callers must observe complete committed triples, never a mixture
 * of another setter's indices. The mock also detects overlapping transactions.
 */
static void *set_repeatedly(void *context)
{
    struct worker *worker = context;
    struct birriasdr_gain request = {worker->code, worker->code, worker->code}, actual;
    int manual;
    for (unsigned i = 0; i < 50; ++i) {
        if (worker->tuning) {
            uint64_t current;
            CHECK(birriasdr_set_center_freq(worker->dev,
                  UINT64_C(2432000000) + worker->code * UINT64_C(1000000)) == 0);
            current = birriasdr_get_center_freq(worker->dev);
            CHECK(current >= UINT64_C(2432000000) && current <= UINT64_C(2437000000));
        } else {
            CHECK(birriasdr_set_gain_codes(worker->dev, &request) == 0);
        }
        CHECK(birriasdr_get_gain_codes(worker->dev, &actual, &manual) == 0);
        CHECK(manual && actual.lna == actual.lpf && actual.lpf == actual.digital);
    }
    return NULL;
}

/* Exercise startup/teardown gates, capability checks, multiple callers and the
 * failure-to-cancellation handoff without a dongle or a libusb event thread.
 */
int main(void)
{
    struct model model = {0};
    struct bs_backend backend = bs_mt7921_backend;
    birriasdr_dev_t dev = {.backend = &backend, .frequency = UINT64_C(2437000000),
                          .rate = 40000000, .manual = true, .gain = {1, 1, 1},
                          .control_mutex = PTHREAD_MUTEX_INITIALIZER};
    struct birriasdr_gain gain = {2, 2, 2}, actual;
    struct worker workers[4];
    pthread_t threads[4];
    int manual;
    dev.io.context = &model;
    backend.change_gain = change_gain;
    backend.change_frequency = change_frequency;
    atomic_init(&dev.run_state, BS_RUNNING);
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_BUSY);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2400000000)) == BIRRIASDR_ERROR_BUSY);
    CHECK(atomic_load(&model.calls) == 0);
    dev.control_ready = true;
    backend.capabilities &= ~BIRRIASDR_CAP_LIVE_GAIN;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_UNSUPPORTED);
    CHECK(atomic_load(&model.calls) == 0);
    backend.capabilities |= BIRRIASDR_CAP_LIVE_GAIN;
    backend.capabilities &= ~BIRRIASDR_CAP_LIVE_TUNING;
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2400000000)) == BIRRIASDR_ERROR_UNSUPPORTED);
    backend.capabilities |= BIRRIASDR_CAP_LIVE_TUNING;
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(900000000)) == BIRRIASDR_ERROR_UNSUPPORTED);
    gain.digital = 13;
    CHECK(birriasdr_set_gain_codes(&dev, &gain) == BIRRIASDR_ERROR_INVALID);
    CHECK(atomic_load(&model.calls) == 0 && atomic_load(&dev.run_state) == BS_RUNNING);
    for (unsigned i = 0; i < 4; ++i) {
        workers[i] = (struct worker){&dev, (uint8_t)(i % 3), i >= 2};
        CHECK(pthread_create(&threads[i], NULL, set_repeatedly, &workers[i]) == 0);
    }
    for (unsigned i = 0; i < 4; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(atomic_load(&model.calls) == 200);
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 0) == 0);
    CHECK(birriasdr_get_gain_codes(&dev, &actual, &manual) == 0 && !manual);
    CHECK(birriasdr_set_gain_codes(&dev, &actual) == BIRRIASDR_ERROR_STATE);
    model.failure = BIRRIASDR_ERROR_IO;
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 1) == BIRRIASDR_ERROR_IO);
    CHECK(!dev.manual && dev.control_error == BIRRIASDR_ERROR_IO && !dev.control_ready);
    CHECK(dev.control_error_stage == BIRRIASDR_STAGE_GAIN);
    CHECK(atomic_load(&dev.run_state) == BS_CONTROL_FAILED);
    CHECK(birriasdr_set_tuner_gain_mode(&dev, 1) == BIRRIASDR_ERROR_BUSY);
    atomic_store(&dev.run_state, BS_RUNNING);
    dev.control_ready = true;
    uint64_t previous = birriasdr_get_center_freq(&dev);
    CHECK(birriasdr_set_center_freq(&dev, UINT64_C(2450000000)) == BIRRIASDR_ERROR_IO);
    CHECK(birriasdr_get_center_freq(&dev) == previous && !dev.control_ready);
    CHECK(dev.control_error_stage == BIRRIASDR_STAGE_RF);
    CHECK(atomic_load(&dev.run_state) == BS_CONTROL_FAILED);
    CHECK(birriasdr_cancel_async(&dev) == 0);
    CHECK(atomic_load(&dev.run_state) == BS_CANCELLING);
    CHECK(pthread_mutex_destroy(&dev.control_mutex) == 0);
    puts("live gain/tuning ownership tests passed");
    return 0;
}
