/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 * SoapySDR module over libbirriasdr: one RX stream, raw gain indices plus the
 * library's measured overall gain in dB, no DSP beyond its optional 2:1
 * decimation.
 */
#include "bands.hpp"
#include "birriasdr.h"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double SAMPLE_RATE = 40e6;
/* Library host decimation (birriasdr_set_sample_rate()). */
constexpr double DECIMATED_RATE = 20e6;
/* Experimental single-chain rate with the BW80 filter profile. */
constexpr double WIDE_RATE = 80e6;
constexpr double FULL_SCALE = 8192.0;
constexpr size_t DEFAULT_BUFFER_SAMPLES = 65536;
constexpr size_t DEFAULT_BUFFERS = 64;
constexpr unsigned DEFAULT_MAX_RESTARTS = 16;

/* maximum is the fallback; element_maximum() asks the library, whose limits
 * depend on band and receive chain (LPF 0..15 for chain 0 at 5 GHz). */
struct GainElement { const char *name; unsigned maximum; uint8_t birriasdr_gain::*code; };
const GainElement GAIN_ELEMENTS[] = {
    {"LNA", 5, &birriasdr_gain::lna},
    {"LPF", 8, &birriasdr_gain::lpf},
    {"DIG", 12, &birriasdr_gain::digital},
};

void check(int result, const char *what)
{
    if (result < 0)
        throw std::runtime_error(std::string("BirriaSDR ") + what + ": " +
                                 birriasdr_strerror(result));
}

/* Per-channel setters log instead of throwing: some applications (SDR++)
 * call them without a try block, and an exception there aborts the process.
 */
bool report(int result, const char *what)
{
    if (result < 0)
        SoapySDR::logf(SOAPY_SDR_ERROR, "BirriaSDR %s: %s", what, birriasdr_strerror(result));
    return result >= 0;
}

const GainElement &gain_element(const std::string &name)
{
    for (const GainElement &element : GAIN_ELEMENTS)
        if (name == element.name)
            return element;
    throw std::runtime_error("BirriaSDR: unknown gain element " + name);
}

std::string usb_location(const birriasdr_device_info &info)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%03u:%03u", info.bus, info.address);
    return text;
}

/* Live setters return BUSY while recovery restarts the acquisition; wait it out. */
template <typename Call> int retry_busy(Call call)
{
    int result = call();
    for (int i = 0; result == BIRRIASDR_ERROR_BUSY && i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        result = call();
    }
    return result;
}

bool parse_flag(const std::string &value)
{
    return value == "true" || value == "1";
}

/* Supported receivers matching the optional index or USB location arguments.
 * Indices are enumeration snapshots, so bus/address wins when both are given.
 */
std::vector<std::pair<uint32_t, birriasdr_device_info>> matching_devices(const SoapySDR::Kwargs &args)
{
    std::vector<std::pair<uint32_t, birriasdr_device_info>> found;
    int count = birriasdr_get_device_count();
    if (count < 0)
        return found;
    for (int i = 0; i < count; ++i) {
        birriasdr_device_info info;
        if (birriasdr_get_device_info((uint32_t)i, &info) || !(info.capabilities & BIRRIASDR_CAP_RX))
            continue;
        if (args.count("usb") && args.at("usb") != usb_location(info))
            continue;
        if (!args.count("usb") && args.count("index") && args.at("index") != std::to_string(i))
            continue;
        found.emplace_back((uint32_t)i, info);
    }
    return found;
}

class BirriaSDRDevice;

/* SDR++ exits without deleting its source, so a running stream never reaches
 * the library's cleanup, which restores gain overrides that a WFSYS reset does
 * not clear; the next start then refuses the forced baseline. Stop any live
 * stream when the module is unloaded at normal process exit. The registry is
 * leaked on purpose so devices destroyed later can still unregister.
 */
struct LiveDevices {
    std::mutex mutex;
    std::vector<BirriaSDRDevice *> devices;
};
LiveDevices &live_devices()
{
    static LiveDevices *registry = new LiveDevices;
    return *registry;
}
struct ExitGuard { ~ExitGuard(); } exit_guard;

class BirriaSDRDevice : public SoapySDR::Device {
public:
    BirriaSDRDevice(uint32_t index, const birriasdr_device_info &info) : info_(info)
    {
        check(birriasdr_open(&dev_, index), "open");
        {
            std::lock_guard<std::mutex> lock(live_devices().mutex);
            live_devices().devices.push_back(this);
        }
        caps_ = birriasdr_get_capabilities(dev_);
        int manual = 0;
        if (birriasdr_get_gain_codes(dev_, &gain_, &manual) == 0)
            manual_ = manual != 0;
    }

    ~BirriaSDRDevice() override
    {
        {
            std::lock_guard<std::mutex> lock(live_devices().mutex);
            auto &devices = live_devices().devices;
            devices.erase(std::remove(devices.begin(), devices.end(), this), devices.end());
        }
        stopReader();
        birriasdr_close(dev_);
    }

