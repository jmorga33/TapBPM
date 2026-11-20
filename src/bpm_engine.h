#pragma once

struct BpmSnapshot {
    double bpm = 0.0;
    double half_bpm = 0.0;
    double double_bpm = 0.0;
    double jitter_ms = 0.0;
    double confidence = 0.0;
    int tap_count = 0;
    int window_count = 0;
    bool has_bpm = false;
    bool is_held = false;
};

class BpmEngine {
public:
    static constexpr int kMaxWindow = 16;
    static constexpr double kAutoResetSec = 2.5;
    static constexpr double kMinIntervalSec = 0.12;
    static constexpr double kMinBpm = 24.0;
    static constexpr double kMaxBpm = 400.0;

    void Reset();
    bool RecordTap(double timestamp_sec);
    BpmSnapshot GetSnapshot(double now_sec) const;

private:
    void Recompute();

    double window_[kMaxWindow] = {};
    int window_count_ = 0;
    int session_taps_ = 0;
    double last_tap_sec_ = 0.0;

    double bpm_ = 0.0;
    double jitter_ms_ = 0.0;
    double confidence_ = 0.0;
    bool has_bpm_ = false;
};
