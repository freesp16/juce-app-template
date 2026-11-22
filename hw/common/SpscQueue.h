// Bounded single-producer / single-consumer lock-free queue, for handing events from one thread to another.
// Wait-free, never allocates after construction, trivially-copyable payloads only. Capacity is a power of two.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace platform
{
template <typename T, std::size_t Capacity>
class SpscQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0 && Capacity >= 2, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "payloads cross threads by copy");

public:
    // Producer thread only. False when full (the caller decides: drop, coalesce or retry next block).
    bool push(const T& value) noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head - tailCache_ == Capacity)
        {
            tailCache_ = tail_.load(std::memory_order_acquire);
            if (head - tailCache_ == Capacity)
                return false;
        }
        slots_[head & (Capacity - 1)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only.
    bool pop(T& out) noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == headCache_)
        {
            headCache_ = head_.load(std::memory_order_acquire);
            if (tail == headCache_)
                return false;
        }
        out = slots_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // Producer and consumer indices on separate cache lines (Cortex-A9: 32-byte lines; 64 is safe everywhere).
    alignas(64) std::atomic<std::size_t> head_ {0};
    std::size_t tailCache_ = 0; // producer's view of tail_
    alignas(64) std::atomic<std::size_t> tail_ {0};
    std::size_t headCache_ = 0; // consumer's view of head_
    alignas(64) std::array<T, Capacity> slots_ {};
};
} // namespace platform
