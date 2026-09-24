#pragma once

#include <Arduino.h>
#include <time.h>

constexpr time_t MIN_VALID_UTC_EPOCH = 1609459200;  // 2021-01-01T00:00:00Z

inline bool utc_time_is_valid() {
    return time(nullptr) >= MIN_VALID_UTC_EPOCH;
}

inline uint32_t utc_now() {
    const time_t current_time = time(nullptr);
    return current_time >= MIN_VALID_UTC_EPOCH
        ? static_cast<uint32_t>(current_time)
        : 0;
}
