#include "audio/PluginScan.h"
#include "audio/PluginHost.h"
#include "PluginRuntimeTestSupport.h"   // last: it brings in <windows.h>

namespace gocue::tests
{
using namespace runtime;

class PluginRuntimeCheckTests : public juce::UnitTest
{
public:
    PluginRuntimeCheckTests() : UnitTest ("PluginRuntimeCheck PE and missing runtimes", "Enqueue") {}
    void runTest() override
    {
        beginTest ("Arin PC: WaveShell loads bundles and mkl_waves imports the missing VC++ 2012 runtime");
        Fixture waves;
        const auto shell = waves.waves();
        auto result = waves.check (shell);
        expectPackage (result, 1, "msvcr110.dll", waves.additional().getChildFile ("mkl_waves.dll"));

        beginTest ("system, adjacent, application and Waves additional folders satisfy exact DLL names");
        for (const auto& folder : { waves.options.environment.windowsSystemDirectory, waves.additional(),
                                    waves.options.environment.applicationDirectory })
        {
            const auto dll = folder.getChildFile ("msvcr110.dll");
            expect (touch (dll));
            expect (waves.check (shell).packages.empty());
            dll.deleteFile();
        }
        const auto direct = waves.plugin ("WaveShell1-VST3-ARA 17.1_x64.vst3");
        expect (write (direct, pe ({ "msvcp120.dll" })));
        expect (touch (waves.additional().getChildFile ("msvcp120.dll")));
        result = waves.check (direct);
        expectEquals ((int) result.packages.size(), 1); // 2012 only; the AdditionalDLLs copy satisfies 2013
        if (! result.packages.empty()) expectEquals (result.packages[0].name, pluginRuntimePackages()[1].name);

        beginTest ("Waves major version and both Program Files roots are inspected and imports are cached per batch");
        waves.additional().getChildFile ("msvcp120.dll").deleteFile();
        expect (write (direct, pe ({ "KERNEL32.dll" })));
        const auto dependency = waves.options.environment.programFiles.getChildFile ("Waves/Plug-Ins V17/Other.bundle/Contents/module.dll");
        expect (write (dependency, pe ({ "msvcp120.dll" })));
        const auto wrongVersion = waves.options.environment.programFiles.getChildFile ("Waves/Plug-Ins V16/ignored.dll");
        expect (write (wrongVersion, pe ({ "msvcr100.dll" })));
        std::map<juce::String, int> reads;
        waves.options.onFileRead = [&] (const juce::File& file) { ++reads[file.getFullPathName()]; };
        PluginRuntimeCheck batch (waves.options);
        result = batch.check (shell.getFullPathName(), "VST3");
        expectEquals ((int) result.packages.size(), 2);
        result = batch.check (direct.getFullPathName(), "VST3");
        expectEquals ((int) result.packages.size(), 2);
        expectEquals (reads[waves.additional().getChildFile ("mkl_waves.dll").getFullPathName()], 1);
        expectEquals (reads[dependency.getFullPathName()], 1);
        expectEquals (reads[wrongVersion.getFullPathName()], 0);

        beginTest ("delayed imports and package order");
        Fixture delayed;
        expect (write (delayed.plugin(), pe ({ "msvcp120.dll", "msvcr100.dll" }, { "VCRUNTIME140_1.dll" })));
        result = delayed.check (delayed.plugin());
        expectEquals ((int) result.packages.size(), 3);
        if (result.packages.size() == 3)
        {
            expectEquals (result.packages[0].name, pluginRuntimePackages()[0].name);
            expectEquals (result.packages[1].name, pluginRuntimePackages()[2].name);
            expectEquals (result.packages[2].name, pluginRuntimePackages()[3].name);
            expectEquals (result.packages[2].evidence[0].dll, juce::String ("vcruntime140_1.dll"));
        }

        beginTest ("32-bit, ARM64, truncated and malformed PE data have no imports");
        Fixture invalid;
        for (const auto& block : { pe ({ "msvcr110.dll" }, {}, 0x14c, 0x10b), pe ({ "msvcr110.dll" }, {}, 0xaa64),
                                   juce::MemoryBlock ("junk", 4), juce::MemoryBlock (pe ({ "msvcr110.dll" }).getData(), 100) })
        {
            expect (write (invalid.plugin(), block));
            expect (invalid.check (invalid.plugin()).packages.empty());
        }
        const auto valid = pe ({ "msvcr110.dll" }, { "msvcp120.dll" });
        for (const auto size : { 0, 63, 150, 300, 510, 4095 })
        {
            expect (write (invalid.plugin(), juce::MemoryBlock (valid.getData(), (size_t) size)));
            expect (readX64PeImports (invalid.plugin()).isEmpty());
        }
        for (const auto offset : { 0x3c, 0x98 + 112 + 8, 0x200 + 12, 0x400 + 4, 0x188 + 20 })
        {
            auto corrupt = valid;
            memset (static_cast<char*> (corrupt.getData()) + offset, 0xff, 4);
            expect (write (invalid.plugin(), corrupt));
            expect (readX64PeImports (invalid.plugin()).isEmpty());
        }
        auto noTerminator = valid;
        memset (static_cast<char*> (noTerminator.getData()) + 0x700, 'a', 261);
        expect (write (invalid.plugin(), noTerminator));
        expect (readX64PeImports (invalid.plugin()).isEmpty());

        beginTest ("a non-Waves VST3 bundle, exact names and recursive VST2 sibling DLLs");
        Fixture bundle;
        const auto binary = bundle.plugin ("X.vst3/Contents/x86_64-win/X.vst3");
        expect (write (binary, pe ({ "MSVCP120.dll" })));
        expectPackage (bundle.check (bundle.plugin ("X.vst3")), 2, "msvcp120.dll", binary);
        expect (touch (bundle.options.environment.windowsSystemDirectory.getChildFile ("msvcp110_win.dll")));
        expect (write (bundle.plugin(), pe ({ "MSVCP110.dll" })));
        expectPackage (bundle.check (bundle.plugin()), 1, "msvcp110.dll", bundle.plugin());
        expect (touch (bundle.options.environment.windowsSystemDirectory.getChildFile ("msvcp120_clr0400.dll")));
        expect (write (bundle.plugin(), pe ({ "MSVCP120.dll" })));
        expectPackage (bundle.check (bundle.plugin()), 2, "msvcp120.dll", bundle.plugin());
        expect (write (bundle.plugin(), pe ({ "core.dll" })));
        const auto core = bundle.plugin ("core.dll");
        expect (write (core, pe ({ "msvcr100.dll", "plugin.dll" })));
        expectPackage (bundle.check (bundle.plugin()), 0, "msvcr100.dll", core);

        beginTest ("all candidates present opens no plugin files; file/time/cancellation bounds stop traversal");
        Fixture fast;
        fast.waves();
        int opens = 0;
        fast.options.onFileRead = [&] (const juce::File&) { ++opens; };
        for (const auto& package : pluginRuntimePackages())
            for (const auto& dll : package.dlls) expect (touch (fast.options.environment.windowsSystemDirectory.getChildFile (dll)));
        expect (fast.check (fast.plugin (shell.getFileName())).packages.empty());
        expectEquals (opens, 0);
        fast.options.environment.windowsSystemDirectory.getChildFile ("msvcr100.dll").deleteFile();
        expect (write (fast.plugin(), pe ({ "msvcr100.dll", "core.dll" })));
        expect (write (fast.plugin ("core.dll"), pe ({ "msvcr100.dll" })));
        expectPackage (fast.check (fast.plugin()), 0, "msvcr100.dll", fast.plugin());
        expectEquals (opens, 1, "all remaining candidates were found before opening the next dependency");
        bundle.options.maxFiles = 1;
        expect (bundle.check (bundle.plugin()).packages.empty());
        bundle.options.maxFiles = 20000;
        bundle.options.maxMilliseconds = 0;
        expect (bundle.check (bundle.plugin()).packages.empty());
        bundle.options.maxMilliseconds = 20000;
        expect (PluginRuntimeCheck (bundle.options).check (bundle.plugin().getFullPathName(), "VST", [] { return true; }).packages.empty());

        beginTest ("a 227 MB DLL is read by metadata ranges only");
        Fixture large;
        expect (write (large.plugin(), pe ({ "msvcr110.dll" })));
        {
            juce::FileOutputStream output (large.plugin());
            expect (output.setPosition (227LL * 1024 * 1024 - 1));
            expect (output.writeByte (0));
        }
        juce::int64 bytesRead = 0;
        expect (readX64PeImports (large.plugin(), &bytesRead).contains ("msvcr110.dll"));
        expect (bytesRead < 8192, "must not read the whole DLL");

        beginTest ("installed System32 msvcp140.dll imports vcruntime140.dll");
        const auto systemDll = PluginRuntimeEnvironment::current().windowsSystemDirectory.getChildFile ("msvcp140.dll");
        if (systemDll.existsAsFile()) expect (readX64PeImports (systemDll).contains ("vcruntime140.dll"));
        else logMessage ("SKIP: System32/msvcp140.dll is not installed");
    }

private:
    void expectPackage (const PluginRuntimeResult& result, size_t index, const juce::String& dll, const juce::File& importer)
    {
        expectEquals ((int) result.packages.size(), 1);
        if (result.packages.size() != 1) return;
        expectEquals (result.packages[0].name, pluginRuntimePackages()[index].name);
        expectEquals (result.packages[0].url, pluginRuntimePackages()[index].url);
        expectEquals ((int) result.packages[0].evidence.size(), 1);
        if (result.packages[0].evidence.empty()) return;
        expectEquals (result.packages[0].evidence[0].dll, dll);
        expect (result.packages[0].evidence[0].importedBy == importer);
    }
};

class PluginRuntimeCoordinatorTests : public juce::UnitTest
{
public:
    PluginRuntimeCoordinatorTests() : UnitTest ("PluginRuntimeCheck coordinator crash diagnostics", "Enqueue") {}
    void runTest() override
    {
        for (const auto* executable : { ENQUEUE_SCAN_WORKER_EXE, LM_SCAN_WORKER_EXE })
        {
            beginTest (juce::File (executable).getFileName() + " crash attaches evidence immediately after the original log line");
            Fixture f;
            const auto crashedBundle = f.plugin ("LiveMix Test Crasher.vst3");
            expect (juce::File (LM_VST3_CRASHER).copyDirectoryTo (crashedBundle));
            const auto importer = crashedBundle.getChildFile ("Contents/x86_64-win/runtime-evidence.dll");
            expect (write (importer, pe ({ "MSVCR110.dll" })));
            PluginScanCoordinator::Options options;
            options.executable = juce::File (executable);
            options.logFile = f.root.getChildFile ("scan.log");
            options.runtimeCheck = f.options;
            options.timeoutMs = 15000;
            PluginHost host;
            auto owned = std::make_unique<PluginScanCoordinator> (options);
            auto* scan = owned.get();
            host.getKnownPlugins().setCustomScanner (std::move (owned));
            juce::OwnedArray<juce::PluginDescription> types;
            expect (! host.getKnownPlugins().scanAndAddFile (crashedBundle.getFullPathName(), false, types, *host.getVST3Format()));
            const auto results = scan->getResults();
            expectEquals ((int) results.size(), 1);
            if (! results.empty())
            {
                expect (results[0].outcome == PluginScanCoordinator::Outcome::crashed);
                expect (results[0].runtime.has_value());
            }
            const auto diagnostics = scan->getRuntimeResults();
            expectEquals ((int) diagnostics.size(), 1);
            if (! diagnostics.empty()) expectEquals ((int) diagnostics[0].packages.size(), 1);
            expectEquals (scan->getRuntimeMissingFile().loadFileAsString(), crashedBundle.getFullPathName() + "\n");
            auto lines = juce::StringArray::fromLines (options.logFile.loadFileAsString());
            lines.removeEmptyStrings();
            expectEquals (lines.size(), 2);
            if (lines.size() == 2)
            {
                expect (lines[0].contains (ko ("튕김(종료 코드 0x")));
                expect (lines[1].contains ("\tVST3\t" + crashedBundle.getFullPathName() + "\t" + ko ("런타임 없음: ")
                                         + pluginRuntimePackages()[1].name + ko (" (msvcr110.dll ← ") + importer.getFullPathName() + ")\t"));
                expect (lines[1].endsWith (" ms"));
            }
            host.getKnownPlugins().scanFinished();
            scan->prepareForScan (host.getKnownPlugins());
            expect (scan->getRuntimeResults().empty());
            expect (! scan->getRuntimeMissingFile().exists());

            beginTest ("the real environment adds no runtime line when nothing is missing");
            options.runtimeCheck = {};
            auto real = std::make_unique<PluginScanCoordinator> (options);
            auto* realScan = real.get();
            host.getKnownPlugins().setCustomScanner (std::move (real));
            expect (! host.getKnownPlugins().scanAndAddFile (LM_VST3_CRASHER, false, types, *host.getVST3Format()));
            auto realLines = juce::StringArray::fromLines (options.logFile.loadFileAsString());
            realLines.removeEmptyStrings();
            expectEquals (realLines.size(), 3);
            expect (realLines[realLines.size() - 1].contains (ko ("튕김(종료 코드 0x")));
            expect (! options.logFile.loadFileAsString().contains (ko ("런타임 확인")));
            host.getKnownPlugins().scanFinished();

            beginTest ("a timed out plugin gets the same runtime diagnostic");
            {
                auto timeoutOptions = options;
                timeoutOptions.timeoutMs = 1800;
                PluginScanCoordinator timeout (timeoutOptions);
                timeout.findPluginTypesFor (*host.getVST3Format(), types, LM_VST3_HANG);
                const auto outcomes = timeout.getResults();
                expectEquals ((int) outcomes.size(), 1);
                if (! outcomes.empty())
                {
                    expect (outcomes[0].outcome == PluginScanCoordinator::Outcome::timedOut);
                    expect (outcomes[0].runtime.has_value());
                }
            }

            beginTest ("zero plugins are diagnosed, while cancellation and worker startup failure are not");
            realScan->prepareForScan (host.getKnownPlugins());
            realScan->findPluginTypesFor (*host.getVST3Format(), types, LM_VST3_EMPTY);
            expectEquals ((int) realScan->getRuntimeResults().size(), 1);
            realScan->scanFinished();
            realScan->prepareForScan (host.getKnownPlugins());
            realScan->cancel();
            realScan->findPluginTypesFor (*host.getVST3Format(), types, LM_VST3_EMPTY);
            expect (realScan->getRuntimeResults().empty());
            options.executable = f.root.getChildFile ("missing.exe");
            PluginScanCoordinator missingWorker (options);
            missingWorker.findPluginTypesFor (*host.getVST3Format(), types, LM_VST3_EMPTY);
            expect (missingWorker.getRuntimeResults().empty());
        }
    }
};

class PluginRuntimeMissingFileTests : public juce::UnitTest
{
public:
    PluginRuntimeMissingFileTests() : UnitTest ("PluginRuntimeCheck missing-file persistence", "Enqueue") {}
    void runTest() override
    {
        beginTest ("completed zero-type scans persist unique UTF-8 paths without blacklisting them");
        Fixture f;
        const auto plugin = f.plugin (ko ("일반 플러그인.vst3"));
        expect (write (plugin, pe ({ "MSVCR110.dll" })));
        PluginScanCoordinator::Options options;
        options.executable = juce::File (ENQUEUE_SCAN_WORKER_EXE);
        options.logFile = f.root.getChildFile ("logs/plugin-scan.log");
        options.runtimeCheck = f.options;
        options.timeoutMs = 15000;
        PluginHost host;
        auto owned = std::make_unique<PluginScanCoordinator> (options);
        auto* scan = owned.get();
        host.getKnownPlugins().setCustomScanner (std::move (owned));
        const auto stateFile = f.root.getChildFile ("logs/plugin-scan-runtime-missing.txt");
        expect (scan->getRuntimeMissingFile() == stateFile);

        expect (scan->getRuntimeCheckFiles ({}).isEmpty());
        juce::OwnedArray<juce::PluginDescription> types;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            expect (! host.getKnownPlugins().scanAndAddFile (plugin.getFullPathName(), false, types, *host.getVST3Format()));
            const auto results = scan->getResults();
            expect (! results.empty());
            if (! results.empty())
            {
                expect (results.back().outcome == PluginScanCoordinator::Outcome::completed);
                expectEquals (results.back().numTypes, 0);
                expect (results.back().runtime && ! results.back().runtime->packages.empty());
            }
        }
        expect (host.getKnownPlugins().getBlacklistedFiles().isEmpty());
        const auto expected = plugin.getFullPathName() + "\n";
        expectEquals (stateFile.loadFileAsString(), expected);
        expectEquals (stateFile.getSize(), (juce::int64) expected.getNumBytesAsUTF8());

