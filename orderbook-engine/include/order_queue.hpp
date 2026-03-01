// order_queue.hpp
// SPSC (single-producer, single-consumer) lock-free ring buffer.
// Wait-free push/pop with acquire-release memory ordering.
#pragma once
#include <atomic>
#include <cstddef>
#include <new>

#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t CACHE_LINE = std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t CACHE_LINE = 64;
#endif

template <typename T, std::size_t N>
class LockFreeQueue {
    static_assert(N >= 2, "Queue capacity must be at least 2 (usable slots = N-1)");

public:
    LockFreeQueue() : head_(0), tail_(0) {}

    bool push(const T& item) {
        std::size_t t = tail_.load(std::memory_order_relaxed);
        std::size_t next = (t + 1) % N;
        if (next == head_.load(std::memory_order_acquire)) return false;
        buffer_[t] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false;
        item = buffer_[h];
        head_.store((h + 1) % N, std::memory_order_release);
        return true;
    }

private:
    T buffer_[N];

    // Separate cache lines to prevent false sharing between producer (tail)
    // and consumer (head).
    alignas(CACHE_LINE) std::atomic<std::size_t> head_;
    alignas(CACHE_LINE) std::atomic<std::size_t> tail_;
};
