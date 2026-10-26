#pragma once

#include "../CollabContracts.h"
#include <array>
#include <cstdint>
#include <string>

namespace gocue::livemix::recording
{
enum class Format : std::uint8_t { wav = 0, mp3 = 1 };
enum class State : std::uint8_t { idle = 0, starting, recording, stopping, error };
enum class Error : std::uint8_t { none = 0, invalidOptions, openFailed, writeFailed, queueOverflow, encoderUnavailable, encoderFailed };
enum class Track : std::uint8_t { master = 0, remote1 = 1, remote2 = 2, mix = 3 };

inline constexpr unsigned wavBitsPerSample = 24;
inline constexpr std::array<unsigned, 3> mp3BitratesKbps {{ 128, 256, 320 }};
inline constexpr unsigned defaultMp3BitrateKbps = 320, mp3FallbackRate = 48000;
inline constexpr std::uint64_t wavSplitBytes = (std::uint64_t { 1 } << 32) - (std::uint64_t { 1 } << 20);
inline constexpr unsigned checkpointMs = 1000, queueSeconds = 4;
inline constexpr char formatSetting[] = "recordingFormat";
inline constexpr char bitrateSetting[] = "recordingMp3Kbps";
inline constexpr char directorySetting[] = "recordingDirectory";

// Control-thread types. Strings and paths are UTF-8, never callback-owned.
struct Options
{
    Format format = Format::wav;
    unsigned mp3Kbps = defaultMp3BitrateKbps;
    std::string directoryUtf8; // empty: Documents / "LiveMix 녹음"
};
struct TrackIdentity
{
    collab::contracts::ParticipantId participantId = 0;
    collab::contracts::ParticipantKind kind = collab::contracts::ParticipantKind::none;
    std::string nicknameUtf8;
};
struct StartRequest
{
    Options options;
    bool collabAtStart = false;
    // Frozen file set: 0 for solo; 1 or 2 remote slots for a collab take.
    // The highest occupied local OBS slot at start determines the count (minimum 1).
    unsigned remoteFileCount = 0;
    std::array<TrackIdentity, 3> people {}; // self, local OBS slot 1, local OBS slot 2
};
struct DeviceFormat
{
    double sampleRate = 48000.0;
    int maxBlockFrames = 0;
    std::uint32_t formatEpoch = 1;
};
struct FileSnapshot
{
    Track track = Track::master;
    unsigned channels = 2, sampleRate = 48000, part = 1;
    std::uint64_t writtenFrames = 0, checkpointFrames = 0, clippedSamples = 0;
    std::string pathUtf8;
};
struct Snapshot
{
    State state = State::idle;
    Error error = Error::none;
    Options options;
    bool collabAtStart = false;
    unsigned fileCount = 0;
    std::uint64_t capturedFrames = 0;
    double elapsedSeconds = 0.0;
    std::array<FileSnapshot, 4> files {};
    std::string errorDetail; // diagnostic data, not a new UI label
};
}
