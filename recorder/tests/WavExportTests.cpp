#include "export/WavExportWriter.h"
#include "export/TimelineExporter.h"
#include "AudioRenderFixtures.h"
#include "TestSupport.h"
#include "support/Platform.h"
#include <limits>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
struct Fault final : FileIoFaultAdapter
{
    FileIoOperation operation; bool hit = false;
    explicit Fault(FileIoOperation op) : operation(op) {}
    juce::Result beforeIo(FileIoOperation op, const juce::File&, std::uint64_t, std::size_t) override
    { if (op == operation) { hit = true; return juce::Result::fail("injected export durable I/O error"); } return juce::Result::ok(); }
};
}
int runWavExportTests()
{
    Suite s;
    s.test("Stereo PCM24 RIFF/RF64 channel layout and interleaved microphone material", []
    {
        const auto huge = WavExportWriter::makeHeader(48000, 0x100000000ULL / 6 + 100, 2);
        const auto reread = WavExportWriter::readHeader(huge.bytes.data(), huge.bytes.size());
        require(reread.rf64 && reread.channels == 2 && reread.sampleCount == huge.sampleCount, "Stereo RF64 ds64 counts frames");
        recorder_audio_fixture::Fixture f(48000, true); ExportActivity gate; ExportControl control(gate);
        ExportJob job(f.project, f.root, {}, SampleRange{0, 4800}); const auto manifest = TimelineExporter::audioMaterials(job, control);
        require(manifest["files"].size() == 2, "One WAV per microphone slot");
        require(!job.outputDirectory.getChildFile("mic01-L.wav").exists() && !job.outputDirectory.getChildFile("mic01-R.wav").exists(), "Stereo slot is one material");
        const auto file = job.outputDirectory.getChildFile("mic01.wav"); require(WavExportWriter::inspect(file).channels == 2, "Interleaved stereo material");
        require(WavExportWriter::inspect(job.outputDirectory.getChildFile("mic02.wav")).channels == 1, "Mono material retained");
        juce::WavAudioFormat format; std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(), true));
        std::vector<float> l(4800), r(4800); float* dst[]{l.data(),r.data()}; require(reader && reader->read(dst, 2, 0, 4800), "Read exported stereo PCM");
        for (int i = 200; i < 4600; ++i)
            require(l[i] == f.sample(0,i) && r[i] == float(-f.pcm[0][i] / 2) / 8388608.0f, "Export L/R exact PCM oracle");
    });
    s.test("RF64 ds64 above 4GiB and independent JUCE header reread without fake payload", []
    {
        const std::uint64_t riffLimit = (0xffffffffULL - 44) / 3;
        for (const auto samples : {std::uint64_t{1}, riffLimit - 2, riffLimit, riffLimit + 2, 0x100000000ULL / 3 + 50, 48000ULL * 60 * 60 * 30})
        {
            const auto header = WavExportWriter::makeHeader(48000, samples), parsed = WavExportWriter::readHeader(header.bytes.data(), header.bytes.size());
            require(parsed.sampleCount == samples && header.dataBytes == samples * 3 && header.fileBytes % 2 == 0, "RF64 lengths and even physical chunk padding");
            require(header.rf64 == (samples * 3 + (samples & 1) + 44 > 0xffffffffULL), "Automatic RIFF/RF64 boundary");
            juce::WavAudioFormat wav; auto memory = new juce::MemoryInputStream(header.bytes.data(), header.bytes.size(), true);
            std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(memory, true));
            require(reader && reader->lengthInSamples == static_cast<Sample>(samples) && reader->bitsPerSample == 24 && reader->numChannels == 1, "JUCE re-reads virtual length header; no payload claim");
        }
        auto invalid = WavExportWriter::makeHeader(48000, 0x100000000ULL); invalid.bytes[36] ^= 1;
        rejects([&] { WavExportWriter::readHeader(invalid.bytes.data(), invalid.bytes.size()); });
        rejects([] { WavExportWriter::makeHeader(48000, (std::numeric_limits<std::uint64_t>::max)()); });
    });
    s.test("PCM24 extrema, odd RIFF padding, durable finish then no-replace publish", []
    {
        recorder_audio_fixture::Fixture f; const auto path = f.root.getChildFile("odd.wav");
        const float pcm[]{-1.0f, 0, 8388607.0f / 8388608.0f, .5f, -.5f}; WavExportWriter w(path, 44100, 5);
        w.append(pcm, 5); require(!path.exists(), "No completed file before verification"); w.finish();
        require(WavExportWriter::inspect(w.partialFile()).fileBytes == 60, "Odd payload has one pad byte"); w.publish();
        require(path.exists() && !w.partialFile().exists(), "Atomic standalone publish");
        juce::WavAudioFormat fmt; std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(path.createInputStream().release(), true));
        float got[5]{}; float* dst = got; require(reader && reader->read(&dst, 1, 0, 5), "Independent exported PCM reader");
        for (int i = 0; i < 5; ++i) require(got[i] == pcm[i], "Exact PCM24 signed endpoints");
        rejects([&] { WavExportWriter duplicate(path, 44100, 5); });
    });
    s.test("durable open/write/header/flush failures propagate, no completed WAV", []
    {
        recorder_audio_fixture::Fixture f;
        for (const auto op : {FileIoOperation::open, FileIoOperation::append, FileIoOperation::patch, FileIoOperation::flushData})
        {
            Fault fault(op); const auto path = f.root.getChildFile(newId() + ".wav");
            rejects([&] { WavExportWriter w(path, 48000, 1, &fault); const float sample = .25f; w.append(&sample, 1); w.finish(); w.publish(); });
            require(fault.hit && !path.exists() && !path.getSiblingFile(path.getFileName() + ".partial").exists(), "Failed output was not published");
        }
    });
    s.test("short writes cannot finish; existing partial and completed files survive cancellation", []
    {
        recorder_audio_fixture::Fixture f; const auto path = f.root.getChildFile("short.wav");
        { WavExportWriter w(path, 48000, 2); float sample = 0; w.append(&sample, 1); rejects([&] { w.finish(); }); w.cancel(); }
        require(!path.exists() && !path.getSiblingFile("short.wav.partial").exists(), "Owned incomplete partial removed");
        const auto other = f.root.getChildFile("occupied.wav.partial"); require(other.replaceWithText("keep"), "Create preexisting partial");
        rejects([&] { WavExportWriter w(f.root.getChildFile("occupied.wav"), 48000, 1); }); require(other.loadFileAsString() == "keep", "Foreign partial untouched");
    });
    s.test("multi-file cancel and manifest-flush failure preserve originals and previous completed exports", []
    {
        recorder_audio_fixture::Fixture f; const auto hashes = f.hashes(); const auto completed = f.root.getChildFile("completed");
        require(completed.createDirectory().wasOk() && completed.getChildFile("final.mp4").replaceWithText("keep completed"), "Completed output sentinel");
        ExportActivity gate; ExportControl control(gate); ExportJob job(f.project, f.root, completed);
        control.onProgress = [&](const ExportProgress&) { control.cancelled = true; };
        rejects([&] { TimelineExporter::audioMaterials(job, control); });
        require(!job.partialDirectory.exists() && !job.outputDirectory.exists() && completed.getChildFile("final.mp4").loadFileAsString() == "keep completed", "Cancel removes only owned partial job");
        control.cancelled = false; control.onProgress = {};
        struct ManifestFault final : FileIoFaultAdapter
        {
            bool hit = false;
            juce::Result beforeIo(FileIoOperation op, const juce::File& p, std::uint64_t, std::size_t) override
            { if (op == FileIoOperation::flushData && p.getFileName() == "export-manifest.json.partial") { hit = true; return juce::Result::fail("manifest flush"); } return juce::Result::ok(); }
        } fault;
        ExportJob second(f.project, f.root); rejects([&] { TimelineExporter::audioMaterials(second, control, false, &fault); });
        require(fault.hit && !second.outputDirectory.exists() && !second.partialDirectory.exists(), "Manifest failure prevents the entire publish");
        require(f.hashes() == hashes, "All original hashes remain unchanged");
    });
    return s.result("WavExportTests");
}
