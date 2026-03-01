// memory_pool.hpp
// Thread-local slab allocator for fixed-size objects.
// Pre-allocates slabs of memory; pointers remain stable across growth.
// Each thread gets its own pool instance via thread_local — zero contention
// in the allocation hot path.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <vector>

// Slab-based pool: allocates objects from fixed-size slabs.
// When a slab is exhausted, a new one is allocated. Existing pointers
// remain valid because slabs are never moved or freed until the pool
// is destroyed. This avoids the pointer-invalidation problem of
// std::vector-based pools.
//
// NOT thread-safe: each thread must own its own instance (see
// get_thread_local_pool below).
template<typename T>
class ThreadLocalPool {
public:
    explicit ThreadLocalPool(std::size_t slab_capacity = 4096)
        : slab_capacity_(slab_capacity > 0 ? slab_capacity : 64)
    {
        allocate_slab();
    }

    ~ThreadLocalPool() {
        for (auto* slab : slabs_) {
            std::free(slab);
        }
    }

    ThreadLocalPool(const ThreadLocalPool&) = delete;
    ThreadLocalPool& operator=(const ThreadLocalPool&) = delete;

    T* allocate() {
        if (index_ >= slab_capacity_) {
            allocate_slab();
        }
        return &current_slab_[index_++];
    }

    std::size_t slab_count() const { return slabs_.size(); }

private:
    void allocate_slab() {
        auto* slab = static_cast<T*>(std::malloc(slab_capacity_ * sizeof(T)));
        slabs_.push_back(slab);
        current_slab_ = slab;
        index_ = 0;
    }

    std::size_t slab_capacity_;
    std::size_t index_{0};
    T* current_slab_{nullptr};
    std::vector<T*> slabs_;
};

// Per-thread pool instance via thread_local.
// Each thread gets its own pool — zero contention, zero locking.
// WARNING: Do not pass the returned reference to another thread.
template<typename T>
inline ThreadLocalPool<T>& get_thread_local_pool(std::size_t slab_capacity = 4096) {
    thread_local ThreadLocalPool<T> pool(slab_capacity);
    return pool;
}
