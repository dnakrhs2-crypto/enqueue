#include "ImportedAudioCache.h"
#include "audio/HighQualityResampler.h"
#include "model/SafeFileWrite.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gocue::recorder
{
namespace
{
using Stage = AudioImportControl::Stage;
void require(bool ok, const juce::String& reason) { if (!ok) throw std::runtime_error(reason.toStdString()); }
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); } // UTF-8 literal bytes stay intact
void checked(const juce::Result& r) { require(r.wasOk(), r.getErrorMessage()); }

class TrimmedSource final : public juce::AudioSource
{
public:
    TrimmedSource(std::unique_ptr<juce::AudioFormatReader> input, const ImportedAudioInfo& sourceInfo, AudioImportControl& c)
        : reader(std::move(input)), info(sourceInfo), control(c)
    {
        require(reader->sampleRate == info.sampleRate && reader->numChannels == static_cast<unsigned>(info.channels), "캐시 생성 중 원본 포맷이 바뀌었습니다.");
        reader->lengthInSamples = (std::max)(reader->lengthInSamples, info.readerStartSample + info.decodedSamples);
    }
    void prepareToPlay(int, double) override
    {
        position = 0;
        // Decode from zero, preserving MP3 reservoir/overlap when removing the leading delay.
        juce::AudioBuffer<float> discard(info.channels, 4096);
        for (Sample at = 0; at < info.readerStartSample;)
        {
            control.checkpoint(Stage::cache, 0);
            const int n = static_cast<int>((std::min)(Sample(4096), info.readerStartSample - at));
            require(reader->read(&discard, 0, n, at, true, info.channels == 2), "MP3 priming 읽기에 실패했습니다."); at += n;
        }
    }
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& output) override
    {
        control.checkpoint(Stage::cache, control.progress.load()); output.clearActiveBufferRegion();
        const int n = static_cast<int>((std::min)(Sample(output.numSamples), info.decodedSamples - position));
        if (n > 0)
        {
            require(reader->read(output.buffer, output.startSample, n, info.readerStartSample + position, true, info.channels == 2), "파생 캐시의 원본 오디오 읽기에 실패했습니다.");
            position += n;
        }
    }
private:
    std::unique_ptr<juce::AudioFormatReader> reader;
    const ImportedAudioInfo info;
    AudioImportControl& control;
    Sample position = 0;
};

