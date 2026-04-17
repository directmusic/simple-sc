#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <sys/time.h>

inline std::string make_date_time_string() {
    char buffer[32];
    time_t now = time(nullptr);
    struct tm* local_time = localtime(&now);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", local_time);
    return buffer;
}

// returns the timestamp in ms. this is a sort of hacky way to keep a
// consistent fps on the output video, but seems to work well enough
inline uint64_t get_timestamp_ms() {
    struct timeval tp;
    gettimeofday(&tp, NULL);
    return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

inline int next_power_of_two(int val) {
    unsigned int v;

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}
