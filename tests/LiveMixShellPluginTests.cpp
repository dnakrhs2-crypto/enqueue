#include "audio/PluginHost.h"
#include "audio/PluginScan.h"
#include "ui/PluginScanListComponent.h"
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>

#include <windows.h>
#include <tlhelp32.h>

namespace gocue::tests
{
namespace
{
    using Scanner = PluginScanCoordinator;
    using Outcome = Scanner::Outcome;

    struct ScanFixture
    {
        juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("livemix-scan-" + juce::Uuid().toString());
        PluginHost host;
        Scanner* scanner = nullptr;
        explicit ScanFixture (int timeout = 15000)
        {
            directory.createDirectory();
            Scanner::Options options;
            options.executable = juce::File (LM_SCAN_WORKER_EXE);
            options.logFile = logFile();
            options.timeoutMs = timeout;
            auto owned = std::make_unique<Scanner> (options);
            scanner = owned.get();
            host.getKnownPlugins().setCustomScanner (std::move (owned));
        }
        ~ScanFixture()
        {
            host.getKnownPlugins().scanFinished();
            directory.deleteRecursively();
        }
        juce::File logFile() const { return directory.getChildFile ("logs/plugin-scan.log"); }
        bool scan (const juce::String& file, juce::OwnedArray<juce::PluginDescription>& types)
        { return host.getKnownPlugins().scanAndAddFile (file, false, types, *host.getVST3Format()); }
    };

    bool processRunning (DWORD pid)
    {
        if (pid == 0) return false;
        const auto process = OpenProcess (SYNCHRONIZE, FALSE, pid);
        if (process == nullptr) return false;
        const bool running = WaitForSingleObject (process, 0) == WAIT_TIMEOUT;
        CloseHandle (process);
        return running;
    }

    int workerCount()
    {
        int count = 0;
        const auto snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return -1;
        PROCESSENTRY32W entry {};
        entry.dwSize = sizeof (entry);
        if (Process32FirstW (snapshot, &entry))
            do
            {
                if (entry.th32ParentProcessID == GetCurrentProcessId()
                    && _wcsicmp (entry.szExeFile, L"LiveMix.exe") == 0 && processRunning (entry.th32ProcessID)) ++count;
            } while (Process32NextW (snapshot, &entry));
        CloseHandle (snapshot);
        return count;
    }

    template <typename Predicate>
    bool pumpUntil (Predicate done, int timeoutMs = 4000)
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
            juce::Thread::sleep (2);
        } while (juce::Time::getMillisecondCounterHiRes() < deadline);
        return done();
    }

    juce::StringArray identities (const juce::OwnedArray<juce::PluginDescription>& descriptions)
    {
        juce::StringArray result;
        for (const auto* d : descriptions)
            result.add (d->name + "|" + juce::String (d->uniqueId) + "|" + d->pluginFormatName
                        + "|" + d->fileOrIdentifier + "|" + d->manufacturerName + "|" + d->version
                        + "|" + juce::String (d->numInputChannels) + "|" + juce::String (d->numOutputChannels));
        result.sort (false);
        return result;
    }

    class ScanJob final : public juce::ThreadPoolJob
    {
    public:
        ScanJob (ScanFixture& f, juce::String path) : ThreadPoolJob ("LiveMix scan test"), fixture (f), file (std::move (path)) {}
        JobStatus runJob() override
        {
            started.signal();
            succeeded = fixture.scan (file, types);
            finished.signal();
            return jobHasFinished;
        }
        ScanFixture& fixture;
        juce::String file;
        juce::OwnedArray<juce::PluginDescription> types;
        juce::WaitableEvent started, finished;
        bool succeeded = false;
    };
}

