#include "ObsPluginInstaller.h"
#include "../livemix/src/ui/MainComponent.h"
#include "lm_obs_protocol.h"

#include <windows.h>

namespace gocue::tests
{
using namespace gocue::livemix;
using Installer = ObsPluginInstaller;
using Result = Installer::Result;

namespace
{
    struct Fixture
    {
        juce::File temp = juce::File::createTempFile ("-obs-install");
        Installer::Roots roots;
        bool running = false;
        bool ready = true;

        explicit Fixture (int major = 32)
        {
            temp.deleteFile();
            roots.programData = temp.getChildFile ("ProgramData");
            roots.obsInstallDir = temp.getChildFile (juce::String::fromUTF8 ("OBS 설치"));
            roots.bundledPlugin = temp.getChildFile (juce::String::fromUTF8 ("LiveMix 설치/obs-plugin/livemix-obs"));
            roots.isObsRunning = [this] { return running; };
            put (juce::File (LM_OBS_TEST_CURRENT_DLL), roots.bundledPlugin.getChildFile ("livemix-obs.dll"));
            auto locale = roots.bundledPlugin.getChildFile ("data/locale");
            ready &= locale.createDirectory().wasOk();
            ready &= locale.getChildFile ("ko-KR.ini").replaceWithText (juce::String::fromUTF8 ("Name=LiveMix 마스터\n"));
            ready &= locale.getChildFile ("en-US.ini").replaceWithText ("Name=LiveMix Master\n");
            if (major != 0)
                put (juce::File (major >= 33 ? LM_OBS_TEST_OBS33_DLL : LM_OBS_TEST_OBS32_DLL),
                     roots.obsInstallDir.getChildFile ("bin/64bit/obs64.exe"));
        }
        ~Fixture() { temp.deleteRecursively(); }
        void put (const juce::File& source, const juce::File& dest)
        {
            ready &= dest.getParentDirectory().createDirectory().wasOk();
            ready &= source.copyFileTo (dest);
        }
        juce::File plugin() const { return roots.programData.getChildFile ("obs-studio/plugins/livemix-obs"); }
        juce::File legacy() const { return plugin().getChildFile ("bin/64bit/livemix-obs.dll"); }
        juce::File modern() const { return plugin().getChildFile ("livemix-obs.dll"); }
        void oldDll (const juce::File& dest) { put (juce::File (LM_OBS_TEST_OLD_DLL), dest); }
        int count (const juce::String& pattern) const
        {
            return plugin().findChildFiles (juce::File::findFiles, true, pattern).size();
        }
    };

    struct HeldFile
    {
        explicit HeldFile (const juce::File& file, bool shareDelete)
            : handle (CreateFileW (file.getFullPathName().toWideCharPointer(), GENERIC_READ,
                                  FILE_SHARE_READ | (shareDelete ? FILE_SHARE_DELETE : 0), nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
        ~HeldFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle (handle); }
        HANDLE handle;
    };

    template <typename Predicate>
    bool until (Predicate done, int timeoutMs = 2500)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        do
        {
            if (done()) return true;
            MSG message;
            for (int i = 0; i < 32 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++i)
            {
                TranslateMessage (&message);
                DispatchMessageW (&message);
            }
            juce::Thread::sleep (2);
        } while (juce::Time::getMillisecondCounterHiRes() < deadline);
        return done();
    }

    juce::ToggleButton* obsToggle (gocue::livemix::MainComponent& main)
    {
        for (auto* child : main.getChildren())
            if (auto* master = dynamic_cast<MasterCard*> (child))
                for (auto* control : master->getChildren())
                    if (auto* toggle = dynamic_cast<juce::ToggleButton*> (control)) return toggle;
        return nullptr;
    }

