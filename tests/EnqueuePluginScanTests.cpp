#include "audio/PluginScan.h"
#include "ui/PluginManager.h"
#include "ui/PluginScanListComponent.h"

#include <windows.h>
#include <tlhelp32.h>

namespace gocue::tests
{
namespace
{
    using Outcome = PluginScanCoordinator::Outcome;

    juce::PropertiesFile::Options propertiesOptions()
    {
        juce::PropertiesFile::Options options;
        options.millisecondsBeforeSaving = -1;
        return options;
    }

    struct Scratch
    {
        juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("enqueue-scan-" + juce::Uuid().toString());
        Scratch() { directory.createDirectory(); }
        ~Scratch() { directory.deleteRecursively(); }
    };

    struct ScanFixture
    {
        Scratch scratch;
        juce::PropertiesFile storage { scratch.directory.getChildFile ("Enqueue/Enqueue.settings"), propertiesOptions() };
        AppSettings settings { storage };
        PluginHost host;
        PluginScanCoordinator* scanner = nullptr;
        explicit ScanFixture (int timeout = 15000)
        {
            PluginScanCoordinator::Options options;
            options.executable = juce::File (ENQUEUE_SCAN_WORKER_EXE);
            options.logFile = logFile();
            options.timeoutMs = timeout;
            auto owned = std::make_unique<PluginScanCoordinator> (options);
            scanner = owned.get();
            host.getKnownPlugins().setCustomScanner (std::move (owned));
        }
        ~ScanFixture() { host.getKnownPlugins().scanFinished(); }
        juce::File logFile() const { return settings.getPluginScanLogFile(); }
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
                    && _wcsicmp (entry.szExeFile, L"Enqueue.exe") == 0 && processRunning (entry.th32ProcessID)) ++count;
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

    juce::StringArray descriptions (const juce::OwnedArray<juce::PluginDescription>& types)
    {
        juce::StringArray result;
        for (const auto* type : types)
        {
            auto xml = type->createXml();
            xml->removeAttribute ("infoUpdateTime"); // the two scans take place at different times
            result.add (xml->toString());
        }
        result.sort (false);
        return result;
    }

    class ScanJob final : public juce::ThreadPoolJob
    {
    public:
        ScanJob (ScanFixture& f, juce::String path) : ThreadPoolJob ("Enqueue scan test"), fixture (f), file (std::move (path)) {}
        JobStatus runJob() override
        {
            succeeded = fixture.scan (file, types);
            finished.signal();
            return jobHasFinished;
        }
        ScanFixture& fixture;
        juce::String file;
        juce::OwnedArray<juce::PluginDescription> types;
        juce::WaitableEvent finished;
        bool succeeded = false;
    };

