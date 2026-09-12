#include "TestSupport.h"
#include "media/AudioImport.h"
#include "playback/ImportedAudioCache.h"
#include "storage/DurableFile.h"
#include <cmath>
#include <chrono>
#include <thread>

using namespace gocue::recorder;
using namespace recorder_test;

namespace recorder_import_test
{
float signal(Sample at, std::uint32_t rate, int channel)
{
    const double t = static_cast<double>(at) / rate;
    return static_cast<float>(.28 * std::sin(6.283185307179586 * ((211 + channel * 157) * t + 271 * t * t))
        + .11 * std::sin(6.283185307179586 * (997 + channel * 263) * t));
}
juce::File writeWav(const juce::File& directory, std::uint32_t rate, int channels, Sample count)
{
    require(directory.createDirectory().wasOk(), "fixture directory");
    auto file = directory.getChildFile(newId() + ".wav");
    std::unique_ptr<juce::OutputStream> out = file.createOutputStream();
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor(out, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(channels).withBitsPerSample(32));
    require(writer != nullptr, "fixture writer");
    juce::AudioBuffer<float> block(channels, 4096);
    for (Sample at = 0; at < count;)
    {
        const int n = static_cast<int>((std::min)(Sample(4096), count - at));
        for (int ch = 0; ch < channels; ++ch) for (int i = 0; i < n; ++i) block.setSample(ch, i, signal(at + i, rate, ch));
        require(writer->writeFromAudioSampleBuffer(block, 0, n), "fixture write"); at += n;
    }
    require(writer->flush(), "fixture flush"); writer.reset(); return file;
}
bool encode(const juce::File& source, const juce::File& target, const juce::String& codec, int rate, juce::String& log, const juce::StringArray& extra = {})
{
    juce::StringArray args{RECORDER_IMPORT_FFMPEG_EXE, "-hide_banner", "-loglevel", "error", "-nostdin", "-n", "-i", source.getFullPathName(), "-map", "0:a:0", "-c:a", codec, "-b:a", "192k"};
    if (target.hasFileExtension("m4a")) args.addArray({"-movie_timescale", juce::String(rate)});
    args.addArray(extra);
    args.add(target.getFullPathName()); juce::ChildProcess process;
    if (!process.start(args)) { log = "fixture encoder unavailable"; return false; }
    if (!process.waitForProcessToFinish(20000)) { process.kill(); log = "fixture encoder timeout"; return false; }
    log = process.readAllProcessOutput(); return process.getExitCode() == 0 && target.existsAsFile();
}
}
namespace
{
using namespace recorder_import_test;
struct Workspace
{
    juce::File directory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-import-test-" + newId());
    ~Workspace() { directory.deleteRecursively(); }
    AudioImportRequest request(RecorderDocument& doc, std::uint32_t rate = 48000, int channels = 2)
    {
        AudioImportRequest r; r.projectDirectory = directory.getChildFile("project");
        r.projectId = doc.getProject().projectId; r.projectFs = doc.getProject().Fs;
        r.source = writeWav(directory.getChildFile("fixtures"), rate, channels, Sample(rate) * 2 + 137); return r;
    }
};
int assetDirectories(const AudioImportRequest& request)
{ return request.projectDirectory.getChildFile("media/imports").findChildFiles(juce::File::findDirectories, false).size(); }
struct CopyFault : FileIoFaultAdapter
{
    AudioImportControl* cancel = nullptr;
    FileIoOperation failAt = FileIoOperation::append;
    juce::Result beforeIo(FileIoOperation op, const juce::File&, std::uint64_t, size_t) override
    {
        if (op != failAt) return juce::Result::ok();
        if (cancel) { cancel->cancelled.store(true); return juce::Result::ok(); }
        return juce::Result::fail("injected copy/disk flush failure");
    }
};
void requireOk(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
void wait(ImportedAudioCache::Worker& w)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (!w.finished() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(w.finished(), "worker timeout");
}
}
int runAudioImportTests()
{
    Suite suite;
    suite.test("imported stereo timeline peaks preserve separate L/R extrema and bound long-file bins", []
    {
        CachedImportedAudio cache; cache.channels = 2; cache.sampleRate = 48000; cache.samplesPerPeak = 256;
        cache.peaks.resize(PeakCache::maximumBins * 2 + 1); cache.samples = Sample(cache.peaks.size()) * 256 - 17;
        cache.peaks[0].minimum[0] = -.25f; cache.peaks[1].maximum[0] = .125f;
        cache.peaks.back().minimum[1] = -.75f; cache.peaks.back().maximum[1] = .5f;
        const auto peaks = ImportedAudioCache::peakSnapshot(cache);
        require(peaks.complete && peaks.sampleRate == 48000 && peaks.samples == cache.samples
            && peaks.bins.size() <= PeakCache::maximumBins, "Bounded waveform lost duration");
        require(peaks.channels == 2 && peaks.bins.front()[0].minimum == -.25f && peaks.bins.front()[0].maximum == .125f
            && peaks.bins.front()[1].minimum == 0 && peaks.bins.front()[1].maximum == 0, "Left envelope mixed into right channel");
        require(peaks.bins.back()[1].minimum == -.75f && peaks.bins.back()[1].maximum == .5f
            && peaks.bins.back()[0].minimum == 0 && peaks.bins.back()[0].maximum == 0,
            "Right-only tail waveform disappeared or mixed into left channel");
        require(peaks.bins.size() * peaks.samplesPerBin >= peaks.samples, "Waveform tail coverage");
    });
    suite.test("mono and empty imported peak snapshots remain compatible", []
    {
        CachedImportedAudio cache; cache.channels = 1; cache.sampleRate = 48000; cache.samples = 256;
        cache.peaks.resize(1); cache.peaks[0].minimum = {-.4f, -.9f}; cache.peaks[0].maximum = {.3f, .8f};
        const auto mono = ImportedAudioCache::peakSnapshot(cache);
        require(mono.channels == 1 && mono.bins[0][0].minimum == -.4f && mono.bins[0][0].maximum == .3f,
            "Legacy mono summary used an absent channel");
        cache.peaks.clear(); cache.samples = 0;
        require(ImportedAudioCache::peakSnapshot(cache).bins.empty(), "Empty peak cache created data");
    });
    suite.test("copy, reopen, SHA-256 and unpublished asset ownership", []
    {
        Workspace w; RecorderDocument doc; auto r = w.request(doc); AudioImportControl c;
        std::unique_ptr<PreparedAudioImport> p; requireOk(AudioImport::prepare(r, c, p));
        require(doc.getProject().media->assets.empty(), "prepare must not register");
        require(p->originalFile().getFileName() == r.source.getFileName(), "original name");
        require(p->info().contentHash == AudioImport::hashFile(r.source, c), "source copy hash");
        require(p->info().decodedSamples == 96137 && p->info().leadingSkipSamples == 0, "PCM sample count");
        const auto copied = p->originalFile(); p.reset(); require(!copied.exists() && assetDirectories(r) == 0, "uncommitted rollback");
        require(r.source.existsAsFile(), "external original survives");
    });
    suite.test("copy write/flush failures and cancellations leave no half asset", []
    {
        for (int mode = 0; mode < 4; ++mode)
        {
            Workspace w; RecorderDocument doc; auto r = w.request(doc); AudioImportControl c; CopyFault fault;
            fault.failAt = mode % 2 ? FileIoOperation::flushData : FileIoOperation::append;
            if (mode >= 2) fault.cancel = &c;
            r.copyFaults = &fault; std::unique_ptr<PreparedAudioImport> p;
            require(AudioImport::prepare(r, c, p).failed(), "injected failure");
            require(!p && assetDirectories(r) == 0 && doc.getProject().media->assets.empty(), "atomic failure");
        }
        Workspace w; RecorderDocument doc; auto r = w.request(doc); AudioImportControl c; c.cancelled.store(true);
        std::unique_ptr<PreparedAudioImport> p; require(AudioImport::prepare(r, c, p).failed() && assetDirectories(r) == 0, "pre-cancel");
    });
    suite.test("missing, codec, 3-channel and truncated files give errors with rollback", []
    {
        for (int mode = 0; mode < 5; ++mode)
        {
            Workspace w; RecorderDocument doc; auto r = w.request(doc, 48000, mode == 2 ? 3 : 1);
            if (mode == 0) r.source = r.source.getSiblingFile("missing.wav");
            if (mode == 1) { r.source = r.source.getSiblingFile("unsupported.flac"); r.source.replaceWithText("invalid codec"); }
            if (mode == 3) { juce::MemoryBlock bytes; r.source.loadFileAsData(bytes); r.source.replaceWithData(bytes.getData(), bytes.getSize() - 128); }
            if (mode == 4) { r.source = r.source.getSiblingFile("broken.m4a"); r.source.replaceWithText("broken or protected codec"); }
            AudioImportControl c; std::unique_ptr<PreparedAudioImport> p; const auto result = AudioImport::prepare(r, c, p);
            require(result.failed() && result.getErrorMessage().isNotEmpty(), "reason required");
            if (mode == 2) require(result.getErrorMessage().contains("1~2"), "explicit channel limit");
            require(!p && assetDirectories(r) == 0, "invalid codec rollback");
        }
    });
    suite.test("44.1/48/96k lengths, sample mapping, float32 cache, peaks and immutable source", []
    {
        for (const auto rate : {44100u, 48000u, 96000u}) for (int channels : {1, 2})
        {
            Workspace w; RecorderDocument doc; auto r = w.request(doc, rate, channels); AudioImportControl c;
            std::unique_ptr<PreparedAudioImport> p; requireOk(AudioImport::prepare(r, c, p));
            CachedImportedAudio cache; requireOk(ImportedAudioCache::build(r.projectDirectory, p->asset(), p->info(), 48000, c, cache));
            const auto expected = rescaleRound(Sample(rate) * 2 + 137, 48000, rate);
            require(cache.samples == expected && p->asset().logicalLength == expected && cache.channels == channels, "length mapping");
            require(cache.peaks.size() == static_cast<size_t>((expected + 255) / 256), "peak coverage");
            const auto snapshot = ImportedAudioCache::peakSnapshot(cache);
            require(snapshot.channels == unsigned(channels), "Decoded cache lost its channel count at the timeline adapter");
            for (int ch = 0; ch < channels; ++ch)
                require(snapshot.bins[1][ch].minimum == cache.peaks[1].minimum[ch]
                    && snapshot.bins[1][ch].maximum == cache.peaks[1].maximum[ch], "Decoded channel envelope changed");
            auto pcm = AudioImport::openReader(cache.pcmFile); juce::AudioBuffer<float> samples(channels, static_cast<int>(expected));
            require(pcm->read(&samples, 0, static_cast<int>(expected), 0, true, channels == 2), "cache read");
            double error = 0; Sample measured = 0;
            for (int ch = 0; ch < channels; ++ch) for (int at = 1024; at < expected - 1024; at += 31)
            { error += std::pow(samples.getSample(ch, at) - signal(at, 48000, ch), 2); ++measured; }
            require(std::sqrt(error / measured) < .0001, "resampler absolute sample alignment");
            for (Sample at : {Sample(0), Sample(17), Sample(48001), expected})
                require(ImportedAudioCache::sourceSampleFor(at, p->info(), 48000) == rescaleRound(at, rate, 48000), "rational source map");
            require(AudioImport::hashFile(r.source, c) == p->info().contentHash && AudioImport::hashFile(p->originalFile(), c) == p->info().contentHash, "original remains byte-identical");
            require(doc.getProject().Fs == 48000, "import must not change project/ASIO Fs");
        }
    });
    suite.test("cache reuses verified bytes and invalidates for corruption, hash and Fs", []
    {
        Workspace w; RecorderDocument doc; auto r = w.request(doc); AudioImportControl c;
        std::unique_ptr<PreparedAudioImport> p; requireOk(AudioImport::prepare(r, c, p)); CachedImportedAudio a, b;
        requireOk(ImportedAudioCache::build(r.projectDirectory, p->asset(), p->info(), 48000, c, a));
        requireOk(ImportedAudioCache::build(r.projectDirectory, p->asset(), p->info(), 48000, c, b)); require(b.reused && a.pcmFile == b.pcmFile, "reuse");
        const auto reusedPeaks = ImportedAudioCache::peakSnapshot(b);
        require(reusedPeaks.channels == 2 && reusedPeaks.bins[1][1].minimum == a.peaks[1].minimum[1]
            && reusedPeaks.bins[1][1].maximum == a.peaks[1].maximum[1], "Reused v1 cache lost right channel");
        requireOk(ImportedAudioCache::build(r.projectDirectory, p->asset(), p->info(), 44100, c, b)); require(!b.reused && a.key != b.key, "Fs invalidation");
        a.pcmFile.replaceWithText("corrupted cache");
        requireOk(ImportedAudioCache::build(r.projectDirectory, p->asset(), p->info(), 48000, c, b)); require(!b.reused && a.pcmFile != b.pcmFile, "corrupt cache rebuilt");
        p->originalFile().replaceWithText("externally modified source");
        require(ImportedAudioCache::build(r.projectDirectory, p->asset(), p->info(), 48000, c, b).failed(), "original hash rejection");
    });
    suite.test("worker yields during recording, cancellation and cache failure clean the import", []
    {
        Workspace w; RecorderDocument doc; auto r = w.request(doc);
        ImportedAudioCache::Worker paused(r, true);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (paused.control.stage.load() != AudioImportControl::Stage::pausedForRecording && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(!paused.finished() && assetDirectories(r) == 0, "recording takes priority");
        paused.cancel(); wait(paused); std::unique_ptr<PreparedAudioImport> p; CachedImportedAudio cache;
        require(paused.takeResult(p, cache).failed() && !p && assetDirectories(r) == 0, "paused cancellation");
        require(r.projectDirectory.getChildFile("cache").createDirectory().wasOk(), "cache fixture folder");
        require(r.projectDirectory.getChildFile("cache/imported-audio").replaceWithText("obstruction"), "cache obstruction");
        ImportedAudioCache::Worker broken(r); wait(broken);
        require(broken.takeResult(p, cache).failed() && !p && assetDirectories(r) == 0, "cache failure rolls copied source back");
    });
    suite.test("cancellation during verification and PCM generation removes partial files", []
    {
        using Stage = AudioImportControl::Stage;
        for (const auto cancelledStage : {Stage::verifying, Stage::cache})
        {
            Workspace w; RecorderDocument doc; auto r = w.request(doc); AudioImportControl c;
            c.onProgress = [&](Stage stage, double progress) { if (stage == cancelledStage && progress >= .2) c.cancelled.store(true); };
            std::unique_ptr<PreparedAudioImport> p; auto result = AudioImport::prepare(r, c, p);
            if (result.wasOk())
            {
                CachedImportedAudio cache; result = ImportedAudioCache::build(r.projectDirectory, p->asset(), p->info(), 48000, c, cache);
                require(cache.pcmFile == juce::File(), "cancelled PCM not published");
            }
            require(result.failed() && c.cancelled.load(), "cancelled at requested phase"); p.reset();
            require(assetDirectories(r) == 0 && doc.getProject().media->assets.empty(), "cancelled asset removed");
            require(r.projectDirectory.getChildFile("cache/imported-audio").findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty(), "partial cache removed");
        }
    });
    suite.test("recording pause resumes a complete worker job", []
    {
        Workspace w; RecorderDocument doc; auto r = w.request(doc); ImportedAudioCache::Worker worker(r, true);
        worker.setRecordingActive(false); wait(worker); std::unique_ptr<PreparedAudioImport> p; CachedImportedAudio cache;
        requireOk(worker.takeResult(p, cache)); require(p && cache.pcmFile.existsAsFile(), "resumed job ready");
        requireOk(commitImportedAudio(doc, *p, worker.control)); require(doc.getProject().validate().wasOk(), "resumed commit");
    });
    suite.test("mono M4A and raw AAC/MP3 without gapless tags retain explicit boundary evidence", []
    {
        for (const juce::String extension : {"m4a", "aac", "mp3"})
        {
            Workspace w; RecorderDocument doc; auto r = w.request(doc, 44100, 1); const auto target = r.source.withFileExtension(extension); juce::String log;
            require(encode(r.source, target, extension == "mp3" ? "libmp3lame" : "aac", 44100, log,
                extension == "mp3" ? juce::StringArray{"-write_xing", "0"} : juce::StringArray{}), log.toRawUTF8());
            r.source = target; AudioImportControl c; std::unique_ptr<PreparedAudioImport> p; requireOk(AudioImport::prepare(r, c, p));
            require(p->info().channels == 1 && p->info().decodedSamples > 0 && p->info().readerDecodedSamples >= p->info().decodedSamples, "actual mono decode length");
            if (extension == "m4a") require(p->info().primingKnown && p->info().decodedSamples == 88337, "mono AAC gapless length");
            else require(!p->info().primingKnown && p->info().primingEvidence.contains("unknown"), "missing encoder metadata is not guessed");
            CachedImportedAudio cache; requireOk(ImportedAudioCache::build(r.projectDirectory, p->asset(), p->info(), 48000, c, cache));
            require(cache.channels == 1 && cache.samples == rescaleRound(p->info().decodedSamples, 48000, 44100), "mono cache mapping");
        }
    });
    suite.test("MP3/M4A priming, final padding, source alignment and seek", []
    {
        for (const juce::String extension : {"mp3", "m4a"})
        {
            Workspace w; RecorderDocument doc; auto r = w.request(doc); auto encoded = r.source.withFileExtension(extension); juce::String log;
            require(encode(r.source, encoded, extension == "mp3" ? "libmp3lame" : "aac", 48000, log), log.toRawUTF8());
            r.source = encoded; AudioImportControl c; std::unique_ptr<PreparedAudioImport> p;
            requireOk(AudioImport::prepare(r, c, p));
            const auto& info = p->info();
            require(info.primingKnown && info.leadingSkipSamples > 0 && info.trailingSkipSamples > 0 && info.decodedSamples == 96137, "gapless front/back count");
            require(info.rawDecodedSamples - info.leadingSkipSamples - info.trailingSkipSamples == info.decodedSamples, "raw-to-presentation sample count");
            CachedImportedAudio cache; requireOk(ImportedAudioCache::build(r.projectDirectory, p->asset(), info, 48000, c, cache));
            auto pcm = AudioImport::openReader(cache.pcmFile); juce::AudioBuffer<float> sequential(2, 96137);
            require(pcm->read(&sequential, 0, 96137, 0, true, true), "decoded cache read");
            double error = 0; int n = 0;
            for (int ch = 0; ch < 2; ++ch) for (int at = 128; at < 96000; at += 13)
            { error += std::pow(sequential.getSample(ch, at) - signal(at, 48000, ch), 2); ++n; }
            const double rms = std::sqrt(error / n);
            std::cout << "  " << extension << " raw=" << info.rawDecodedSamples << " lead=" << info.leadingSkipSamples << " tail=" << info.trailingSkipSamples << " rms=" << rms << '\n';
            require(rms < .025, "compressed source alignment");
            auto sourceReader = AudioImport::openReader(p->originalFile()); juce::AudioBuffer<float> block(2, 257);
            for (Sample at : {Sample(45007), Sample(2049), Sample(95880), Sample(17)})
            {
                require(sourceReader->read(&block, 0, 257, info.readerStartSample + at, true, true), "source random seek");
                double maxError = 0;
                for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < 257; ++s)
                    maxError = (std::max)(maxError, std::abs(double(block.getSample(ch, s) - sequential.getSample(ch, static_cast<int>(at) + s))));
                require(maxError < .002, "source seek preroll matches sequential PCM");
            }
        }
    });
    return suite.result("AudioImportTests");
}
