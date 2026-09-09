#include "AudioRenderFixtures.h"
#include "TestSupport.h"
#include "audio/MicroFade.h"
#include "model/ClipEdits.h"
#include <algorithm>
#include <cmath>
#include <limits>

using namespace gocue::recorder;
using namespace recorder_test;
using namespace recorder_audio_fixture;
namespace
{
using K = AudioSourceMask::Kind;
struct Run { Sample start, length, source; };
// Independent golden oracle: hand-written source runs for each spec example.
// It does not inspect the compiled spans or ask MicroFade for its envelope.
void oracle(const Fixture& f, unsigned channel, const std::vector<Run>& runs, const StereoRender& out)
{
    for (Sample i = 0; i < static_cast<Sample>(out.left.size()); ++i)
    {
        float expected = 0;
        for (const auto& run : runs) if (i >= run.start && i < run.start + run.length)
        {
            expected = f.sample(channel, run.source + i - run.start);
            const auto fade = (std::min)((Sample(f.project.Fs) * 3 + 500) / 1000, run.length / 2);
            const auto from = i - run.start, to = run.start + run.length - 1 - i;
            if (fade > 0 && (from < fade || to < fade))
                expected *= fade == 1 ? 0.0f : static_cast<float>((std::min)(from, to)) / static_cast<float>(fade - 1);
            break;
        }
        require(out.left[static_cast<std::size_t>(i)] == expected && out.right[static_cast<std::size_t>(i)] == expected,
                "PCM differs from independent oracle (including exact PCM outside fades)");
    }
}
AudioSourceMask material(const RecorderProject& p, unsigned lane) { return {K::materialTrack, p.tracks[lane].trackId, {}}; }
std::vector<AudioSourceBinding> sources(const Fixture& f) { return openAudioSources(*compileAudioRenderPlan(f.project), f.root); }
RecorderProject checked(ClipEditResult edit) { require(edit.status.wasOk(), edit.status.getErrorMessage().toRawUTF8()); return std::move(edit.project); }
}
int runAudioCutRenderTests()
{
    Suite suite;
    suite.test("section 11.1 example 1: independent mic cut leaves 10s and a silent second", []
    {
        Fixture f; const auto before = f.hashes(); const auto p = f.example(1); const auto bindings = sources(f); const auto fs = Sample(p.Fs);
        const auto plan = RenderPlanCompiler::compile(p); require(plan->timelineEnd == 10 * fs, "Common 10s range");
        for (unsigned lane = 0; lane < 2; ++lane) require(p.tracks[lane].clips.items().size() == 1 && p.tracks[lane].clips.items()[0].lengthSamples == fs * 10, "Camera cuts were changed by independent audio edit");
        oracle(f, 0, {{0, 10 * fs, 0}}, render(p, bindings, material(p, 2), {0, plan->timelineEnd}, 509));
        oracle(f, 1, {{0, 2 * fs, 0}, {3 * fs, 7 * fs, 3 * fs}}, render(p, bindings, material(p, 3), {0, plan->timelineEnd}, 997));
        require(f.hashes() == before, "Original WAV SHA-256 changed");
    });
    suite.test("section 11.1 example 2: mic-only ripple retains common 10s with 1s silent tail", []
    {
        Fixture f; const auto before = f.hashes(); const auto p = f.example(2); const auto bindings = sources(f); const auto fs = Sample(p.Fs);
        require(p.activeTimelineEnd() == 10 * fs, "Independent ripple shortened the common range");
        oracle(f, 1, {{0, 2 * fs, 0}, {2 * fs, 7 * fs, 3 * fs}}, render(p, bindings, material(p, 3), {0, 10 * fs}, 401));
        oracle(f, 0, {{0, 10 * fs, 0}}, render(p, bindings, material(p, 2), {0, 10 * fs}));
        require(f.hashes() == before, "Ripple modified original chunks");
    });
    suite.test("section 11.1 example 3: global ripple makes every lane 9s with matching source cuts", []
    {
        Fixture f; const auto p = f.example(3); const auto bindings = sources(f); const auto fs = Sample(p.Fs);
        require(p.activeTimelineEnd() == 9 * fs, "Global ripple length");
        for (unsigned mic = 0; mic < 2; ++mic) oracle(f, mic, {{0, 2 * fs, 0}, {2 * fs, 7 * fs, 3 * fs}}, render(p, bindings, material(p, mic + 2), {0, 9 * fs}, 257));
        for (unsigned camera = 0; camera < 2; ++camera)
        {
            const auto& clips = p.tracks[camera].clips.items(); require(clips.size() == 2, "Camera global cut count");
            require(clips[1].timelineStartSample == 2 * fs && clips[1].sourceIn == 3 * fs && clips[1].timelineEnd() == 9 * fs, "Camera source mapping");
        }
    });
    suite.test("source-continuous splits at 1 sample and inside end fades are bit-identical", []
    {
        Fixture f; const auto bindings = sources(f); const auto length = f.project.activeTimelineEnd();
        const auto baseline = render(f.project, bindings, material(f.project, 3), {0, length}, 4096);
        for (const auto splitAt : {Sample{1}, Sample{71}, Sample{131071}, length - 1})
        {
            const auto p = checked(ClipEdits::split(f.project, {f.project.tracks[3].clips.items()[0].clipId}, splitAt));
            const auto output = render(p, bindings, material(p, 3), {0, length}, 317);
            require(output.left == baseline.left && output.right == baseline.right, "No-op split changed PCM or fade half-length cap");
        }
    });
    suite.test("one-sample independent audio move is not rounded to a video frame", []
    {
        Fixture f; const auto p = checked(ClipEdits::move(f.project, {f.project.tracks[3].clips.items()[0].clipId}, 1));
        oracle(f, 1, {{1, Sample(p.Fs) * 10, 0}}, render(p, sources(f), material(p, 3), {0, p.activeTimelineEnd()}, 239));
        require(p.tracks[0].clips.items()[0].timelineStartSample == 0 && p.activeTimelineEnd() == Sample(p.Fs) * 10 + 1, "Audio move changed camera or common tail");
    });
    suite.test("fixed K includes silent/empty lanes, muted solo selects silence, video solo is irrelevant", []
    {
        Fixture f; auto p = f.example(1); const auto bindings = sources(f); const Sample at = Sample(p.Fs) * 2 + 1000;
        auto output = render(p, bindings, {}, {at, 1}); require(output.left[0] == f.sample(0, at) * .5f, "Gap changed fixed track weight");
        p.tracks[3].clips.edit().clear(); p.tracks[0].solo = true;
        output = render(p, bindings, {}, {5000, 1}); require(output.left[0] == f.sample(0, 5000) * .5f, "Empty lane/video solo changed K");
        p.tracks[3].solo = p.tracks[3].mute = true;
        output = render(p, bindings, {}, {5000, 1}); require(output.left[0] == 0 && output.right[0] == 0, "Muted solo must yield K=0");
        p.tracks[3].solo = false; p.tracks[2].mute = true;
        require(render(p, bindings, {}, {5000, 1}).left[0] == 0, "All-muted mix must be silent");
    });
    suite.test("individual microphone honors only its own mute; material ignores all mute/solo", []
    {
        Fixture f; auto p = f.project; const auto bindings = sources(f); p.tracks[3].solo = true;
        const AudioSourceMask individual{K::microphone, p.tracks[2].trackId, {}};
        require(render(p, bindings, individual, {6000, 1}).left[0] == f.sample(0, 6000), "Unrelated solo muted individual mic");
        p.tracks[2].mute = true;
        require(render(p, bindings, individual, {6000, 1}).left[0] == 0, "Individual mic ignores own mute");
        require(render(p, bindings, material(p, 2), {6000, 1}).left[0] == f.sample(0, 6000), "Material obeyed mute/solo");
        rejects([&] { render(p, bindings, {K::microphone, p.tracks[0].trackId, {}}, {0, 1}); });
    });
    suite.test("virtual chunk boundaries are invisible, explicit asset gaps are silent and faded", []
    {
        Fixture f; auto bindings = sources(f);
        const auto output = render(f.project, bindings, material(f.project, 2), {131000, 180}, 31);
        for (unsigned i = 0; i < output.left.size(); ++i) require(output.left[i] == f.sample(0, 131000 + i), "PRBS/impulse chunk boundary changed");
        auto p = f.project; auto registry = std::make_shared<MediaRegistry>(*p.media); auto& asset = registry->assets[3];
        asset.availableRanges = {{0, 10000}, {11000, asset.logicalLength - 11000}}; asset.gaps = {{10000, 1000}}; p.media = registry;
        oracle(f, 1, {{0, 10000, 0}, {11000, asset.logicalLength - 11000, 11000}}, render(p, bindings, material(p, 3), {0, p.activeTimelineEnd()}, 113));
    });
    suite.test("source masks can prepare only selected assets and ignore unrelated stale sources", []
    {
        Fixture f; const auto plan = compileAudioRenderPlan(f.project); const auto mask = material(f.project, 2);
        const auto selected = openAudioSources(*plan, f.root, {}, &mask); require(selected.size() == 1, "Opened unselected asset");
        require(render(f.project, selected, mask, {5000, 1}).left[0] == f.sample(0, 5000), "Material requires unrelated source binding");
        auto all = sources(f); ++all[1].source->epoch->value;
        TimelineAudioRenderer renderer(48000, 128); renderer.setPlan(plan->timeline, all, mask);
        require(renderer.status().wasOk(), "Unselected stale source blocked selected material");
        float l[1], r[1]; renderer.renderAudio(5000, 1, l, r); require(l[0] == f.sample(0, 5000), "Mask read unselected source");
    });
    suite.test("short fragments have nonoverlapping half-length linear fades; gain-changing splits are discontinuous", []
    {
        require(MicroFade::defaultLength(44100) == 132 && MicroFade::defaultLength(48000) == 144, "3ms integer rounding");
        for (Sample n = 1; n < 400; ++n) require(MicroFade::clampLength(144, n) * 2 <= n, "Fade overlaps/extends short fragment");
        Fixture f; auto p = f.project; auto& c = p.tracks[3].clips.edit()[0]; c.sourceIn = 1000; c.lengthSamples = 7; c.timelineStartSample = 100;
        const auto out = render(p, sources(f), material(p, 3), {0, 200}, 2);
        oracle(f, 1, {{100, 7, 1000}}, out);
        RenderSpan a{{0, 5}, "a", "source", 0, 1}, b{{5, 5}, "b", "source", 5, 1};
        require(MicroFade::continuous(a, b) && !MicroFade::continuous(a, b, 1, .5f), "No-op continuity ignored gain");
        b.mediaGeneration = 2; require(!MicroFade::continuous(a, b), "Different media generations joined");
    });
    suite.test("only active retake version renders, frozen source descriptors survive later model edits", []
    {
        Fixture f; auto p = f.project; auto& clips = p.tracks[3].clips.edit(); auto old = clips[0];
        TakeStack stack; stack.spanSamples = Sample(p.Fs) * 10; TakeVersion a, b;
        old.lengthSamples = Sample(p.Fs) * 8; old.takeStackId = stack.stackId; old.versionId = a.versionId;
        auto next = old; next.clipId = newId(); next.sourceIn = p.Fs; next.versionId = b.versionId;
        clips = {old, next}; a.clipIds = {old.clipId}; b.clipIds = {next.clipId}; stack.versions = {a, b}; stack.activeVersionId = b.versionId; p.takeStacks = {stack};
        auto frozen = compileAudioRenderPlan(p); require(frozen->timeline->activeClips.size() == 4, "Inactive retake compiled");
        const auto bindings = openAudioSources(*frozen, f.root);
        require(render(p, bindings, material(p, 3), {5000, 1}).left[0] == f.sample(1, p.Fs + 5000), "Inactive retake mixed into active audio");
        p.takeStacks[0].activeVersionId = a.versionId; ++p.editRevision;
        require(frozen->timeline->editRevision == 14 && frozen->sources.size() == 2, "Plan mutated with project revision");
        require(render(p, bindings, material(p, 3), {5000, 1}).left[0] == f.sample(1, 5000), "Restored take version mapping");
    });
    suite.test("shared renderer pads arbitrary export ranges and rejects stale/short source reads", []
    {
        Fixture f; const auto plan = compileAudioRenderPlan(f.project); const auto bindings = openAudioSources(*plan, f.root);
        TimelineAudioRenderer renderer(f.project.Fs, 256); renderer.setPlan(plan->timeline, bindings);
        float l[100], r[100]; renderer.renderAudio(*plan->timeline, {plan->timeline->timelineEnd, 100}, {}, l, r);
        require(std::all_of(l, l + 100, [](float x) { return x == 0; }), "Export extension was not padded");
        rejects([&] { renderer.renderAudio(*plan->timeline, {(std::numeric_limits<Sample>::max)(), 2}, {}, l, r); });
        bindings[0].source->epoch->value.fetch_add(1);
        rejects([&] { renderer.renderAudio(5000, 100, l, r); });
        bindings[0].source->epoch->value.fetch_sub(1);
        // Intentional damage only to this test's temporary copy, never a project original.
        require(f.originals[0].deleteFile(), "Inject missing fixture file");
        rejects([&] { renderer.renderAudio(5000, 100, l, r); });
    });
    suite.test("real 44.1k import cache keeps stereo and project-sample sourceIn without double resampling", []
    {
        Fixture f; AudioImportControl control; const auto original = f.root.getChildFile("stereo-source.wav");
        {
            std::unique_ptr<juce::OutputStream> stream = original.createOutputStream(); juce::WavAudioFormat format;
            auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(44100).withNumChannels(2).withBitsPerSample(32));
            require(writer != nullptr, "Stereo fixture writer"); juce::AudioBuffer<float> pcm(2, 4410);
            for (int i = 0; i < pcm.getNumSamples(); ++i) { pcm.setSample(0, i, (i % 29 - 14) / 64.0f); pcm.setSample(1, i, (i % 37 - 18) / 64.0f); }
            require(writer->writeFromAudioSampleBuffer(pcm, 0, 4410), "Stereo fixture PCM");
        }
        AudioImportRequest request; request.source = original; request.projectDirectory = f.root; request.projectId = f.project.projectId; request.projectFs = 48000; request.playhead = 64001;
        std::unique_ptr<PreparedAudioImport> prepared; auto status = AudioImport::prepare(request, control, prepared); require(status.wasOk(), status.getErrorMessage().toRawUTF8());
        CachedImportedAudio cache; status = ImportedAudioCache::build(f.root, prepared->asset(), prepared->info(), 48000, control, cache); require(status.wasOk(), status.getErrorMessage().toRawUTF8());
        auto p = f.project; auto registry = std::make_shared<MediaRegistry>(*p.media); registry->assets.push_back(prepared->asset()); p.media = registry;
        auto track = prepared->track(); auto clip = prepared->clip(); clip.sourceIn = 401; clip.lengthSamples = 3000;
        track.clips.edit() = {clip}; track.solo = true; p.tracks.push_back(track);
        const auto plan = compileAudioRenderPlan(p); const auto bindings = openAudioSources(*plan, f.root, {{prepared->asset().assetId, prepared->asset().mediaGeneration, &cache}});
        const AudioSourceMask completed{K::completedAudio, track.trackId, clip.assetId};
        auto rendered = render(p, bindings, completed, {clip.timelineStartSample + 400, 1500}, 127);
        auto reader = AudioImport::openReader(cache.pcmFile); juce::AudioBuffer<float> expected(2, 1500);
        require(reader->read(&expected, 0, 1500, 801, true, true), "Read independent cache oracle"); bool stereo = false;
        for (int i = 0; i < 1500; ++i)
        { require(rendered.left[i] == expected.getSample(0, i) && rendered.right[i] == expected.getSample(1, i), "Stereo cache sample mapping changed"); stereo |= rendered.left[i] != rendered.right[i]; }
        require(stereo, "Stereo import collapsed to mono");
        const auto at = clip.timelineStartSample + 500;
        const auto mic = render(p, bindings, {K::microphoneMix, {}, {}}, {at, 1});
        require(mic.left[0] == f.sample(0, at) * .5f + f.sample(1, at) * .5f, "Import solo affected microphone-only mix");
        require(render(p, bindings, {}, {at, 1}).left == render(p, bindings, completed, {at, 1}).left, "Listening solo did not select import");
        p.tracks.back().mute = true;
        require(render(p, bindings, completed, {at, 1}).left[0] == 0, "Completed mask ignores own mute");
        require(render(p, bindings, {K::materialTrack, track.trackId, {}}, {at, 1}).left[0] != 0, "Imported material muted");
        require(AudioImport::hashFile(original, control) == prepared->info().contentHash && AudioImport::hashFile(prepared->originalFile(), control) == prepared->info().contentHash, "Import original changed");
    });
    return suite.result("audio-cut-render");
}
