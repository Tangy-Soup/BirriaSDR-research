/* SPDX-License-Identifier: MIT
 * Streaming Bluetooth LE advertising collector for BirriaSDR.
 *
 * Demodulates the LE 1M advertising channels inside the capture (37 and 38 at
 * the default 2414 MHz / 40 MSps) as samples arrive, and keeps only CRC-valid
 * PDUs: a JSON line each, a Wireshark PCAP (LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR)
 * and an advertiser summary. No raw I/Q is stored, so disk use is a few hundred
 * bytes per packet and memory is a fixed ~40 MB ring, independent of run length.
 *
 * Per channel: 1 kHz-exact table mixer, order-3 CIC-shaped FIR to 8 MSps,
 * 48-tap Hamming FIR to 4 MSps (4 samples/symbol), FM discriminator, per-phase
 * 32-bit access-address correlator, least-squares DC/amplitude fit on the
 * access address, dewhitening and CRC-24. A failed CRC is retried one sample
 * early and late; nothing is ever corrected to fit the CRC.
 *
 * The USB callback only copies into the ring; a worker thread demodulates. If
 * the worker falls behind, whole blocks are dropped and counted, never stalled.
 */
#include "birriasdr.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <time.h>

#define OUT_RATE 4000000u   /* demodulator rate: 4 samples per 1 Msym/s symbol */
#define MID_RATE 8000000u   /* after the CIC-shaped first stage */
#define SPS 4
#define ADV_AA 0x8E89BED6u
#define HIST 16384u         /* 4.1 ms of history: more than a 255-byte PDU (2.1 ms) */
#define HMASK (HIST - 1u)
#define SLOT_SAMPLES 65536u
#define SLOTS 160u          /* 40 MiB of I/Q: 262 ms at 40 MSps */
#define B_TAPS 48
#define MAX_CHANNELS 3
#define MAX_PENDING 8
#define MAX_PDU 255
#define MAX_BITS ((2 + MAX_PDU + 3) * 8)
#define DEVICES 65536u      /* advertiser table size, a power of two */
#define FLOOR_BLOCK 4096u   /* 1 ms power blocks for the noise-floor estimate */
#define FLOOR_BLOCKS 128u
#define SUMMARY_RESERVE (16u << 20) /* --max-bytes headroom kept for the summary */

static volatile sig_atomic_t interrupted;

static void on_signal(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static const char *const pdu_names[16] = {
    "ADV_IND", "ADV_DIRECT_IND", "ADV_NONCONN_IND", "SCAN_REQ", "SCAN_RSP", "CONNECT_IND",
    "ADV_SCAN_IND", "ADV_EXT_IND", "PDU_8", "PDU_9", "PDU_10", "PDU_11", "PDU_12", "PDU_13",
    "PDU_14", "PDU_15"};

struct candidate {
    uint64_t n_end;     /* output index of the last access-address symbol */
    uint64_t need;      /* decode once this output index exists */
    float dc, amplitude, power;
    int aa_errors, length; /* length < 0 until the header is read */
};

struct channel {
    int number;                 /* 37, 38 or 39 */
    long offset_khz;            /* channel minus center, snapped to 1 kHz */
    float *mix_c, *mix_s;       /* one exact period of cos/sin(2*pi*offset*n/fs) */
    uint32_t mix_len, mix_pos;
    float *ai, *aq;             /* stage A input history (mixed) */
    uint32_t a_fill, a_next;
    float *bi, *bq;             /* stage B input history (8 MSps) */
    uint32_t b_fill, b_next;
    float *oi, *oq, *od, *op;   /* one block of 4 MSps output, [0] = previous sample */
    float d[HIST], s[HIST], p[HIST];
    uint64_t n;                 /* 4 MSps outputs since the last discontinuity */
    double dsum;                /* sum of the last 128 discriminator values */
    uint32_t reg[SPS];
    struct candidate pending[MAX_PENDING];
    int npending;
    uint64_t quiet_until;
    float floor_blocks[FLOOR_BLOCKS], floor;
    double block_sum;
    uint32_t block_count, floor_next, floor_filled;
    uint8_t whitening[MAX_BITS];
    uint64_t valid, failed, failed_blanked, detections, pending_full;
};

struct device {
    uint8_t key[7];             /* TxAdd, then the address as transmitted */
    uint8_t used;
    uint16_t company, type_mask;
    uint32_t count, per_channel[MAX_CHANNELS];
    double first, last;
    float best_rssi, best_snr;
    char name[32];
};

struct slot {
    int16_t *iq;
    uint32_t count;
    int discontinuity;
    double wall;
};

struct collector {
    /* configuration */
    uint32_t rate, d1, dt;
    uint64_t center_hz;
    int nchannels, aa_max_errors, live;
    float *a_taps, b_taps[B_TAPS];
    int a_len;
    double delay_out;           /* filter delay in 4 MSps output samples */
    struct channel *ch[MAX_CHANNELS];
    /* segment state: reset at every discontinuity */
    double anchor;              /* wall (live) or file time of segment output 0 */
    double seg_base;            /* seconds of earlier segments, for offline time */
    uint64_t seg_in;            /* input samples in this segment */
    uint8_t blank[HIST];        /* exact-zero input, per 4 MSps output period */
    uint64_t blank_last;
    /* outputs */
    FILE *jsonl, *pcap;
    uint64_t max_bytes, bytes, logged, truncated;
    struct device *devices;
    uint32_t ndevices, devices_full;
    /* statistics */
    uint64_t input_samples, zero_samples, segments, dropped, gaps;
    double next_status, started_wall, started_mono;
    int error;
    /* live ring */
    struct slot slots[SLOTS];
    uint64_t head, tail;
    int done, pending_discontinuity;
    struct slot *filling;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    birriasdr_dev_t *dev;
    uint64_t skip, limit, delivered;
};

static double now_realtime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double now_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* atan2 within ~1e-5 rad, branch-free enough for the compiler to vectorize. */
static inline float fast_atan2(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    float a = mn / (mx + 1e-30f), s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    r = ay > ax ? 1.57079637f - r : r;
    r = x < 0 ? 3.14159274f - r : r;
    return y < 0 ? -r : r;
}

static inline float dot(const float *restrict h, const float *restrict x, int n)
{
    float acc = 0;
    for (int k = 0; k < n; ++k)
        acc += h[k] * x[k];
    return acc;
}

/* Data whitening sequence for one channel, as on air (Core Vol 6 Part B 3.2). */
static void make_whitening(uint8_t *out, int channel)
{
    /* Same register as decoders/ble.py: int(f"{chan:08b}"[::-1], 2) | 2. */
    unsigned lfsr = 0;
    for (int i = 0; i < 8; ++i)
        if (channel >> i & 1)
            lfsr |= 1u << (7 - i);
    lfsr |= 2;
    for (int i = 0; i < MAX_BITS; ++i) {
        out[i] = 0;
        if (lfsr & 0x80) {
            lfsr ^= 0x11;
            out[i] = 1;
        }
        lfsr = (lfsr << 1) & 0xFF;
    }
}

/* CRC-24 over dewhitened PDU bits in air order; the received CRC reads MSB first. */
static uint32_t crc24(const uint8_t *bits, int n)
{
    uint32_t crc = 0x555555;
    for (int i = 0; i < n; ++i) {
        uint32_t feedback = (crc >> 23 & 1) ^ bits[i];
        crc = crc << 1 & 0xFFFFFF;
        if (feedback)
            crc ^= 0x00065B;
    }
    return crc;
}

static void pack_bytes(const uint8_t *bits, int nbytes, uint8_t *out)
{
    for (int k = 0; k < nbytes; ++k) {
        uint8_t v = 0;
        for (int b = 0; b < 8; ++b)
            v |= (uint8_t)(bits[8 * k + b] << b);
        out[k] = v;
    }
}

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { put_le16(p, (uint16_t)v); put_le16(p + 2, (uint16_t)(v >> 16)); }

static float percentile20(const float *values, uint32_t n)
{
    float sorted[FLOOR_BLOCKS];
    memcpy(sorted, values, n * sizeof(*sorted));
    for (uint32_t i = 1; i < n; ++i) /* n <= 128, once per millisecond */
        for (uint32_t j = i; j && sorted[j - 1] > sorted[j]; --j) {
            float t = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = t;
        }
    return sorted[n / 5];
}

/* Advertiser table: open addressing on TxAdd + first address field. */
static struct device *device_slot(struct collector *c, const uint8_t key[7])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 7; ++i)
        h = (h ^ key[i]) * 16777619u;
    for (uint32_t probe = 0; probe < DEVICES; ++probe) {
        struct device *d = &c->devices[(h + probe) & (DEVICES - 1)];
        if (d->used && !memcmp(d->key, key, 7))
            return d;
        if (!d->used) {
            if (c->ndevices >= DEVICES * 3 / 4) {
                c->devices_full = 1;
                return NULL;
            }
            memset(d, 0, sizeof(*d));
            memcpy(d->key, key, 7);
            d->used = 1;
            d->company = 0xFFFF;
            d->best_rssi = d->best_snr = -INFINITY;
            ++c->ndevices;
            return d;
        }
    }
    return NULL;
}

