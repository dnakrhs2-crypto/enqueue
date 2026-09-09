#pragma once
#include <juce_core/juce_core.h>
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gocue::recorder
{
enum class FileIoOperation { open, append, patch, flushData, appendProgress, patchProgress };

// Optional test seam. Invoked only by the file owner (never by an audio callback).
// The adapter must outlive every file using it; it may delay or return an error.
class FileIoFaultAdapter
{
public:
    virtual ~FileIoFaultAdapter() = default;
    virtual juce::Result beforeIo(FileIoOperation, const juce::File&,
                                  std::uint64_t offset, std::size_t bytes) = 0;
    // Test-only virtual coordinates; never passed to the native filesystem.
    virtual std::uint64_t observedOffset(std::uint64_t offset) const { return offset; }
    virtual void ioStarted() noexcept {}
    virtual void ioFinished() noexcept {}
    virtual bool splitWritesForTesting() const noexcept { return true; }
};

// Single writer, synchronous Win32 I/O, no application buffering. Existing bytes
// are never truncated. A failed operation latches an error until close/reopen.
class DurableFile
{
public:
    enum class OpenMode { createNew, appendOrCreate };
    explicit DurableFile(FileIoFaultAdapter* adapter = nullptr) : faults(adapter) {}
    ~DurableFile(); // Handle cleanup only. Explicit flushData()/close() report errors.
    DurableFile(const DurableFile&) = delete;
    DurableFile& operator=(const DurableFile&) = delete;

    juce::Result open(const juce::File&, OpenMode = OpenMode::appendOrCreate);
    juce::Result write(const void* bytes, std::size_t count);
    // Restricted in-place header update: must stay inside the existing file.
    juce::Result writeAt(std::uint64_t offset, const void* bytes, std::size_t count);
    juce::Result flushApplicationBuffers(); // No-op for this unbuffered-at-app layer.
    juce::Result flushData();               // Calls FlushFileBuffers; failure is observable.
    juce::Result close();                   // Does NOT imply a durable flush.
    std::uint64_t writtenBytes() const noexcept { return length.load(std::memory_order_acquire); }
    // Length at the last successful FlushFileBuffers in this open session, initially 0.
    // This is not proof of device power-loss protection or of a journal commit.
    std::uint64_t durableBytes() const noexcept { return durable.load(std::memory_order_acquire); }
    const juce::Result& status() const noexcept { return error; } // Owner thread only.
    bool isOpen() const noexcept { return handle != INVALID_HANDLE_VALUE; }
    const juce::File& file() const noexcept { return path; }

private:
    juce::Result remember(juce::Result);
    juce::Result windowsError(const char*, DWORD);
    juce::Result check(FileIoOperation, std::uint64_t, std::size_t);
    juce::Result writeImpl(std::uint64_t, const void*, std::size_t, FileIoOperation);
    HANDLE handle = INVALID_HANDLE_VALUE;
    juce::File path;
    FileIoFaultAdapter* faults = nullptr;
    juce::Result error = juce::Result::ok();
    std::atomic<std::uint64_t> length{0}, durable{0};
};
}