    bool hasObsStatus (gocue::livemix::MainComponent& main, const char* utf8)
    {
        for (auto* child : main.getChildren())
            if (auto* master = dynamic_cast<MasterCard*> (child))
                for (auto* control : master->getChildren())
                    if (auto* label = dynamic_cast<juce::Label*> (control); label != nullptr && label->getTooltip() == juce::String::fromUTF8 (utf8)) return true;
        return false;
    }
}

class LiveMixObsInstallerTests : public juce::UnitTest
{
public:
    LiveMixObsInstallerTests() : juce::UnitTest ("LiveMix OBS installer", "LiveMix") {}

    void runTest() override
    {
        juce::String message;
        beginTest ("OBS 32 uses the legacy layout and native VERSIONINFO; Korean paths and locales survive");
        {
            Fixture f;
            expect (f.ready);
            expectEquals (Installer::obsMajorVersion (f.roots), 32);
            expect (! Installer::isInstalledAndCurrent (f.roots));
            expect (Installer::install (f.roots, message) == Result::installed, message);
            expect (f.legacy().existsAsFile() && ! f.modern().exists());
            expect (Installer::isInstalledAndCurrent (f.roots));
            expectEquals (f.plugin().getChildFile ("data/locale/ko-KR.ini").loadFileAsString().replace ("\r\n", "\n"),
                          juce::String::fromUTF8 ("Name=LiveMix 마스터\n"));
            expect (f.plugin().getChildFile ("data/locale/ko-KR.ini").hasIdenticalContentTo (f.roots.bundledPlugin.getChildFile ("data/locale/ko-KR.ini")));
            expect (f.plugin().getChildFile ("data/locale/en-US.ini").existsAsFile());
            expectEquals (message, juce::String::fromUTF8 ("OBS 플러그인을 설치했습니다. OBS를 켜고 소스(+)에서 'LiveMix 마스터'를 추가하거나, 오디오 소스의 필터에서 'LiveMix 마스터 받기'를 추가하세요."));
        }

        beginTest ("OBS 33 migrates legacy to the new layout; OBS 32 migrates back without duplicate DLLs");
        for (int major : { 32, 33 })
        {
            Fixture f (major);
            auto target = major >= 33 ? f.modern() : f.legacy();
            auto other = major >= 33 ? f.legacy() : f.modern();
            f.oldDll (other);
            expect (f.ready);
            expectEquals (Installer::obsMajorVersion (f.roots), major);
            expect (Installer::install (f.roots, message) == Result::installed, message);
            expect (target.existsAsFile() && ! other.exists());
            expectEquals (f.count ("*.old-*"), 0);
            expectEquals (f.count ("*.tmp-*"), 0);
        }

        beginTest ("Same version is alreadyCurrent with no copies or file timestamp changes; newer is not downgraded");
        {
            Fixture f;
            expect (Installer::install (f.roots, message) == Result::installed, message);
            const auto fixedTime = juce::Time::getCurrentTime() - juce::RelativeTime::days (2);
            auto files = f.plugin().findChildFiles (juce::File::findFiles, true);
            for (const auto& file : files) expect (file.setLastModificationTime (fixedTime));
            int copies = 0;
            f.roots.copyFile = [&] (const auto&, const auto&) { ++copies; return (juce::uint32) ERROR_ACCESS_DENIED; };
            expect (Installer::install (f.roots, message) == Result::alreadyCurrent, message);
            for (const auto& file : files) expectEquals (file.getLastModificationTime().toMilliseconds(), fixedTime.toMilliseconds());
            expectEquals (copies, 0);
            f.put (juce::File (LM_OBS_TEST_OLD_DLL), f.roots.bundledPlugin.getChildFile ("livemix-obs.dll"));
            expect (Installer::isInstalledAndCurrent (f.roots));
            expect (Installer::install (f.roots, message) == Result::alreadyCurrent, message);
            expectEquals (copies, 0);
        }

        beginTest ("A loaded older DLL sharing delete is renamed, replaced and retained until OBS stops");
        {
            Fixture f;
            f.running = true;
            f.oldDll (f.legacy());
            expect (f.ready);
            {
                HeldFile held (f.legacy(), true);
                expect (held.handle != INVALID_HANDLE_VALUE);
                expect (Installer::install (f.roots, message) == Result::installedRestartObs, message);
                expect (Installer::isInstalledAndCurrent (f.roots));
                expectEquals (f.count ("livemix-obs.dll.old-*"), 1);
                expectEquals (f.count ("*.tmp-*"), 0);
                expectEquals (message, juce::String::fromUTF8 ("OBS 플러그인을 설치했습니다. OBS를 다시 시작하면 연결됩니다."));
                auto retired = f.plugin().findChildFiles (juce::File::findFiles, true, "livemix-obs.dll.old-*");
                if (! retired.isEmpty()) expect (retired[0].hasIdenticalContentTo (juce::File (LM_OBS_TEST_OLD_DLL)));
            }
            f.running = false;
            expect (Installer::install (f.roots, message) == Result::alreadyCurrent, message);
            expectEquals (f.count ("*.old-*"), 0);
        }

        beginTest ("A DLL without share-delete is untouched and asks to close OBS, including during layout migration");
        for (bool migrate : { false, true })
        {
            Fixture f (migrate ? 33 : 32);
            f.running = true;
            f.oldDll (f.legacy());
            auto previousTime = f.legacy().getLastModificationTime();
            HeldFile held (f.legacy(), false);
            expect (held.handle != INVALID_HANDLE_VALUE);
            expect (Installer::install (f.roots, message) == Result::obsBusyCloseIt, message);
            expectEquals (message, juce::String::fromUTF8 ("OBS가 예전 플러그인을 쓰고 있어 바꾸지 못했습니다. OBS를 종료한 뒤 다시 눌러 주세요."));
            expect (f.legacy().hasIdenticalContentTo (juce::File (LM_OBS_TEST_OLD_DLL)));
            expect (f.legacy().getLastModificationTime() == previousTime);
            expect (! f.modern().exists());
            expectEquals (f.count ("*.tmp-*"), 0);
            expectEquals (f.count ("*.old-*"), 0);
        }

        beginTest ("headless result paths and single elevation retry are shared with the UI helper");
        for (const auto& flags : { "", " --allow-elevation", " --allow-elevation --elevated-helper" })
        {
            Fixture f;
            f.roots.copyFile = [] (const juce::File&, const juce::File&) -> juce::uint32 { return ERROR_ACCESS_DENIED; };
            const auto report = f.temp.getChildFile (ko ("caller TEMP/고유 결과.txt"));
            const auto command = juce::String ("--install-obs-plugin") + flags + " --result " + report.getFullPathName().quoted();
            int elevations = 0;
            bool reported = false;
            const auto result = Installer::runInstallCommandLine (command, f.roots, message, reported, [&] (juce::String& reply)
            {
                ++elevations;
                reply = ko ("OBS를 다시 시작하세요");
                return Result::installedRestartObs;
            });
            const bool mayElevate = juce::String (flags) == " --allow-elevation";
            expectEquals (elevations, mayElevate ? 1 : 0);
            expect (reported && report.existsAsFile());
            expect (result == (mayElevate ? Result::installedRestartObs : Result::needsElevation));
            expect (Installer::readResult (report, message) == result);
            expect (Installer::resultFile (command) == report);
            expect (Installer::resultFile ("--install-obs-plugin") == Installer::resultFile());
            const auto elevated = Installer::elevatedCommandLine (report);
            expect (elevated.contains ("--elevated-helper") && ! elevated.contains ("--allow-elevation"));
            expect (Installer::resultFile (elevated) == report);
        }

        beginTest ("Access denied, including a partial locale copy, requests elevation and preserves the old layout");
        for (int failCopy : { 1, 3 })
        {
            Fixture f (33);
            f.oldDll (f.legacy());
            int copies = 0;
            f.roots.copyFile = [&] (const juce::File& from, const juce::File& to) -> juce::uint32
            {
                if (++copies == failCopy)
                {
                    to.replaceWithText ("partial copy");
                    return ERROR_ACCESS_DENIED;
                }
                return CopyFileW (from.getFullPathName().toWideCharPointer(), to.getFullPathName().toWideCharPointer(), TRUE)
                    ? ERROR_SUCCESS : GetLastError();
            };
            expect (Installer::install (f.roots, message) == Result::needsElevation, message);
            expectEquals (copies, failCopy);
            expect (f.legacy().hasIdenticalContentTo (juce::File (LM_OBS_TEST_OLD_DLL)));
            expect (! f.modern().exists());
            expectEquals (f.count ("*.tmp-*"), 0);
            expectEquals (f.count ("*.old-*"), 0);
            expectEquals (f.count ("*.ini"), 0);
        }

        beginTest ("OBS not found still installs legacy and explains how to use it later");
        {
            Fixture f (0);
            expectEquals (Installer::obsMajorVersion (f.roots), 0);
            expect (Installer::install (f.roots, message) == Result::installed, message);
            expect (f.legacy().existsAsFile() && ! f.modern().exists());
            expectEquals (message, juce::String::fromUTF8 ("OBS를 찾지 못했습니다. OBS를 설치하면 바로 쓸 수 있게 플러그인을 넣어 두었습니다."));
        }

        beginTest ("A locale locked during publication restores the retired DLL and all original data");
        {
            Fixture f;
            expect (Installer::install (f.roots, message) == Result::installed, message);
            f.oldDll (f.legacy());
            const auto locale = f.plugin().getChildFile ("data/locale/ko-KR.ini");
            expect (locale.replaceWithText ("previous locale"));
            HeldFile held (locale, false);
            expect (held.handle != INVALID_HANDLE_VALUE);
            expect (Installer::install (f.roots, message) == Result::failed, message);
            expect (message.startsWith (juce::String::fromUTF8 ("OBS 플러그인을 설치하지 못했습니다: ")));
            expect (f.legacy().hasIdenticalContentTo (juce::File (LM_OBS_TEST_OLD_DLL)));
            expectEquals (locale.loadFileAsString(), juce::String ("previous locale"));
            expect (! f.modern().exists());
            expectEquals (f.count ("*.tmp-*"), 0);
            expectEquals (f.count ("*.old-*"), 0);
        }

        beginTest ("Missing bundle or locales never creates an installation; an unversioned bundle is rejected");
        for (int missing : { 0, 1, 2 })
        {
            Fixture f;
            if (missing == 0) f.roots.bundledPlugin.getChildFile ("livemix-obs.dll").deleteFile();
            if (missing == 1) f.roots.bundledPlugin.getChildFile ("data").deleteRecursively();
            if (missing == 2) f.roots.bundledPlugin.getChildFile ("livemix-obs.dll").replaceWithText ("not a DLL");
            expect (! Installer::isInstalledAndCurrent (f.roots));
            expect (Installer::install (f.roots, message) == (missing == 2 ? Result::failed : Result::noBundledFiles), message);
            expect (! f.legacy().exists() && ! f.modern().exists());
            expect (message.startsWith (juce::String::fromUTF8 ("OBS 플러그인을 설치하지 못했습니다: ")));
        }

        beginTest ("Cleanup visits both layouts even when the current version needs no write");
        {
            Fixture f;
            expect (Installer::install (f.roots, message) == Result::installed, message);
            f.put (juce::File (LM_OBS_TEST_OLD_DLL), f.modern().getSiblingFile ("livemix-obs.dll.old-71"));
            f.put (juce::File (LM_OBS_TEST_OLD_DLL), f.legacy().getSiblingFile ("livemix-obs.dll.old-72"));
            f.running = true;
            expect (Installer::install (f.roots, message) == Result::alreadyCurrent, message);
            expectEquals (f.count ("*.old-*"), 2);
            f.running = false;
            expect (Installer::install (f.roots, message) == Result::alreadyCurrent, message);
            expectEquals (f.count ("*.old-*"), 0);
        }

        beginTest ("Single-instance bypass accepts only the exact install flag token");
        expect (Installer::isInstallCommandLine ("--install-obs-plugin"));
        expect (Installer::isInstallCommandLine ("  --safe-mode  --install-obs-plugin  "));
        expect (Installer::isInstallCommandLine ("\"--install-obs-plugin\""));
        for (const auto& args : { "", "--safe-mode", "--install-obs-plugin-extra", "--install-obs-plugin=true",
                                 "\"C:\\sessions\\--install-obs-plugin.livemix\"", "\"prefix --install-obs-plugin\"" })
            expect (! Installer::isInstallCommandLine (args), args);

        beginTest ("Headless reports round-trip all result names as one UTF-8 line and reject missing or malformed reports");
        {
            Fixture f;
            auto report = f.temp.getChildFile ("temp/LiveMix/obs-install-result.txt");
            for (auto result : { Result::alreadyCurrent, Result::installed, Result::installedRestartObs,
                                 Result::needsElevation, Result::obsBusyCloseIt, Result::noBundledFiles, Result::failed })
            {
                auto korean = juce::String::fromUTF8 ("설치 결과: 한글 경로");
                expect (Installer::writeResult (report, result, korean));
                expectEquals (report.loadFileAsString(), juce::String (Installer::resultName (result)) + "\t" + korean + "\n");
                expect (Installer::readResult (report, message) == result);
                expectEquals (message, korean);
            }
            expect (Installer::writeResult (report, Result::failed, "a\r\nb\tc"));
            expectEquals (report.loadFileAsString().retainCharacters ("\n").length(), 1);
            for (const auto& invalid : { "installed", "unknown\tmessage\n", "installed\t\n", "installed\tx\nfailed\ty\n" })
            {
                report.replaceWithText (invalid);
                expect (Installer::readResult (report, message) == Result::failed);
            }
            report.deleteFile();
            expect (Installer::readResult (report, message) == Result::failed);
        }
    }
};
static LiveMixObsInstallerTests liveMixObsInstallerTests;

class LiveMixObsInstallerUiTests : public juce::UnitTest
{
public:
    LiveMixObsInstallerUiTests() : juce::UnitTest ("LiveMix OBS installer UI", "LiveMix") {}
    void runTest() override
    {
        beginTest ("startup and session load with OBS enabled install once without unattended elevation");
        for (bool atStartup : { true, false })
        for (bool denied : { false, true })
        {
            Fixture f;
            if (! atStartup) f.oldDll (f.legacy());
            MixEngine engine ("Local\\LiveMix.ObsStartupTest." + juce::Uuid().toString());
            MixDocument document (engine);
            document.getSession().master.sendToObs = true;
            const auto sessionFile = f.temp.getChildFile ("enabled.livemix");
            expect (document.save (sessionFile).wasOk());
            document.getSession().master.sendToObs = atStartup;
            document.applyToEngine();
            LiveMixSettings settings (f.temp.getChildFile ("settings"));
            std::atomic<int> copies { 0 }, elevations { 0 };
            f.roots.copyFile = [&] (const juce::File& from, const juce::File& to) -> juce::uint32
            {
                ++copies;
                if (denied) return ERROR_ACCESS_DENIED;
                return CopyFileW (from.getFullPathName().toWideCharPointer(), to.getFullPathName().toWideCharPointer(), TRUE)
                    ? ERROR_SUCCESS : GetLastError();
            };
            ObsPluginActions actions;
            actions.roots = [&] { return f.roots; };
            actions.elevate = [&] (juce::String&) { ++elevations; return Result::needsElevation; };
            gocue::livemix::MainComponent main (document, settings, actions);
            if (! atStartup) expect (document.load (sessionFile).wasOk());
            auto* toggle = obsToggle (main);
            expect (until ([&] { return copies.load() > 0 && toggle != nullptr && toggle->getButtonText() != ko ("설치 중..."); }));
            const int once = copies.load();
            until ([] { return false; }, 650);
            expectEquals (copies.load(), once);
            expectEquals (elevations.load(), 0);
            if (denied)
            {
                expect (hasObsStatus (main, "OBS 플러그인 설치 필요"));
                for (auto* child : main.getChildren())
                    if (auto* master = dynamic_cast<MasterCard*> (child))
                        for (auto* control : master->getChildren())
                            if (auto* label = dynamic_cast<juce::Label*> (control); label != nullptr && label->getTooltip() == ko ("OBS 플러그인 설치 필요"))
                            {
                                const auto now = juce::Time::getCurrentTime();
                                const juce::MouseEvent click (juce::Desktop::getInstance().getMainMouseSource(), { 1.0f, 1.0f }, {},
                                    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, label, label, now, { 1.0f, 1.0f }, now, 1, false);
                                master->mouseUp (click);
                            }
                expect (until ([&] { return elevations.load() == 1 && toggle->getButtonText() != ko ("설치 중..."); }));
            }
            else
                expect (Installer::isInstalledAndCurrent (f.roots));
        }

        beginTest ("ring creation failure is shown on the master card with its reason");
        {
            Fixture f;
            juce::String installMessage;
            expect (Installer::install (f.roots, installMessage) == Result::installed);
            const auto mapping = "Local\\LiveMix.ObsFailureTest." + juce::Uuid().toString();
            HANDLE tiny = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, mapping.toWideCharPointer());
            expect (tiny != nullptr);
            {
                MixEngine engine (mapping);
                MixDocument document (engine);
                document.getSession().master.sendToObs = true;
                document.applyToEngine();
                LiveMixSettings settings (f.temp.getChildFile ("settings"));
                ObsPluginActions actions;
                actions.roots = [&] { return f.roots; };
                actions.elevate = [] (juce::String&) { return Result::needsElevation; };
                gocue::livemix::MainComponent main (document, settings, actions);
                expect (until ([&]
                {
                    for (auto* child : main.getChildren())
                        if (auto* master = dynamic_cast<MasterCard*> (child))
                            for (auto* control : master->getChildren())
                                if (auto* label = dynamic_cast<juce::Label*> (control); label != nullptr
                                    && label->getTooltip().contains (ko ("OBS 보내기 실패")) && label->getTooltip().contains ("Win32")) return true;
                    return false;
                }));
                expect (! engine.getObsSender().isEnabled());
            }
            if (tiny != nullptr) CloseHandle (tiny);
        }

