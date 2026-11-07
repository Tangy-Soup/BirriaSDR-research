/* SPDX-License-Identifier: MIT
 *
 * Slow host gain loop for automatic mode: the decision logic only, no device
 * access, so it can be tested without hardware.
 *
 * The goal is the highest analog gain that does not clip, not a constant level:
 * on this receiver the captured floor is set after the analog stages, so gain
 * ahead of it is SNR. It starts at the top of the given steps and moves in the
 * library's measured overall-gain steps.
 *
 * - Down: clipped values above clip_fraction over a down_window_s window drop
 *   at least down_tenths at once.
 * - Up: after up_window_s with no change, rise only as far as the window's peak
 *   leaves margin_tenths of headroom. A drop of more than the margin therefore
 *   cannot climb straight back: that is the hysteresis.
 * - Rate limit: a change at most every down_window_s, and samples for settle_s
 *   after a change are ignored, since buffered samples precede it.
 */
#ifndef BIRRIASDR_SOAPY_HOST_AGC_HPP
#define BIRRIASDR_SOAPY_HOST_AGC_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace birria_soapy {

/* Sample statistics since the previous update; `values` counts I and Q separately. */
struct AgcStats {
    uint64_t values = 0;
    uint64_t clipped = 0;
    int peak = 0;
};

/* Peak and near-full-scale count of interleaved I/Q; meant to vectorize. */
inline void agc_scan(const int16_t *iq, size_t values, int clip_level, AgcStats &stats)
{
    int peak = 0;
    uint32_t clipped = 0;
    for (size_t i = 0; i < values; ++i) {
        int magnitude = iq[i] < 0 ? -(int)iq[i] : (int)iq[i];
        peak = magnitude > peak ? magnitude : peak;
        clipped += magnitude >= clip_level;
    }
    stats.values += values;
    stats.clipped += clipped;
    stats.peak = std::max(stats.peak, peak);
}

class HostAgc {
public:
    struct Config {
        double settle_s = 0.2;
        double down_window_s = 2.0;
        double up_window_s = 30.0;
        double clip_fraction = 1e-3;
        int down_tenths = 60;
        int margin_tenths = 30;
        int full_scale = 8191;
    };

    HostAgc() = default;
    explicit HostAgc(const Config &config) : config_(config) {}

    /* `steps` ascending in tenths of a dB; the loop stays within them. */
    void start(std::vector<int> steps, size_t index, double now)
    {
        steps_ = std::move(steps);
        index_ = steps_.empty() ? 0 : std::min(index, steps_.size() - 1);
        restart_windows(now);
    }

    /* Same steps, new surroundings (a retune): restart the measurements only. */
    void restart_windows(double now)
    {
        settle_until_ = now + config_.settle_s;
        down_ = AgcStats{};
        up_ = AgcStats{};
        down_start_ = up_start_ = settle_until_;
    }

    /* Follow a gain the device actually holds, after a failed change. */
    void sync(int tenths, double now)
    {
        size_t best = 0;
        for (size_t i = 1; i < steps_.size(); ++i)
            if (std::abs(steps_[i] - tenths) < std::abs(steps_[best] - tenths))
                best = i;
        index_ = best;
        restart_windows(now);
    }

    const std::vector<int> &steps() const { return steps_; }
    size_t index() const { return index_; }
    int tenths() const { return steps_.empty() ? 0 : steps_[index_]; }

    /* Feed the stats since the last call. Returns true when index() changed and
     * the new tenths() must be applied. */
    bool update(const AgcStats &stats, double now)
    {
        if (steps_.empty() || now < settle_until_)
            return false;
        add(down_, stats);
        add(up_, stats);
        if (now - down_start_ >= config_.down_window_s) {
            const bool clipping = down_.values &&
                (double)down_.clipped / (double)down_.values > config_.clip_fraction;
            down_ = AgcStats{};
            down_start_ = now;
            if (clipping && index_ > 0) {
                move_to(highest_at_most(steps_[index_] - config_.down_tenths), now);
                return true;
            }
        }
        if (now - up_start_ >= config_.up_window_s) {
            /* No samples (a stalled stream) must not read as endless headroom. */
            const bool seen = up_.values > 0;
            const int headroom = up_.peak > 0
                ? (int)std::floor(200.0 * std::log10((double)config_.full_scale / up_.peak))
                : 1000;
            const size_t target = highest_at_most(steps_[index_] + headroom - config_.margin_tenths);
            up_ = AgcStats{};
            up_start_ = now;
            if (seen && target > index_) {
                move_to(target, now);
                return true;
            }
        }
        return false;
    }

private:
    static void add(AgcStats &total, const AgcStats &stats)
    {
        total.values += stats.values;
        total.clipped += stats.clipped;
        total.peak = std::max(total.peak, stats.peak);
    }

    size_t highest_at_most(int tenths) const
    {
        size_t found = 0;
        for (size_t i = 0; i < steps_.size(); ++i)
            if (steps_[i] <= tenths)
                found = i;
        return found;
    }

    void move_to(size_t index, double now)
    {
        index_ = index;
        restart_windows(now);
    }

    Config config_;
    std::vector<int> steps_;
    size_t index_ = 0;
    double settle_until_ = 0, down_start_ = 0, up_start_ = 0;
    AgcStats down_, up_;
};

} // namespace birria_soapy

#endif