static void parse_ad(struct device *d, const uint8_t *ad, int n)
{
    for (int i = 0; i + 1 < n;) {
        int len = ad[i];
        if (!len || i + 1 + len > n)
            break;
        int type = ad[i + 1];
        const uint8_t *v = ad + i + 2;
        if ((type == 0x08 || type == 0x09) && (type == 0x09 || !d->name[0])) {
            int m = len - 1 < (int)sizeof(d->name) - 1 ? len - 1 : (int)sizeof(d->name) - 1;
            memcpy(d->name, v, (size_t)m);
            d->name[m] = 0;
        } else if (type == 0xFF && len >= 3) {
            d->company = (uint16_t)(v[0] | v[1] << 8);
        }
        i += 1 + len;
    }
}

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            fprintf(f, "\\%c", ch);
        else if (ch < 0x20 || ch >= 0x7F)
            fprintf(f, "\\u%04x", ch);
        else
            fputc(ch, f);
    }
    fputc('"', f);
}

static void emit(struct collector *c, struct channel *ch, const struct candidate *k,
                 const uint8_t *bits, int length, int shift)
{
    uint8_t pdu[2 + MAX_PDU + 3];
    int nbytes = 2 + length + 3;
    pack_bytes(bits, nbytes, pdu);
    uint64_t n_start = k->n_end + (uint64_t)shift - SPS * (32 + 8 - 1);
    double t = c->anchor + ((double)n_start + c->delay_out) / OUT_RATE;
    float rssi = 10.0f * log10f(k->power + 1e-20f);
    float snr = ch->floor > 0 ? 10.0f * log10f(k->power / ch->floor) : 0;
    float cfo_khz = k->dc * (float)OUT_RATE / (SPS * 2.0f * 3.14159265f) / 1000.0f;
    int type = pdu[0] & 0xF, txadd = pdu[0] >> 6 & 1;
    int has_addr = type != 7 && length >= 6;
    char addr[18] = "";
    if (has_addr)
        snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 pdu[7], pdu[6], pdu[5], pdu[4], pdu[3], pdu[2]);
    ++ch->valid;

    if (has_addr) {
        uint8_t key[7] = {(uint8_t)txadd, pdu[2], pdu[3], pdu[4], pdu[5], pdu[6], pdu[7]};
        struct device *d = device_slot(c, key);
        if (d) {
            int ci = 0;
            for (int i = 0; i < c->nchannels; ++i)
                if (c->ch[i] == ch)
                    ci = i;
            if (!d->count)
                d->first = t;
            d->last = t;
            ++d->count;
            ++d->per_channel[ci];
            d->type_mask |= (uint16_t)(1u << type);
            if (rssi > d->best_rssi) d->best_rssi = rssi;
            if (snr > d->best_snr) d->best_snr = snr;
            if (type == 0 || type == 2 || type == 4 || type == 6)
                parse_ad(d, pdu + 8, length - 6);
        }
    }

    char line[1024];
    int pos = snprintf(line, sizeof(line),
                       "{\"t\":%.6f,\"ch\":%d,\"rssi_dbfs\":%.1f,\"snr_db\":%.1f,\"cfo_khz\":%.1f,"
                       "\"aa_errors\":%d,\"type\":\"%s\",\"txadd\":%d,\"addr\":%s%s%s,\"pdu\":\"",
                       t, ch->number, rssi, snr, cfo_khz, k->aa_errors, pdu_names[type], txadd,
                       has_addr ? "\"" : "", has_addr ? addr : "null", has_addr ? "\"" : "");
    for (int i = 0; i < 2 + length; ++i)
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%02x", pdu[i]);
    pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "\"}\n");
    size_t pcap_len = c->pcap ? 16 + 10 + 4 + (size_t)nbytes : 0;
    if (c->bytes + (uint64_t)pos + pcap_len + SUMMARY_RESERVE > c->max_bytes) {
        if (!c->truncated++)
            fprintf(stderr, "Storage limit reached: no further packets are logged (counts continue)\n");
        return;
    }
    if (c->jsonl && fwrite(line, 1, (size_t)pos, c->jsonl) != (size_t)pos)
        c->error = 1;
    if (c->pcap) {
        /* Record header, then the LE LL pseudo-header: RF channel index (0, 12, 39
         * for 37, 38, 39), no calibrated power, AA offenses, reference AA, flags:
         * dewhitened, AA offenses and reference AA valid, CRC checked and valid.
         */
        uint8_t rec[16 + 10 + 4 + sizeof(pdu)];
        double whole = floor(t);
        put_le32(rec, (uint32_t)whole);
        put_le32(rec + 4, (uint32_t)((t - whole) * 1e6));
        put_le32(rec + 8, (uint32_t)(pcap_len - 16));
        put_le32(rec + 12, (uint32_t)(pcap_len - 16));
        rec[16] = (uint8_t)(ch->number == 37 ? 0 : ch->number == 38 ? 12 : 39);
        rec[17] = rec[18] = 0;
        rec[19] = (uint8_t)k->aa_errors;
        put_le32(rec + 20, ADV_AA);
        put_le16(rec + 24, 0x0001 | 0x0010 | 0x0020 | 0x0400 | 0x0800);
        put_le32(rec + 26, ADV_AA);
        memcpy(rec + 30, pdu, (size_t)nbytes);
        if (fwrite(rec, 1, pcap_len, c->pcap) != pcap_len)
            c->error = 1;
    }
    c->bytes += (uint64_t)pos + pcap_len;
    ++c->logged;
}