        for (int mode : { 0, 1, 2 })
        {
            beginTest (mode == 0 ? "A blocked install leaves the message thread and sender running"
                       : mode == 1 ? "UAC decline stays orange, preserves sending and permits a retry"
                                   : "Elevation runs once on a worker; an old reader cannot clear the restart notice");
            Fixture f;
            f.running = mode == 2;
            const auto mapping = "Local\\LiveMix.ObsInstallerTest." + juce::Uuid().toString();
            MixEngine engine (mapping);
            MixDocument document (engine);
            document.applyToEngine();
            LiveMixSettings settings (f.temp.getChildFile ("settings"));
            juce::WaitableEvent copying;
            juce::WaitableEvent continueCopy { true };
            std::atomic<bool> wrongThread { false };
            std::atomic<int> elevations { 0 };
            const auto messageThread = juce::Thread::getCurrentThreadId();
            f.roots.copyFile = [&] (const juce::File& from, const juce::File& to) -> juce::uint32
            {
                wrongThread.store (wrongThread.load() || juce::Thread::getCurrentThreadId() == messageThread);
                copying.signal();
                continueCopy.wait (3000);
                if (mode != 0) return ERROR_ACCESS_DENIED;
                return CopyFileW (from.getFullPathName().toWideCharPointer(), to.getFullPathName().toWideCharPointer(), TRUE)
                    ? ERROR_SUCCESS : GetLastError();
            };
            ObsPluginActions actions;
            actions.roots = [&]
            {
                wrongThread.store (wrongThread.load() || juce::Thread::getCurrentThreadId() == messageThread);
                return f.roots;
            };
            actions.elevate = [&] (juce::String& message)
            {
                ++elevations;
                wrongThread.store (wrongThread.load() || juce::Thread::getCurrentThreadId() == messageThread);
                if (mode == 1)
                {
                    message = juce::String::fromUTF8 ("설치를 취소했습니다");
                    return Result::needsElevation;
                }
                auto elevatedRoots = f.roots;
                elevatedRoots.copyFile = {};
                return Installer::install (elevatedRoots, message);
            };
            gocue::livemix::MainComponent main (document, settings, actions);
            auto* toggle = obsToggle (main);
            expect (toggle != nullptr);
            if (toggle == nullptr) continue;
            expect (until ([&] { return hasObsStatus (main, "OBS 플러그인 설치 필요"); }));
            toggle->setToggleState (true, juce::dontSendNotification);
            toggle->onClick();
            expect (document.getSession().master.sendToObs && engine.getObsSender().isEnabled());
            expectEquals (toggle->getButtonText(), juce::String::fromUTF8 ("설치 중..."));
            expect (until ([&] { return copying.wait (0); }));

            auto* readerHandle = OpenFileMappingW (FILE_MAP_READ | FILE_MAP_WRITE, FALSE, (mapping + ".Readers").toWideCharPointer());
            auto* readers = readerHandle != nullptr ? static_cast<lm_obs_readers*> (MapViewOfFile (readerHandle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof (lm_obs_readers))) : nullptr;
            expect (readers != nullptr);
            auto connect = [&]
            {
                if (readers == nullptr) return;
                LARGE_INTEGER stamp {};
                QueryPerformanceCounter (&stamp);
                lm_obs_store_release (&readers->slot[0].heartbeat_qpc, stamp.QuadPart);
                lm_obs_store_release (&readers->slot[0].pid, GetCurrentProcessId());
            };
            if (mode == 2) connect();
            bool dispatched = false;
            juce::MessageManager::callAsync ([&] { dispatched = true; });
            expect (until ([&] { return dispatched; }));
            continueCopy.signal();
            // Manual-reset event allows the remaining locale copies through as well.
            expect (until ([&]
            {
                continueCopy.signal();
                return toggle->getButtonText() != juce::String::fromUTF8 ("설치 중...");
            }));
            expect (! wrongThread.load());
            expectEquals (elevations.load(), mode == 0 ? 0 : 1);
            expect (engine.getObsSender().isEnabled());
            if (mode == 1)
            {
                expect (hasObsStatus (main, "OBS 플러그인 설치 필요"));
                bool notice = false;
                for (auto* child : main.getChildren())
                    if (auto* text = dynamic_cast<juce::TextEditor*> (child)) notice |= text->getText().contains (juce::String::fromUTF8 ("설치를 취소했습니다"));
                expect (notice);
                toggle->setToggleState (false, juce::dontSendNotification);
                toggle->onClick();
                toggle->setToggleState (true, juce::dontSendNotification);
                toggle->onClick();
                expect (until ([&] { continueCopy.signal(); return elevations.load() == 2 && toggle->getButtonText() != juce::String::fromUTF8 ("설치 중..."); }));
            }
            else if (mode == 2)
            {
                expect (hasObsStatus (main, "OBS를 다시 시작하세요"));
                until ([&] { connect(); return false; }, 650);
                expect (hasObsStatus (main, "OBS를 다시 시작하세요"));
                if (readers != nullptr) lm_obs_store_release (&readers->slot[0].pid, 0);
                until ([] { return false; }, 650);
                connect();
                // This offline engine has no device: once a new reader connects the restart notice gives way to audio-stopped.
                expect (until ([&] { connect(); return hasObsStatus (main, "오디오 멈춤"); }));
            }
            else
                expect (Installer::isInstalledAndCurrent (f.roots));
            if (readers != nullptr) UnmapViewOfFile (readers);
            if (readerHandle != nullptr) CloseHandle (readerHandle);
        }
    }
};
static LiveMixObsInstallerUiTests liveMixObsInstallerUiTests;
}
