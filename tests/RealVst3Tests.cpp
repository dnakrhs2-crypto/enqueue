#include "audio/AudioEngine.h"
#include "audio/PluginChain.h"
#include "audio/PluginHost.h"
#include "model/ProjectSerializer.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

/** Exercises the real VST3 hosting path with whatever effects are installed on this machine
    (default JUCE locations + %LOCALAPPDATA%\Programs\Common\VST3). Skips gracefully when
    nothing is installed, so CI machines without plugins still pass.

    Set GOCUE_WRITE_DEMO_PROJECT=<dir> to also write a .gocue project that references the
    first effect (used for the GUI end-to-end smoke test). */
class RealVst3Tests : public juce::UnitTest
{
public:
    RealVst3Tests() : juce::UnitTest ("Real VST3 plugins (optional)", "GoCue") {}

    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 512;

    juce::File writeSine (const juce::File& file, double seconds, float amplitude)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                       .withSampleRate (sampleRate)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (16));
        expect (writer != nullptr);

        if (writer == nullptr)
            return {};

        const int numSamples = (int) (seconds * sampleRate);
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));

        writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
        return file;
    }

    static void fillSine (juce::AudioBuffer<float>& buffer, float amplitude)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample (ch, i, amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));
    }

    static bool isFinite (const juce::AudioBuffer<float>& buffer)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (! std::isfinite (buffer.getSample (ch, i)))
                    return false;

        return true;
    }

    void runTest() override
    {
        beginTest ("scan the VST3 folders for effects");

        PluginHost host;
        auto* format = host.getVST3Format();
        expect (format != nullptr);

        if (format == nullptr)
            return;

        auto searchPaths = format->getDefaultLocationsToSearch();
        const auto localAppData = juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {});

        if (localAppData.isNotEmpty())
            searchPaths.addIfNotAlreadyThere (juce::File (localAppData).getChildFile ("Programs").getChildFile ("Common").getChildFile ("VST3"));

        logMessage ("Search paths: " + searchPaths.toString());
        const auto files = format->searchPathsForPlugins (searchPaths, true, false);
        logMessage ("VST3 files found: " + juce::String (files.size()));

        if (files.isEmpty())
        {
            logMessage ("No VST3 plugins installed on this machine - real-plugin checks skipped");
            expect (true);
            return;
        }

        juce::OwnedArray<juce::PluginDescription> types;

        for (const auto& file : files)
            format->findAllTypesForFile (types, file);

        expect (types.size() > 0, "installed .vst3 files yielded no plugin descriptions");

        for (auto* type : types)
            host.getKnownPlugins().addType (*type);

        const auto effects = host.getEffectTypes();
        expect (effects.size() > 0, "no effect plugins found (only instruments?)");

        if (effects.isEmpty())
            return;

        juce::PluginDescription chosen = effects[0];

        for (const auto& e : effects)
            if (e.name == "3BandEQ")
                chosen = e;

        logMessage ("Using plugin: " + chosen.name + "  [" + chosen.fileOrIdentifier + "]  uid=" + juce::String (chosen.uniqueId));

        beginTest ("instantiate, prepare and process a real VST3 through PluginChain");

        PluginChain chain;
        chain.prepare (sampleRate, blockSize);

        juce::String error;
        auto instance = host.createInstance (chosen, sampleRate, blockSize, error);
        expect (instance != nullptr, "createInstance failed: " + error);

        if (instance == nullptr)
            return;

        auto* raw = instance.get();
        logMessage ("hasEditor: " + juce::String ((int) raw->hasEditor()) + "  tail: " + juce::String (raw->getTailLengthSeconds()));
        chain.addPlugin (std::move (instance));

        expectEquals (chain.getNumSlots(), 1);
        expectEquals (raw->getTotalNumInputChannels(), 2);
        expectEquals (raw->getTotalNumOutputChannels(), 2);

        juce::AudioBuffer<float> buffer (2, blockSize);
        float rms = 0.0f;

        for (int i = 0; i < 8; ++i)
        {
            fillSine (buffer, 0.5f);
            chain.process (buffer, blockSize);
            rms = buffer.getRMSLevel (0, 0, blockSize);
        }

        expect (isFinite (buffer), "plugin produced NaN/inf");
        expectGreaterThan (rms, 0.01f);   // an effect at default settings still passes signal

        beginTest ("real plugin state round-trips through getStates() -> restore()");

        const auto states = chain.getStates();
        expectEquals ((int) states.size(), 1);
        expectEquals (states[0].name, chosen.name);
        expectEquals (states[0].fileOrIdentifier, chosen.fileOrIdentifier);
        expectEquals (states[0].format, juce::String ("VST3"));
        expect (states[0].descriptionXml.contains (chosen.name));

        PluginChain restored;
        restored.prepare (sampleRate, blockSize);
        auto errors = restored.restore (states, host.makeFactory (sampleRate, blockSize));
        expectEquals (errors.size(), 0);
        expectEquals (restored.getNumSlots(), 1);
        expect (! restored.getSlot (0).isMissing());

        auto minimal = states;
        minimal[0].descriptionXml.clear();            // older files: file + uid only
        PluginChain restoredFromIds;
        restoredFromIds.prepare (sampleRate, blockSize);
        errors = restoredFromIds.restore (minimal, host.makeFactory (sampleRate, blockSize));
        expectEquals (errors.size(), 0);
        expect (! restoredFromIds.getSlot (0).isMissing());

        beginTest ("the engine plays a cue through a real VST3 insert");

        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("gocue_realvst3_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());
        const auto tone = writeSine (dir.getChildFile ("tone.wav"), 1.0, 0.5f);

        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            engine.getPluginHost().getKnownPlugins().addType (chosen);

            Cue cue;
            cue.name = "vst3";
            cue.file = tone;

            errors = engine.getCueChain (cue.id).restore (states, engine.makePluginFactory());
            expectEquals (errors.size(), 0);

            expect (engine.play (cue));
            juce::AudioBuffer<float> out (2, blockSize);

            for (int i = 0; i < 10; ++i)
                engine.renderBlock (out, blockSize);

            expect (isFinite (out));
            expectGreaterThan (out.getRMSLevel (0, 0, blockSize), 0.02f);

            engine.stopAll();
            engine.renderBlock (out, blockSize);
            engine.renderBlock (out, blockSize);
            engine.reapFinishedPlayers();
            expectEquals (engine.getNumPlaying(), 0);
            engine.shutdown();
        }

        const auto demoDir = juce::SystemStats::getEnvironmentVariable ("GOCUE_WRITE_DEMO_PROJECT", {});

        if (demoDir.isNotEmpty())
        {
            const juce::File outDir (demoDir);
            outDir.createDirectory();
            const auto demoTone = writeSine (outDir.getChildFile ("demo_tone.wav"), 4.0, 0.4f);

            Project project;
            project.name = "vst3demo";

            Cue withPlugin;
            withPlugin.name = "VST3 " + chosen.name;
            withPlugin.file = demoTone;
            withPlugin.audio.envelope = Envelope::fromFadeIn (0.3);
            withPlugin.fadeOutMs = 800;
            withPlugin.plugins = states;
            project.cues().push_back (withPlugin);

            Cue plain;
            plain.name = "plain tone";
            plain.file = demoTone;
            project.cues().push_back (plain);

            project.masterPlugins = states;

            const auto projectFile = outDir.getChildFile ("vst3demo.gocue");
            expect (ProjectSerializer::save (project, projectFile).wasOk());
            logMessage ("Demo project written: " + projectFile.getFullPathName());
        }

        expect (dir.deleteRecursively());
    }
};

static RealVst3Tests realVst3Tests;

} // namespace gocue::tests
