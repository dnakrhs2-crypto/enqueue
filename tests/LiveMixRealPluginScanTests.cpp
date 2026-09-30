// Opt-in: a real installed plugin module (Waves' WaveShell, say) through LiveMix's own scan worker and PluginHost:
// the scan as the plugin manager runs it (a pool thread asking the worker process, which scans on its message thread),
// then instances made on the message thread, 20 blocks through each, and each editor shown in a window.
// Runs only when LIVEMIX_REAL_VST3 names a module; LIVEMIX_REAL_LOAD picks the instances ("name:Rhapsody|first:2",
// default "first:2"). 2026-09-30: Waves 17 crashed LiveMix when its WaveShell was scanned on a background thread.
#include "audio/PluginHost.h"
#include "PluginScan.h"

#include <windows.h>

namespace gocue::tests
{
namespace
{
    template <typename Predicate>
    bool pumpMessagesUntil (Predicate done, double timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        do
        {
            MSG message;
            for (int n = 0; n < 64 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++n)
            {
                TranslateMessage (&message);
                DispatchMessageW (&message);
            }
            if (done()) return true;
            juce::Thread::sleep (5);
        } while (juce::Time::getMillisecondCounterHiRes() < deadline);
        return done();
    }

    struct RealScanJob final : public juce::ThreadPoolJob
    {
        RealScanJob (PluginHost& h, juce::String path) : ThreadPoolJob ("LiveMix real plugin scan"), host (h), file (std::move (path)) {}
        JobStatus runJob() override
        {
            found = host.getKnownPlugins().scanAndAddFile (file, false, types, *host.getVST3Format());
            done = true;
            return jobHasFinished;
        }
        PluginHost& host;
        juce::String file;
        juce::OwnedArray<juce::PluginDescription> types;
        std::atomic<bool> done { false };
        bool found = false;
    };
}

class LiveMixRealPluginScanTests : public juce::UnitTest
{
public:
    LiveMixRealPluginScanTests() : juce::UnitTest ("LiveMix real plugin scan", "LiveMix") {}

    void runTest() override
    {
        const auto module = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_REAL_VST3", {});
        if (module.isEmpty())
            return;

        beginTest ("real module scans through the worker and every picked instance loads, plays and shows its editor");
        const auto workDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("livemix-real-" + juce::Uuid().toString());
        workDir.createDirectory();

        PluginHost host;
        livemix::PluginScanCoordinator::Options options;
        options.executable = juce::File (LM_SCAN_WORKER_EXE);
        options.logFile = workDir.getChildFile ("plugin-scan.log");
        options.timeoutMs = 60 * 60 * 1000;
        auto owned = std::make_unique<livemix::PluginScanCoordinator> (options);
        auto* coordinator = owned.get();
        host.getKnownPlugins().setCustomScanner (std::move (owned));

        juce::ThreadPool pool (juce::ThreadPoolOptions{}.withNumberOfThreads (1));
        auto* job = new RealScanJob (host, module);
        const auto start = juce::Time::getMillisecondCounterHiRes();
        pool.addJob (job, false);
        expect (pumpMessagesUntil ([job] { return job->done.load(); }, 70.0 * 60.0 * 1000.0), "scan did not finish");
        const auto seconds = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
        logMessage ("scan: " + juce::String (job->types.size()) + " types in " + juce::String (seconds, 1) + " s, found=" + juce::String ((int) job->found)
                    + ", log: " + options.logFile.loadFileAsString().trim());
        for (const auto& r : coordinator->getResults())
            logMessage ("scan result: outcome " + juce::String ((int) r.outcome) + " exit 0x" + juce::String::toHexString ((int) r.exitCode)
                        + " messageThread " + juce::String ((int) r.onMessageThread) + " types " + juce::String (r.numTypes));
        expect (job->found && job->types.size() > 0, "the worker found no types");

        juce::OwnedArray<juce::PluginDescription> types;
        types.swapWith (job->types);
        pool.removeAllJobs (true, 1000);
        host.getKnownPlugins().scanFinished();

        juce::Array<int> picks;
        const auto spec = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_REAL_LOAD", "first:2");
        for (const auto& s : juce::StringArray::fromTokens (spec, "|", ""))
        {
            if (s.startsWith ("first:"))
                for (int i = 0; i < juce::jmin (s.fromFirstOccurrenceOf (":", false, false).getIntValue(), types.size()); ++i) picks.addIfNotAlreadyThere (i);
            else if (s.startsWith ("name:"))
                for (int i = 0; i < types.size(); ++i)
                    if (types[i]->name.containsIgnoreCase (s.fromFirstOccurrenceOf (":", false, false))) picks.addIfNotAlreadyThere (i);
        }

        for (const int i : picks)
        {
            const auto& d = *types[i];
            juce::String error;
            auto instance = host.createInstance (d, 48000.0, 512, error);
            expect (instance != nullptr, "could not create " + d.name + ": " + error);
            if (instance == nullptr)
                continue;

            const auto got = instance->getPluginDescription();
            expectEquals (got.name, d.name, "wrong plugin for " + d.name);
            expectEquals (got.uniqueId, d.uniqueId, "wrong plugin id for " + d.name);

            instance->prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> buffer (juce::jmax (2, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels()), 512);
            juce::MidiBuffer midi;
            for (int block = 0; block < 20; ++block)
            {
                buffer.clear();
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.setSample (ch, 0, 0.25f);
                instance->processBlock (buffer, midi);
            }
            instance->releaseResources();

            if (instance->hasEditor())
            {
                auto* editor = instance->createEditorAndMakeActive();
                expect (editor != nullptr, "no editor for " + d.name);
                if (editor != nullptr)
                {
                    juce::DocumentWindow window (d.name, juce::Colours::black, juce::DocumentWindow::closeButton);
                    window.setUsingNativeTitleBar (true);
                    window.setContentOwned (editor, true);
                    window.setVisible (true);
                    pumpMessagesUntil ([] { return false; }, 2000.0);
                    window.clearContentComponent();
                }
            }

            logMessage ("loaded, played and showed: " + d.name);
            instance.reset();
            pumpMessagesUntil ([] { return false; }, 300.0);
        }

        workDir.deleteRecursively();
    }
};

static LiveMixRealPluginScanTests liveMixRealPluginScanTests;

} // namespace gocue::tests
