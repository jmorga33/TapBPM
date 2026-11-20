#include "bpm_engine.h"

#include <cmath>

namespace {

inline double ClampVal(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace

void BpmEngine::Reset() {
    for (int i = 0; i < kMaxWindow; ++i) {
        window_[i] = 0.0;
    }
    window_count_ = 0;
    session_taps_ = 0;
    last_tap_sec_ = 0.0;
    bpm_ = 0.0;
    jitter_ms_ = 0.0;
    confidence_ = 0.0;
    has_bpm_ = false;
}

bool BpmEngine::RecordTap(double timestamp_sec) {
    if (window_count_ > 0) {
        const double dt = timestamp_sec - last_tap_sec_;
        if (dt >= 0.0 && dt < kMinIntervalSec) {
            return false;
        }
        if (dt > kAutoResetSec || dt < 0.0) {
            Reset();
        }
    }

    if (window_count_ == kMaxWindow) {
        for (int i = 1; i < kMaxWindow; ++i) {
            window_[i - 1] = window_[i];
        }
        window_[kMaxWindow - 1] = timestamp_sec;
    } else {
        window_[window_count_++] = timestamp_sec;
    }

    ++session_taps_;
    last_tap_sec_ = timestamp_sec;
    Recompute();
    return true;
}

void BpmEngine::Recompute() {
    if (window_count_ < 2) {
        bpm_ = 0.0;
        jitter_ms_ = 0.0;
        confidence_ = 0.0;
        has_bpm_ = false;
        return;
    }

    const int n = window_count_;
    const double t0 = window_[0];
    double sum_t = 0.0;
    for (int i = 0; i < n; ++i) {
        sum_t += (window_[i] - t0);
    }

    const double nd = static_cast<double>(n);
    const double mean_i = 0.5 * (nd - 1.0);
    const double mean_t = sum_t / nd;

    double num = 0.0;
    for (int i = 0; i < n; ++i) {
        const double di = static_cast<double>(i) - mean_i;
        const double dt = (window_[i] - t0) - mean_t;
        num += di * dt;
    }

    const double den = nd * (nd * nd - 1.0) / 12.0;
    const double slope = (den > 0.0) ? (num / den) : 0.0;
    if (slope <= 1e-6) {
        bpm_ = 0.0;
        jitter_ms_ = 0.0;
        confidence_ = 0.0;
        has_bpm_ = false;
        return;
    }

    bpm_ = ClampVal(60.0 / slope, kMinBpm, kMaxBpm);
    has_bpm_ = true;

    const double count_factor = ClampVal(static_cast<double>(n - 1) / 7.0, 0.0, 1.0);
    if (n >= 3) {
        double sum_sq = 0.0;
        for (int i = 0; i < n; ++i) {
            const double pred = mean_t + slope * (static_cast<double>(i) - mean_i);
            const double err = (window_[i] - t0) - pred;
            sum_sq += err * err;
        }
        const double rms_sec = std::sqrt(sum_sq / static_cast<double>(n - 2));
        jitter_ms_ = rms_sec * 1000.0;

        const double rel_err = rms_sec / slope;
        const double penalty = (rel_err > 0.015) ? ((rel_err - 0.015) / 0.12) : 0.0;
        const double steady_factor = ClampVal(1.0 - penalty, 0.15, 1.0);
        confidence_ = count_factor * steady_factor;
    } else {
        jitter_ms_ = 0.0;
        confidence_ = count_factor * 0.5;
    }
}

BpmSnapshot BpmEngine::GetSnapshot(double now_sec) const {
    BpmSnapshot s{};
    if (session_taps_ <= 0) {
        return s;
    }

    const bool timed_out = (now_sec - last_tap_sec_) > kAutoResetSec;
    if (session_taps_ == 1 && timed_out) {
        return s;
    }

    s.tap_count = session_taps_;
    s.window_count = window_count_;
    s.has_bpm = has_bpm_;
    if (has_bpm_) {
        s.bpm = bpm_;
        s.half_bpm = bpm_ * 0.5;
        s.double_bpm = bpm_ * 2.0;
        s.jitter_ms = jitter_ms_;
        s.confidence = confidence_;
        s.is_held = timed_out;
    }
    return s;
}
