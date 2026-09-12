#pragma once
#include "DurableFile.h"
#include <functional>
#include <vector>
#include <stdexcept>

namespace gocue::recorder::recovery
{
using Hook = std::function<void(const char*)>; // Worker-only deterministic crash seam.
struct OutputError : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool, const juce::String&);
void check(const juce::Result&);
juce::var object();
void set(juce::var&, const char*, const juce::var&);
juce::var integer(std::int64_t);
std::int64_t number(const juce::var&);
void hit(const Hook&, const char*);
void writeNew(const juce::File&, const void*, std::size_t, FileIoFaultAdapter* = nullptr);
void writeJson(const juce::File&, const juce::var&, FileIoFaultAdapter* = nullptr);
juce::String sha256(const juce::File&);
juce::File child(const juce::File& root, const juce::String& relative);

class WriterLock
{
public:
    explicit WriterLock(const juce::File&);
    ~WriterLock();
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
private:
    HANDLE handle = INVALID_HANDLE_VALUE;
};

// RCV1, same LE framing/CRC/COMMIT01 layout as the take journal. Separate
// namespace of kinds, no reinterpretation of old RCJ1 records. Streaming replay
// retains one payload plus transaction IDs for deduplication. Callers own the
// project lock for edit/commit logs.
enum class Kind : std::uint16_t { edit = 1, recovery = 2, codec = 3, fragment = 4 };
struct Record { Kind kind; std::uint64_t sequence; juce::Uuid transaction; juce::var payload; };
struct Replay
{
    std::uint64_t sequence = 0, bytes = 0, duplicates = 0;
    bool ignoredTail = false;
    juce::String reason;
};
class Log
{
public:
    explicit Log(FileIoFaultAdapter* fault = nullptr) : file(fault) {}
    void open(const juce::File&, bool createNew = false);
    void append(Kind, const juce::var&, const juce::Uuid& = juce::Uuid(), const Hook& = {});
    void close();
    static Replay read(const juce::File&, const std::function<bool(const Record&)>&);
private:
    DurableFile file;
    std::uint64_t sequence = 0;
};
}