/* Slice, dewhiten and CRC-check a candidate at its timing, then one sample either side. */
static void decode(struct collector *c, struct channel *ch, struct candidate *k)
{
    static const int shifts[3] = {0, -1, 1};
    int total = 16 + 8 * k->length + 24;
    uint8_t bits[MAX_BITS];
    for (int t = 0; t < 3; ++t) {
        for (int i = 0; i < total; ++i)
            bits[i] = (uint8_t)((ch->s[(k->n_end + (uint64_t)shifts[t] + SPS * (uint64_t)(i + 1)) & HMASK] > k->dc) ^
                                ch->whitening[i]);
        uint32_t received = 0;
        for (int i = 0; i < 24; ++i)
            received = received << 1 | bits[total - 24 + i];
        if (crc24(bits, total - 24) == received) {
            emit(c, ch, k, bits, k->length, shifts[t]);
            ch->quiet_until = k->n_end + SPS * (uint64_t)total;
            return;
        }
    }
    ++ch->failed;
    /* Blanking diagnostic: exact-zero input anywhere under the packet's filter span. */
    uint64_t first = k->n_end - SPS * 40, last = k->n_end + SPS * (uint64_t)total + 26;
    for (uint64_t m = first; m <= last && m < c->blank_last + 1; ++m)
        if (c->blank[m & HMASK]) {
            ++ch->failed_blanked;
            break;
        }
}

static void detect(struct collector *c, struct channel *ch, uint64_t n)
{
    double sum_s = 0, sum_b = 0, sum_sb = 0, power = 0;
    for (int i = 0; i < 32; ++i) {
        float s = ch->s[(n - SPS * (uint64_t)(31 - i)) & HMASK];
        float b = ADV_AA >> i & 1 ? 1.0f : -1.0f;
        sum_s += s;
        sum_b += b;
        sum_sb += s * b;
    }
    for (int i = 0; i < 32 * SPS; ++i)
        power += ch->p[(n - (uint64_t)i) & HMASK];
    double mb = sum_b / 32, ms = sum_s / 32;
    float amplitude = (float)((sum_sb / 32 - ms * mb) / (1 - mb * mb));
    float dc = (float)(ms - amplitude * mb);
    if (amplitude <= 0)
        return;
    int errors = 0;
    for (int i = 0; i < 32; ++i)
        errors += (ch->s[(n - SPS * (uint64_t)(31 - i)) & HMASK] > dc) != (int)(ADV_AA >> i & 1);
    if (errors > c->aa_max_errors)
        return;
    ++ch->detections;
    /* Adjacent sample phases of one packet: keep the best-centred timing. */
    for (int i = 0; i < ch->npending; ++i) {
        struct candidate *k = &ch->pending[i];
        if (k->length < 0 && n - k->n_end <= 2) {
            if (amplitude > k->amplitude) {
                k->n_end = n;
                k->need = n + SPS * 16;
                k->dc = dc;
                k->amplitude = amplitude;
                k->power = (float)(power / (32 * SPS));
                k->aa_errors = errors;
            }
            return;
        }
    }
    if (ch->npending == MAX_PENDING) {
        ++ch->pending_full;
        return;
    }
    ch->pending[ch->npending++] = (struct candidate){
        n, n + SPS * 16, dc, amplitude, (float)(power / (32 * SPS)), errors, -1};
}

static void service(struct collector *c, struct channel *ch, uint64_t n)
{
    for (int i = 0; i < ch->npending;) {
        struct candidate *k = &ch->pending[i];
        if (k->need > n) {
            ++i;
            continue;
        }
        if (k->length < 0) {
            int length = 0;
            for (int b = 0; b < 8; ++b)
                length |= ((ch->s[(k->n_end + SPS * (uint64_t)(9 + b)) & HMASK] > k->dc) ^
                           ch->whitening[8 + b]) << b;
            if (length) {
                k->length = length;
                k->need = k->n_end + SPS * (uint64_t)(16 + 8 * length + 24) + 1;
                ++i;
                continue;
            }
            ++ch->failed; /* zero-length PDUs are not advertising */
        } else if (k->n_end >= ch->quiet_until) {
            decode(c, ch, k);
        }
        ch->pending[i] = ch->pending[--ch->npending];
    }
}

