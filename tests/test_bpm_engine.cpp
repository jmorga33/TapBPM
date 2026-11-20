#include "../src/bpm_engine.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failed = 0;
int g_total = 0;

bool Near(double a, double b, double eps) {
    return std::fabs(a - b) <= eps;
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_total;                                                           \
        if (!(cond)) {                                                       \
            ++g_failed;                                                      \
            std::printf("FAIL [line %d]: %s\n", __LINE__, #cond);            \
        }                                                                    \
    } while (0)

void TestInitialIdleState() {
    BpmEngine eng;
    const BpmSnapshot s = eng.GetSnapshot(10.0);
    CHECK(!s.has_bpm);
    CHECK(!s.is_held);
    CHECK(s.tap_count == 0);
    CHECK(s.window_count == 0);
    CHECK(Near(s.bpm, 0.0, 1e-9));
    CHECK(Near(s.half_bpm, 0.0, 1e-9));
    CHECK(Near(s.double_bpm, 0.0, 1e-9));
    CHECK(Near(s.jitter_ms, 0.0, 1e-9));
    CHECK(Near(s.confidence, 0.0, 1e-9));
}

void TestSingleTapState() {
    BpmEngine eng;
    CHECK(eng.RecordTap(1.0));
    const BpmSnapshot s = eng.GetSnapshot(1.2);
    CHECK(!s.has_bpm);
    CHECK(!s.is_held);
    CHECK(s.tap_count == 1);
    CHECK(s.window_count == 1);
}

void TestExact120Bpm() {
    BpmEngine eng;
    for (int i = 0; i < 8; ++i) {
        CHECK(eng.RecordTap(10.0 + 0.5 * i));
    }
    const BpmSnapshot s = eng.GetSnapshot(13.6);
    CHECK(s.has_bpm);
    CHECK(!s.is_held);
    CHECK(s.tap_count == 8);
    CHECK(s.window_count == 8);
    CHECK(Near(s.bpm, 120.0, 1e-4));
    CHECK(Near(s.half_bpm, 60.0, 1e-4));
    CHECK(Near(s.double_bpm, 240.0, 1e-4));
    CHECK(s.jitter_ms < 0.001);
    CHECK(Near(s.confidence, 1.0, 1e-4));
}

void TestRolling16WindowAt128Bpm() {
    BpmEngine eng;
    const double step = 60.0 / 128.0;
    double last_t = 0.0;
    for (int i = 0; i < 20; ++i) {
        last_t = 5.0 + step * i;
        CHECK(eng.RecordTap(last_t));
    }
    const BpmSnapshot s = eng.GetSnapshot(last_t + 0.1);
    CHECK(s.has_bpm);
    CHECK(!s.is_held);
    CHECK(s.tap_count == 20);
    CHECK(s.window_count == 16);
    CHECK(Near(s.bpm, 128.0, 1e-4));
    CHECK(Near(s.half_bpm, 64.0, 1e-4));
    CHECK(Near(s.double_bpm, 256.0, 1e-4));
    CHECK(s.jitter_ms < 0.001);
}

void TestHumanJittered128Bpm() {
    BpmEngine eng;
    const double step = 60.0 / 128.0;
    double last_t = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double jitter = (i % 2 == 0) ? 0.015 : -0.015;
        last_t = 2.0 + step * i + jitter;
        CHECK(eng.RecordTap(last_t));
    }
    const BpmSnapshot s = eng.GetSnapshot(last_t + 0.1);
    CHECK(s.has_bpm);
    CHECK(Near(s.bpm, 128.0, 0.5));
    CHECK(s.jitter_ms >= 10.0 && s.jitter_ms <= 25.0);
    CHECK(s.confidence > 0.6);
}

void TestDebounceKeyChatter() {
    BpmEngine eng;
    CHECK(eng.RecordTap(1.000));
    CHECK(!eng.RecordTap(1.050));  // 50 ms < 120 ms debounce
    CHECK(!eng.RecordTap(1.119));  // 119 ms < 120 ms debounce
    CHECK(eng.RecordTap(1.500));   // 500 ms -> 120 BPM
    const BpmSnapshot s = eng.GetSnapshot(1.600);
    CHECK(s.tap_count == 2);
    CHECK(s.window_count == 2);
    CHECK(s.has_bpm);
    CHECK(Near(s.bpm, 120.0, 1e-4));
}