    /* Identification */
    std::string getDriverKey() const override { return "BirriaSDR"; }
    std::string getHardwareKey() const override { return info_.name; }

    SoapySDR::Kwargs getHardwareInfo() const override
    {
        char id[16], caps[16];
        std::snprintf(id, sizeof(id), "%04x:%04x", info_.vid, info_.pid);
        std::snprintf(caps, sizeof(caps), "0x%x", caps_);
        return {{"usb_id", id}, {"usb", usb_location(info_)}, {"capabilities", caps},
                {"library", birriasdr_get_version()}};
    }

    /* Channels: chain 1 is a second receiver on the same tuner, not an antenna. */
    size_t getNumChannels(const int direction) const override
    {
        if (direction != SOAPY_SDR_RX)
            return 0;
        return caps_ & BIRRIASDR_CAP_CHAIN_SELECT ? 2 : 1;
    }

    SoapySDR::Kwargs getChannelInfo(const int, const size_t channel) const override
    {
        return {{"chain", std::to_string(channel)}};
    }

    std::vector<std::string> listAntennas(const int, const size_t) const override { return {"RX"}; }
    std::string getAntenna(const int, const size_t) const override { return "RX"; }
    void setAntenna(const int, const size_t, const std::string &name) override
    {
        if (name != "RX")
            SoapySDR::logf(SOAPY_SDR_ERROR, "BirriaSDR: no antenna switch, ignoring %s", name.c_str());
    }

    /* Stream formats */
    std::vector<std::string> getStreamFormats(const int, const size_t) const override
    {
        return {SOAPY_SDR_CS16, SOAPY_SDR_CF32};
    }

    std::string getNativeStreamFormat(const int, const size_t, double &fullScale) const override
    {
        fullScale = FULL_SCALE;
        return SOAPY_SDR_CS16;
    }

    SoapySDR::ArgInfoList getStreamArgsInfo(const int, const size_t) const override
    {
        SoapySDR::ArgInfo bufflen;
        bufflen.key = "bufflen";
        bufflen.value = std::to_string(DEFAULT_BUFFER_SAMPLES);
        bufflen.name = "Buffer samples";
        bufflen.description = "Complex samples per USB callback buffer, a multiple of 2048 up to 262144";
        bufflen.type = SoapySDR::ArgInfo::INT;
        SoapySDR::ArgInfo buffers;
        buffers.key = "buffers";
        buffers.value = std::to_string(DEFAULT_BUFFERS);
        buffers.name = "Ring buffers";
        buffers.description = "Buffers queued between the USB thread and readStream";
        buffers.type = SoapySDR::ArgInfo::INT;
        SoapySDR::ArgInfo transfers;
        transfers.key = "transfers";
        transfers.value = "0";
        transfers.name = "USB transfers";
        transfers.description = "Queued USB transfers, 4 to 256; 0 selects the library default";
        transfers.type = SoapySDR::ArgInfo::INT;
        return {bufflen, buffers, transfers};
    }

    /* Channel list picks the receive chain: {0}, {1} or both in either order. */
    SoapySDR::Stream *setupStream(const int direction, const std::string &format,
                                  const std::vector<size_t> &channels,
                                  const SoapySDR::Kwargs &args) override
    {
        if (direction != SOAPY_SDR_RX)
            throw std::runtime_error("BirriaSDR: receive only");
        if (format != SOAPY_SDR_CS16 && format != SOAPY_SDR_CF32)
            throw std::runtime_error("BirriaSDR: format must be CS16 or CF32");
        if (stream_open_)
            throw std::runtime_error("BirriaSDR: only one stream at a time");
        std::vector<size_t> chans = channels.empty() ? std::vector<size_t>{0} : channels;
        int chain;
        if (chans.size() == 1 && chans[0] <= 1)
            chain = chans[0] == 0 ? BIRRIASDR_CHAIN_0 : BIRRIASDR_CHAIN_1;
        else if (chans.size() == 2 && chans[0] + chans[1] == 1)
            chain = BIRRIASDR_CHAIN_BOTH;
        else
            throw std::runtime_error("BirriaSDR: channels must be {0}, {1} or {0, 1}");
        if (chain != BIRRIASDR_CHAIN_0 && !(caps_ & BIRRIASDR_CAP_CHAIN_SELECT))
            throw std::runtime_error("BirriaSDR: this device has only chain 0");
        if (chain == BIRRIASDR_CHAIN_BOTH && getSampleRate(SOAPY_SDR_RX, 0) != SAMPLE_RATE)
            throw std::runtime_error("BirriaSDR: both chains need the 40 MSps rate");

        size_t bufflen = args.count("bufflen") ? std::stoul(args.at("bufflen")) : DEFAULT_BUFFER_SAMPLES;
        size_t buffers = args.count("buffers") ? std::stoul(args.at("buffers")) : DEFAULT_BUFFERS;
        unsigned transfers = args.count("transfers") ? (unsigned)std::stoul(args.at("transfers")) : 0;
        if (bufflen == 0 || bufflen % 2048 || bufflen > 262144)
            throw std::runtime_error("BirriaSDR: bufflen must be a multiple of 2048 up to 262144");
        if (buffers < 2)
            throw std::runtime_error("BirriaSDR: buffers must be at least 2");
        if (transfers && (transfers < 4 || transfers > 256))
            throw std::runtime_error("BirriaSDR: transfers must be 0 or 4 to 256");

        check(birriasdr_set_receive_chain(dev_, chain), "receive chain");
        if (chain == BIRRIASDR_CHAIN_BOTH)
            SoapySDR::log(SOAPY_SDR_WARNING, "BirriaSDR: both chains carry a static DC offset per chain; "
                                             "subtract it before demodulating");
        chain_slots_ = chans;
        if (chain != BIRRIASDR_CHAIN_BOTH)
            chain_slots_ = {0};
        float_output_ = format == SOAPY_SDR_CF32;
        buffer_samples_ = bufflen;
        transfers_ = transfers;
        ring_.assign(buffers, Slot{});
        for (Slot &slot : ring_)
            slot.iq.resize(bufflen * 2);
        stream_open_ = true;
        return reinterpret_cast<SoapySDR::Stream *>(this);
    }