        beginTest ("log rotation preserves the path list and reopening merges it with the blacklist");
        expect (options.logFile.replaceWithText (juce::String::repeatedString ("x", 1024 * 1024 + 1)));
        const auto another = f.plugin ("Another.vst3");
        expect (write (another, pe ({ "MSVCR110.dll" })));
        expect (! host.getKnownPlugins().scanAndAddFile (another.getFullPathName(), false, types, *host.getVST3Format()));
        host.getKnownPlugins().scanFinished();
        expect (options.logFile.getSiblingFile ("plugin-scan.log.1").existsAsFile());
        expectEquals (stateFile.loadFileAsString(), expected + another.getFullPathName() + "\n");
        PluginScanCoordinator reopened (options);
        const auto blacklisted = f.plugin ("Blacklisted.vst3").getFullPathName();
        const auto combined = reopened.getRuntimeCheckFiles ({ plugin.getFullPathName(), blacklisted });
        expectEquals (combined.size(), 3);
        expect (combined.contains (plugin.getFullPathName()) && combined.contains (another.getFullPathName()) && combined.contains (blacklisted));

        beginTest ("preparing a new scan deletes remembered paths; missing and damaged state is ignored");
        reopened.prepareForScan (host.getKnownPlugins());
        expect (! stateFile.exists());
        expect (reopened.getRuntimeCheckFiles ({}).isEmpty());
        expect (write (stateFile, juce::MemoryBlock ("\xff\xfe", 2)));
        expect (reopened.getRuntimeCheckFiles ({}).isEmpty());
        expect (stateFile.replaceWithText ("not an absolute path\n"));
        expect (reopened.getRuntimeCheckFiles ({}).isEmpty());
        expect (reopened.getRuntimeCheckFiles ({ blacklisted }) == juce::StringArray { blacklisted });
    }
};

static PluginRuntimeCheckTests pluginRuntimeCheckTests;
static PluginRuntimeCoordinatorTests pluginRuntimeCoordinatorTests;
static PluginRuntimeMissingFileTests pluginRuntimeMissingFileTests;
} // namespace gocue::tests