static void channel_reset(struct channel *ch)
{
    ch->a_fill = ch->a_next = ch->b_fill = ch->b_next = 0;
    ch->oi[0] = ch->oq[0] = 0;
    memset(ch->d, 0, sizeof(ch->d));
    memset(ch->s, 0, sizeof(ch->s));
    memset(ch->p, 0, sizeof(ch->p));
    ch->n = 0;
    ch->dsum = 0;
    memset(ch->reg, 0, sizeof(ch->reg));
    ch->npending = 0;
    ch->quiet_until = 0;
    ch->block_sum = 0;
    ch->block_count = 0;
}

/* Mix, filter and demodulate one block of input for one channel. */
static void channel_process(struct collector *c, struct channel *ch, const int16_t *iq, uint32_t count)
{
    const float scale = 1.0f / 8192;
    for (uint32_t i = 0; i < count;) {
        uint32_t m = count - i < ch->mix_len - ch->mix_pos ? count - i : ch->mix_len - ch->mix_pos;
        const float *mc = ch->mix_c + ch->mix_pos, *ms = ch->mix_s + ch->mix_pos;
        const int16_t *x = iq + 2 * (size_t)i;
        float *ai = ch->ai + ch->a_fill, *aq = ch->aq + ch->a_fill;
        for (uint32_t j = 0; j < m; ++j) {
            float xi = x[2 * j] * scale, xq = x[2 * j + 1] * scale;
            ai[j] = xi * mc[j] + xq * ms[j];
            aq[j] = xq * mc[j] - xi * ms[j];
        }
        ch->a_fill += m;
        ch->mix_pos = (ch->mix_pos + m) % ch->mix_len;
        i += m;
    }
    for (; ch->a_next + (uint32_t)c->a_len <= ch->a_fill; ch->a_next += c->d1) {
        ch->bi[ch->b_fill] = dot(c->a_taps, ch->ai + ch->a_next, c->a_len);
        ch->bq[ch->b_fill++] = dot(c->a_taps, ch->aq + ch->a_next, c->a_len);
    }
    memmove(ch->ai, ch->ai + ch->a_next, (ch->a_fill - ch->a_next) * sizeof(float));
    memmove(ch->aq, ch->aq + ch->a_next, (ch->a_fill - ch->a_next) * sizeof(float));
    ch->a_fill -= ch->a_next;
    ch->a_next = 0;

    uint32_t out = 0;
    for (; ch->b_next + B_TAPS <= ch->b_fill; ch->b_next += 2, ++out) {
        ch->oi[1 + out] = dot(c->b_taps, ch->bi + ch->b_next, B_TAPS);
        ch->oq[1 + out] = dot(c->b_taps, ch->bq + ch->b_next, B_TAPS);
    }
    memmove(ch->bi, ch->bi + ch->b_next, (ch->b_fill - ch->b_next) * sizeof(float));
    memmove(ch->bq, ch->bq + ch->b_next, (ch->b_fill - ch->b_next) * sizeof(float));
    ch->b_fill -= ch->b_next;
    ch->b_next = 0;

    for (uint32_t j = 0; j < out; ++j) { /* vectorizable discriminator and power */
        float yi = ch->oi[j + 1], yq = ch->oq[j + 1], pi = ch->oi[j], pq = ch->oq[j];
        ch->od[j] = fast_atan2(yq * pi - yi * pq, yi * pi + yq * pq);
        ch->op[j] = yi * yi + yq * yq;
    }
    if (out) {
        ch->oi[0] = ch->oi[out];
        ch->oq[0] = ch->oq[out];
    }
    for (uint32_t j = 0; j < out; ++j) {
        uint64_t n = ch->n++;
        uint32_t k = (uint32_t)n & HMASK;
        float d = ch->od[j];
        ch->dsum += d - ch->d[(n - 128) & HMASK];
        ch->d[k] = d;
        float s = d + ch->d[(n - 1) & HMASK] + ch->d[(n - 2) & HMASK] + ch->d[(n - 3) & HMASK];
        ch->s[k] = s;
        ch->p[k] = ch->op[j];
        ch->block_sum += ch->op[j];
        if (++ch->block_count == FLOOR_BLOCK) {
            ch->floor_blocks[ch->floor_next] = (float)(ch->block_sum / FLOOR_BLOCK);
            ch->floor_next = (ch->floor_next + 1) % FLOOR_BLOCKS;
            if (ch->floor_filled < FLOOR_BLOCKS)
                ++ch->floor_filled;
            ch->floor = percentile20(ch->floor_blocks, ch->floor_filled);
            ch->block_sum = 0;
            ch->block_count = 0;
        }
        if (!(n & 0xFFFFF)) { /* refresh the running sum against rounding drift */
            ch->dsum = 0;
            for (int i = 0; i < 128; ++i)
                ch->dsum += ch->d[(n - (uint64_t)i) & HMASK];
        }
        /* Hard decision against the local mean frequency, which absorbs carrier offset. */
        uint32_t phase = (uint32_t)n & (SPS - 1);
        uint32_t bit = s * 32.0f > (float)ch->dsum;
        ch->reg[phase] = ch->reg[phase] >> 1 | bit << 31;
        if (n >= 160 && n >= ch->quiet_until &&
            __builtin_popcount(ch->reg[phase] ^ ADV_AA) <= c->aa_max_errors)
            detect(c, ch, n);
        if (ch->npending)
            service(c, ch, n);
    }
}

static void print_status(struct collector *c, double seconds)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
    double wall = now_monotonic() - c->started_mono;
    fprintf(stderr, "%7.1f s", seconds);
    for (int i = 0; i < c->nchannels; ++i)
        fprintf(stderr, "  ch%d %llu ok/%llu bad", c->ch[i]->number, (unsigned long long)c->ch[i]->valid,
                (unsigned long long)c->ch[i]->failed);
    fprintf(stderr, "  advertisers %u  log %.2f MB  cpu %.0f%%  dropped %llu  gaps %llu\n", c->ndevices,
            c->bytes / 1e6, wall > 0 ? 100 * cpu / wall : 0, (unsigned long long)c->dropped,
            (unsigned long long)c->gaps);
}

