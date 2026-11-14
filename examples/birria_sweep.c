/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 *
 * birria_sweep: a stepped power-spectrum sweep for receive-only band surveys.
 *
 * One stream runs for the whole sweep; the center is retuned live for each
 * step. After every retune the samples of a settling interval are discarded
 * (counted from the delivered sample index at the moment the call returned, so
 * the interval also covers samples still queued from the previous center).
 * The next dwell is averaged into a Hann-windowed power spectrum. Only the
 * central part of each step's span is kept (default 20 MHz of 40 MSps: the
 * stock capture path folds strong signals from 12 to 16 MHz off center), and
 * the bins next to DC are interpolated across the LO leak. Gain is forced per
 * band, because released gain can leave the receiver deaf for seconds after
 * opening.
 *
 * Output: rtl_power-compatible CSV (one row per step: date, time, Hz low,
 * Hz high, Hz step, samples, dB...) or JSON lines with the mean and peak
 * spectrum. Levels are dBFS: a full-scale complex tone in one bin reads 0.
 *
 * --settle-scan F1 F2 N measures the settling instead of sweeping: it retunes
 * N times between two centers and prints the mean block power around each
 * retune against the delivered sample index when the call returned.
 */
#include "birriasdr.h"
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RING_SECONDS 0.5
#define RECOVER_MAX_RESTARTS 16u
#define HIGH_BAND_HZ 4000000000ull

static atomic_bool interrupted;
static void on_signal(int number)
{
    (void)number;
    atomic_store(&interrupted, true);
}

/* The read callback only copies into this ring; the main thread reads windows
 * by delivered sample index once they have fully arrived. */
static struct {
    int16_t *iq;
    uint64_t capacity;
    _Atomic uint64_t received;
    _Atomic unsigned gaps;
} ring;

static void on_samples(const int16_t *iq, uint32_t count, uint64_t first, void *context)
{
    (void)first;
    (void)context;
    uint64_t at = atomic_load(&ring.received);
    uint32_t done = 0;
    while (done < count) {
        uint64_t slot = (at + done) % ring.capacity;
        uint64_t n = ring.capacity - slot;
        if (n > count - done) n = count - done;
        memcpy(ring.iq + 2 * slot, iq + 2 * (size_t)done, (size_t)n * 4);
        done += (uint32_t)n;
    }
    atomic_store(&ring.received, at + count);
}

static void on_gap(const struct birriasdr_recovery_event *event, void *context)
{
    (void)event;
    (void)context;
    atomic_fetch_add(&ring.gaps, 1);
}

