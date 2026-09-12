#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace gocue::recorder
{
// One producer, one consumer. Reservation lets a borrowed COM sample be AddRef'ed ONLY
// when there is room. A full queue never takes ownership and never needs callback Release.
template<class T, std::size_t Capacity> class BoundedSpscQueue
{
    static_assert(Capacity > 0 && std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
public:
    T* reserve() noexcept
    {
        const auto w = written.load(std::memory_order_relaxed);
        if (w - read.load(std::memory_order_acquire) == Capacity) return nullptr;
        return &slots[w % Capacity];
    }
    void commit() noexcept { written.fetch_add(1, std::memory_order_release); }
    bool push(const T& value) noexcept
    {
        if (auto* slot = reserve()) { *slot = value; commit(); return true; }
        return false;
    }
    bool pop(T& value) noexcept
    {
        const auto r = read.load(std::memory_order_relaxed);
        if (r == written.load(std::memory_order_acquire)) return false;
        value = slots[r % Capacity];
        read.store(r + 1, std::memory_order_release);
        return true;
    }
    std::size_t producerSize() const noexcept
    {
        return static_cast<std::size_t>(written.load(std::memory_order_relaxed) - read.load(std::memory_order_acquire));
    }
    static constexpr std::size_t capacity = Capacity;
private:
    std::array<T, Capacity> slots{};
    alignas(64) std::atomic<std::uint64_t> written{0};
    alignas(64) std::atomic<std::uint64_t> read{0};
};
}