    juce::TextButton* resetButton (juce::Component& content)
    {
        for (auto* child : content.getChildren())
            if (auto* button = dynamic_cast<juce::TextButton*> (child))
                if (button->getButtonText() == juce::String::fromUTF8 ("목록 초기화")) return button;
        return nullptr;
    }
}

class EnqueuePluginScanTests : public juce::UnitTest
{
public:
    EnqueuePluginScanTests() : UnitTest ("Enqueue plugin scan subprocess", "Enqueue") {}
    void runTest() override
    {
        beginTest ("worker command line and failed startup exit without entering the normal application");
        expect (PluginScanWorker::isWorkerCommandLine ("--plugin-scan-worker:p123"));
        expect (PluginScanWorker::isWorkerCommandLine ("--plugin-scan-worker"));
        expect (! PluginScanWorker::isWorkerCommandLine ("--safe-mode"));
        expect (! PluginScanWorker::isWorkerCommandLine ("\"C:\\Shows\\show.enqueue\""));
        expectEquals (PluginScanCoordinator::Options{}.timeoutMs, 60 * 60 * 1000);
        expect (juce::File (ENQUEUE_SCAN_WORKER_EXE).existsAsFile());
        juce::ChildProcess invalid;
        expect (invalid.start (juce::StringArray { ENQUEUE_SCAN_WORKER_EXE, "--plugin-scan-worker" }));
        expect (invalid.waitForProcessToFinish (5000), "worker startup entered the normal application");
        if (invalid.isRunning()) invalid.kill();
        expectEquals ((int) invalid.getExitCode(), 1);
        expectEquals (workerCount(), 0);

        beginTest ("background scan matches every shell description on the Enqueue worker message thread");
        PluginHost local;
        juce::OwnedArray<juce::PluginDescription> inProcess;
        local.getVST3Format()->findAllTypesForFile (inProcess, LM_VST3_SHELL);
        expectEquals (inProcess.size(), 3);
        ScanFixture f;
        juce::ThreadPool pool (1);
        ScanJob shell (f, LM_VST3_SHELL);
        pool.addJob (&shell, false);
        expect (shell.finished.wait (20000));
        expect (pool.removeJob (&shell, true, 5000));
        expect (shell.succeeded);
        expect (descriptions (inProcess) == descriptions (shell.types));
        auto results = f.scanner->getResults();
        expectEquals ((int) results.size(), 1);
        if (results.empty()) return;
        expect (results.back().outcome == Outcome::completed);
        expect (results.back().onMessageThread);
        expect (results.back().workerPid != GetCurrentProcessId());
        expect (processRunning (results.back().workerPid));

        beginTest ("two workers coexist without Enqueue single-instance command forwarding");
        {
            ScanFixture other;
            juce::OwnedArray<juce::PluginDescription> types;
            expect (other.scan (LM_VST3_SHELL, types));
            expect (descriptions (types) == descriptions (inProcess));
            expect (other.scanner->getResults().back().workerPid != results.back().workerPid);
            expectEquals (workerCount(), 2);
        }
        expectEquals (workerCount(), 1);

        beginTest ("crash preserves the test process, blacklists one file and restarts for the next file");
        juce::OwnedArray<juce::PluginDescription> crashed;
        expect (! f.scan (LM_VST3_CRASHER, crashed));
        const auto crash = f.scanner->getResults().back();
        expect (crash.outcome == Outcome::crashed);
        expect (crash.exitCode != 0 && crash.exitCode != STILL_ACTIVE);
        expect (! processRunning (crash.workerPid));
        expect (f.host.getKnownPlugins().getBlacklistedFiles().contains (LM_VST3_CRASHER));
        expect (f.scanner->getSkippedMessage().contains (juce::String::fromUTF8 ("1개는 건너뛰었습니다")));
        expect (f.scanner->getSkippedMessage().contains ("LiveMix Test Crasher.vst3"));
        juce::OwnedArray<juce::PluginDescription> afterCrash;
        expect (f.scan (LM_VST3_SHELL, afterCrash));
        expect (descriptions (afterCrash) == descriptions (inProcess));
        expect (f.scanner->getResults().back().workerPid != crash.workerPid);
        f.host.getKnownPlugins().scanFinished();
        expectEquals (workerCount(), 0);

        beginTest ("Enqueue log lives under its settings folder and records UTF-8 crash details");
        const auto log = f.logFile().loadFileAsString();
        logMessage ("Enqueue scan log: " + f.logFile().getFullPathName() + "\n" + log);
        expect (f.logFile().existsAsFile());
        expectEquals (f.logFile().getRelativePathFrom (f.scratch.directory).replaceCharacter ('\\', '/'),
                      juce::String ("Enqueue/logs/plugin-scan.log"));
        expect (log.contains (juce::String::fromUTF8 ("튕김(종료 코드 0x")));
        expect (log.contains ("\tVST3\t"));
        expect (! f.scratch.directory.getChildFile ("LiveMix").exists());

        beginTest ("rescan clears the blacklist and stale crash marker before directory scanning");
        const auto marker = f.scratch.directory.getChildFile ("RecentlyCrashedPluginsList");
        expect (marker.replaceWithText (LM_VST3_CRASHER));
        f.scanner->prepareForScan (f.host.getKnownPlugins(), marker);
        expect (! marker.exists());
        expect (f.host.getKnownPlugins().getBlacklistedFiles().isEmpty());
        expect (f.scanner->getResults().empty());
        {
            juce::PluginDirectoryScanner directory (f.host.getKnownPlugins(), *f.host.getVST3Format(), {}, false, marker, true);
            directory.setFilesOrIdentifiersToScan ({ LM_VST3_CRASHER });
            juce::String scannedName;
            directory.scanNextFile (false, scannedName);
            directory.setFilesOrIdentifiersToScan ({ LM_VST3_EMPTY });
            directory.scanNextFile (false, scannedName);
            expect (directory.getFailedFiles().contains (LM_VST3_EMPTY));
        }
        results = f.scanner->getResults();
        expectEquals ((int) results.size(), 2);
        if (results.size() == 2)
        {
            expect (results[0].outcome == Outcome::crashed);
            expect (results[1].outcome == Outcome::completed);
            expectEquals (results[1].numTypes, 0);
            expect (results[1].onMessageThread);
        }
        expect (! f.host.getKnownPlugins().getBlacklistedFiles().contains (LM_VST3_EMPTY));
        expectEquals (workerCount(), 0);

        beginTest ("hung plugin times out and the next file can use a new Enqueue worker");
        {
            ScanFixture timeout (1800);
            juce::OwnedArray<juce::PluginDescription> types;
            expect (! timeout.scan (LM_VST3_HANG, types));
            const auto result = timeout.scanner->getResults().back();
            expect (result.outcome == Outcome::timedOut);
            expect (result.workerPid != 0);
            expect (! processRunning (result.workerPid));
            expect (timeout.host.getKnownPlugins().getBlacklistedFiles().contains (LM_VST3_HANG));
            expect (timeout.scan (LM_VST3_SHELL, types));
        }
        expectEquals (workerCount(), 0);

        beginTest ("Enqueue scan dialog cancellation stops a hung file without blacklisting");
        {
            ScanFixture cancel;
            juce::OwnedArray<juce::PluginDescription> warmup;
            cancel.scan (LM_VST3_EMPTY, warmup);
            const auto pid = cancel.scanner->getResults().back().workerPid;
            cancel.scanner->prepareForScan (cancel.host.getKnownPlugins());
            juce::PropertiesFile::Options propertiesOptions;
            propertiesOptions.millisecondsBeforeSaving = -1;
            juce::PropertiesFile properties (cancel.scratch.directory.getChildFile ("Enqueue/Enqueue.settings"), propertiesOptions);
            PluginScanListComponent list (cancel.host.getFormatManager(), cancel.host.getKnownPlugins(), marker,
                                          *cancel.scanner, juce::String::fromUTF8 ("Enqueue 플러그인 스캔"), &properties);
            list.scanFor (*cancel.host.getVST3Format(), { LM_VST3_HANG });
            auto* progress = juce::Component::getCurrentlyModalComponent();
            expect (progress != nullptr);
            juce::Thread::sleep (150);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            if (progress != nullptr) progress->exitModalState (0);
            expect (pumpUntil ([&] { return ! list.isScanning(); }));
            expect (juce::Time::getMillisecondCounterHiRes() - start < 2000);
            const auto cancelled = cancel.scanner->getResults();
            expectEquals ((int) cancelled.size(), 1);
            if (! cancelled.empty()) expect (cancelled.back().outcome == Outcome::cancelled);
            expect (! cancel.host.getKnownPlugins().getBlacklistedFiles().contains (LM_VST3_HANG));
            expect (! processRunning (pid));
            expectEquals (workerCount(), 0);
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
            pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() == nullptr; });
        }
    }
};