void TestAutoResetAndHold() {
    BpmEngine eng;
    CHECK(eng.RecordTap(1.0));
    CHECK(eng.RecordTap(1.5));  // 120.0 BPM
    CHECK(eng.RecordTap(2.0));  // 120.0 BPM

    const BpmSnapshot live = eng.GetSnapshot(4.0);  // 2.0s elapsed <= 2.5s
    CHECK(live.has_bpm);
    CHECK(!live.is_held);
    CHECK(Near(live.bpm, 120.0, 1e-4));

    const BpmSnapshot held = eng.GetSnapshot(4.6);  // 2.6s elapsed > 2.5s
    CHECK(held.has_bpm);
    CHECK(held.is_held);
    CHECK(Near(held.bpm, 120.0, 1e-4));
    CHECK(held.tap_count == 3);

    // Tapping again after hold starts a fresh session
    CHECK(eng.RecordTap(5.0));
    const BpmSnapshot restarted = eng.GetSnapshot(5.1);
    CHECK(!restarted.has_bpm);
    CHECK(!restarted.is_held);
    CHECK(restarted.tap_count == 1);
    CHECK(restarted.window_count == 1);

    CHECK(eng.RecordTap(5.6));  // 0.6s interval -> 100.0 BPM
    const BpmSnapshot second = eng.GetSnapshot(5.7);
    CHECK(second.has_bpm);
    CHECK(!second.is_held);
    CHECK(second.tap_count == 2);
    CHECK(Near(second.bpm, 100.0, 1e-4));
}

void TestSingleTapTimeout() {
    BpmEngine eng;
    CHECK(eng.RecordTap(10.0));
    CHECK(eng.GetSnapshot(12.0).tap_count == 1);

    const BpmSnapshot idle = eng.GetSnapshot(12.6);
    CHECK(!idle.has_bpm);
    CHECK(!idle.is_held);
    CHECK(idle.tap_count == 0);
    CHECK(idle.window_count == 0);
}

void TestHighQpcUptimeOffset() {
    BpmEngine eng;
    const double t0 = 1000000.0;
    const double step = 60.0 / 125.0;  // 0.48s
    for (int i = 0; i < 8; ++i) {
        CHECK(eng.RecordTap(t0 + step * i));
    }
    const BpmSnapshot s = eng.GetSnapshot(t0 + step * 7 + 0.1);
    CHECK(s.has_bpm);
    CHECK(Near(s.bpm, 125.0, 1e-5));
    CHECK(s.jitter_ms < 0.001);
}

void TestTempoBounds() {
    // 40 BPM (1.5s per beat)
    {
        BpmEngine eng;
        CHECK(eng.RecordTap(0.0));
        CHECK(eng.RecordTap(1.5));
        CHECK(eng.RecordTap(3.0));
        const BpmSnapshot s = eng.GetSnapshot(3.1);
        CHECK(s.has_bpm);
        CHECK(Near(s.bpm, 40.0, 1e-4));
    }
    // 200 BPM (0.3s per beat)
    {
        BpmEngine eng;
        CHECK(eng.RecordTap(0.0));
        CHECK(eng.RecordTap(0.3));
        CHECK(eng.RecordTap(0.6));
        const BpmSnapshot s = eng.GetSnapshot(0.7);
        CHECK(s.has_bpm);
        CHECK(Near(s.bpm, 200.0, 1e-4));
    }
    // Ultra-fast tapping just above 120 ms debounce (e.g. 125 ms -> 480 BPM) clamps to 400.0 BPM
    {
        BpmEngine eng;
        CHECK(eng.RecordTap(0.0));
        CHECK(eng.RecordTap(0.125));
        CHECK(eng.RecordTap(0.250));
        const BpmSnapshot s = eng.GetSnapshot(0.3);
        CHECK(s.has_bpm);
        CHECK(Near(s.bpm, 400.0, 1e-4));
    }
}

void TestManualReset() {
    BpmEngine eng;
    CHECK(eng.RecordTap(1.0));
    CHECK(eng.RecordTap(1.5));
    CHECK(eng.RecordTap(2.0));
    CHECK(eng.GetSnapshot(2.1).has_bpm);

    eng.Reset();
    const BpmSnapshot s = eng.GetSnapshot(2.2);
    CHECK(!s.has_bpm);
    CHECK(!s.is_held);
    CHECK(s.tap_count == 0);
    CHECK(s.window_count == 0);
    CHECK(Near(s.bpm, 0.0, 1e-9));
}

}  // namespace

int main() {
    TestInitialIdleState();
    TestSingleTapState();
    TestExact120Bpm();
    TestRolling16WindowAt128Bpm();
    TestHumanJittered128Bpm();
    TestDebounceKeyChatter();
    TestAutoResetAndHold();
    TestSingleTapTimeout();
    TestHighQpcUptimeOffset();
    TestTempoBounds();
    TestManualReset();

    std::printf("TapBPM engine tests: %d/%d checks passed.\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