/* Worker-side entry for one block of input; a discontinuity resets all state. */
static void process(struct collector *c, const int16_t *iq, uint32_t count, int discontinuity, double wall)
{
    if (discontinuity) {
        if (c->segments)
            c->seg_base += (double)c->seg_in / c->rate;
        ++c->segments;
        c->anchor = c->live ? wall : c->seg_base;
        c->seg_in = 0;
        memset(c->blank, 0, sizeof(c->blank));
        c->blank_last = 0;
        for (int i = 0; i < c->nchannels; ++i)
            channel_reset(c->ch[i]);
    }
    /* Exact-zero samples (receiver blanking) at 4 MSps resolution. */
    const uint32_t *pairs = (const uint32_t *)(const void *)iq;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t m = (c->seg_in + i) / c->dt;
        if (m != c->blank_last || !(c->seg_in + i)) {
            c->blank_last = m;
            c->blank[m & HMASK] = 0;
        }
        if (!pairs[i]) {
            c->blank[m & HMASK] = 1;
            ++c->zero_samples;
        }
    }
    c->seg_in += count;
    c->input_samples += count;
    for (int i = 0; i < c->nchannels; ++i)
        channel_process(c, c->ch[i], iq, count);
    double seconds = (double)c->input_samples / c->rate;
    if (seconds >= c->next_status) {
        print_status(c, seconds);
        c->next_status += 10;
        if (c->jsonl) fflush(c->jsonl);
        if (c->pcap) fflush(c->pcap);
    }
}

/* ---- live input: callback copies into the ring, worker thread demodulates ---- */

static void publish(struct collector *c)
{
    if (!c->filling)
        return;
    pthread_mutex_lock(&c->lock);
    ++c->head;
    pthread_cond_signal(&c->ready);
    pthread_mutex_unlock(&c->lock);
    c->filling = NULL;
}

static void on_samples(const int16_t *iq, uint32_t count, uint64_t first, void *context)
{
    struct collector *c = context;
    (void)first;
    if (c->skip) { /* packet-detect settling prefix */
        uint32_t n = count < c->skip ? count : (uint32_t)c->skip;
        c->skip -= n;
        iq += (size_t)n * 2;
        count -= n;
    }
    if (c->limit && count > c->limit - c->delivered)
        count = (uint32_t)(c->limit - c->delivered);
    c->delivered += count;
    while (count) {
        if (!c->filling) {
            pthread_mutex_lock(&c->lock);
            int full = c->head - c->tail == SLOTS;
            pthread_mutex_unlock(&c->lock);
            if (full) { /* never stall USB: drop and mark the discontinuity */
                c->dropped += count;
                c->pending_discontinuity = 1;
                break;
            }
            c->filling = &c->slots[c->head % SLOTS];
            c->filling->count = 0;
            c->filling->discontinuity = c->pending_discontinuity;
            c->filling->wall = now_realtime();
            c->pending_discontinuity = 0;
        }
        uint32_t n = SLOT_SAMPLES - c->filling->count;
        if (n > count)
            n = count;
        memcpy(c->filling->iq + 2 * (size_t)c->filling->count, iq, (size_t)n * 4);
        c->filling->count += n;
        iq += (size_t)n * 2;
        count -= n;
        if (c->filling->count == SLOT_SAMPLES)
            publish(c);
    }
    if (interrupted || c->error || (c->limit && c->delivered >= c->limit))
        birriasdr_cancel_async(c->dev);
}

static void on_gap(const struct birriasdr_recovery_event *event, void *context)
{
    struct collector *c = context;
    (void)event;
    ++c->gaps;
    publish(c);
    c->pending_discontinuity = 1;
}

static void *worker(void *context)
{
    struct collector *c = context;
    for (;;) {
        pthread_mutex_lock(&c->lock);
        while (c->tail == c->head && !c->done)
            pthread_cond_wait(&c->ready, &c->lock);
        if (c->tail == c->head) {
            pthread_mutex_unlock(&c->lock);
            return NULL;
        }
        struct slot *s = &c->slots[c->tail % SLOTS];
        pthread_mutex_unlock(&c->lock);
        process(c, s->iq, s->count, s->discontinuity, s->wall);
        pthread_mutex_lock(&c->lock);
        ++c->tail;
        pthread_mutex_unlock(&c->lock);
    }
}

/* ---- setup ---- */

static int design_filters(struct collector *c)
{
    if (c->rate % MID_RATE) {
        fprintf(stderr, "Sample rate must be a multiple of 8 MHz (40000000 or 80000000)\n");
        return -1;
    }
    c->d1 = c->rate / MID_RATE;
    c->dt = c->rate / OUT_RATE;
    /* Order-3 CIC response as a FIR: nulls on every alias of the 8 MSps band. */
    c->a_len = 3 * (int)c->d1 - 2;
    c->a_taps = calloc((size_t)c->a_len, sizeof(float));
    if (!c->a_taps)
        return -1;
    double norm = (double)c->d1 * c->d1 * c->d1;
    for (int k = 0; k < c->a_len; ++k) {
        int sum = 0;
        for (int i = 0; i < (int)c->d1; ++i)
            for (int j = 0; j < (int)c->d1; ++j) {
                int l = k - i - j;
                sum += l >= 0 && l < (int)c->d1;
            }
        c->a_taps[k] = (float)(sum / norm);
    }
    /* 0.75 MHz Hamming-windowed sinc at 8 MSps: passes LE 1M GFSK plus +/-150 kHz CFO. */
    double total = 0, h[B_TAPS];
    for (int k = 0; k < B_TAPS; ++k) {
        double x = k - (B_TAPS - 1) / 2.0, fc = 0.75e6 / MID_RATE;
        h[k] = (x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x)) *
               (0.54 - 0.46 * cos(2 * M_PI * k / (B_TAPS - 1)));
        total += h[k];
    }
    for (int k = 0; k < B_TAPS; ++k)
        c->b_taps[k] = (float)(h[k] / total);
    c->delay_out = ((c->a_len - 1) / 2.0 / c->d1 + (B_TAPS - 1) / 2.0) / 2.0;
    return 0;
}