    void closeStream(SoapySDR::Stream *) override
    {
        stopReader();
        ring_.clear();
        stream_open_ = false;
    }

    size_t getStreamMTU(SoapySDR::Stream *) const override
    {
        return buffer_samples_ / chain_slots_.size();
    }

    int activateStream(SoapySDR::Stream *, const int flags, const long long timeNs,
                       const size_t numElems) override
    {
        (void)timeNs;
        (void)numElems;
        if (flags)
            return SOAPY_SDR_NOT_SUPPORTED;
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (reader_.joinable()) {
            {
                std::lock_guard<std::mutex> lock(ring_mutex_);
                if (!reader_done_)
                    return 0;
            }
            reader_.join();
        }
        try {
            applyStaged();
        } catch (const std::exception &error) {
            SoapySDR::log(SOAPY_SDR_ERROR, error.what());
            return SOAPY_SDR_STREAM_ERROR;
        }
        head_ = tail_ = filled_ = offset_ = 0;
        for (Slot &slot : ring_)
            slot.gap = false;
        stream_rate_ = getSampleRate(SOAPY_SDR_RX, 0);
        lost_ns_ = 0;
        last_callback_ns_ = 0;
        time_gap_ = false;
        reader_done_ = false;
        pending_gap_ = false;
        gap_report_ = GapReport{};
        int result = max_restarts_ ? birriasdr_set_recovery(dev_, max_restarts_, onRecovery, this)
                                   : birriasdr_set_recovery(dev_, 0, nullptr, nullptr);
        if (result < 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "BirriaSDR recovery: %s", birriasdr_strerror(result));
            return SOAPY_SDR_STREAM_ERROR;
        }
        reader_ = std::thread([this] {
            int read = birriasdr_read_async(dev_, onSamples, this, transfers_, (uint32_t)buffer_samples_);
            reportGaps(true);
            if (read < 0) {
                birriasdr_stream_stats stats{};
                birriasdr_get_stream_stats(dev_, &stats);
                SoapySDR::logf(SOAPY_SDR_ERROR,
                               "BirriaSDR stream stopped: %s (stage %d, firmware state %u, reset: %s)",
                               birriasdr_strerror(read), (int)stats.stage, stats.firmware_state,
                               birriasdr_strerror(stats.reset_error));
            }
            std::lock_guard<std::mutex> lock(ring_mutex_);
            reader_done_ = true;
            ring_ready_.notify_all();
        });
        return 0;
    }

    int deactivateStream(SoapySDR::Stream *, const int flags, const long long) override
    {
        if (flags)
            return SOAPY_SDR_NOT_SUPPORTED;
        stopReader();
        return 0;
    }

    /* A recovered gap or a full host ring is reported once as OVERFLOW, at the
     * point in the sample sequence where continuity was lost. timeNs counts
     * samples since activation at the nominal rate. Across a gap it also adds
     * the lost interval as measured on the host clock (about a millisecond of
     * uncertainty); it is a sample label, not a calibrated hardware clock.
     */
    int readStream(SoapySDR::Stream *, void *const *buffs, const size_t numElems, int &flags,
                   long long &timeNs, const long timeoutUs) override
    {
        flags = 0;
        timeNs = 0;
        if (!reader_.joinable())
            return SOAPY_SDR_STREAM_ERROR;
        {
            std::unique_lock<std::mutex> lock(ring_mutex_);
            if (!ring_ready_.wait_for(lock, std::chrono::microseconds(timeoutUs),
                                     [this] { return filled_ > 0 || reader_done_; }))
                return SOAPY_SDR_TIMEOUT;
            if (filled_ == 0) {
                /* Callers such as SDR++ retry at once; don't let a dead stream spin a core. */
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::microseconds(timeoutUs));
                return SOAPY_SDR_STREAM_ERROR;
            }
        }
        Slot &slot = ring_[head_];
        if (offset_ == 0 && slot.gap) {
            slot.gap = false;
            return SOAPY_SDR_OVERFLOW;
        }
        const size_t width = chain_slots_.size();
        size_t n = std::min(numElems, (slot.count - offset_) / width);
        timeNs = slot.time_ns + std::llround((double)(offset_ / width) * 1e9 / stream_rate_);
        flags |= SOAPY_SDR_HAS_TIME;
        const int16_t *iq = slot.iq.data() + offset_ * 2;
        for (size_t c = 0; c < width; ++c) {
            const int16_t *in = iq + chain_slots_[c] * 2;
            if (float_output_) {
                float *out = static_cast<float *>(buffs[c]);
                for (size_t i = 0; i < n; ++i) {
                    out[2 * i] = (float)(in[2 * width * i] / FULL_SCALE);
                    out[2 * i + 1] = (float)(in[2 * width * i + 1] / FULL_SCALE);
                }
            } else {
                int16_t *out = static_cast<int16_t *>(buffs[c]);
                if (width == 1)
                    std::memcpy(out, in, n * 2 * sizeof(int16_t));
                else
                    for (size_t i = 0; i < n; ++i) {
                        out[2 * i] = in[2 * width * i];
                        out[2 * i + 1] = in[2 * width * i + 1];
                    }
            }
        }
        offset_ += n * width;
        if (slot.count - offset_ < width) {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            head_ = (head_ + 1) % ring_.size();
            --filled_;
            offset_ = 0;
        }
        return (int)n;
    }

    /* Frequency: one tuner shared by both chains, live retuning supported. */
    std::vector<std::string> listFrequencies(const int, const size_t) const override { return {"RF"}; }

    /* One range per contiguous band (2.4 and 5 GHz on MT7921), so applications
     * see the gap between them instead of one interval spanning it.
     */
    std::vector<birria_soapy::Band> bands() const
    {
        return birria_soapy::collect_bands([this](uint32_t index, uint64_t *minimum, uint64_t *maximum) {
            return birriasdr_get_tuning_band(dev_, index, minimum, maximum);
        });
    }

    SoapySDR::RangeList getFrequencyRange(const int, const size_t) const override
    {
        SoapySDR::RangeList ranges;
        for (const birria_soapy::Band &band : bands())
            ranges.emplace_back((double)band.minimum, (double)band.maximum);
        return ranges;
    }

    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel,
                                          const std::string &) const override
    {
        return getFrequencyRange(direction, channel);
    }

    /* A request outside every band (SDR++ starts at 100 MHz; the 2.4/5 GHz gap)
     * leaves the tuner unchanged. With no band list the library decides.
     */
    void setFrequency(const int, const size_t, const double frequency, const SoapySDR::Kwargs &) override
    {
        const std::vector<birria_soapy::Band> known = bands();
        uint64_t hz = birria_soapy::in_band(known, frequency);
        if (!hz && known.empty() && frequency > 0 && frequency <= 1e15)
            hz = (uint64_t)std::llround(frequency);
        if (!hz) {
            std::string list;
            for (const birria_soapy::Band &band : known) {
                char text[48];
                std::snprintf(text, sizeof(text), "%s%.0f-%.0f", list.empty() ? "" : ", ",
                              band.minimum / 1e6, band.maximum / 1e6);
                list += text;
            }
            SoapySDR::logf(SOAPY_SDR_ERROR, "BirriaSDR: %.6f MHz is outside the tuning bands (%s MHz); staying at %.6f MHz",
                           frequency / 1e6, list.c_str(), birriasdr_get_center_freq(dev_) / 1e6);
            return;
        }
        if (report(retry_busy([&] { return birriasdr_set_center_freq(dev_, hz); }), "frequency")) {
            /* Leaving chain 0's 5 GHz band lowers LPF codes above 8 (library). */
            std::lock_guard<std::mutex> lock(gain_mutex_);
            refresh_gain_codes();
        }
    }

    void setFrequency(const int direction, const size_t channel, const std::string &name,
                      const double frequency, const SoapySDR::Kwargs &args) override
    {
        if (name != "RF") {
            SoapySDR::logf(SOAPY_SDR_ERROR, "BirriaSDR: unknown frequency component %s", name.c_str());
            return;
        }
        setFrequency(direction, channel, frequency, args);
    }

    double getFrequency(const int, const size_t) const override
    {
        return (double)birriasdr_get_center_freq(dev_);
    }

    double getFrequency(const int direction, const size_t channel, const std::string &) const override
    {
        return getFrequency(direction, channel);
    }

    /* Sample rate: the 40 MSps capture, 20 MSps after the library's halfband
     * (flat to +/-8.5 MHz; single chain only), or the experimental 80 MSps
     * capture (single chain, BW80 profile, 320 MB/s over USB).
     */
    std::vector<double> listSampleRates(const int, const size_t) const override
    {
        return {DECIMATED_RATE, SAMPLE_RATE, WIDE_RATE};
    }

    SoapySDR::RangeList getSampleRateRange(const int, const size_t) const override
    {
        return {SoapySDR::Range(DECIMATED_RATE, DECIMATED_RATE), SoapySDR::Range(SAMPLE_RATE, SAMPLE_RATE),
                SoapySDR::Range(WIDE_RATE, WIDE_RATE)};
    }

    void setSampleRate(const int, const size_t, const double rate) override
    {
        if (std::llround(rate) == std::llround(getSampleRate(SOAPY_SDR_RX, 0)))
            return;
        if (reader_.joinable()) {
            SoapySDR::log(SOAPY_SDR_ERROR, "BirriaSDR: sample rate can change only while idle");
            return;
        }
        report(birriasdr_set_sample_rate(dev_, (uint32_t)std::llround(rate)), "sample rate");
    }

    double getSampleRate(const int, const size_t) const override
    {
        return (double)birriasdr_get_sample_rate(dev_);
    }

    /* Bandwidth: nominal 20 or 40 MHz profiles, rounded to the nearer one.
     * The library accepts them only while idle; during a stream it is staged.
     * At 80 MSps the BW80 profile is used instead, and getBandwidth says so.
     */
    std::vector<double> listBandwidths(const int, const size_t) const override
    {
        if (!(caps_ & BIRRIASDR_CAP_FILTER))
            return {};
        return {20e6, 40e6};
    }

    void setBandwidth(const int, const size_t, const double bandwidth) override
    {
        if (!(caps_ & BIRRIASDR_CAP_FILTER) || !(bandwidth > 0))
            return;
        uint32_t profile = bandwidth < 30e6 ? 20000000u : 40000000u;
        if (reader_.joinable()) {
            if (profile != birriasdr_get_tuner_bandwidth(dev_))
                SoapySDR::logf(SOAPY_SDR_WARNING, "BirriaSDR: %u Hz filter applies at the next stream start",
                               profile);
            staged_bandwidth_ = profile;
            return;
        }
        report(birriasdr_set_tuner_bandwidth(dev_, profile), "bandwidth");
        staged_bandwidth_ = 0;
    }

    double getBandwidth(const int, const size_t) const override
    {
        if (getSampleRate(SOAPY_SDR_RX, 0) == WIDE_RATE)
            return WIDE_RATE;
        return staged_bandwidth_ ? staged_bandwidth_ : (double)birriasdr_get_tuner_bandwidth(dev_);
    }

    /* Gain elements are raw hardware indices, not dB. The overall gain is the
     * library's measured relative dB table (birriasdr_set_tuner_gain()), which
     * picks the nearest step's codes. Automatic mode releases the manual
     * overrides; codes or an overall gain set meanwhile are kept for the next
     * manual mode.
     */
    std::vector<std::string> listGains(const int, const size_t) const override
    {
        std::vector<std::string> names;
        for (const GainElement &element : GAIN_ELEMENTS)
            names.push_back(element.name);
        return names;
    }

    bool hasGainMode(const int, const size_t) const override { return true; }

    void setGainMode(const int, const size_t, const bool automatic) override
    {
        std::lock_guard<std::mutex> lock(gain_mutex_);
        if (!report(retry_busy([&] { return birriasdr_set_tuner_gain_mode(dev_, automatic ? 0 : 1); }),
                    "gain mode"))
            return;
        manual_ = !automatic;
        if (!manual_)
            return;
        if (pending_tenths_db_) {
            if (report(retry_busy([&] { return birriasdr_set_tuner_gain(dev_, *pending_tenths_db_); }), "gain"))
                refresh_gain_codes();
            pending_tenths_db_.reset();
        } else {
            report(retry_busy([&] { return birriasdr_set_gain_codes(dev_, &gain_); }), "gain codes");
        }
    }

    bool getGainMode(const int, const size_t) const override { return !manual_; }

    SoapySDR::Range getGainRange(const int, const size_t, const std::string &name) const override
    {
        return SoapySDR::Range(0, element_maximum(gain_element(name)), 1);
    }

    SoapySDR::Range getGainRange(const int, const size_t) const override
    {
        std::vector<int> gains = tuner_gains();
        if (!gains.empty())
            return SoapySDR::Range(gains.front() / 10.0, gains.back() / 10.0);
        unsigned total = 0;
        for (const GainElement &element : GAIN_ELEMENTS)
            total += element_maximum(element);
        return SoapySDR::Range(0, total, 1);
    }

    /* Overall gain in dB. Without the library's dB table, fall back to Soapy's
     * default distribution over the raw elements.
     */
    void setGain(const int direction, const size_t channel, const double value) override
    {
        if (!(caps_ & BIRRIASDR_CAP_GAIN_DB)) {
            SoapySDR::Device::setGain(direction, channel, value);
            return;
        }
        std::lock_guard<std::mutex> lock(gain_mutex_);
        int tenths = (int)std::lround(value * 10);
        if (!manual_) {
            pending_tenths_db_ = tenths;
            return;
        }
        if (report(retry_busy([&] { return birriasdr_set_tuner_gain(dev_, tenths); }), "gain"))
            refresh_gain_codes();
    }

    double getGain(const int direction, const size_t channel) const override
    {
        if (!(caps_ & BIRRIASDR_CAP_GAIN_DB))
            return SoapySDR::Device::getGain(direction, channel);
        std::lock_guard<std::mutex> lock(gain_mutex_);
        if (pending_tenths_db_)
            return *pending_tenths_db_ / 10.0;
        int tenths = 0;
        return birriasdr_get_tuner_gain(dev_, &tenths) == 0 ? tenths / 10.0 : 0.0;
    }

    void setGain(const int, const size_t, const std::string &name, const double value) override
    {
        auto known = std::find_if(std::begin(GAIN_ELEMENTS), std::end(GAIN_ELEMENTS),
                                  [&](const GainElement &element) { return name == element.name; });
        if (known == std::end(GAIN_ELEMENTS)) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "BirriaSDR: unknown gain element %s", name.c_str());
            return;
        }
        std::lock_guard<std::mutex> lock(gain_mutex_);
        birriasdr_gain gain = gain_;
        gain.*known->code = (uint8_t)std::clamp(std::lround(value), 0L, (long)element_maximum(*known));
        if (manual_ && !report(retry_busy([&] { return birriasdr_set_gain_codes(dev_, &gain); }), "gain codes"))
            return;
        gain_ = gain;
        pending_tenths_db_.reset();
    }

    /* The library's limit for the staged band and chain, so ranges follow a
     * retune into 5 GHz; applications that cache ranges see it on re-query. */
    unsigned element_maximum(const GainElement &element) const
    {
        birriasdr_gain maximum{};
        if (birriasdr_get_gain_code_limits(dev_, &maximum) < 0)
            return element.maximum;
        return maximum.*element.code;
    }

    double getGain(const int, const size_t, const std::string &name) const override
    {
        std::lock_guard<std::mutex> lock(gain_mutex_);
        return gain_.*gain_element(name).code;
    }

    /* Settings */
    SoapySDR::ArgInfoList getSettingInfo() const override
    {
        SoapySDR::ArgInfoList settings;
        if (caps_ & BIRRIASDR_CAP_PD_SUPPRESS) {
            SoapySDR::ArgInfo pd;
            pd.key = "packet_detect_suppression";
            pd.value = "false";
            pd.name = "Packet-detect suppression";
            pd.description = "Research setting: stop PHY blanking (zero runs) after about 250 ms. "
                             "Applies at the next stream start";
            pd.type = SoapySDR::ArgInfo::BOOL;
            settings.push_back(pd);
        }
        if (caps_ & BIRRIASDR_CAP_CHANNEL_FILTER) {
            SoapySDR::ArgInfo filter;
            filter.key = "channel_filter";
            filter.value = "false";
            filter.name = "Channel filter";
            filter.description = "Capture after the PHY's digital channel filter, which follows the bandwidth "
                                 "profile; best with the 20 MHz profile. Applies at the next stream start";
            filter.type = SoapySDR::ArgInfo::BOOL;
            settings.push_back(filter);
        }
        SoapySDR::ArgInfo restarts;
        restarts.key = "max_restarts";
        restarts.value = std::to_string(DEFAULT_MAX_RESTARTS);
        restarts.name = "Recovery budget";
        restarts.description = "Device faults recovered per stream, reported as OVERFLOW; "
                               "clean overruns are free. 0 ends the stream at the first gap";
        restarts.type = SoapySDR::ArgInfo::INT;
        settings.push_back(restarts);
        return settings;
    }

    void writeSetting(const std::string &key, const std::string &value) override
    {
        if (key == "packet_detect_suppression") {
            if (!(caps_ & BIRRIASDR_CAP_PD_SUPPRESS))
                throw std::runtime_error("BirriaSDR: packet-detect suppression is unsupported");
            staged_pd_ = parse_flag(value) ? 1 : 0;
            if (!reader_.joinable())
                applyStaged();
            else
                SoapySDR::log(SOAPY_SDR_WARNING, "BirriaSDR: packet-detect suppression applies at the next stream start");
        } else if (key == "channel_filter") {
            if (!(caps_ & BIRRIASDR_CAP_CHANNEL_FILTER))
                throw std::runtime_error("BirriaSDR: channel filter is unsupported");
            staged_filter_ = parse_flag(value) ? 1 : 0;
            if (!reader_.joinable())
                applyStaged();
            else
                SoapySDR::log(SOAPY_SDR_WARNING, "BirriaSDR: channel filter applies at the next stream start");
        } else if (key == "max_restarts") {
            max_restarts_ = (unsigned)std::stoul(value);
        } else {
            throw std::runtime_error("BirriaSDR: unknown setting " + key);
        }
    }

    std::string readSetting(const std::string &key) const override
    {
        if (key == "packet_detect_suppression") {
            int enabled = staged_pd_ >= 0 ? staged_pd_ : birriasdr_get_packet_detect_suppression(dev_);
            return enabled ? "true" : "false";
        }
        if (key == "channel_filter") {
            int enabled = staged_filter_ >= 0 ? staged_filter_ : birriasdr_get_channel_filter(dev_);
            return enabled ? "true" : "false";
        }
        if (key == "max_restarts")
            return std::to_string(max_restarts_);
        throw std::runtime_error("BirriaSDR: unknown setting " + key);
    }