static int stream_result;
static void *reader(void *dev)
{
    stream_result = birriasdr_read_async(dev, on_samples, NULL, 0, 0);
    return NULL;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static void sleep_ms(double ms)
{
    struct timespec t = {(time_t)(ms / 1000), (long)(fmod(ms, 1000) * 1e6)};
    nanosleep(&t, NULL);
}

/* Wait until [from, to) has been delivered; false on interrupt or stream end. */
static bool wait_for(uint64_t to)
{
    while (atomic_load(&ring.received) < to) {
        if (atomic_load(&interrupted) || stream_result) return false;
        sleep_ms(0.5);
    }
    return true;
}

/* Copy a delivered window out of the ring; false if it was already overwritten. */
static bool copy_window(uint64_t from, uint64_t to, int16_t *out)
{
    if (atomic_load(&ring.received) - from > ring.capacity) return false;
    for (uint64_t i = from; i < to;) {
        uint64_t slot = i % ring.capacity, n = ring.capacity - slot;
        if (n > to - i) n = to - i;
        memcpy(out + 2 * (i - from), ring.iq + 2 * slot, (size_t)n * 4);
        i += n;
    }
    return atomic_load(&ring.received) - from <= ring.capacity;
}

/* In-place radix-2 complex FFT with a precomputed twiddle table. */
static double *twiddle_re, *twiddle_im;
static unsigned twiddle_n;

static void fft(double *re, double *im, unsigned n)
{
    if (twiddle_n != n) {
        free(twiddle_re);
        free(twiddle_im);
        twiddle_re = malloc(n / 2 * sizeof(double));
        twiddle_im = malloc(n / 2 * sizeof(double));
        for (unsigned k = 0; k < n / 2; ++k) {
            twiddle_re[k] = cos(-2 * M_PI * k / n);
            twiddle_im[k] = sin(-2 * M_PI * k / n);
        }
        twiddle_n = n;
    }
    for (unsigned i = 1, j = 0; i < n; ++i) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (unsigned len = 2; len <= n; len <<= 1) {
        unsigned half = len / 2, stride = n / len;
        for (unsigned i = 0; i < n; i += len) {
            for (unsigned k = 0; k < half; ++k) {
                double wr = twiddle_re[k * stride], wi = twiddle_im[k * stride];
                double *ar = &re[i + k], *ai = &im[i + k], *br = &re[i + k + half], *bi = &im[i + k + half];
                double xr = *br * wr - *bi * wi, xi = *br * wi + *bi * wr;
                *br = *ar - xr;
                *bi = *ai - xi;
                *ar += xr;
                *ai += xi;
            }
        }
    }
}

struct options {
    double rate, usable_hz, settle_ms, dwell_ms, dc_hz;
    unsigned fft, passes;
    struct birriasdr_gain gain_low, gain_high;
    bool json, channel_filter;
    const char *output;
    double ranges[16][2];
    unsigned range_count;
    double scan[2];
    unsigned scan_count;
};

/* Mean and peak power spectrum of one dwell, fftshifted, in dBFS. */
static void spectrum(const struct options *o, const int16_t *iq, uint64_t samples,
                     double *mean, double *peak)
{
    unsigned n = o->fft;
    double *re = malloc(n * sizeof(double)), *im = malloc(n * sizeof(double));
    double *window = malloc(n * sizeof(double)), *acc = calloc(n, sizeof(double));
    double *mx = calloc(n, sizeof(double)), gain = 0;
    for (unsigned i = 0; i < n; ++i) {
        window[i] = 0.5 - 0.5 * cos(2 * M_PI * i / n);
        gain += window[i];
    }
    double scale = 1.0 / (gain * gain * 8192.0 * 8192.0);
    uint64_t frames = samples / n;
    for (uint64_t f = 0; f < frames; ++f) {
        const int16_t *x = iq + 2 * f * n;
        for (unsigned i = 0; i < n; ++i) {
            re[i] = x[2 * i] * window[i];
            im[i] = x[2 * i + 1] * window[i];
        }
        fft(re, im, n);
        for (unsigned i = 0; i < n; ++i) {
            double p = (re[i] * re[i] + im[i] * im[i]) * scale;
            acc[i] += p;
            if (p > mx[i]) mx[i] = p;
        }
    }
    for (unsigned i = 0; i < n; ++i) {
        unsigned src = (i + n / 2) % n;
        mean[i] = 10 * log10(acc[src] / (frames ? frames : 1) + 1e-30);
        peak[i] = 10 * log10(mx[src] + 1e-30);
    }
    /* Interpolate across the LO leak at DC. */
    unsigned guard = (unsigned)ceil(o->dc_hz / (o->rate / n));
    if (guard && guard < n / 4) {
        unsigned lo = n / 2 - guard - 1, hi = n / 2 + guard + 1;
        for (unsigned i = lo + 1; i < hi; ++i) {
            double t = (double)(i - lo) / (hi - lo);
            mean[i] = mean[lo] + t * (mean[hi] - mean[lo]);
            peak[i] = peak[lo] + t * (peak[hi] - peak[lo]);
        }
    }
    free(re); free(im); free(window); free(acc); free(mx);
}

static bool high_band(double hz) { return hz >= HIGH_BAND_HZ; }

/* The nearest center this board can tune (bands differ by board). */
static double clamp_center(birriasdr_dev_t *dev, double hz)
{
    uint64_t lo, hi;
    double best = hz, distance = 1e300;
    for (uint32_t band = 0; band < 8; ++band) {
        if (birriasdr_get_tuning_band(dev, band, &lo, &hi)) continue;
        double c = hz < lo ? (double)lo : hz > hi ? (double)hi : hz;
        if (fabs(c - hz) < distance) {
            distance = fabs(c - hz);
            best = c;
        }
    }
    return best;
}

/* Retune live, switching the forced gain in the order the limits allow: lower
 * the filter code before leaving 5 GHz, raise it after arriving. */
static int retune(birriasdr_dev_t *dev, const struct options *o, double hz, bool *in_high)
{
    bool high = high_band(hz);
    int result = 0;
    if (*in_high && !high) result = birriasdr_set_gain_codes(dev, &o->gain_low);
    if (!result) result = birriasdr_set_center_freq(dev, (uint64_t)llround(hz));
    if (!result && high && !*in_high) result = birriasdr_set_gain_codes(dev, &o->gain_high);
    if (!result) *in_high = high;
    return result;
}

static int settle_scan(birriasdr_dev_t *dev, const struct options *o, bool in_high)
{
    const double block_ms = 0.05, before_ms = 45, after_ms = 30;
    uint64_t block = (uint64_t)(o->rate * block_ms / 1000);
    unsigned blocks = (unsigned)((before_ms + after_ms) / block_ms);
    double *sum[2] = {calloc(blocks, sizeof(double)), calloc(blocks, sizeof(double))};
    double *worst[2] = {calloc(blocks, sizeof(double)), calloc(blocks, sizeof(double))};
    unsigned count[2] = {0, 0};
    uint64_t span = (uint64_t)blocks * block;
    int16_t *buffer = malloc(span * 4);
    double call_ms_sum = 0, call_samples_sum = 0;
    for (unsigned i = 0; i < blocks; ++i) worst[0][i] = worst[1][i] = 1e30;
    for (unsigned k = 0; k < o->passes && !atomic_load(&interrupted); ++k) {
        unsigned dir = k % 2;
        double target = o->scan[1 - dir];
        uint64_t s0 = atomic_load(&ring.received);
        double t0 = now_s();
        int result = retune(dev, o, target, &in_high);
        double t1 = now_s();
        uint64_t s1 = atomic_load(&ring.received);
        if (result) {
            fprintf(stderr, "retune to %.0f: %s\n", target, birriasdr_strerror(result));
            return 1;
        }
        uint64_t from = s1 - (uint64_t)(o->rate * before_ms / 1000);
        unsigned gaps = atomic_load(&ring.gaps);
        if (!wait_for(from + span) || !copy_window(from, from + span, buffer)) break;
        if (atomic_load(&ring.gaps) != gaps) continue;
        for (unsigned b = 0; b < blocks; ++b) {
            double p = 0;
            for (uint64_t i = 0; i < 2 * block; ++i) {
                double v = buffer[2 * b * block + i];
                p += v * v;
            }
            double db = 10 * log10(p / block / (8192.0 * 8192.0) + 1e-30);
            sum[dir][b] += db;
            if (db < worst[dir][b]) worst[dir][b] = db;
        }
        ++count[dir];
        call_ms_sum += (t1 - t0) * 1e3;
        call_samples_sum += (double)(s1 - s0);
        sleep_ms(30);
    }
    printf("# settle scan: %.0f <-> %.0f Hz, %u + %u retunes, mean call %.2f ms (%.0f samples delivered during it)\n",
           o->scan[0], o->scan[1], count[0], count[1], call_ms_sum / (count[0] + count[1]),
           call_samples_sum / (count[0] + count[1]));
    printf("# offset_ms (from the delivered index at return), mean dBFS and minimum dBFS: to %.0f | to %.0f\n",
           o->scan[1], o->scan[0]);
    for (unsigned b = 0; b < blocks; ++b)
        printf("%.2f %.2f %.2f %.2f %.2f\n", b * block_ms - before_ms,
               count[0] ? sum[0][b] / count[0] : NAN, count[0] ? worst[0][b] : NAN,
               count[1] ? sum[1][b] / count[1] : NAN, count[1] ? worst[1][b] : NAN);
    free(sum[0]); free(sum[1]); free(worst[0]); free(worst[1]); free(buffer);
    return 0;
}

static void write_row(FILE *out, const struct options *o, double center, uint64_t samples,
                      const double *mean, const double *peak, double t)
{
    unsigned n = o->fft;
    double bin = o->rate / n;
    unsigned first = (unsigned)lround((o->rate - o->usable_hz) / 2 / bin), last = n - first;
    double lo = center - o->rate / 2 + first * bin;
    /* Clip the edge steps to the requested range. */
    double range_lo = 0, range_hi = 1e30;
    for (unsigned r = 0; r < o->range_count; ++r)
        if (center >= o->ranges[r][0] - o->usable_hz && center <= o->ranges[r][1] + o->usable_hz) {
            range_lo = o->ranges[r][0];
            range_hi = o->ranges[r][1];
        }
    while (first < last && lo < range_lo - 1) { ++first; lo += bin; }
    while (last > first && center - o->rate / 2 + last * bin > range_hi + bin) --last;
    time_t secs = (time_t)t;
    struct tm tm;
    localtime_r(&secs, &tm);
    if (o->json) {
        fprintf(out, "{\"t\":%.3f,\"center_hz\":%.0f,\"f_low_hz\":%.1f,\"bin_hz\":%.3f,\"samples\":%llu,"
                "\"dc_interp_hz\":%.0f,\"mean_dbfs\":[", t, center, lo, bin, (unsigned long long)samples, o->dc_hz);
        for (unsigned i = first; i < last; ++i) fprintf(out, "%s%.2f", i > first ? "," : "", mean[i]);
        fprintf(out, "],\"peak_dbfs\":[");
        for (unsigned i = first; i < last; ++i) fprintf(out, "%s%.2f", i > first ? "," : "", peak[i]);
        fprintf(out, "]}\n");
    } else {
        fprintf(out, "%04d-%02d-%02d, %02d:%02d:%02d, %.0f, %.0f, %.2f, %llu", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, lo,
                lo + (last - first) * bin, bin, (unsigned long long)samples);
        for (unsigned i = first; i < last; ++i) fprintf(out, ", %.2f", mean[i]);
        fputc('\n', out);
    }
}

static int sweep(birriasdr_dev_t *dev, const struct options *o, bool in_high, FILE *out)
{
    double centers[4096];
    unsigned steps = 0;
    for (unsigned r = 0; r < o->range_count; ++r) {
        /* Step from the first tunable center, so a range starting below the
         * board's band edge doesn't produce overlapping first steps. */
        for (double c = clamp_center(dev, o->ranges[r][0] + o->usable_hz / 2); steps < 4096; c += o->usable_hz) {
            double clamped = clamp_center(dev, c);
            if (!steps || clamped != centers[steps - 1]) centers[steps++] = clamped;
            if (c + o->usable_hz / 2 >= o->ranges[r][1] || clamped != c) break;
        }
    }
    uint64_t settle = (uint64_t)(o->rate * o->settle_ms / 1000);
    uint64_t dwell = (uint64_t)(o->rate * o->dwell_ms / 1000) / o->fft * o->fft;
    int16_t *buffer = malloc(dwell * 4);
    double *mean = malloc(o->fft * sizeof(double)), *peak = malloc(o->fft * sizeof(double));
    fprintf(stderr, "sweep: %u steps per pass, usable %.1f MHz, settle %.1f ms, dwell %.1f ms, %u-point FFT (%.1f kHz bins)\n",
            steps, o->usable_hz / 1e6, o->settle_ms, o->dwell_ms, o->fft, o->rate / o->fft / 1e3);
    unsigned redo = 0;
    for (unsigned pass = 0; (!o->passes || pass < o->passes) && !atomic_load(&interrupted); ++pass) {
        double start = now_s(), retune_ms = 0;
        for (unsigned s = 0; s < steps && !atomic_load(&interrupted); ++s) {
            unsigned gaps = atomic_load(&ring.gaps);
            double t0 = now_s();
            int result = retune(dev, o, centers[s], &in_high);
            retune_ms += (now_s() - t0) * 1e3;
            if (result) {
                fprintf(stderr, "retune to %.0f Hz: %s\n", centers[s], birriasdr_strerror(result));
                return 1;
            }
            uint64_t from = atomic_load(&ring.received) + settle;
            if (!wait_for(from + dwell)) break;
            if (!copy_window(from, from + dwell, buffer) || atomic_load(&ring.gaps) != gaps) {
                ++redo;
                --s;           /* a gap or an overwritten window: measure this step again */
                if (redo > 100) return 1;
                continue;
            }
            spectrum(o, buffer, dwell, mean, peak);
            write_row(out, o, centers[s], dwell, mean, peak, t0);
        }
        fflush(out);
        fprintf(stderr, "pass %u: %.3f s (%u steps, mean retune %.1f ms), gaps %u, redone steps %u\n",
                pass, now_s() - start, steps, retune_ms / steps, atomic_load(&ring.gaps), redo);
    }
    free(buffer); free(mean); free(peak);
    return 0;
}

/* No hardware: a full-scale complex tone on a bin center must read 0 dBFS in
 * that bin, the bins two away must be empty (Hann), and the DC interpolation
 * must bridge a DC spike. Returns 0 on success. */
static int self_test(void)
{
    struct options o = {.rate = 40e6, .fft = 1024, .dc_hz = 100e3};
    unsigned n = o.fft, k = 100, frames = 8;
    int16_t *iq = malloc((size_t)n * frames * 4);
    double mean[1024], peak[1024];
    for (unsigned i = 0; i < n * frames; ++i) {
        double phase = 2 * M_PI * k * i / n;
        iq[2 * i] = (int16_t)lrint(8191 * cos(phase)) + 200;    /* + a DC offset */
        iq[2 * i + 1] = (int16_t)lrint(8191 * sin(phase));
    }
    spectrum(&o, iq, (uint64_t)n * frames, mean, peak);
    free(iq);
    unsigned bin = n / 2 + k;
    int failures = 0;
    if (fabs(mean[bin]) > 0.05 || fabs(peak[bin]) > 0.05) failures |= 1;
    if (mean[bin + 2] > -100 || mean[bin - 2] > -100) failures |= 2;
    if (mean[n / 2] > -100) failures |= 4;                     /* the DC spike was bridged */
    printf("self-test: tone bin %.3f dBFS, bins +-2 %.1f/%.1f dBFS, DC %.1f dBFS: %s\n", mean[bin],
           mean[bin - 2], mean[bin + 2], mean[n / 2], failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}

static bool parse_range(const char *text, double *lo, double *hi)
{
    char *end;
    *lo = strtod(text, &end) * 1e6;
    if (*end != ':') return false;
    *hi = strtod(end + 1, &end) * 1e6;
    return !*end && *hi > *lo;
}

static void usage(const char *name)
{
    fprintf(stderr,
            "Usage: %s [options] --range LOW:HIGH [--range LOW:HIGH ...]   (MHz)\n"
            "       %s [options] --settle-scan F1 F2 RETUNES                 (MHz)\n"
            "  --output FILE      default stdout\n"
            "  --json             JSON lines (mean and peak spectrum) instead of rtl_power CSV\n"
            "  --passes N         sweeps to run, 0 = until interrupted (default 1)\n"
            "  --rate HZ          20000000, 40000000 (default) or 80000000\n"
            "  --usable HZ        span kept from each step, also the step (default rate/2)\n"
            "  --settle MS        discarded after each retune returns (default 3; the\n"
            "                     signal is final before the call returns on the EDUP)\n"
            "  --dwell MS         averaged per step (default 5)\n"
            "  --fft N            power of two (default 1024)\n"
            "  --dc HZ            interpolated each side of DC (default 100000)\n"
            "  --gain24 L P D     forced codes below 4 GHz (default 5 7 0)\n"
            "  --gain5 L P D      forced codes at 5/6 GHz (default 5 14 0)\n"
            "  --channel-filter   use the library's channel filter\n"
            "  --self-test        check the spectrum scale without hardware\n", name, name);
}

int main(int argc, char **argv)
{
    struct options o = {.rate = 40e6, .settle_ms = 3, .dwell_ms = 5, .dc_hz = 100e3, .fft = 1024,
                        .passes = 1, .gain_low = {5, 7, 0}, .gain_high = {5, 14, 0}};
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--range") && more && o.range_count < 16) {
            if (!parse_range(argv[++i], &o.ranges[o.range_count][0], &o.ranges[o.range_count][1])) {
                usage(argv[0]);
                return 2;
            }
            ++o.range_count;
        } else if (!strcmp(a, "--settle-scan") && i + 3 < argc) {
            o.scan[0] = atof(argv[++i]) * 1e6;
            o.scan[1] = atof(argv[++i]) * 1e6;
            o.passes = (unsigned)atoi(argv[++i]);
            o.scan_count = 2;
        } else if (!strcmp(a, "--self-test")) {
            return self_test();
        } else if (!strcmp(a, "--output") && more) o.output = argv[++i];
        else if (!strcmp(a, "--json")) o.json = true;
        else if (!strcmp(a, "--channel-filter")) o.channel_filter = true;
        else if (!strcmp(a, "--passes") && more) o.passes = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--rate") && more) o.rate = atof(argv[++i]);
        else if (!strcmp(a, "--usable") && more) o.usable_hz = atof(argv[++i]);
        else if (!strcmp(a, "--settle") && more) o.settle_ms = atof(argv[++i]);
        else if (!strcmp(a, "--dwell") && more) o.dwell_ms = atof(argv[++i]);
        else if (!strcmp(a, "--fft") && more) o.fft = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--dc") && more) o.dc_hz = atof(argv[++i]);
        else if ((!strcmp(a, "--gain24") || !strcmp(a, "--gain5")) && i + 3 < argc) {
            struct birriasdr_gain *g = !strcmp(a, "--gain24") ? &o.gain_low : &o.gain_high;
            g->lna = (uint8_t)atoi(argv[++i]);
            g->lpf = (uint8_t)atoi(argv[++i]);
            g->digital = (uint8_t)atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!o.usable_hz) o.usable_hz = o.rate / 2;
    if ((!o.range_count && !o.scan_count) || o.fft < 64 || (o.fft & (o.fft - 1)) ||
        o.usable_hz > o.rate || o.dwell_ms * o.rate / 1000 < o.fft) {
        usage(argv[0]);
        return 2;
    }
    FILE *out = o.output ? fopen(o.output, "w") : stdout;
    if (!out) {
        perror(o.output);
        return 1;
    }
    ring.capacity = (uint64_t)(o.rate * RING_SECONDS);
    ring.iq = malloc(ring.capacity * 4);
    if (!ring.iq) return 1;

    double first = o.scan_count ? o.scan[0] : o.ranges[0][0] + o.usable_hz / 2;
    bool in_high = high_band(first);
    birriasdr_dev_t *dev = NULL;
    int result = birriasdr_open(&dev, 0);
    if (!result) result = birriasdr_set_sample_rate(dev, (uint32_t)o.rate);
    if (!result) {
        first = clamp_center(dev, first);
        in_high = high_band(first);
        result = birriasdr_set_center_freq(dev, (uint64_t)llround(first));
    }
    if (!result) result = birriasdr_set_tuner_gain_mode(dev, 1);
    if (!result) result = birriasdr_set_gain_codes(dev, in_high ? &o.gain_high : &o.gain_low);
    if (!result && o.channel_filter) result = birriasdr_set_channel_filter(dev, 1);
    if (!result) result = birriasdr_set_recovery(dev, RECOVER_MAX_RESTARTS, on_gap, NULL);
    if (result) {
        fprintf(stderr, "setup: %s\n", birriasdr_strerror(result));
        if (dev) birriasdr_close(dev);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    pthread_t thread;
    if (pthread_create(&thread, NULL, reader, dev)) return 1;
    if (!wait_for((uint64_t)(o.rate * 0.2))) {
        fprintf(stderr, "stream: %s\n", birriasdr_strerror(stream_result));
    }
    int rc = o.scan_count ? settle_scan(dev, &o, in_high) : sweep(dev, &o, in_high, out);
    birriasdr_cancel_async(dev);
    pthread_join(thread, NULL);
    if (stream_result) {
        fprintf(stderr, "stream: %s\n", birriasdr_strerror(stream_result));
        rc = 1;
    }
    birriasdr_close(dev);
    if (out != stdout) fclose(out);
    free(ring.iq);
    return rc;
}
