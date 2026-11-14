#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace nsp {

template <std::size_t N>
double EstimateSourcePeriod100ns(const double (&intervals)[N], std::size_t count) {
    assert(count <= N);
    if (count < 6) return 0.0;
    double ordered[N];
    std::copy(intervals, intervals + count, ordered);
    std::nth_element(ordered, ordered + count / 2, ordered + count);
    const double median = ordered[count / 2];
    if (median <= 0.0) return 0.0;

    // At 60 fps on 165 Hz, valid two/four-tick intervals can have a two-tick
    // median. Trimming at 1.6 * that median discards the compensating long
    // intervals and overestimates the rate. Estimate their scale first.
    const double lower = median * 0.4;
    double sum = 0.0;
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (intervals[i] >= lower && intervals[i] <= median * 2.5) {
            sum += intervals[i];
            ++accepted;
        }
    }
    const double scale = accepted ? sum / static_cast<double>(accepted) : median;
    const double upper = 1.6 * (std::max)(median, scale);
    sum = 0.0;
    accepted = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (intervals[i] >= lower && intervals[i] <= upper) {
            sum += intervals[i];
            ++accepted;
        }
    }
    return accepted ? sum / static_cast<double>(accepted) : median;
}

}  // namespace nsp
