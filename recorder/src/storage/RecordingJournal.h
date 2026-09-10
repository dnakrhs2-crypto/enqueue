#pragma once
#include "DurableFile.h"
#include <vector>
#include <functional>

namespace gocue::recorder
{
enum class JournalKind : std::uint16_t
{
    TakeStarted = 1, Checkpoint, TakeStopped, TakeFinalized,
    EditTransaction = 16, TakePlacement, RetakeVersionSwitch, EditCheckpoint, MediaRegistry
}; // RetakeVersionSwitch is a storage contract; controller binding is round 27/28.
struct JournalPcmFormat
{
    std::uint32_t sampleRate = 48000;
    std::uint16_t channels = 1, bitsPerSample = 24, blockAlign = 3;
    std::uint32_t dataOffset = 44;
    juce::String nativeFormat = "signed PCM24 in right-aligned int32; no conversion";
};
struct JournalFileDescription
{
    juce::String assetId, path, plannedPath; // Project-relative paths; plannedPath may be a chunk pattern.
    unsigned channels = 0; // WAV override; 0 inherits the take PCM format (legacy journals).
};
struct JournalDeviceMapping
{
    juce::String deviceId, name;
    int mic = 0, activeIndex = 0, physicalIndex = 0; // mic is 1-based, device indices are 0-based.
    int rightActiveIndex = -1, rightPhysicalIndex = -1; // -1 = mono
    unsigned channels() const noexcept { return rightPhysicalIndex >= 0 ? 2u : 1u; }
};
struct JournalTakeStarted
{
    juce::Uuid takeId;
    std::vector<JournalFileDescription> files;
    JournalPcmFormat pcm;
    std::int64_t n0 = 0, o0 = 0, pstart = 0;
    bool usesOutputOrigin = false;
    juce::String placementMode = "normal"; // Independent of input/output clock selection.
    std::vector<JournalDeviceMapping> devices;
};
struct JournalFilePosition
{
    juce::String path;
    std::uint64_t validBytes = 0, validSamples = 0, firstSample = 0;
    std::int64_t pts = 0; // Exclusive valid end in the stated time base, not a wall-clock time.
    std::uint32_t ptsTimeBaseNum = 1, ptsTimeBaseDen = 1, dataOffset = 44, blockAlign = 3;
};
struct JournalCheckpoint { juce::Uuid takeId; std::vector<JournalFilePosition> files; };
struct JournalTakeStopped { juce::Uuid takeId; std::int64_t nstop = 0; juce::Uuid placementEditId; };
struct JournalTakeFinalized { juce::Uuid takeId; };
struct JournalRecord
{
    JournalKind kind = JournalKind::TakeStarted;
    std::uint64_t sequence = 0;
    juce::Uuid transaction;
    juce::var payload; // Validated schema-1 JSON; integer coordinates retain int64 precision.
};
struct JournalReplay
{
    std::vector<JournalRecord> records;
    std::uint64_t lastSequence = 0, duplicateTransactions = 0, validBytesInLastFile = 0;
    unsigned fileCount = 0;
    bool ignoredTail = false;
    juce::String tailReason;
    juce::File tailFile;
};

// Schema-1 framing, all integers little-endian: magic:u32, totalLength:u32,
// schema:u16, kind:u16, sequence:u64, transaction:16 bytes, payloadLength:u32,
// payloadCRC32:u32, headerCRC32:u32 (first 44 bytes), UTF-8 JSON, COMMIT01:8 bytes.
// CRC32 is IEEE; totalLength includes the 48-byte header and 8-byte commit.
// Single owner thread and one journal writer per directory. Media flushes must
// succeed BEFORE appending Checkpoint. append() includes the journal OS flush.
class RecordingJournal
{
public:
    static constexpr std::uint16_t schemaVersion = 1;
    static constexpr std::size_t headerBytes = 48, commitBytes = 8, maxRecordBytes = 16 * 1024 * 1024;
    explicit RecordingJournal(FileIoFaultAdapter* adapter = nullptr) : stream(adapter) {}
    ~RecordingJournal();
    RecordingJournal(const RecordingJournal&) = delete;
    RecordingJournal& operator=(const RecordingJournal&) = delete;
    juce::Result open(const juce::File& journalDirectory, std::uint64_t rotationBytes = 8 * 1024 * 1024);
    juce::Result append(const JournalTakeStarted&, const juce::Uuid& transaction = juce::Uuid());
    juce::Result append(const JournalCheckpoint&, const juce::Uuid& transaction = juce::Uuid());
    juce::Result append(const JournalTakeStopped&, const juce::Uuid& transaction = juce::Uuid());
    juce::Result append(const JournalTakeFinalized&, const juce::Uuid& transaction = juce::Uuid(),
                        const std::function<void(const char*)>& = {});
    juce::Result close();
    std::uint64_t durableSequence() const noexcept { return sequence; } // Owner thread.
    static juce::File logFile(const juce::File&, unsigned number);
    // Structural corruption stops replay across ALL later segments. It is reported
    // in ignoredTail; OS/read errors return failure. No repair or truncation here.
    static juce::Result replay(const juce::File& journalDirectory, JournalReplay&);
    // Independent edit sequence/lock. A verified checkpoint supplies the retained
    // first segment and preceding sequence after old segments have been collected.
    juce::Result openEdits(const juce::File&, unsigned firstSegment = 1,
                           std::uint64_t precedingSequence = 0, std::uint64_t rotationBytes = 8 * 1024 * 1024);
    static juce::Result replayEdits(const juce::File&, JournalReplay&, unsigned firstSegment = 1,
                                   std::uint64_t precedingSequence = 0);
    static juce::File editLogFile(const juce::File&, unsigned);
    juce::Result appendEditRecord(JournalKind, const juce::var&, const juce::Uuid& = juce::Uuid(),
                                  const std::function<void(const char*)>& = {});
    juce::Result rotate();
    unsigned currentSegment() const noexcept { return segment; }

private:
    juce::Result openDomain(const juce::File&, std::uint64_t, bool, unsigned, std::uint64_t);
    static juce::Result replayDomain(const juce::File&, JournalReplay&, bool, unsigned, std::uint64_t);
    juce::Result appendRecord(JournalKind, const juce::Uuid&, const juce::var&,
                              const std::function<void(const char*)>& = {});
    juce::Result fail(juce::Result);
    DurableFile stream;
    HANDLE writerLock = INVALID_HANDLE_VALUE;
    juce::File directory;
    std::uint64_t sequence = 0, rotateAt = 0;
    unsigned segment = 0;
    bool edits = false;
    juce::Result error = juce::Result::ok();
};
}
