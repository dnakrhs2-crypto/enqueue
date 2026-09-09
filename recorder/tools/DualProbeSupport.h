#pragma once
#include "storage/DurableFile.h"
#include "support/Platform.h"
#include <chrono>
#include <thread>

namespace gocue::recorder::probe
{
enum class AudioStopReason { none, rawOverflow, invalidRaw, writerFailure, discontinuity, driverReset, driverError };
class TakeStopSignal
{
public:
    void request(AudioStopReason why) noexcept
    { auto expected = AudioStopReason::none; reason.compare_exchange_strong(expected, why); }
    bool requested() const noexcept { return reason.load() != AudioStopReason::none; }
    AudioStopReason get() const noexcept { return reason.load(); }
private:
    std::atomic<AudioStopReason> reason{AudioStopReason::none};
};
// One adapter per writer owner, so both MP4s and WAV stop independently at the
// same requested wall-clock point. Preparation/header writes are never stalled.
class WriterStall final : public FileIoFaultAdapter
{
public:
    explicit WriterStall(unsigned milliseconds = 0) : milliseconds(milliseconds) {}
    void arm(std::int64_t at) noexcept { deadline.store(at); }
    juce::Result beforeIo(FileIoOperation op, const juce::File&, std::uint64_t, std::size_t) override
    {
        const auto at = deadline.load();
        if (milliseconds && at && qpcNow() >= at && op == FileIoOperation::append && !injected.exchange(true))
        {
            began = qpcNow();
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
            ended = qpcNow();
        }
        return juce::Result::ok();
    }
    juce::var toJson() const // writer joined
    {
        auto v = jsonObject(); jsonSet(v, "requestedMs", milliseconds); jsonSet(v, "injected", injected.load());
        jsonSet(v, "beginQpc", std::to_string(began)); jsonSet(v, "endQpc", std::to_string(ended));
        jsonSet(v, "measuredMs", 1000.0 * (ended - began) / qpcFrequency()); return v;
    }
private:
    unsigned milliseconds;
    std::atomic<std::int64_t> deadline{0};
    std::atomic<bool> injected{false};
    std::int64_t began = 0, ended = 0;
};
}
