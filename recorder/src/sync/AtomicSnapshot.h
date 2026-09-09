#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

namespace gocue::recorder
{
// Single worker publishes; any reader receives an immutable VALUE. Every payload
// word is atomic (an ordinary POD seqlock would have a C++ data race). SC ordering
// makes version/payload coherence explicit. No shared_ptr implementation locks,
// allocation, reclamation, or waiting for a preempted writer. Contention is a
// bounded unavailable result; callers must not substitute a stale epoch.
template<class T> class AtomicSnapshot
{
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static constexpr std::size_t words = (sizeof(T) + 7) / 8;
public:
    AtomicSnapshot() noexcept { publish(T{}); }
    void publish(const T& value) noexcept
    {
        std::array<std::uint64_t, words> copy{};
        std::memcpy(copy.data(), &value, sizeof(T));
        version.fetch_add(1);
        for (std::size_t i = 0; i < words; ++i) payload[i].store(copy[i]);
        version.fetch_add(1);
    }
    std::optional<T> read() const noexcept
    {
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            const auto before = version.load();
            if (before & 1) continue;
            std::array<std::uint64_t, words> copy{};
            for (std::size_t i = 0; i < words; ++i) copy[i] = payload[i].load();
            if (before != version.load()) continue;
            T value{};
            std::memcpy(&value, copy.data(), sizeof(T));
            return value;
        }
        return {};
    }
private:
    std::atomic<std::uint64_t> version{0};
    std::array<std::atomic<std::uint64_t>, words> payload{};
};
}
