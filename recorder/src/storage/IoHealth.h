#pragma once
#include "DurableFile.h"
#include <chrono>
#include <array>

namespace gocue::recorder
{
// Shared by the take's WAV and journal owners, never called by the ASIO callback.
// Counts an outstanding real operation, including fault-adapter delays and flush.
class IoHealth final : public FileIoFaultAdapter
{
public:
    explicit IoHealth(FileIoFaultAdapter* next = nullptr) : downstream(next) {}
    static std::int64_t now() noexcept
    { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void ioStarted() noexcept override
    {
        const auto at = now();
        // One slot per file owner avoids hiding a stalled WAV behind journal I/O.
        for (auto& slot : started) { std::int64_t empty = 0; if (slot.compare_exchange_strong(empty, at)) { currentSlot = &slot; break; } }
    }
    void ioFinished() noexcept override
    {
        if (currentSlot) { const auto at = currentSlot->exchange(0); if (now() - at >= 500) lastDelay = now(); currentSlot = nullptr; }
    }
    bool delayed() const noexcept
    {
        const auto at = now();
        if (lastDelay.load() && at - lastDelay.load() < 1000) return true;
        for (const auto& slot : started) { const auto begin = slot.load(); if (begin && at - begin >= 500) return true; }
        return false;
    }
    juce::Result beforeIo(FileIoOperation op, const juce::File& file, std::uint64_t offset, std::size_t size) override
    { return downstream ? downstream->beforeIo(op, file, offset, size) : juce::Result::ok(); }
    std::uint64_t observedOffset(std::uint64_t offset) const override
    { return downstream ? downstream->observedOffset(offset) : offset; }
    bool splitWritesForTesting() const noexcept override { return downstream && downstream->splitWritesForTesting(); }
private:
    FileIoFaultAdapter* downstream;
    std::array<std::atomic<std::int64_t>, 16> started{};
    std::atomic<std::int64_t> lastDelay{0};
    inline static thread_local std::atomic<std::int64_t>* currentSlot = nullptr;
};
}
