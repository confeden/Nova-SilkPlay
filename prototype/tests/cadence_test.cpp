#include "../nsp_cadence.h"

#include <cmath>
#include <cstdio>

namespace {
int failed = 0, checks = 0;
void Check(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::printf("FAIL: %s\n", name); ++failed; }
}
}

int main() {
    double intervals[32] = {};
    Check(nsp::EstimateSourcePeriod100ns(intervals, 5) == 0.0, "not primed");
    Check(nsp::EstimateSourcePeriod100ns(intervals, 32) == 0.0, "zero intervals");
    for (double display : {60.0, 120.0, 144.0, 165.0}) {
        const double tick = 10000000.0 / display;
        for (double fps : {10.0, 15.0, 20.0, 24000.0 / 1001.0, 24.0, 25.0,
                           27.0, 30000.0 / 1001.0, 30.0, 44.0, 48.0, 50.0,
                           60000.0 / 1001.0, 60.0, 72.0, 90.0, 120.0}) {
            if (fps > display) continue;
            const double period = 10000000.0 / fps;
            for (int start = 0; start < 16; ++start) {
                for (int i = 0; i < 32; ++i) {
                    const double a = std::ceil((start + i) * period / tick - 1e-9) * tick;
                    const double b = std::ceil((start + i + 1) * period / tick - 1e-9) * tick;
                    intervals[i] = b - a;
                }
                const double estimate = nsp::EstimateSourcePeriod100ns(intervals, 32);
                if (period < 1.5 * tick) {
                    // The renderer passes these sources through. Capture stamps
                    // cannot distinguish a near-refresh source from rare drops.
                    Check(estimate < 1.5 * tick, "near-refresh source remains on the pass-through path");
                } else {
                    Check(std::abs(estimate - period) <= tick / 32.0 + 1e-6,
                          "interpolated source period stays within one timestamp tick per window");
                }
            }
        }
    }

    const double tick = 10000000.0 / 165.0;
    const int jitter[] = {2, 4, 2, 2, 4, 2, 2, 4};
    for (int i = 0; i < 32; ++i) intervals[i] = jitter[i % 8] * tick;
    const double expected = 10000000.0 / 60.0;
    Check(std::abs(nsp::EstimateSourcePeriod100ns(intervals, 32) - expected) < 1e-6,
          "compensating four-tick intervals are not discarded");

    // Recorded 60 fps / 165 Hz arrivals: the old median trim reports 81.69 fps.
    const double recorded[32] = {
        242752.8, 121290.5, 120187.6, 243868.2, 121568.5, 243448.2, 123170.9, 118401.6,
        235272.9, 123036.8, 116422.4, 246616.0, 121835.6, 383142.9, 136810.0, 293422.1,
        128411.3, 121218.0, 240978.8, 122661.2, 363641.0, 108098.6, 245345.1, 125607.1,
        125392.8, 241544.6, 122777.1, 236009.0, 241183.7, 243215.5, 124925.5, 121589.5};
    Check(std::abs(10000000.0 / nsp::EstimateSourcePeriod100ns(recorded, 32) - 60.0) < 1.0,
          "recorded jitter and gaps do not produce a false 80 fps source");

    for (double fps : {24.0, 30.0, 60.0}) {
        const double period = 10000000.0 / fps;
        std::fill(intervals, intervals + 32, period);
        intervals[7] = period * 2.0;
        Check(std::abs(nsp::EstimateSourcePeriod100ns(intervals, 32) - period) < 1e-6,
              "isolated dropped source frame does not change period");
        intervals[7] = 2400000.0;
        Check(std::abs(nsp::EstimateSourcePeriod100ns(intervals, 32) - period) < 1e-6,
              "isolated source stall does not change period");
    }
    std::fill(intervals, intervals + 32, 10000000.0 / 24.0);
    intervals[3] = 60606.0;
    intervals[9] = 121212.0;
    Check(std::abs(nsp::EstimateSourcePeriod100ns(intervals, 32) - 10000000.0 / 24.0) < 1e-6,
          "existing short UI-interval rejection is retained");
    std::printf("%s: %d cadence checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed);
    return failed ? 1 : 0;
}