class EnqueuePluginListResetTests : public juce::UnitTest
{
public:
    EnqueuePluginListResetTests() : UnitTest ("Enqueue plugin list reset", "Enqueue") {}
    void runTest() override
    {
        beginTest ("all buttons fit the minimum width and show mode locks list reset");
        Scratch scratch;
        juce::PropertiesFile::Options options;
        options.millisecondsBeforeSaving = -1;
        juce::PropertiesFile storage (scratch.directory.getChildFile ("Enqueue/Enqueue.settings"), options);
        AppSettings settings (storage);
        PluginHost host;
        juce::OwnedArray<juce::PluginDescription> types;
        host.getKnownPlugins().scanAndAddFile (LM_VST3_SHELL, false, types, *host.getVST3Format());
        if (types.isEmpty()) { expect (false, "fixture was not scanned"); return; }
        host.setPluginEnabled (*types[0], false);
        settings.setDisabledPlugins (host.getDisabledPlugins());
        const auto disabled = host.getDisabledPlugins();
        host.getKnownPlugins().addToBlacklist (LM_VST3_CRASHER);
        const auto marker = settings.getDeadMansPedalFile();
        marker.getParentDirectory().createDirectory();
        PluginManagerWindow window (host, settings);
        // JUCE consumes an old marker when its list component is constructed.
        expect (marker.replaceWithText (LM_VST3_CRASHER));
        window.setSize (640, 420);
        auto* content = window.getContentComponent();
        auto* reset = resetButton (*content);
        expect (reset != nullptr);
        if (reset == nullptr) return;

        for (auto* child : content->getChildren())
            if (auto* button = dynamic_cast<juce::TextButton*> (child))
            {
                expect (content->getLocalBounds().contains (button->getBounds()));
                if (button != reset) expect (! button->getBounds().intersects (reset->getBounds()));
            }
        window.setLocked (true);
        expect (! reset->isEnabled());
        reset->onClick();
        expect (juce::Component::getCurrentlyModalComponent() == nullptr);
        expectEquals (host.getKnownPlugins().getNumTypes(), 3);
        window.setLocked (false);
        expect (reset->isEnabled());

        beginTest ("cancelling reset leaves the catalogue and crash records intact");
        reset->onClick();
        expect (pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() != nullptr; }));
        if (auto* alert = juce::Component::getCurrentlyModalComponent()) alert->exitModalState (0);
        pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() == nullptr; });
        expectEquals (host.getKnownPlugins().getNumTypes(), 3);
        expect (host.getKnownPlugins().getBlacklistedFiles().contains (LM_VST3_CRASHER));
        expect (marker.existsAsFile());

        beginTest ("scan preparation clears crash records, keeps saved paths and refuses reset while scanning");
        PluginScanListComponent* scanner = nullptr;
        juce::TextButton* scan = nullptr;
        for (auto* child : content->getChildren())
        {
            if (auto* list = dynamic_cast<PluginScanListComponent*> (child)) scanner = list;
            if (auto* button = dynamic_cast<juce::TextButton*> (child))
                if (button->getButtonText() == juce::String::fromUTF8 ("VST3 스캔...")) scan = button;
        }
        expect (scanner != nullptr && scan != nullptr);
        if (scanner != nullptr && scan != nullptr)
        {
            const auto folder = scratch.directory.getChildFile ("scan-folder");
            folder.createDirectory();
            const juce::FileSearchPath savedPath (folder.getFullPathName());
            juce::PluginListComponent::setLastSearchPath (storage, *host.getVST3Format(), savedPath);
            scan->onClick(); // stop in the folder dialog: the test runner is not a worker executable
            expect (scanner->isScanning());
            expect (host.getKnownPlugins().getBlacklistedFiles().isEmpty());
            expect (! marker.exists());
            auto* chooser = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
            expect (chooser != nullptr);
            if (chooser != nullptr)
            {
                auto* paths = dynamic_cast<juce::FileSearchPathListComponent*> (chooser->getCustomComponent (0));
                expect (paths != nullptr);
                if (paths != nullptr) expectEquals (paths->getPath().toString(), savedPath.toString());
                reset->onClick();
                expect (juce::Component::getCurrentlyModalComponent() == chooser);
                expectEquals (host.getKnownPlugins().getNumTypes(), 3);
                chooser->exitModalState (0);
            }
            expect (pumpUntil ([&] { return ! scanner->isScanning(); }));
        }
        host.getKnownPlugins().addToBlacklist (LM_VST3_CRASHER);
        expect (marker.replaceWithText (LM_VST3_CRASHER));

        beginTest ("confirmed reset clears catalogue and crash records while preserving use switches");
        reset->onClick();
        expect (pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() != nullptr; }));
        if (auto* alert = juce::Component::getCurrentlyModalComponent()) alert->exitModalState (1);
        expect (pumpUntil ([&] { return host.getKnownPlugins().getNumTypes() == 0; }));
        expect (host.getKnownPlugins().getBlacklistedFiles().isEmpty());
        expect (! marker.exists());
        expect (host.getDisabledPlugins() == disabled);
        expect (settings.getDisabledPlugins() == disabled);
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() == nullptr; });
    }
};

static EnqueuePluginScanTests enqueuePluginScanTests;
static EnqueuePluginListResetTests enqueuePluginListResetTests;
} // namespace gocue::tests