class LiveMixShellIdentityTests : public juce::UnitTest
{
public:
    LiveMixShellIdentityTests() : UnitTest ("LiveMix shell class identity", "LiveMix") {}
    void runTest() override
    {
        beginTest ("shell discovery lists the effects the factory lists before any host context");
        PluginHost host;
        juce::OwnedArray<juce::PluginDescription> descriptions;
        expect (host.getKnownPlugins().scanAndAddFile (LM_VST3_SHELL, false, descriptions, *host.getVST3Format()));
        expectEquals (descriptions.size(), 3, "Shell must enumerate Alpha, Beta and Gamma");
        juce::StringArray names;
        for (const auto* d : descriptions) names.add (d->name);
        logMessage ("Discovered shell classes: " + names.joinIntoString (", "));
        // Waves 17's WaveShell only repeats its classes once the host context is set (718 -> 1436, same names and ids,
        // measured 2026-09-30), so the scan keeps JUCE's context-free listing and a context-only class is not listed.
        expect (! names.contains ("Delta"), "the scan lists the classes without a host context");

        for (const auto* description : descriptions)
        {
            beginTest (description->name + " direct description instantiation");
            juce::String error;
            {
                auto instance = host.createInstance (*description, 48000.0, 128, error);
                checkIdentity (instance.get(), *description, error);
            }
            beginTest (description->name + " saved PluginSlotState descriptionXml instantiation");
            PluginSlotState state;
            state.descriptionXml = description->createXml()->toString();
            // Empty known list forces the saved XML path without any catalogue fallback.
            PluginHost restoredHost;
            auto restored = restoredHost.createInstance (state, 48000.0, 128, error);
            checkIdentity (restored.get(), *description, error);
        }
    }
private:
    void checkIdentity (juce::AudioPluginInstance* instance, const juce::PluginDescription& d, const juce::String& error)
    {
        expect (instance != nullptr, d.name + " createInstance returned null: " + error);
        if (instance == nullptr) return;
        const juce::StringArray names { "Alpha", "Beta", "Gamma", "Delta" };
        instance->prepareToPlay (48000.0, 128);
        expectEquals (instance->getName(), d.name, d.name + " instance name");
        expectEquals (instance->getLatencySamples(), 101 + names.indexOf (d.name), d.name + " latency identifies the actual component");
        expectEquals (instance->getParameters().size(), 1);
        if (! instance->getParameters().isEmpty())
        {
            auto* parameter = instance->getParameters()[0];
            expectEquals (parameter->getName (128), d.name, d.name + " parameter identifies the actual component");
            parameter->setValue (0.75f);
            juce::MemoryBlock state;
            instance->getStateInformation (state);
            parameter->setValue (0.25f);
            instance->setStateInformation (state.getData(), (int) state.getSize());
            expectWithinAbsoluteError (parameter->getValue(), 0.75f, 0.0001f);
        }
        juce::AudioBuffer<float> audio (2, 128);
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < 128; ++s) audio.setSample (c, s, (float) (c * 128 + s) / 512.0f);
        juce::MidiBuffer midi;
        instance->processBlock (audio, midi);
        expectWithinAbsoluteError (audio.getSample (0, 64), 0.125f, 0.0001f);
        expectWithinAbsoluteError (audio.getSample (1, 64), 0.375f, 0.0001f);
        instance->releaseResources();
    }
};

