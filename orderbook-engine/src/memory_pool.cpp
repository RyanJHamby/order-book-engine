// memory_pool.cpp
#include "memory_pool.hpp"
#include <cstring>

namespace ob {

MemoryPool::MemoryPool(std::size_t block_size, std::size_t block_count)
    : block_size_(block_size < sizeof(void*) ? sizeof(void*) : block_size)
    , block_count_(block_count)
    , free_count_(block_count)
{
    pool_ = static_cast<std::uint8_t*>(std::malloc(block_size_ * block_count_));
    free_list_ = static_cast<std::uint8_t**>(std::malloc(block_count_ * sizeof(std::uint8_t*)));

    // Initialize free list — stack of pointers to each block.
    for (std::size_t i = 0; i < block_count_; ++i) {
        free_list_[i] = pool_ + (i * block_size_);
    }
}

MemoryPool::~MemoryPool() {
    std::free(free_list_);
    std::free(pool_);
}

void* MemoryPool::allocate() {
    if (free_count_ == 0) return nullptr;
    return free_list_[--free_count_];
}

void MemoryPool::deallocate(void* ptr) noexcept {
    if (ptr == nullptr) return;
    free_list_[free_count_++] = static_cast<std::uint8_t*>(ptr);
}

} // namespace ob
