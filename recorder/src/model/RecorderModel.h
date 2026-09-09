#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace gocue::recorder
{
using Sample = std::int64_t;
using Id = juce::String;
Id newId();
bool isId(const Id&);

// Copying an edit snapshot shares clip storage. A writer must detach via edit().
template<class T> class SharedList
{
public:
    const std::vector<T>& items() const noexcept { return *values; }
    std::vector<T>& edit()
    {
        if (values.use_count() != 1) values = std::make_shared<std::vector<T>>(*values);
        return *values;
    }
private:
    std::shared_ptr<std::vector<T>> values = std::make_shared<std::vector<T>>();
};

struct FrameRate
{
    std::uint32_t numerator = 30, denominator = 1;
    bool operator==(const FrameRate& other) const noexcept
    { return numerator == other.numerator && denominator == other.denominator; }
    bool operator!=(const FrameRate& other) const noexcept { return !(*this == other); }
};

// Absolute-origin, exact integer rescaling; ties round away from zero.
// Throws invalid_argument for zero factors, overflow_error if the result cannot fit.
Sample rescaleRound(Sample position, std::uint64_t numerator, std::uint64_t denominator);
Sample frameToSample(Sample frame, std::uint32_t Fs, FrameRate fps);
Sample sampleToFrame(Sample sample, std::uint32_t Fs, FrameRate fps);

struct SampleRange { Sample start = 0, length = 0; }; // [start, start + length)
enum class AssetKind { camera, mic, importAudio };
enum class TrackKind { cam1, cam2, mic, importAudio };
enum class TakeMode { normal, dub };
enum class TakeState { recording, stopped, finalising, complete, partial };

struct OriginalFormat
{
    juce::String codec;
    std::uint32_t sampleRate = 0;
    int channels = 0, bitsPerSample = 0, width = 0, height = 0;
    FrameRate fps;
};
struct MediaChunk
{
    juce::String relativePath;
    SampleRange sourceRange;
};
struct MediaAsset
{
    Id assetId = newId();
    AssetKind kind = AssetKind::camera;
    juce::String relativePath; // empty only when chunks provide the source
    std::vector<MediaChunk> chunks;
    OriginalFormat originalFormat;
    juce::String contentIdentity;
    Sample logicalLength = 0; // source coordinates in project Fs, including explicit gaps
    std::vector<SampleRange> availableRanges, gaps; // disjoint partition of [0, logicalLength)
    std::uint64_t sourceUnitsNumerator = 1, sourceUnitsDenominator = 1;
    Sample mediaGeneration = 0;
};
struct CaptureSnapshot
{
    std::array<juce::String, 2> cameraDeviceIds, cameraModes;
    juce::String asioDeviceId, calibrationDate, calibrationIdentity;
    std::vector<int> physicalInputs;
    std::array<Sample, 2> cameraOffsetSamples {};
    Sample inputOffsetSamples = 0, outputOffsetSamples = 0;
};
struct Take
{
    Id takeId = newId();
    int number = 0; // assigned by placeTake when zero
    juce::String createdAt, name;
    TakeMode mode = TakeMode::normal;
    Sample N0 = 0, O0 = 0, placementSample = 0, logicalLength = 0;
    Id cam1AssetId, cam2AssetId;
    std::vector<Id> microphoneAssetIds;
    CaptureSnapshot capture;
    TakeState state = TakeState::stopped;
};
struct MediaRegistry
{
    std::vector<MediaAsset> assets;
    std::vector<Take> takes;
    const MediaAsset* findAsset(const Id&) const;
    const Take* findTake(const Id&) const;
};
struct Clip
{
    Id clipId = newId(), trackId, assetId;
    Sample sourceIn = 0, lengthSamples = 0, timelineStartSample = 0;
    Id linkGroupId, takeStackId, versionId;
    Sample timelineEnd() const; // checked addition
};
struct Track
{
    Id trackId = newId();
    TrackKind kind = TrackKind::cam1;
    juce::String name;
    bool mute = false, solo = false;
    int microphoneIndex = -1; // logical 0..7 for mic, -1 otherwise
    SharedList<Clip> clips;
};
struct Marker { Id markerId = newId(); Sample sample = 0; juce::String name, colour = "#4c8dff"; };
struct LinkGroup { Id linkGroupId = newId(); std::vector<Id> clipIds; };
struct TakeVersion { Id versionId = newId(); std::vector<Id> clipIds; };
struct TakeStack
{
    Id stackId = newId();
    Sample anchorSample = 0, spanSamples = 0;
    std::vector<TakeVersion> versions;
    Id activeVersionId;
};
struct EditState
{
    juce::String name = juce::String::fromUTF8("새 프로젝트");
    std::vector<Track> tracks;
    std::vector<Marker> markers;
    std::vector<LinkGroup> linkGroups;
    std::vector<TakeStack> takeStacks;
};
struct RecorderProject : EditState
{
    static constexpr int currentSchemaVersion = 1;
    int schemaVersion = currentSchemaVersion;
    Id projectId = newId();
    std::uint32_t Fs = 48000; // provisional until the first asset; no device is opened here
    FrameRate fps;
    Sample editRevision = 0;
    std::shared_ptr<const MediaRegistry> media = std::make_shared<const MediaRegistry>();
    juce::Result validate() const;
    bool isActive(const Clip&) const;
    Sample activeTimelineEnd() const;
    const Clip* findClip(const Id&) const;
};

// Data only. Publish as shared_ptr<const RenderPlan>; compilation belongs to round 14.
struct RenderClip
{
    Id clipId, trackId, assetId;
    Sample sourceIn = 0, timelineStartSample = 0, lengthSamples = 0, mediaGeneration = 0;
    std::uint64_t sourceUnitsNumerator = 1, sourceUnitsDenominator = 1;
    std::vector<SampleRange> gaps;
    bool mute = false, solo = false;
    Sample microfadeInSamples = 0, microfadeOutSamples = 0;
};
struct RenderPlan
{
    const Id projectId;
    const Sample editRevision;
    const std::uint32_t Fs;
    const FrameRate fps;
    const std::vector<RenderClip> activeClips;
};
bool isProjectRelativePath(const juce::String&);
} // namespace gocue::recorder