class LiveMixShellFixtureTests : public juce::UnitTest
{
public:
    LiveMixShellFixtureTests() : UnitTest ("LiveMix shell fixture contract", "LiveMix") {}
    void runTest() override
    {
        beginTest ("raw factory exports, changing class order, idempotent context and controller rejection");
        juce::DynamicLibrary library;
        const auto module = juce::File (LM_VST3_SHELL).getChildFile ("Contents/x86_64-win/LiveMix Test Shell.vst3");
        expect (library.open (module.getFullPathName()));
        using FactoryFn = Steinberg::IPluginFactory* (PLUGIN_API*)();
        using ModuleFn = bool (PLUGIN_API*)();
        const auto getFactory = (FactoryFn) library.getFunction ("GetPluginFactory");
        const auto init = (ModuleFn) library.getFunction ("InitDll");
        const auto exit = (ModuleFn) library.getFunction ("ExitDll");
        expect (getFactory != nullptr && init != nullptr && exit != nullptr);
        if (getFactory == nullptr || init == nullptr || exit == nullptr) return;
        expect (init());
        auto* base = getFactory();
        Steinberg::IPluginFactory3* factory = nullptr;
        expectEquals<Steinberg::tresult> (base->queryInterface (Steinberg::IPluginFactory3_iid, (void**) &factory), Steinberg::kResultOk);
        if (factory != nullptr)
        {
            expectEquals (factory->countClasses(), 3);
            factory->setHostContext (base);
            expectEquals (factory->countClasses(), 7);
            factory->setHostContext (base);
            expectEquals (factory->countClasses(), 7);
            const juce::StringArray expected { "Alpha", "AlphaCtrl", "Beta", "BetaCtrl", "Gamma", "GammaCtrl", "Delta" };
            for (int i = 0; i < 7; ++i)
            {
                Steinberg::PClassInfo info {};
                expectEquals<Steinberg::tresult> (factory->getClassInfo (i, &info), Steinberg::kResultOk);
                expectEquals (juce::String::fromUTF8 (info.name), expected[i]);
                Steinberg::Vst::IComponent* component = nullptr;
                const auto result = factory->createInstance (info.cid, Steinberg::Vst::IComponent_iid, (void**) &component);
                if (i % 2 != 0)
                {
                    expectEquals<Steinberg::tresult> (result, Steinberg::kNoInterface);
                    expect (component == nullptr);
                }
                else
                {
                    expectEquals<Steinberg::tresult> (result, Steinberg::kResultOk);
                    if (component != nullptr)
                    {
                        Steinberg::Vst::IAudioProcessor* processor = nullptr;
                        expectEquals<Steinberg::tresult> (component->queryInterface (Steinberg::Vst::IAudioProcessor_iid, (void**) &processor), Steinberg::kResultOk);
                        if (processor != nullptr)
                        {
                            expectEquals ((int) processor->getLatencySamples(), 101 + i / 2);
                            processor->release();
                        }
                        component->release();
                    }
                }
            }
            factory->setHostContext (nullptr);
            factory->setHostContext (nullptr);
            expectEquals (factory->countClasses(), 3);
            factory->setHostContext (base);
            expectEquals (factory->countClasses(), 7);
            factory->release();
        }
        base->release();
        expect (exit());
    }
};