static struct channel *channel_create(const struct collector *c, int number)
{
    struct channel *ch = calloc(1, sizeof(*ch));
    if (!ch)
        return NULL;
    long freq_khz = 2402000 + (number == 37 ? 0 : number == 38 ? 24000 : 78000);
    ch->number = number;
    ch->offset_khz = freq_khz - (long)((c->center_hz + 500) / 1000);
    ch->mix_len = c->rate / 1000; /* a 1 kHz grid makes one table period exact */
    ch->mix_c = malloc(ch->mix_len * sizeof(float));
    ch->mix_s = malloc(ch->mix_len * sizeof(float));
    ch->ai = malloc((SLOT_SAMPLES + (size_t)c->a_len) * sizeof(float));
    ch->aq = malloc((SLOT_SAMPLES + (size_t)c->a_len) * sizeof(float));
    size_t mid = SLOT_SAMPLES / c->d1 + B_TAPS + 2, out = mid / 2 + 2;
    ch->bi = malloc(mid * sizeof(float));
    ch->bq = malloc(mid * sizeof(float));
    ch->oi = malloc(out * sizeof(float));
    ch->oq = malloc(out * sizeof(float));
    ch->od = malloc(out * sizeof(float));
    ch->op = malloc(out * sizeof(float));
    if (!ch->mix_c || !ch->mix_s || !ch->ai || !ch->aq || !ch->bi || !ch->bq || !ch->oi || !ch->oq ||
        !ch->od || !ch->op)
        return NULL;
    for (uint32_t i = 0; i < ch->mix_len; ++i) {
        long cycle = (long)(((long long)ch->offset_khz * i) % (long long)ch->mix_len);
        double phase = 2 * M_PI * (double)cycle / ch->mix_len;
        ch->mix_c[i] = (float)cos(phase);
        ch->mix_s[i] = (float)sin(phase);
    }
    make_whitening(ch->whitening, number);
    channel_reset(ch);
    return ch;
}

