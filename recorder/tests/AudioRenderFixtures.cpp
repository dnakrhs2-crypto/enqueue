#include "AudioRenderFixtures.h"
#include "TestSupport.h"
#include "model/ClipEdits.h"
#include <algorithm>

namespace recorder_audio_fixture
{
using recorder_test::require;
void writePcm24(const juce::File& file, std::uint32_t Fs, const std::vector<std::int32_t>& data, Sample first, Sample count)
{
    require(file.getParentDirectory().createDirectory().wasOk(), "Fixture directory");
    auto stream = file.createOutputStream(); require(stream != nullptr, "Fixture WAV stream");
    stream->write("RIFF", 4); stream->writeInt(static_cast<int>(36 + count * 3)); stream->write("WAVEfmt ", 8);
    stream->writeInt(16); stream->writeShort(1); stream->writeShort(1); stream->writeInt(static_cast<int>(Fs));
    stream->writeInt(static_cast<int>(Fs * 3)); stream->writeShort(3); stream->writeShort(24); stream->write("data", 4);
    stream->writeInt(static_cast<int>(count * 3));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count) * 3);
    for (Sample i = 0; i < count; ++i)
    {
        const auto value = static_cast<std::uint32_t>(data[static_cast<std::size_t>(first + i)]);
        for (unsigned b = 0; b < 3; ++b) bytes[static_cast<std::size_t>(i) * 3 + b] = static_cast<std::uint8_t>(value >> (b * 8));
    }
    require(stream->write(bytes.data(), bytes.size()), "Fixture PCM write"); stream->flush(); require(stream->getStatus().wasOk(), "Fixture WAV flush");
}
Fixture::Fixture(std::uint32_t Fs)
{
    root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-r14-fixture-" + newId());
    require(root.createDirectory().wasOk(), "Create fixture root"); project.Fs = Fs; project.editRevision = 14;
    auto registry = std::make_shared<MediaRegistry>();
    Take take; take.number = 1; take.createdAt = "2026-09-09"; take.logicalLength = Sample(Fs) * 10; take.state = TakeState::complete;
    for (unsigned camera = 0; camera < 2; ++camera)
    {
        MediaAsset asset; asset.kind = AssetKind::camera; asset.relativePath = "media/takes/fixture/cam" + juce::String(camera + 1) + ".mp4";
        asset.contentIdentity = "metadata-only-camera"; asset.logicalLength = take.logicalLength; asset.availableRanges = {{0, asset.logicalLength}};
        asset.originalFormat.codec = "h264"; asset.originalFormat.width = 1920; asset.originalFormat.height = 1080;
        asset.sourceUnitsNumerator = 30; asset.sourceUnitsDenominator = Fs;
        if (camera) take.cam2AssetId = asset.assetId; else take.cam1AssetId = asset.assetId;
        Track track; track.kind = camera ? TrackKind::cam2 : TrackKind::cam1;
        Clip clip; clip.trackId = track.trackId; clip.assetId = asset.assetId; clip.lengthSamples = take.logicalLength;
        track.clips.edit().push_back(clip); project.tracks.push_back(track); registry->assets.push_back(asset);
    }
    for (unsigned channel = 0; channel < 2; ++channel)
    {
        auto& data = pcm[channel]; data.resize(static_cast<std::size_t>(take.logicalLength));
        std::uint32_t state = 0x91e10da5u ^ (0x9e3779b9u * (channel + 1));
        for (Sample i = 0; i < take.logicalLength; ++i)
        {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            data[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(state & 0x7fffff) - 0x400000;
            if (i % 9973 == 0 || i % 131071 == 0) data[static_cast<std::size_t>(i)] = channel ? -0x700001 : 0x700003;
        }
        MediaAsset asset; asset.kind = AssetKind::mic; asset.logicalLength = take.logicalLength; asset.mediaGeneration = 1;
        asset.originalFormat.codec = "pcm_s24le"; asset.originalFormat.sampleRate = Fs; asset.originalFormat.channels = 1; asset.originalFormat.bitsPerSample = 24;
        asset.contentIdentity = "prbs-impulse-mic-" + juce::String(channel); asset.availableRanges = {{0, asset.logicalLength}};
        for (Sample at = 0; at < take.logicalLength; at += 131071)
        {
            const auto count = (std::min)(Sample{131071}, take.logicalLength - at);
            const auto path = "media/takes/fixture/mic" + juce::String(channel) + "/" + juce::String(at) + ".wav";
            const auto file = root.getChildFile(path); writePcm24(file, Fs, data, at, count); originals.push_back(file);
            asset.chunks.push_back({path, {at, count}});
        }
        take.microphoneAssetIds.push_back(asset.assetId); take.capture.physicalInputs.push_back(static_cast<int>(channel));
        Track track; track.kind = TrackKind::mic; track.microphoneIndex = static_cast<int>(channel);
        Clip clip; clip.assetId = asset.assetId; clip.trackId = track.trackId; clip.lengthSamples = take.logicalLength;
        track.clips.edit().push_back(clip); project.tracks.push_back(track); registry->assets.push_back(asset);
    }
    registry->takes.push_back(take); project.media = registry; require(project.validate().wasOk(), "Validate synthetic project");
}
Fixture::~Fixture()
{
    if (root.getParentDirectory() == juce::File::getSpecialLocation(juce::File::tempDirectory)
        && root.getFileName().startsWith("recorder-r14-fixture-")) root.deleteRecursively();
}
RecorderProject Fixture::example(unsigned number) const
{
    ClipEditResult edited(project);
    const SampleRange cut{Sample(project.Fs) * 2, project.Fs};
    if (number == 1) edited = ClipEdits::remove(project, {project.tracks[3].clips.items()[0].clipId}, cut);
    else if (number == 2) edited = ClipEdits::rippleDeleteTracks(project, cut, {project.tracks[3].trackId});
    else if (number == 3) edited = ClipEdits::rippleDeleteAll(project, cut);
    else throw std::invalid_argument("Fixture example must be 1..3");
    require(edited.status.wasOk(), "Actual clip edit for section 11.1 fixture"); ++edited.project.editRevision; return edited.project;
}
std::vector<juce::String> Fixture::hashes() const
{
    std::vector<juce::String> result; AudioImportControl control;
    for (const auto& file : originals) result.push_back(AudioImport::hashFile(file, control, AudioImportControl::Stage::verifying));
    return result;
}
float Fixture::sample(unsigned channel, Sample at) const { return static_cast<float>(pcm[channel][static_cast<std::size_t>(at)]) / 8388608.0f; }
StereoRender render(const RecorderProject& p, const std::vector<AudioSourceBinding>& bindings, const AudioSourceMask& mask, SampleRange range, unsigned block)
{
    const auto plan = RenderPlanCompiler::compile(p); TimelineAudioRenderer renderer(p.Fs, block); renderer.setPlan(plan, bindings, mask);
    StereoRender output; output.left.resize(static_cast<std::size_t>(range.length)); output.right.resize(output.left.size());
    for (Sample at = 0; at < range.length; at += block)
        renderer.renderAudio(*plan, {range.start + at, (std::min)(Sample(block), range.length - at)}, mask, output.left.data() + at, output.right.data() + at);
    return output;
}
}