class LiveMixPluginScanTests : public juce::UnitTest
{
public:
    LiveMixPluginScanTests() : UnitTest ("LiveMix plugin scan subprocess", "LiveMix") {}
    void runTest() override
    {
        beginTest ("worker command line does not enter the normal application");
        expect (PluginScanWorker::isWorkerCommandLine ("--plugin-scan-worker:p123"));
        expect (PluginScanWorker::isWorkerCommandLine ("--plugin-scan-worker"));
        expect (! PluginScanWorker::isWorkerCommandLine ("--safe-mode"));
        expect (juce::File (LM_SCAN_WORKER_EXE).existsAsFile());
        expectEquals (workerCount(), 0);

        beginTest ("background scan returns the same shell descriptions using the worker message thread");
        ScanFixture f;
        PluginHost local;
        juce::OwnedArray<juce::PluginDescription> inProcess;
        local.getVST3Format()->findAllTypesForFile (inProcess, LM_VST3_SHELL);
        juce::ThreadPool pool (1);
        ScanJob shell (f, LM_VST3_SHELL);
        pool.addJob (&shell, false);
        expect (shell.finished.wait (20000), "shell worker did not finish");
        pool.removeJob (&shell, true, 5000);
        expect (shell.succeeded);
        expect (identities (inProcess) == identities (shell.types));
        auto results = f.scanner->getResults();
        expectEquals ((int) results.size(), 1);
        if (results.empty()) return;
        expect (results.back().outcome == Outcome::completed);
        expect (results.back().onMessageThread);
        expect (results.back().workerPid != GetCurrentProcessId());
        expect (processRunning (results.back().workerPid));

        beginTest ("crasher is blacklisted, logged with the exit code, and the next file restarts the worker");
        juce::OwnedArray<juce::PluginDescription> crashed;
        expect (! f.scan (LM_VST3_CRASHER, crashed));
        results = f.scanner->getResults();
        const auto crash = results.back();
        expect (crash.outcome == Outcome::crashed);
        expect (crash.exitCode != 0 && crash.exitCode != STILL_ACTIVE);
        expect (! processRunning (crash.workerPid));
        expect (f.host.getKnownPlugins().getBlacklistedFiles().contains (LM_VST3_CRASHER));
        expect (f.logFile().loadFileAsString().contains (juce::String::fromUTF8 ("튕김(종료 코드 0x")));
        expect (f.scanner->getSkippedMessage().contains (juce::String::fromUTF8 ("1개는 건너뛰었습니다")));
        expect (f.scanner->getSkippedMessage().contains ("LiveMix Test Crasher.vst3"));
        juce::OwnedArray<juce::PluginDescription> afterCrash;
        expect (f.scan (LM_VST3_SHELL, afterCrash));
        expect (identities (afterCrash) == identities (inProcess));
        expect (f.scanner->getResults().back().workerPid != crash.workerPid);
        f.host.getKnownPlugins().scanFinished();
        expectEquals (workerCount(), 0);

        beginTest ("a user rescan clears both blacklist and stale crash marker and retries the crasher");
        const auto marker = f.directory.getChildFile ("scan.crashed");
        expect (marker.replaceWithText (LM_VST3_CRASHER));
        const auto previousCount = f.scanner->getResults().size();
        juce::OwnedArray<juce::PluginDescription> skipped;
        expect (! f.scan (LM_VST3_CRASHER, skipped));
        expectEquals ((int) f.scanner->getResults().size(), (int) previousCount);
        f.scanner->prepareForScan (f.host.getKnownPlugins(), marker);
        expect (f.host.getKnownPlugins().getBlacklistedFiles().isEmpty());
        {
            juce::PluginDirectoryScanner directory (f.host.getKnownPlugins(), *f.host.getVST3Format(), {}, false, marker, true);
            directory.setFilesOrIdentifiersToScan ({ LM_VST3_CRASHER });
            juce::String scannedName;
            directory.scanNextFile (false, scannedName);
        }
        results = f.scanner->getResults();
        expectEquals ((int) results.size(), 1);
        expect (results.back().outcome == Outcome::crashed);
        expectEquals (workerCount(), 0);

        beginTest ("empty shell yields zero types and JUCE's failed-to-load list, without blacklisting");
        {
            juce::PluginDirectoryScanner directory (f.host.getKnownPlugins(), *f.host.getVST3Format(), {}, false, marker, true);
            directory.setFilesOrIdentifiersToScan ({ LM_VST3_EMPTY });
            juce::String scannedName;
            directory.scanNextFile (false, scannedName);
            expect (directory.getFailedFiles().contains (LM_VST3_EMPTY));
        }
        results = f.scanner->getResults();
        expect (results.back().outcome == Outcome::completed);
        expectEquals (results.back().numTypes, 0);
        expect (! f.host.getKnownPlugins().getBlacklistedFiles().contains (LM_VST3_EMPTY));
        expect (f.logFile().loadFileAsString().contains (juce::String::fromUTF8 ("0개")));
        expectEquals (workerCount(), 0);

       #if JUCE_PLUGINHOST_VST
        beginTest ("VST2 requests use the worker's VST2 format, independently of the main host switch");
        f.host.setVst2Enabled (true);
        juce::OwnedArray<juce::PluginDescription> vst2Types;
        f.host.getKnownPlugins().scanAndAddFile (LM_OBS_TEST_CURRENT_DLL, false, vst2Types, *f.host.getFormat ("VST"));
        expect (f.scanner->getResults().back().outcome == Outcome::completed);
        expectEquals (f.scanner->getResults().back().format, juce::String ("VST"));
        expect (f.scanner->getResults().back().onMessageThread);
        f.host.getKnownPlugins().scanFinished();
        expectEquals (workerCount(), 0);
       #endif

        beginTest ("hung initialize times out, is logged, and leaves no worker");
        {
            ScanFixture timeout (1800);
            juce::OwnedArray<juce::PluginDescription> types;
            expect (! timeout.scan (LM_VST3_HANG, types));
            const auto r = timeout.scanner->getResults().back();
            expect (r.outcome == Outcome::timedOut);
            expect (r.workerPid != 0, "timeout must reach the plugin, not worker startup");
            expect (r.elapsedMs >= 1800 && r.elapsedMs < 5000);
            expect (! processRunning (r.workerPid));
            expect (timeout.host.getKnownPlugins().getBlacklistedFiles().contains (LM_VST3_HANG));
            expect (timeout.logFile().loadFileAsString().contains (juce::String::fromUTF8 ("시간 초과")));
            juce::OwnedArray<juce::PluginDescription> next;
            expect (timeout.scan (LM_VST3_SHELL, next));
            timeout.host.getKnownPlugins().scanFinished();
            expectEquals (workerCount(), 0);
        }

        beginTest ("JUCE ThreadPoolJob cancellation kills the hung worker promptly without blacklisting");
        {
            ScanFixture cancel;
            juce::OwnedArray<juce::PluginDescription> warmup;
            expect (cancel.scan (LM_VST3_EMPTY, warmup) == false); // successful zero-type scan
            const auto pid = cancel.scanner->getResults().back().workerPid;
            ScanJob hanging (cancel, LM_VST3_HANG);
            pool.addJob (&hanging, false);
            expect (hanging.started.wait (3000));
            juce::Thread::sleep (150);
            const auto started = juce::Time::getMillisecondCounterHiRes();
            expect (pool.removeJob (&hanging, true, 3000));
            expect (juce::Time::getMillisecondCounterHiRes() - started < 2000);
            expect (cancel.scanner->getResults().back().outcome == Outcome::cancelled);
            expect (! cancel.host.getKnownPlugins().getBlacklistedFiles().contains (LM_VST3_HANG));
            expect (! processRunning (pid));
            expect (cancel.logFile().loadFileAsString().contains (juce::String::fromUTF8 ("취소")));
            expectEquals (workerCount(), 0);
        }

        beginTest ("dismissing the real JUCE scan dialog cancels a hanging file before its timeout");
        {
            ScanFixture cancel;
            juce::OwnedArray<juce::PluginDescription> warmup;
            cancel.scan (LM_VST3_EMPTY, warmup);
            const auto pid = cancel.scanner->getResults().back().workerPid;
            cancel.scanner->prepareForScan (cancel.host.getKnownPlugins());
            PluginScanListComponent list (cancel.host.getFormatManager(), cancel.host.getKnownPlugins(),
                                          cancel.directory.getChildFile ("scan.crashed"), *cancel.scanner,
                                          juce::String::fromUTF8 ("LiveMix 플러그인 스캔"));
            list.scanFor (*cancel.host.getVST3Format(), { LM_VST3_HANG });
            auto* progress = juce::Component::getCurrentlyModalComponent();
            expect (progress != nullptr);
            juce::Thread::sleep (150);
            const auto started = juce::Time::getMillisecondCounterHiRes();
            if (progress != nullptr) progress->exitModalState (0); // same path as Cancel / Escape
            expect (pumpUntil ([&] { return ! list.isScanning(); }));
            expect (juce::Time::getMillisecondCounterHiRes() - started < 2000);
            const auto cancelled = cancel.scanner->getResults();
            expectEquals ((int) cancelled.size(), 1);
            if (! cancelled.empty()) expect (cancelled.back().outcome == Outcome::cancelled);
            expect (! processRunning (pid));
            expectEquals (workerCount(), 0);
            // JUCE may display its existing failed-file notice for the interrupted file.
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
            pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() == nullptr; });
        }

        beginTest ("UTF-8 log rotates at one MiB and preserves one record per attempted file");
        logMessage ("Scan log before rotation:\n" + f.logFile().loadFileAsString());
        const auto oldLog = juce::String::repeatedString ("x", 1024 * 1024 + 1);
        expect (f.logFile().replaceWithText (oldLog));
        juce::OwnedArray<juce::PluginDescription> empty;
        f.scan (LM_VST3_EMPTY, empty);
        expectEquals (f.logFile().getSiblingFile ("plugin-scan.log.1").getSize(), (juce::int64) (1024 * 1024 + 1));
        auto lines = juce::StringArray::fromLines (f.logFile().loadFileAsString());
        lines.removeEmptyStrings();
        expectEquals (lines.size(), 1);
        expect (lines[0].contains ("\tVST3\t"));
        expect (lines[0].contains ("LiveMix Test Empty.vst3"));
        expect (lines[0].contains (juce::String::fromUTF8 ("\t0개\t")));
        expect (lines[0].endsWith (" ms"));
        f.host.getKnownPlugins().scanFinished();
        expectEquals (workerCount(), 0);
    }
};

static LiveMixShellIdentityTests liveMixShellIdentityTests;
static LiveMixShellFixtureTests liveMixShellFixtureTests;
static LiveMixPluginScanTests liveMixPluginScanTests;
} // namespace gocue::tests