static int compare_devices(const void *a, const void *b)
{
    const struct device *x = *(const struct device *const *)a, *y = *(const struct device *const *)b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static int write_summary(struct collector *c, const char *path, double wall_seconds, int result)
{
    FILE *f = fopen(path, "wx");
    struct rusage ru;
    if (!f)
        return -1;
    getrusage(RUSAGE_SELF, &ru);
    struct device **list = malloc((c->ndevices + 1) * sizeof(*list));
    uint32_t n = 0;
    for (uint32_t i = 0; list && i < DEVICES; ++i)
        if (c->devices[i].used)
            list[n++] = &c->devices[i];
    if (list)
        qsort(list, n, sizeof(*list), compare_devices);
    time_t started = (time_t)c->started_wall;
    char iso[32];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", gmtime(&started));
    fprintf(f, "{\n  \"tool\": \"birria_ble\",\n  \"started_utc\": \"%s\",\n  \"source\": \"%s\",\n"
               "  \"time_reference\": \"%s\",\n  \"center_hz\": %llu,\n  \"rate\": %u,\n"
               "  \"rf_seconds\": %.3f,\n  \"wall_seconds\": %.3f,\n  \"stream\": \"%s\",\n",
            iso, c->live ? "device" : "file", c->live ? "unix" : "file start",
            (unsigned long long)c->center_hz, c->rate, (double)c->input_samples / c->rate, wall_seconds,
            birriasdr_strerror(result));
    fprintf(f, "  \"input_samples\": %llu,\n  \"zero_samples\": %llu,\n  \"dropped_samples\": %llu,\n"
               "  \"gaps\": %llu,\n  \"segments\": %llu,\n",
            (unsigned long long)c->input_samples, (unsigned long long)c->zero_samples,
            (unsigned long long)c->dropped, (unsigned long long)c->gaps, (unsigned long long)c->segments);
    fprintf(f, "  \"cpu_user_s\": %.2f,\n  \"cpu_system_s\": %.2f,\n  \"max_rss_kb\": %ld,\n",
            ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6,
            ru.ru_maxrss);
    fprintf(f, "  \"packets_logged\": %llu,\n  \"packets_not_logged\": %llu,\n  \"log_bytes\": %llu,\n"
               "  \"max_bytes\": %llu,\n  \"advertiser_table_full\": %s,\n  \"channels\": [",
            (unsigned long long)c->logged, (unsigned long long)c->truncated, (unsigned long long)c->bytes,
            (unsigned long long)c->max_bytes, c->devices_full ? "true" : "false");
    for (int i = 0; i < c->nchannels; ++i) {
        struct channel *ch = c->ch[i];
        fprintf(f, "%s\n    {\"ch\": %d, \"offset_hz\": %ld, \"valid\": %llu, \"failed\": %llu, "
                   "\"failed_blanked\": %llu, \"detections\": %llu, \"pending_full\": %llu}",
                i ? "," : "", ch->number, ch->offset_khz * 1000, (unsigned long long)ch->valid,
                (unsigned long long)ch->failed, (unsigned long long)ch->failed_blanked,
                (unsigned long long)ch->detections, (unsigned long long)ch->pending_full);
    }
    fprintf(f, "\n  ],\n  \"advertisers\": [");
    for (uint32_t i = 0; i < n; ++i) {
        struct device *d = list[i];
        fprintf(f, "%s\n    {\"addr\": \"%02x:%02x:%02x:%02x:%02x:%02x\", \"random\": %s, \"packets\": %u, \"per_channel\": [",
                i ? "," : "", d->key[6], d->key[5], d->key[4], d->key[3], d->key[2], d->key[1],
                d->key[0] ? "true" : "false", d->count);
        for (int j = 0; j < c->nchannels; ++j)
            fprintf(f, "%s%u", j ? ", " : "", d->per_channel[j]);
        fprintf(f, "], \"first\": %.6f, \"last\": %.6f, \"best_rssi_dbfs\": %.1f, \"best_snr_db\": %.1f, \"types\": [",
                d->first, d->last, d->best_rssi, d->best_snr);
        for (int t = 0, k = 0; t < 16; ++t)
            if (d->type_mask >> t & 1)
                fprintf(f, "%s\"%s\"", k++ ? ", " : "", pdu_names[t]);
        fprintf(f, "]");
        if (d->company != 0xFFFF)
            fprintf(f, ", \"company_id\": %u", d->company);
        if (d->name[0]) {
            fprintf(f, ", \"name\": ");
            json_string(f, d->name);
        }
        fprintf(f, "}");
    }
    fprintf(f, "\n  ]\n}\n");
    free(list);
    return fclose(f) ? -1 : 0;
}

static int parse_number(const char *text, uint64_t *value)
{
    char *end;
    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    unsigned long long number = strtoull(text, &end, 10);
    if (errno || *end || !number)
        return -1;
    *value = (uint64_t)number;
    return 0;
}

static int parse_gain_code(const char *text, unsigned maximum, uint8_t *value)
{
    char *end;
    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    unsigned long code = strtoul(text, &end, 10);
    if (errno || *end || code > maximum)
        return -1;
    *value = (uint8_t)code;
    return 0;
}

static FILE *open_output(const char *prefix, const char *suffix, char *path, size_t size)
{
    if (snprintf(path, size, "%s%s", prefix, suffix) >= (int)size)
        return NULL;
    FILE *f = fopen(path, "wbx");
    if (f)
        setvbuf(f, NULL, _IOFBF, 1 << 20);
    else
        fprintf(stderr, "%s: %s (outputs must not already exist)\n", path, strerror(errno));
    return f;
}

int main(int argc, char **argv)
{
    static struct collector c;
    const char *input = NULL;
    uint64_t frequency = 2414000000ull, rate = 40000000, seconds = 0, max_bytes = 5000000000ull;
    uint64_t aa_errors = 1;
    struct birriasdr_gain gain = {2, 8, 0};
    int argument = 1, explicit_gain = 0, pd_n8 = 0, channel_filter = 0, no_pcap = 0;
    int channels[MAX_CHANNELS], nchannels = 0, result = 0;
    char path[4096], summary_path[4096];

    while (argument < argc && !strncmp(argv[argument], "--", 2)) {
        const char *opt = argv[argument];
        int has = argument + 1 < argc;
        if (!strcmp(opt, "--input") && has) {
            input = argv[++argument];
        } else if (!strcmp(opt, "--frequency") && has) {
            if (parse_number(argv[++argument], &frequency)) return 1;
        } else if (!strcmp(opt, "--rate") && has) {
            if (parse_number(argv[++argument], &rate) || rate > UINT32_MAX) return 1;
        } else if (!strcmp(opt, "--seconds") && has) {
            if (parse_number(argv[++argument], &seconds)) return 1;
        } else if (!strcmp(opt, "--max-bytes") && has) {
            if (parse_number(argv[++argument], &max_bytes)) return 1;
        } else if (!strcmp(opt, "--aa-errors") && has) {
            if (!strcmp(argv[argument + 1], "0")) aa_errors = 0, ++argument;
            else if (parse_number(argv[++argument], &aa_errors) || aa_errors > 4) return 1;
        } else if (!strcmp(opt, "--channels") && has) {
            char *list = argv[++argument], *save = NULL;
            for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                int number = atoi(tok);
                if ((number < 37 || number > 39) || nchannels == MAX_CHANNELS) {
                    fprintf(stderr, "--channels takes a comma list of 37, 38, 39\n");
                    return 1;
                }
                channels[nchannels++] = number;
            }
        } else if (!strcmp(opt, "--gain") && argument + 3 < argc) {
            if (parse_gain_code(argv[argument + 1], 5, &gain.lna) ||
                parse_gain_code(argv[argument + 2], 15, &gain.lpf) ||
                parse_gain_code(argv[argument + 3], 12, &gain.digital)) {
                fprintf(stderr, "Gain codes require LNA 0..5, filter 0..8 (0..15 at 5 GHz), "
                                "digital 0..12\n");
                return 1;
            }
            explicit_gain = 1;
            argument += 3;
        } else if (!strcmp(opt, "--pd-n8")) {
            pd_n8 = 1;
        } else if (!strcmp(opt, "--channel-filter")) {
            channel_filter = 1;
        } else if (!strcmp(opt, "--no-pcap")) {
            no_pcap = 1;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", opt);
            return 1;
        }
        ++argument;
    }
    if (argc - argument != 1) {
        fprintf(stderr,
                "Usage: %s [--frequency Hz] [--rate 40000000|80000000] [--channels 37,38,39] [--seconds N]\n"
                "          [--gain LNA FILTER DIGITAL] [--pd-n8] [--channel-filter] [--max-bytes N]\n"
                "          [--aa-errors 0..4] [--no-pcap] [--input capture.cs16|-] output-prefix\n"
                "Writes output-prefix.jsonl, .pcap and .summary.json. Defaults: 2414 MHz, 40 MSps\n"
                "(channels 37 and 38), 5 GB storage cap. --input decodes a recording instead of the device.\n",
                argv[0]);
        return 1;
    }
    const char *prefix = argv[argument];
    c.rate = (uint32_t)rate;
    c.center_hz = frequency;
    c.aa_max_errors = (int)aa_errors;
    c.max_bytes = max_bytes;
    c.live = !input;
    if (design_filters(&c))
        return 1;
    if (!nchannels) /* every advertising channel at least 3 MHz inside Nyquist */
        for (int number = 37; number <= 39; ++number) {
            double offset = 2402e6 + (number == 37 ? 0 : number == 38 ? 24e6 : 78e6) - (double)frequency;
            if (fabs(offset) <= rate / 2.0 - 3e6)
                channels[nchannels++] = number;
        }
    if (!nchannels) {
        fprintf(stderr, "No advertising channel (37=2402, 38=2426, 39=2480 MHz) inside the capture\n");
        return 1;
    }
    c.nchannels = nchannels;
    for (int i = 0; i < nchannels; ++i)
        if (!(c.ch[i] = channel_create(&c, channels[i]))) {
            fprintf(stderr, "Out of memory\n");
            return 1;
        }
    c.devices = calloc(DEVICES, sizeof(*c.devices));
    if (!c.devices)
        return 1;

    struct statvfs vfs;
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", prefix);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");
    if (!statvfs(dir, &vfs) && (uint64_t)vfs.f_bavail * vfs.f_frsize < max_bytes)
        fprintf(stderr, "Note: %.1f GB free here, below the %.1f GB cap (under WSL, check the host drive too)\n",
                (double)vfs.f_bavail * vfs.f_frsize / 1e9, max_bytes / 1e9);
    if (max_bytes <= SUMMARY_RESERVE) {
        fprintf(stderr, "--max-bytes must exceed the %u-byte summary reserve\n", SUMMARY_RESERVE);
        return 1;
    }
    if (!(c.jsonl = open_output(prefix, ".jsonl", path, sizeof(path))))
        return 1;
    if (!no_pcap) {
        if (!(c.pcap = open_output(prefix, ".pcap", path, sizeof(path))))
            return 1;
        uint8_t header[24];
        put_le32(header, 0xA1B2C3D4);
        put_le16(header + 4, 2);
        put_le16(header + 6, 4);
        put_le32(header + 8, 0);
        put_le32(header + 12, 0);
        put_le32(header + 16, 512);
        put_le32(header + 20, 256); /* LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR */
        fwrite(header, 1, sizeof(header), c.pcap);
        c.bytes += sizeof(header);
    }
    if (snprintf(summary_path, sizeof(summary_path), "%s.summary.json", prefix) >= (int)sizeof(summary_path))
        return 1;

    fprintf(stderr, "Center %.3f MHz, %.0f MSps, channels", frequency / 1e6, rate / 1e6);
    for (int i = 0; i < nchannels; ++i)
        fprintf(stderr, " %d (%+ld kHz)", c.ch[i]->number, c.ch[i]->offset_khz);
    fprintf(stderr, "; storage cap %.2f GB\n", max_bytes / 1e9);

    c.started_wall = now_realtime();
    c.started_mono = now_monotonic();
    c.next_status = 10;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (input) {
        FILE *in = strcmp(input, "-") ? fopen(input, "rb") : stdin;
        int16_t *iq = malloc(SLOT_SAMPLES * 4);
        if (!in || !iq) {
            perror(input);
            return 1;
        }
        uint64_t limit = seconds * rate, done = 0;
        int first = 1;
        size_t n;
        while (!interrupted && !c.error && (n = fread(iq, 4, SLOT_SAMPLES, in)) > 0) {
            if (limit && n > limit - done)
                n = (size_t)(limit - done);
            process(&c, iq, (uint32_t)n, first, 0);
            first = 0;
            done += n;
            if (limit && done >= limit)
                break;
        }
        if (in != stdin)
            fclose(in);
        free(iq);
    } else {
        birriasdr_dev_t *dev = NULL;
        pthread_t thread;
        struct birriasdr_stream_stats stats;
        for (uint32_t i = 0; i < SLOTS; ++i)
            if (!(c.slots[i].iq = malloc(SLOT_SAMPLES * 4)))
                return 1;
        pthread_mutex_init(&c.lock, NULL);
        pthread_cond_init(&c.ready, NULL);
        c.pending_discontinuity = 1;
        c.limit = seconds * rate;
        /* --pd-n8 settling: discard ~600 ms, as birria_rx does. */
        c.skip = pd_n8 ? (uint64_t)11719 * 2048 * (rate / 40000000) : 0;
        result = birriasdr_open(&dev, 0);
        if (result < 0) {
            fprintf(stderr, "Open: %s\n", birriasdr_strerror(result));
            return 1;
        }
        c.dev = dev;
        if (!result) result = birriasdr_set_sample_rate(dev, (uint32_t)rate);
        if (!result && pd_n8) result = birriasdr_set_packet_detect_suppression(dev, 1);
        if (!result && channel_filter) result = birriasdr_set_channel_filter(dev, 1);
        if (!result) result = birriasdr_set_center_freq(dev, frequency);
        if (!result) result = birriasdr_set_recovery(dev, 16, on_gap, &c);
        if (!result && explicit_gain) {
            result = birriasdr_set_tuner_gain_mode(dev, 1);
            if (!result) {
                result = birriasdr_set_gain_codes(dev, &gain);
                if (result == BIRRIASDR_ERROR_INVALID) {
                    struct birriasdr_gain maximum;
                    if (!birriasdr_get_gain_code_limits(dev, &maximum))
                        fprintf(stderr, "Gain codes exceed this band and chain's limits: LNA 0..%u, "
                                                "filter 0..%u, digital 0..%u\n", maximum.lna, maximum.lpf, maximum.digital);
                }
            }
        }
        if (result) {
            fprintf(stderr, "Setup: %s\n", birriasdr_strerror(result));
            birriasdr_close(dev);
            return 1;
        }
        if (pthread_create(&thread, NULL, worker, &c)) {
            birriasdr_close(dev);
            return 1;
        }
        result = birriasdr_read_async(dev, on_samples, &c, 0, 0);
        publish(&c);
        pthread_mutex_lock(&c.lock);
        c.done = 1;
        pthread_cond_signal(&c.ready);
        pthread_mutex_unlock(&c.lock);
        pthread_join(thread, NULL);
        birriasdr_get_stream_stats(dev, &stats);
        if (birriasdr_close(dev))
            c.error = 1;
        fprintf(stderr, "Stream %s; firmware state=%u; stage=%d\n", birriasdr_strerror(result),
                stats.firmware_state, stats.stage);
    }

    double wall = now_monotonic() - c.started_mono;
    print_status(&c, (double)c.input_samples / c.rate);
    if (c.jsonl && fclose(c.jsonl)) c.error = 1;
    if (c.pcap && fclose(c.pcap)) c.error = 1;
    if (write_summary(&c, summary_path, wall, result)) {
        fprintf(stderr, "%s: %s\n", summary_path, strerror(errno));
        c.error = 1;
    }
    fprintf(stderr, "%u advertisers, %llu packets logged (%.2f MB) in %s.{jsonl%s,summary.json}%s\n",
            c.ndevices, (unsigned long long)c.logged, c.bytes / 1e6, prefix, no_pcap ? "" : ",pcap",
            c.dropped ? "; WARNING: samples were dropped (demodulator behind real time)" : "");
    return result || c.error ? 1 : 0;
}