private:
    friend struct ExitGuard;

    /* The library's overall-gain steps in tenths of a dB; empty without them. */
    std::vector<int> tuner_gains() const
    {
        int count = birriasdr_get_tuner_gains(dev_, nullptr, 0);
        std::vector<int> gains(count > 0 ? (size_t)count : 0);
        if (count > 0 && birriasdr_get_tuner_gains(dev_, gains.data(), gains.size()) != count)
            gains.clear();
        return gains;
    }

    /* Mirror the codes the library committed for an overall gain; gain_mutex_ held. */
    void refresh_gain_codes()
    {
        int manual = 0;
        birriasdr_gain gain;
        if (birriasdr_get_gain_codes(dev_, &gain, &manual) == 0)
            gain_ = gain;
    }
    struct Slot { std::vector<int16_t> iq; size_t count = 0; bool gap = false; long long time_ns = 0; };
    struct GapReport {
        std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
        unsigned device_gaps = 0, dropped_buffers = 0;
        uint64_t dropped_samples = 0;
    };

    /* Runs on the library's read thread: copy into the next free slot or drop
     * the buffer and flag the next one, so a slow consumer never stalls USB.
     */
    static void onSamples(const int16_t *iq, uint32_t count, uint64_t first, void *context)
    {
        auto *self = static_cast<BirriaSDRDevice *>(context);
        const double width = (double)self->chain_slots_.size();
        long long time_ns = self->sampleTime(first / width, count / width);
        size_t tail;
        {
            std::lock_guard<std::mutex> lock(self->ring_mutex_);
            if (self->filled_ == self->ring_.size()) {
                self->pending_gap_ = true;
                ++self->gap_report_.dropped_buffers;
                self->gap_report_.dropped_samples += count;
                self->reportGaps(false);
                return;
            }
            tail = self->tail_;
        }
        Slot &slot = self->ring_[tail];
        std::memcpy(slot.iq.data(), iq, (size_t)count * 2 * sizeof(int16_t));
        slot.count = count;
        slot.time_ns = time_ns;
        slot.gap = self->pending_gap_.exchange(false);
        {
            std::lock_guard<std::mutex> lock(self->ring_mutex_);
            self->tail_ = (tail + 1) % self->ring_.size();
            ++self->filled_;
        }
        self->ring_ready_.notify_one();
        self->reportGaps(false);
    }

    static void onRecovery(const birriasdr_recovery_event *event, void *context)
    {
        auto *self = static_cast<BirriaSDRDevice *>(context);
        self->pending_gap_ = true;
        self->time_gap_ = true;
        ++self->gap_report_.device_gaps;
        SoapySDR::logf(SOAPY_SDR_DEBUG, "BirriaSDR gap %u after %llu samples: %s", event->attempt,
                       (unsigned long long)event->next_sample, birriasdr_strerror(event->error));
    }

    /* Applications may ignore SOAPY_SDR_OVERFLOW (SDR++ does), so summarize
     * losses at most once per second. Read thread only: no locking needed.
     */
    void reportGaps(bool final)
    {
        GapReport &report = gap_report_;
        auto now = std::chrono::steady_clock::now();
        if (!final && now - report.since < std::chrono::seconds(1))
            return;
        if (report.device_gaps || report.dropped_buffers) {
            double seconds = std::chrono::duration<double>(now - report.since).count();
            char device[96] = "", host[128] = "";
            if (report.device_gaps)
                std::snprintf(device, sizeof(device), "%u device gap%s (lost before reaching the host)",
                              report.device_gaps, report.device_gaps == 1 ? "" : "s");
            if (report.dropped_buffers)
                std::snprintf(host, sizeof(host), "%s%u buffer%s (%llu samples) dropped because the application fell behind",
                              report.device_gaps ? "; " : "", report.dropped_buffers,
                              report.dropped_buffers == 1 ? "" : "s", (unsigned long long)report.dropped_samples);
            SoapySDR::logf(SOAPY_SDR_WARNING, "BirriaSDR: last %.1f s: %s%s", seconds, device, host);
        }
        report = GapReport{};
        report.since = now;
    }

    /* Read thread only. The library's sample offsets skip lost samples, so
     * after a device gap add the interval between the end of the last buffer
     * before it and the start of this one, both estimated on the host clock.
     */
    long long sampleTime(double first, double count)
    {
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        long long duration = std::llround(count * 1e9 / stream_rate_);
        if (time_gap_ && last_callback_ns_)
            lost_ns_ += std::max(0LL, now - duration - last_callback_ns_);
        time_gap_ = false;
        last_callback_ns_ = now;
        return std::llround(first * 1e9 / stream_rate_) + lost_ns_;
    }

    void applyStaged()
    {
        if (staged_bandwidth_) {
            check(birriasdr_set_tuner_bandwidth(dev_, staged_bandwidth_), "bandwidth");
            staged_bandwidth_ = 0;
        }
        if (staged_pd_ >= 0) {
            check(birriasdr_set_packet_detect_suppression(dev_, staged_pd_), "packet-detect suppression");
            staged_pd_ = -1;
        }
        if (staged_filter_ >= 0) {
            check(birriasdr_set_channel_filter(dev_, staged_filter_), "channel filter");
            staged_filter_ = -1;
        }
    }

    /* cancel_async returns STATE until read_async has started, so retry until
     * the request lands or the reader has already finished on its own.
     */
    void stopReader()
    {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (!reader_.joinable())
            return;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(ring_mutex_);
                if (reader_done_)
                    break;
            }
            if (birriasdr_cancel_async(dev_) == 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        reader_.join();
    }

    birriasdr_dev_t *dev_ = nullptr;
    birriasdr_device_info info_;
    uint32_t caps_ = 0;

    mutable std::mutex gain_mutex_;
    birriasdr_gain gain_ = {1, 4, 0};
    /* An overall gain set in automatic mode, applied on the next manual mode. */
    std::optional<int> pending_tenths_db_;
    std::atomic<bool> manual_{false};
    uint32_t staged_bandwidth_ = 0;
    int staged_pd_ = -1;
    int staged_filter_ = -1;
    unsigned max_restarts_ = DEFAULT_MAX_RESTARTS;

    bool stream_open_ = false;
    bool float_output_ = false;
    std::vector<size_t> chain_slots_{0};
    size_t buffer_samples_ = DEFAULT_BUFFER_SAMPLES;
    unsigned transfers_ = 0;
    std::thread reader_;
    std::mutex lifecycle_mutex_;

    std::mutex ring_mutex_;
    std::condition_variable ring_ready_;
    std::vector<Slot> ring_;
    size_t head_ = 0, tail_ = 0, filled_ = 0, offset_ = 0;
    std::atomic<bool> pending_gap_{false};
    bool reader_done_ = false;
    double stream_rate_ = SAMPLE_RATE;
    long long lost_ns_ = 0, last_callback_ns_ = 0;
    std::atomic<bool> time_gap_{false};
    GapReport gap_report_;
};

