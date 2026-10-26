#pragma once
#include <juce_core/juce_core.h>
#include <cstdint>
#include <limits>
#include <memory>

namespace gocue::livemix::recording
{
// Owns the real stream, retains write/seek/flush errors, and publishes only completed checkpoints.
// There is deliberately no startup directory scan or automatic resume.
class RecordingRecovery
{
public:
    static constexpr std::uint64_t unlimited = std::numeric_limits<std::uint64_t>::max();
    class Stream final : public juce::OutputStream
    {
    public:
        explicit Stream(const juce::File&, std::uint64_t failAfterBytes = unlimited);
        bool write(const void*, std::size_t) override;
        void flush() override;
        juce::int64 getPosition() override;
        bool setPosition(juce::int64) override;
        bool ok() const;
        juce::String error() const;
    private:
        juce::FileOutputStream file;
        std::uint64_t budget, bytes = 0;
        bool failed = false;
    };
    // Non-owning adapter allows the WAV writer to die (including its final header write) before file flush.
    static std::unique_ptr<juce::OutputStream> borrow(Stream&);
    bool checkpoint(Stream&, std::uint64_t frames);
    std::uint64_t completedFrames() const noexcept { return completed; }
private:
    std::uint64_t completed = 0;
};
}
