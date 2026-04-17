#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

template <typename T, int Size, bool StackAllocated = false>
class RingBuffer {
    using StorageType = std::conditional_t<StackAllocated, std::array<T, Size>, T*>;
    StorageType data_;
    alignas(64) std::atomic<size_t> readIdx_ { 0 };
    alignas(64) size_t writeIdxCached_ { 0 };
    alignas(64) std::atomic<size_t> writeIdx_ { 0 };
    alignas(64) size_t readIdxCached_ { 0 };

public:
    // Delete copy constructor and assignment operator
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    RingBuffer() {
        if constexpr (!StackAllocated) {
            data_ = (T*)aligned_alloc(32, sizeof(T) * Size);
        }
    }

    ~RingBuffer() {
        if constexpr (!StackAllocated) {
            aligned_free(data_);
        }
    }

    bool write(const T& val) {
        auto const writeIdx = writeIdx_.load(std::memory_order_relaxed);
        auto nextWriteIdx = writeIdx + 1;
        if (nextWriteIdx == Size) {
            nextWriteIdx = 0;
        }
        if (nextWriteIdx == readIdxCached_) {
            readIdxCached_ = readIdx_.load(std::memory_order_acquire);
            if (nextWriteIdx == readIdxCached_) {
                return false;
            }
        }
        data_[writeIdx] = val;
        writeIdx_.store(nextWriteIdx, std::memory_order_release);
        return true;
    }

    // for only copying a portion of the buffer
    bool write(const T& val, size_t size_to_copy_bytes) {
        auto const writeIdx = writeIdx_.load(std::memory_order_relaxed);
        auto nextWriteIdx = writeIdx + 1;
        if (nextWriteIdx == Size) {
            nextWriteIdx = 0;
        }
        if (nextWriteIdx == readIdxCached_) {
            readIdxCached_ = readIdx_.load(std::memory_order_acquire);
            if (nextWriteIdx == readIdxCached_) {
                return false;
            }
        }
        // data_[writeIdx] = val;
        memcpy(&data_[writeIdx], &val, size_to_copy_bytes);
        writeIdx_.store(nextWriteIdx, std::memory_order_release);
        return true;
    }

    bool read(T* out) {
        auto const readIdx = readIdx_.load(std::memory_order_relaxed);
        if (readIdx == writeIdxCached_) {
            writeIdxCached_ = writeIdx_.load(std::memory_order_acquire);
            if (readIdx == writeIdxCached_) {
                return false;
            }
        }
        memcpy(out, &data_[readIdx], sizeof(T));
        auto nextReadIdx = readIdx + 1;
        if (nextReadIdx == Size) {
            nextReadIdx = 0;
        }
        readIdx_.store(nextReadIdx, std::memory_order_release);
        return true;
    }

    T read() {
        auto const readIdx = readIdx_.load(std::memory_order_relaxed);
        if (readIdx == writeIdxCached_) {
            writeIdxCached_ = writeIdx_.load(std::memory_order_acquire);
            if (readIdx == writeIdxCached_) {
                return T();
            }
        }
        T val = data_[readIdx];
        auto nextReadIdx = readIdx + 1;
        if (nextReadIdx == Size) {
            nextReadIdx = 0;
        }
        readIdx_.store(nextReadIdx, std::memory_order_release);
        return val;
    }

    int number_of_new_frames() {
        return (writeIdx_.load(std::memory_order_relaxed) - readIdx_.load(std::memory_order_relaxed) + Size) % Size;
    }

    [[nodiscard]] int empty() const {
        return writeIdx_.load(std::memory_order_relaxed) == readIdx_.load(std::memory_order_relaxed);
    }

    size_t size() {
        return Size;
    }

    void resetLatency() {
        writeIdx_ = 0;
        readIdx_ = 0;
        for (size_t i = 0; i < Size; i++) {
            data_[i] = { 0, 0 };
        }
    }

    void clear() {
        writeIdx_.store(0);
        writeIdxCached_ = 0;
        readIdx_.store(0);
        readIdxCached_ = 0;

        for (int i = 0; i < Size; i++) {
            data_[i] = {};
        }
    }
};
