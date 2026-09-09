#include "export/TimelineExporter.h"
#include "export/WavExportWriter.h"
#include "AudioRenderFixtures.h"
#include "TestSupport.h"
#include "model/ClipEdits.h"
#include "app/RecorderDocument.h"
#include <cmath>
#include <limits>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
void compareExample(const recorder_audio_fixture::Fixture& fixture, unsigned example, const ExportJob& job)
{
    const auto Fs = Sample(fixture.project.Fs);
    for (unsigned mic = 0; mic < 2; ++mic)
    {
        const auto file = job.outputDirectory.getChildFile(mic ? "mic02.wav" : "mic01.wav");
        const auto header = WavExportWriter::inspect(file); require(header.sampleCount == std::uint64_t(job.range.sampleCount), "Common WAV lengths");
        juce::WavAudioFormat format; std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(), true));
        require(reader && reader->numChannels == 1 && reader->bitsPerSample == 24 && reader->sampleRate == Fs, "Independent JUCE PCM24 reader");
        std::vector<float> pcm(static_cast<std::size_t>(job.range.sampleCount)); float* channel = pcm.data();
        require(reader->read(&channel, 1, 0, static_cast<int>(pcm.size())), "Read all exported PCM");
        for (Sample n = 0; n < job.range.sampleCount; ++n)
        {
            // Independent section-11.1 oracle: no compiler spans/fade metadata.
            bool gap = false; Sample source = n;
            if (example == 1 && mic == 1) gap = n >= 2 * Fs && n < 3 * Fs;
            if (example == 2 && mic == 1) { gap = n >= 9 * Fs; if (n >= 2 * Fs) source += Fs; }
            if (example == 3 && n >= 2 * Fs) source += Fs;
            bool fade = false;
            if (mic == 1 || example == 3)
                for (const auto boundary : {2 * Fs, (example == 1 ? 3 : 9) * Fs}) fade |= std::abs(n - boundary) <= 144;
            if (fade) continue;
            require(pcm[static_cast<std::size_t>(n)] == (gap ? 0.0f : fixture.sample(mic, source)), "Independent source PCM oracle outside microfade");
        }
    }
}
}
int runExportRangeTests()
{
    Suite s;
    s.test("outward common range, half-sample rational duration, overflow rejection", []
    {
        for (const auto Fs : {44100u, 48000u, 48001u}) for (const auto fps : {FrameRate{30,1}, FrameRate{60,1}, FrameRate{60000,1001}})
            for (Sample first = 0; first < 7000; first += 113)
        {
            const Sample length = 8123; const auto r = ExportRange::expand({first, length}, Fs, fps);
            require(r.startSample <= first && frameToSample(r.endFrame(), Fs, fps) >= first + length, "Expand both grid endpoints");
            const long double exact = static_cast<long double>(r.frameCount) * Fs * fps.denominator / fps.numerator;
            require(std::abs(exact - r.sampleCount) <= .500001L, "Round common duration once, <=0.5 sample");
            const auto again = ExportRange::expand({r.startSample, frameToSample(r.endFrame(), Fs, fps) - r.startSample}, Fs, fps);
            require(again.firstFrame == r.firstFrame && again.frameCount == r.frameCount, "Already aligned grid is stable");
        }
        rejects([] { ExportRange::expand({0,0}, 48000, {}); });
        rejects([] { ExportRange::expand({(std::numeric_limits<Sample>::max)() - 4, 9}, 48000, {}); });
        rejects([] { ExportRange::expand({0,1}, 0, {}); });
    });
    s.test("section 11.1 three independent edits, muted materials, common lengths and immutable originals", []
    {
        recorder_audio_fixture::Fixture f; const auto hashes = f.hashes(); ExportActivity activity; ExportControl control(activity);
        for (unsigned example = 1; example <= 3; ++example)
        {
            auto p = f.example(example); p.tracks[3].mute = true; p.tracks[2].solo = true;
            ExportJob job(p, f.root, f.root.getChildFile("example-" + juce::String(example)));
            const auto manifest = TimelineExporter::audioMaterials(job, control);
            require(job.range.sampleCount == Sample(example == 3 ? 9 : 10) * p.Fs, "Common duration follows all active clips");
            require(static_cast<juce::int64>(manifest["editRevision"]) == p.editRevision && manifest["files"].getArray()->size() == 2, "Fixed revision manifest");
            require(job.outputDirectory.getChildFile("export-manifest.json").existsAsFile() && !job.partialDirectory.exists(), "Whole job is atomically published");
            compareExample(f, example, job);
        }
        require(f.hashes() == hashes, "Original WAV SHA-256 unchanged");
    });
    s.test("snapshot survives later trims/mutes, explicit nonzero range rebases and pads end", []
    {
        recorder_audio_fixture::Fixture f; auto p = f.project;
        ExportJob job(p, f.root, {}, SampleRange{1701, 1901});
        const auto revision = p.editRevision; ++p.editRevision; p.tracks[2].mute = true; p.tracks[2].clips.edit().clear();
        require(job.plan().editRevision == revision && !job.snapshot.tracks[2].mute && !job.snapshot.tracks[2].clips.items().empty(), "Frozen plan and metadata");
        AudioSourceMask mask{AudioSourceMask::Kind::materialTrack, job.snapshot.tracks[2].trackId};
        ExportActivity gate; ExportControl control(gate); ExportAudioRenderer renderer(job, TimelineExporter::openSources(job, mask, control), mask);
        std::vector<float> l(static_cast<std::size_t>(job.range.sampleCount)), r(l.size()); renderer.render(0, static_cast<unsigned>(l.size()), l.data(), r.data());
        require(job.range.startSample == 1600 && job.range.sampleCount == 3200, "30fps outward selection grid");
        for (Sample i = 0; i < job.range.sampleCount; ++i)
            require(l[std::size_t(i)] == (i + job.range.startSample < 3602 ? f.sample(0, i + job.range.startSample) : 0.0f) && l[std::size_t(i)] == r[std::size_t(i)], "Rebased content and silent extension");
    });
    s.test("gap and one-sample movement retain shared realtime/offline microfades", []
    {
        recorder_audio_fixture::Fixture f; const auto moved = ClipEdits::move(f.project, {f.project.tracks[3].clips.items()[0].clipId}, 1, false);
        require(moved.status.wasOk(), "Move audio by one sample"); ExportJob job(moved.project, f.root);
        AudioSourceMask mask{AudioSourceMask::Kind::materialTrack, job.snapshot.tracks[3].trackId};
        ExportActivity gate; ExportControl control(gate); const auto bindings = TimelineExporter::openSources(job, mask, control);
        TimelineAudioRenderer realtime(job.snapshot.Fs, 4096); realtime.setPlan(job.audioPlan->timeline, bindings, mask);
        ExportAudioRenderer offline(job, bindings, mask); std::vector<float> a(1009), b(1009), l(1009), r(1009);
        for (Sample at = 0; at < job.range.sampleCount; at += 1009)
        {
            const auto n = static_cast<unsigned>((std::min)(Sample{1009}, job.range.sampleCount - at));
            realtime.renderAudio(at, n, a.data(), b.data()); offline.render(at, n, l.data(), r.data());
            require(std::equal(a.begin(), a.begin() + n, l.begin()) && std::equal(b.begin(), b.begin() + n, r.begin()), "Same PCM including every microfade and tail");
        }
    });
    s.test("record/document flags and atomic activity gate prevent overlap", []
    {
        recorder_audio_fixture::Fixture f; rejects([&] { ExportJob j(f.project, f.root, {}, {}, true); });
        RecorderDocument doc; doc.setRecordingStructureLock(true); rejects([&] { ExportJob::fromDocument(doc); });
        ExportActivity gate; require(gate.beginRecording(), "Acquire recording"); rejects([&] { ExportActivity::Lease l(gate); });
        gate.endRecording(); { ExportActivity::Lease l(gate); require(!gate.beginRecording(), "Export blocks recording start"); rejects([&] { ExportActivity::Lease second(gate); }); }
        require(gate.beginRecording(), "Release export gate"); gate.endRecording();
    });
    return s.result("ExportRangeTests");
}