ExitGuard::~ExitGuard()
{
    std::lock_guard<std::mutex> lock(live_devices().mutex);
    for (BirriaSDRDevice *device : live_devices().devices)
        device->stopReader();
}

SoapySDR::KwargsList findBirriaSDR(const SoapySDR::Kwargs &args)
{
    SoapySDR::KwargsList results;
    auto found = matching_devices(args);
    for (const auto &[index, info] : found) {
        /* Applications such as SDR++ key saved settings by label, and the USB
         * address changes on every replug; add it only to tell twins apart.
         */
        const char *name = info.name;
        auto twins = std::count_if(found.begin(), found.end(),
                                   [&](const auto &other) { return !std::strcmp(other.second.name, name); });
        SoapySDR::Kwargs device;
        device["driver"] = "birriasdr";
        device["label"] = std::string("BirriaSDR ") + info.name;
        if (twins > 1)
            device["label"] += " [" + usb_location(info) + "]";
        device["index"] = std::to_string(index);
        device["usb"] = usb_location(info);
        results.push_back(device);
    }
    return results;
}

SoapySDR::Device *makeBirriaSDR(const SoapySDR::Kwargs &args)
{
    auto found = matching_devices(args);
    if (found.empty())
        throw std::runtime_error("BirriaSDR: no matching supported device");
    return new BirriaSDRDevice(found[0].first, found[0].second);
}

SoapySDR::Registry registerBirriaSDR("birriasdr", &findBirriaSDR, &makeBirriaSDR, SOAPY_SDR_ABI_VERSION);

} // namespace