void writePeaks(const CachedImportedAudio& cache)
{
    auto out = cache.peaksFile.createOutputStream(); require(out && out->openedOk(), "파형 캐시 파일을 쓸 수 없습니다.");
    out->writeInt(0x31504d49); out->writeInt(cache.channels); out->writeInt(cache.samplesPerPeak); out->writeInt64(cache.samples);
    for (const auto& p : cache.peaks) for (int ch = 0; ch < cache.channels; ++ch)
    { out->writeFloat(p.minimum[static_cast<size_t>(ch)]); out->writeFloat(p.maximum[static_cast<size_t>(ch)]); }
    out->flush(); checked(out->getStatus());
}
void readPeaks(CachedImportedAudio& cache)
{
    auto in = cache.peaksFile.createInputStream(); require(in && in->openedOk(), "파형 캐시가 없습니다.");
    const auto bins = (cache.samples - 1) / cache.samplesPerPeak + 1;
    require(in->getTotalLength() == 20 + bins * cache.channels * 8 && in->readInt() == 0x31504d49
        && in->readInt() == cache.channels && in->readInt() == cache.samplesPerPeak && in->readInt64() == cache.samples,
        "파형 캐시 길이 또는 포맷이 잘못되었습니다.");
    cache.peaks.resize(static_cast<size_t>(bins));
    for (auto& p : cache.peaks) for (int ch = 0; ch < cache.channels; ++ch)
    {
        auto& low = p.minimum[static_cast<size_t>(ch)]; auto& high = p.maximum[static_cast<size_t>(ch)];
        low = in->readFloat(); high = in->readFloat(); require(std::isfinite(low) && std::isfinite(high) && low <= high, "파형 캐시 샘플이 손상되었습니다.");
    }
    checked(in->getStatus());
}
void verifyPcm(const CachedImportedAudio& cache)
{
    auto r = AudioImport::openReader(cache.pcmFile);
    require(r->sampleRate == cache.sampleRate && r->numChannels == static_cast<unsigned>(cache.channels)
        && r->lengthInSamples == cache.samples && r->bitsPerSample == 32 && r->usesFloatingPointData,
        "파생 PCM 캐시의 포맷 또는 실제 길이가 다릅니다.");
}
bool reuse(const juce::File& directory, const juce::File& manifest, const ImportedAudioInfo& info,
           AudioImportControl& control, CachedImportedAudio& cache)
{
    try
    {
        const auto v = juce::JSON::parse(manifest);
        if (v["key"].toString() != cache.key || v["sourceHash"].toString() != info.contentHash
            || static_cast<int>(v["Fs"]) != static_cast<int>(cache.sampleRate)
            || static_cast<juce::int64>(v["samples"]) != cache.samples || static_cast<int>(v["channels"]) != cache.channels) return false;
        const auto generation = v["generation"].toString();
        require(generation.startsWith(cache.key + "-") && !generation.containsAnyOf("/\\:."), "잘못된 캐시 세대 경로입니다.");
        cache.pcmFile = directory.getChildFile(generation).getChildFile("audio.wav");
        cache.peaksFile = directory.getChildFile(generation).getChildFile("peaks.bin");
        require(AudioImport::hashFile(cache.pcmFile, control, Stage::cache) == v["pcmHash"].toString()
            && AudioImport::hashFile(cache.peaksFile, control, Stage::cache) == v["peaksHash"].toString(), "파생 캐시 hash가 다릅니다.");
        verifyPcm(cache); readPeaks(cache); cache.reused = true; return true;
    }
    catch (const std::exception&) { if (control.cancelled.load()) throw; return false; }
}
}
juce::String ImportedAudioCache::keyFor(const ImportedAudioInfo& info, std::uint32_t Fs)
{
    require(Fs > 0 && Fs <= 768000 && info.contentHash.length() == 64
        && info.contentHash.containsOnly("0123456789abcdef"), "원본 hash 또는 프로젝트 Fs가 잘못되었습니다.");
    return info.contentHash + "-" + juce::String(Fs) + "-v1-" + juce::String(info.readerStartSample) + "-" + juce::String(info.decodedSamples);
}
Sample ImportedAudioCache::sourceSampleFor(Sample projectSample, const ImportedAudioInfo& info, std::uint32_t Fs)
{
    require(projectSample >= 0, "원본 샘플 위치는 음수일 수 없습니다.");
    return rescaleRound(projectSample, info.sampleRate, Fs);
}
PeakSnapshot ImportedAudioCache::peakSnapshot(const CachedImportedAudio& cache)
{
    PeakSnapshot result; result.sampleRate = cache.sampleRate;
    result.channels = cache.channels == 2 ? 2u : 1u;
    result.samples = static_cast<std::uint64_t>(cache.samples); result.complete = true;
    const auto stride = (std::max)(size_t{1}, (cache.peaks.size() + PeakCache::maximumBins - 1) / PeakCache::maximumBins);
    result.samplesPerBin = std::uint64_t(cache.samplesPerPeak) * stride;
    for (size_t at = 0; at < cache.peaks.size(); at += stride)
    {
        std::array<PeakBin, 16> bin{};
        for (unsigned channel = 0; channel < result.channels; ++channel)
            bin[channel] = {cache.peaks[at].minimum[channel], cache.peaks[at].maximum[channel]};
        for (size_t i = at; i < (std::min)(at + stride, cache.peaks.size()); ++i)
            for (unsigned channel = 0; channel < result.channels; ++channel)
            {
                bin[channel].minimum = (std::min)(bin[channel].minimum, cache.peaks[i].minimum[channel]);
                bin[channel].maximum = (std::max)(bin[channel].maximum, cache.peaks[i].maximum[channel]);
            }
        result.bins.push_back(bin);
    }
    return result;
}
juce::Result ImportedAudioCache::build(const juce::File& projectDirectory, const MediaAsset& asset,
                                     const ImportedAudioInfo& info, std::uint32_t Fs,
                                     AudioImportControl& control, CachedImportedAudio& result)
{
    result = {};
    juce::File owned;
    bool published = false;
    const auto directory = projectDirectory.getChildFile("cache/imported-audio");
    try
    {
        control.checkpoint(Stage::cache, 0);
        require(asset.kind == AssetKind::importAudio && isProjectRelativePath(asset.relativePath)
            && asset.relativePath.startsWith("media/imports/") && info.contentHash == asset.contentIdentity
            && info.sampleRate == asset.originalFormat.sampleRate && info.channels == asset.originalFormat.channels
            && asset.logicalLength == rescaleRound(info.decodedSamples, asset.sourceUnitsDenominator, asset.sourceUnitsNumerator),
            "파생 캐시와 import 원본 정보가 다릅니다.");
        CachedImportedAudio cache; cache.key = keyFor(info, Fs); cache.sampleRate = Fs; cache.channels = info.channels;
        cache.samples = rescaleRound(info.decodedSamples, Fs, info.sampleRate);
        require(cache.samples > 0 && cache.channels >= 1 && cache.channels <= 2, "파생 캐시 길이/채널이 올바르지 않습니다.");
        const auto original = projectDirectory.getChildFile(asset.relativePath);
        require(AudioImport::hashFile(original, control, Stage::cache) == info.contentHash, "원본 오디오 hash가 바뀌었습니다. 다시 불러오세요.");
        const auto manifest = directory.getChildFile(cache.key + ".json");
        if (reuse(directory, manifest, info, control, cache))
        { control.checkpoint(Stage::ready, 1); result = std::move(cache); return juce::Result::ok(); }
        checked(directory.createDirectory());
        const auto generation = cache.key + "-" + newId();
        owned = directory.getChildFile(generation + ".partial");
        require(!owned.exists(), "파생 캐시 작업 경로가 이미 존재합니다."); checked(owned.createDirectory());
        cache.pcmFile = owned.getChildFile("audio.wav"); cache.peaksFile = owned.getChildFile("peaks.bin");
        cache.peaks.clear(); cache.reused = false;
        {
            TrimmedSource source(AudioImport::openReader(original), info, control);
            gocue::HighQualityResampler resampler(source, cache.channels, info.sampleRate);
            resampler.setDeviceRate(Fs); resampler.prepareToPlay(4096, Fs);
            std::unique_ptr<juce::OutputStream> stream = cache.pcmFile.createOutputStream();
            require(stream != nullptr, "파생 PCM 파일을 만들 수 없습니다.");
            auto* fileStream = static_cast<juce::FileOutputStream*>(stream.get());
            juce::WavAudioFormat format;
            auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(Fs)
                .withNumChannels(cache.channels).withBitsPerSample(32));
            require(writer != nullptr, "float32 WAV 캐시 writer를 만들 수 없습니다.");
            juce::AudioBuffer<float> block(cache.channels, 4096);
            ImportedPeak bin; int binSamples = 0;
            for (Sample at = 0; at < cache.samples;)
            {
                control.checkpoint(Stage::cache, static_cast<double>(at) / static_cast<double>(cache.samples));
                const int n = static_cast<int>((std::min)(Sample(4096), cache.samples - at));
                resampler.getNextAudioBlock({&block, 0, n});
                for (int s = 0; s < n; ++s)
                {
                    for (int ch = 0; ch < cache.channels; ++ch)
                    {
                        const float sample = block.getSample(ch, s); const auto c = static_cast<size_t>(ch);
                        require(std::isfinite(sample), "파생 PCM에 유효하지 않은 샘플이 있습니다.");
                        bin.minimum[c] = binSamples ? (std::min)(bin.minimum[c], sample) : sample;
                        bin.maximum[c] = binSamples ? (std::max)(bin.maximum[c], sample) : sample;
                    }
                    if (++binSamples == cache.samplesPerPeak) { cache.peaks.push_back(bin); binSamples = 0; }
                }
                require(writer->writeFromAudioSampleBuffer(block, 0, n), "파생 PCM 캐시 쓰기에 실패했습니다."); at += n;
            }
            if (binSamples) cache.peaks.push_back(bin);
            require(writer->flush(), "파생 PCM 캐시 flush에 실패했습니다."); checked(fileStream->getStatus());
            writer.reset(); resampler.releaseResources();
        }
        writePeaks(cache); verifyPcm(cache); readPeaks(cache);
        const auto pcmHash = AudioImport::hashFile(cache.pcmFile, control, Stage::cache);
        const auto peaksHash = AudioImport::hashFile(cache.peaksFile, control, Stage::cache);
        require(AudioImport::hashFile(original, control, Stage::cache) == info.contentHash, "캐시 생성 중 원본 hash가 바뀌었습니다.");
        control.checkpoint(Stage::cache, .99);
        const auto finalDirectory = directory.getChildFile(generation);
        require(MoveFileW(owned.getFullPathName().toWideCharPointer(), finalDirectory.getFullPathName().toWideCharPointer()) != 0,
                "완료한 파생 캐시의 이름을 확정할 수 없습니다.");
        owned = finalDirectory;
        cache.pcmFile = owned.getChildFile("audio.wav"); cache.peaksFile = owned.getChildFile("peaks.bin");
        auto* m = new juce::DynamicObject(); juce::var metadata(m);
        m->setProperty("key", cache.key); m->setProperty("generation", generation); m->setProperty("sourceHash", info.contentHash);
        m->setProperty("Fs", static_cast<int>(Fs)); m->setProperty("samples", juce::int64(cache.samples)); m->setProperty("channels", cache.channels);
        m->setProperty("pcmHash", pcmHash); m->setProperty("peaksHash", peaksHash);
        checked(gocue::SafeFileWrite::writeTextVerified(manifest, juce::JSON::toString(metadata),
            [&](const juce::String& text) { return juce::JSON::parse(text)["generation"].toString() == generation ? juce::Result::ok() : juce::Result::fail(juce::String::fromUTF8("캐시 manifest 검증 실패")); }));
        published = true; control.checkpoint(Stage::ready, 1); result = std::move(cache); return juce::Result::ok();
    }
    catch (const std::exception& e)
    {
        if (!published && owned != juce::File() && owned.isAChildOf(directory)) owned.deleteRecursively();
        control.stage.store(control.cancelled.load() ? Stage::cancelled : Stage::failed);
        return juce::Result::fail(juce::String::fromUTF8(e.what()));
    }
}
ImportedAudioCache::Worker::Worker(AudioImportRequest request, bool recording)
{
    control.recordingActive.store(recording);
    thread = std::thread([this, request = std::move(request)]
    {
        const bool background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
        if (!background) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        try
        {
            result = AudioImport::prepare(request, control, prepared);
            if (result.wasOk()) result = ImportedAudioCache::build(request.projectDirectory, prepared->asset(), prepared->info(), request.projectFs, control, cached);
            if (result.failed()) prepared.reset();
        }
        catch (const std::exception& e) { result = juce::Result::fail(juce::String::fromUTF8(e.what())); prepared.reset(); }
        if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        done.store(true, std::memory_order_release);
    });
}
ImportedAudioCache::Worker::~Worker() { cancel(); if (thread.joinable()) thread.join(); }
juce::Result ImportedAudioCache::Worker::takeResult(std::unique_ptr<PreparedAudioImport>& output, CachedImportedAudio& cache)
{
    if (!finished() || taken) return juce::Result::fail(juce::String::fromUTF8("오디오 작업이 아직 끝나지 않았거나 결과를 이미 가져왔습니다."));
    if (thread.joinable()) thread.join(); taken = true;
    if (control.cancelled.load()) { prepared.reset(); return juce::Result::fail(juce::String::fromUTF8("오디오 불러오기를 취소했습니다.")); }
    output = std::move(prepared); cache = std::move(cached); return result;
}
}
