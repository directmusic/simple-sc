#pragma once

#include <atomic>
#include <cstddef>
#include <stdlib.h>
#include <string.h>

// https://www.snellman.net/blog/archive/2016-12-13-ring-buffers/
template <typename T, size_t Capacity>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Size must be a power of two!");
    std::atomic<uint32_t> read_head = 0;
    std::atomic<uint32_t> write_head = 0;
    T* array = nullptr;

public:
    RingBuffer() { array = (T*)malloc(sizeof(T) * Capacity); }
    ~RingBuffer() { free(array); }

    uint32_t mask(uint32_t x) { return x & (Capacity - 1); }
    void write(T val) {
        uint32_t slot = mask(write_head.load(std::memory_order_relaxed));
        array[slot] = val;
        write_head.fetch_add(1, std::memory_order_release);
    }

    T read() {
        uint32_t slot = mask(read_head.load(std::memory_order_relaxed));
        T val = array[slot];
        read_head.fetch_add(1, std::memory_order_release);
        return val;
    }

    bool empty() const { return read_head.load(std::memory_order_acquire) == write_head.load(std::memory_order_acquire); }
    uint32_t count() { return write_head - read_head; }
    bool full() const { return count() == Capacity; }
    uint32_t capacity() { return Capacity; }
};
