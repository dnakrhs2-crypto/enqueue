#include "RecordingRecovery.h"

namespace gocue::livemix::recording
{
RecordingRecovery::Stream::Stream(const juce::File& path, std::uint64_t limit)
    : file(path, 65536), budget(limit) { failed = !file.openedOk(); }
bool RecordingRecovery::Stream::write(const void* data, std::size_t n)
{
    if (failed || n > budget - bytes) { failed = true; return false; }
    bytes += n;
    if (!file.write(data, n)) failed = true;
    return ok();
}
void RecordingRecovery::Stream::flush() { file.flush(); failed = failed || file.getStatus().failed(); }
juce::int64 RecordingRecovery::Stream::getPosition() { return file.getPosition(); }
bool RecordingRecovery::Stream::setPosition(juce::int64 p)
{
    if (!file.setPosition(p)) failed = true;
    return ok();
}
bool RecordingRecovery::Stream::ok() const { return !failed && file.getStatus().wasOk(); }
juce::String RecordingRecovery::Stream::error() const
{
    return file.getStatus().failed() ? file.getStatus().getErrorMessage() : (failed ? "file write/seek failed" : "");
}
std::unique_ptr<juce::OutputStream> RecordingRecovery::borrow(Stream& stream)
{
    struct Borrowed final : juce::OutputStream
    {
        explicit Borrowed(Stream& s) : source(s) {}
        bool write(const void* p, std::size_t n) override { return source.write(p, n); }
        void flush() override { source.flush(); }
        juce::int64 getPosition() override { return source.getPosition(); }
        bool setPosition(juce::int64 p) override { return source.setPosition(p); }
        Stream& source;
    };
    return std::make_unique<Borrowed>(stream);
}
bool RecordingRecovery::checkpoint(Stream& stream, std::uint64_t frames)
{
    stream.flush();
    if (!stream.ok()) return false;
    completed = frames;
    return true;
}
}
