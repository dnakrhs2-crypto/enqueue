#pragma once
#include "AudioRenderFixtures.h"
#include "TestSupport.h"
#include <cmath>

namespace recorder_audio_fixture
{
enum class RecordedGap { missingFile, shortFile, uncommittedFile, middleChunk, singleFileTail };
inline const char* gapName(RecordedGap gap)
{
    switch (gap)
    {
        case RecordedGap::missingFile: return "missing file";
        case RecordedGap::shortFile: return "short header";
        case RecordedGap::uncommittedFile: return "uncommitted complete file";
        case RecordedGap::middleChunk: return "missing middle chunk";
        case RecordedGap::singleFileTail: return "single-file durable prefix";
    }
    return "unknown";
}
inline constexpr RecordedGap recordedGaps[]{RecordedGap::missingFile, RecordedGap::shortFile,
    RecordedGap::uncommittedFile, RecordedGap::middleChunk, RecordedGap::singleFileTail};

// Two consecutive takes on one microphone lane: a damaged take, then a healthy
// take of the same channel layout. Camera assets are metadata for CPU mux tests.
struct RecordedGapFixture
{
    static constexpr Sample takeFrames = 4800, totalFrames = takeFrames * 2;
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-gap-fixture-" + newId());
    RecorderProject project;
    unsigned channels;
    RecordedGap gap;
    std::vector<std::int32_t> pcm = std::vector<std::int32_t>(takeFrames);

    RecordedGapFixture(unsigned ch, RecordedGap kind) : channels(ch), gap(kind)
    {
        using recorder_test::require;
        require(root.createDirectory().wasOk(), "Create gap fixture directory");
        for (Sample i = 0; i < takeFrames; ++i)
            pcm[std::size_t(i)] = std::int32_t(2000000 * std::sin(6.283185307179586 * 440 * i / project.Fs));
        Track camera; camera.kind = TrackKind::cam1;
        Track mic; mic.kind = TrackKind::mic; mic.microphoneIndex = 0;
        auto registry = std::make_shared<MediaRegistry>();
        for (unsigned n = 0; n < 2; ++n)
        {
            Take take; take.number = int(n + 1); take.createdAt = "2026-09-10";
            take.state = n ? TakeState::complete : TakeState::partial;
            take.logicalLength = takeFrames; take.placementSample = n * takeFrames;
            MediaAsset video; video.kind = AssetKind::camera; video.logicalLength = takeFrames;
            video.relativePath = "media/takes/" + take.takeId + "/cam1.mp4";
            video.contentIdentity = "metadata-only-camera"; video.availableRanges = {{0, takeFrames}};
            video.originalFormat.codec = "h264"; video.originalFormat.width = 1920; video.originalFormat.height = 1080;
            video.sourceUnitsNumerator = 30; video.sourceUnitsDenominator = project.Fs;
            take.cam1AssetId = video.assetId;
            MediaAsset audio; audio.kind = AssetKind::mic; audio.logicalLength = takeFrames; audio.mediaGeneration = n ? 1 : 2;
            audio.contentIdentity = "gap-regression-mic"; audio.originalFormat.codec = "pcm_s24le";
            audio.originalFormat.sampleRate = project.Fs; audio.originalFormat.channels = int(channels); audio.originalFormat.bitsPerSample = 24;
            const auto path = "media/takes/" + take.takeId + "/mic01/000001.wav";
            if (n || gap == RecordedGap::middleChunk)
            {
                const std::vector<SampleRange> ranges = n ? std::vector<SampleRange>{{0, takeFrames}}
                    : std::vector<SampleRange>{{0, 1600}, {3200, 1600}};
                for (const auto range : ranges)
                {
                    const auto chunkPath = "media/takes/" + take.takeId + "/mic01/" + juce::String(range.start) + ".wav";
                    writePcm24(root.getChildFile(chunkPath), project.Fs, pcm, range.start, range.length, channels);
                    audio.chunks.push_back({chunkPath, range});
                }
                audio.availableRanges = ranges;
                if (!n) audio.gaps = {{1600, 1600}};
            }
            else
            {
                audio.relativePath = path;
                if (gap == RecordedGap::singleFileTail)
                {
                    writePcm24(root.getChildFile(path), project.Fs, pcm, 0, 3200, channels);
                    audio.availableRanges = {{0, 3200}}; audio.gaps = {{3200, 1600}};
                }
                else
                {
                    audio.gaps = {{0, takeFrames}};
                    if (gap == RecordedGap::uncommittedFile)
                        writePcm24(root.getChildFile(path), project.Fs, pcm, 0, takeFrames, channels);
                    if (gap == RecordedGap::shortFile)
                    {
                        require(root.getChildFile(path).getParentDirectory().createDirectory().wasOk(), "Create short WAV parent");
                        require(root.getChildFile(path).replaceWithData("RIFF", 4), "Write torn WAV header");
                    }
                }
            }
            take.microphoneAssetIds = {audio.assetId}; take.capture.physicalInputs = {0};
            if (channels == 2) take.capture.physicalInputsRight = {1};
            Clip v; v.trackId = camera.trackId; v.assetId = video.assetId; v.timelineStartSample = take.placementSample; v.lengthSamples = takeFrames;
            camera.clips.edit().push_back(v);
            Clip a; a.trackId = mic.trackId; a.assetId = audio.assetId; a.timelineStartSample = take.placementSample; a.lengthSamples = takeFrames;
            mic.clips.edit().push_back(a);
            registry->assets.push_back(video); registry->assets.push_back(audio); registry->takes.push_back(take);
        }
        project.tracks = {camera, mic}; project.media = registry;
        const auto valid = project.validate(); require(valid.wasOk(), valid.getErrorMessage().toRawUTF8());
    }
    ~RecordedGapFixture()
    {
        if (root.getParentDirectory() == juce::File::getSpecialLocation(juce::File::tempDirectory)
            && root.getFileName().startsWith("recorder-gap-fixture-")) root.deleteRecursively();
    }
    bool silent(Sample at) const
    {
        if (at >= takeFrames) return false;
        if (gap == RecordedGap::middleChunk) return at >= 1600 && at < 3200;
        if (gap == RecordedGap::singleFileTail) return at >= 3200;
        return true;
    }
    float sample(Sample at, unsigned channel) const
    {
        if (silent(at)) return 0;
        const auto value = pcm[std::size_t(at % takeFrames)];
        return float(channels == 2 && channel ? -value / 2 : value) / 8388608.0f;
    }
    void verifyPcm(const std::vector<float>& l, const std::vector<float>& r) const
    {
        recorder_test::require(l.size() == totalFrames && r.size() == l.size(), "Both takes retain their full logical duration");
        for (Sample at = 0; at < totalFrames; ++at)
        {
            // Gap samples are always exactly zero. Compare available PCM away
            // from the documented 144-sample microfades at edit/gap boundaries.
            if (silent(at) || (at % 1600 > 144 && at % 1600 < 1456))
                recorder_test::require(l[std::size_t(at)] == sample(at, 0) && r[std::size_t(at)] == sample(at, 1),
                    "Gap silence or healthy take/channel PCM changed");
        }
    }
};
}
