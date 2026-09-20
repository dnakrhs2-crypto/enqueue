#include "app/Commands.h"
#include "app/ReopenLastProject.h"
#include "ui/MainComponent.h"
#include "ui/GoCueLookAndFeel.h"
#include "ui/MidiModalScope.h"
#include "ui/UiUtils.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
struct ReopenLastProjectTestAccess
{
    static bool saveAs (MainComponent& main, const juce::File& file) { return main.writeProjectToFile (file); }
    static bool pendingAutoStart (const MainComponent& main) { return main.pendingStartOnOpenCue.isNotEmpty(); }
    static ShortcutRouter& keyboard (MainComponent& main) { return *main.shortcutRouter; }
};

namespace
{
using Policy = ReopenLastProjectPolicy;
using Action = ReopenLastProjectDecision::Action;
constexpr std::array policies { Policy::ask, Policy::always, Policy::never };
constexpr std::array commandIDs { CommandIDs::reopenLastProjectAsk, CommandIDs::reopenLastProjectAlways,
                                  CommandIDs::reopenLastProjectNever };

void drainMessages()
{
   #if JUCE_WINDOWS
    MSG message {};
    for (int count = 0; count < 1000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++count)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
   #endif
}

struct Fixture
{
    static juce::PropertiesFile::Options options (bool failSave = false)
    {
        juce::PropertiesFile::Options result;
        result.storageFormat = juce::PropertiesFile::storeAsXML;
        result.millisecondsBeforeSaving = -1;
        result.doNotSave = failSave;
        return result;
    }
    explicit Fixture (bool failSave = false)
        : storage (folder.getChildFile ("test.settings"), options (failSave)), settings (storage) {}
    ~Fixture()
    {
        main.reset();
        drainMessages();
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        drainMessages();
        storage.saveIfNeeded();
        if (folder.isAChildOf (juce::File::getSpecialLocation (juce::File::tempDirectory)))
            folder.deleteRecursively();
    }
    void createMain()
    {
        main = std::make_unique<MainComponent> (engine, settings, commands);
        main->setLookAndFeel (&theme);
    }
    juce::File persistedSession() const
    {
        juce::PropertiesFile disk (storage.getFile(), options());
        return AppSettings (disk).getLastSessionProject();
    }
    bool seedSession (const juce::File& file, Policy policy)
    {
        juce::PropertiesFile disk (storage.getFile(), options());
        AppSettings persisted (disk);
        persisted.setLastProjectFile (file);
        persisted.setReopenLastProjectPolicy (policy);
        persisted.setLastSessionProject (file);
        return persisted.saveNow() && storage.reload();
    }
    bool makeProject (const juce::File& file, bool autoStart = false)
    {
        ProjectDocument project;
        Cue cue;
        cue.type = CueType::control;
        cue.control.kind = ControlKind::wait;
        cue.control.seconds = 60.0;
        cue.number = "1";
        project.cues.add (cue);
        project.settings.startOnOpen = autoStart;
        project.settings.startOnOpenCue = "1";
        project.settings.backupBeforeSave = false;
        return folder.createDirectory().wasOk() && project.save (file).wasOk();
    }
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("EnqueueReopenTest-" + juce::Uuid().toString());
    juce::PropertiesFile storage;
    AppSettings settings;
    AudioEngine engine;
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
};
}

class ReopenLastProjectTests : public juce::UnitTest
{
public:
    ReopenLastProjectTests() : UnitTest ("Reopen last project", "Enqueue") {}

    void runTest() override
    {
        testSettings();
        testDecisions();
        testLifecycle();
        testMenu();
        testSaveFailures();
       #if JUCE_WINDOWS
        testPrompt();
       #endif
    }

private:
    void testSettings()
    {
        beginTest ("new settings default to an empty session and ask; file chooser and update paths stay independent");
        Fixture f;
        expect (f.settings.getLastSessionProject() == juce::File());
        expect (f.settings.getReopenLastProjectPolicy() == Policy::ask);
        const auto chooserFile = f.folder.getChildFile ("chooser.enqueue");
        const auto updateFile = f.folder.getChildFile ("update.enqueue");
        const auto sessionFile = f.folder.getChildFile (ko ("공연 세션.enqueue"));
        f.settings.setLastProjectFile (chooserFile);
        f.settings.setReopenProjectAfterUpdate (updateFile);

        beginTest ("both keys round trip as path and ask/always/never strings, immediately on a session change");
        const std::array<juce::String, 3> values { "ask", "always", "never" };
        for (size_t i = 0; i < policies.size(); ++i)
        {
            expect (f.settings.setReopenLastProjectPolicy (policies[i]));
            expect (f.settings.setLastSessionProject (sessionFile));
            juce::PropertiesFile disk (f.storage.getFile(), Fixture::options());
            AppSettings reloaded (disk);
            expectEquals (disk.getValue ("lastSessionProject"), sessionFile.getFullPathName());
            expectEquals (disk.getValue ("reopenLastProjectPolicy"), values[i]);
            expect (reloaded.getLastSessionProject() == sessionFile);
            expect (reloaded.getReopenLastProjectPolicy() == policies[i]);
            expect (reloaded.getLastProjectFile() == chooserFile);
            expect (reloaded.getReopenProjectAfterUpdate() == updateFile);
        }

        beginTest ("empty sessions follow the existing removed-file-key convention; unknown policy values read as ask");
        f.settings.setLastSessionProject ({});
        expect (! f.storage.containsKey ("lastSessionProject"));
        expect (f.persistedSession() == juce::File());
        for (const auto* value : { "", "future", "ALWAYS", " always ", "0" })
        {
            f.storage.setValue ("reopenLastProjectPolicy", value);
            expect (f.settings.saveNow());
            juce::PropertiesFile disk (f.storage.getFile(), Fixture::options());
            expect (AppSettings (disk).getReopenLastProjectPolicy() == Policy::ask);
        }
    }

    void testDecisions()
    {
        beginTest ("192 startup combinations: CLI/update/safe mode/policies/empty/missing/already open");
        const std::array actions { Action::prompt, Action::open, Action::none };
        for (size_t i = 0; i < policies.size(); ++i)
            for (int bits = 0; bits < 64; ++bits)
            {
                const ReopenLastProjectContext context { (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0,
                                                        (bits & 8) != 0, (bits & 16) != 0, (bits & 32) != 0 };
                const auto result = decideReopenLastProject (policies[i], context);
                const auto label = "policy " + juce::String (static_cast<int> (i)) + ", flags " + juce::String (bits);
                // Only flags 24 (a stored, existing file and no launch/open blockers) can reopen.
                expect (result.action == (bits == 24 ? actions[i] : Action::none), label);
                expect (result.clearLastSessionProject == ((bits & 24) == 8), label);
            }
    }

    void testLifecycle()
    {
        beginTest ("constructing an empty window preserves the previous session until the startup decision");
        Fixture f;
        const auto original = f.folder.getChildFile ("original.enqueue");
        const auto savedAs = f.folder.getChildFile (ko ("다른 이름.enqueue"));
        expect (f.makeProject (original));
        f.settings.setLastSessionProject (original);
        f.createMain();
        expect (f.persistedSession() == original);

        beginTest ("successful open persists the session before shutdown");
        f.main->openProjectFile (original);
        expect (f.main->getProjectFile() == original);
        expect (f.persistedSession() == original);

        beginTest ("save-as completion persists its new path immediately");
        expect (ReopenLastProjectTestAccess::saveAs (*f.main, savedAs));
        expect (f.main->getProjectFile() == savedAs);
        expect (f.persistedSession() == savedAs);
        expect (f.settings.getLastProjectFile() == savedAs);

        beginTest ("failed open and save-as retain the current session and existing error notices");
        const auto broken = f.folder.getChildFile ("broken.enqueue");
        expect (broken.replaceWithText ("invalid project"));
        f.main->openProjectFile (broken);
        expect (f.main->getProjectFile() == savedAs);
        expect (f.persistedSession() == savedAs);
        expect (! ReopenLastProjectTestAccess::saveAs (*f.main, broken.getChildFile ("blocked.enqueue")));
        expect (f.main->getProjectFile() == savedAs);
        expect (f.persistedSession() == savedAs);
        drainMessages(); // showMessageBoxAsync creates the existing failure notices on the next dispatch.
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        drainMessages();

        beginTest ("new project clears the session immediately while preserving the chooser start path");
        f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::newProject));
        expect (f.main->getProjectFile() == juce::File());
        expect (f.persistedSession() == juce::File());
        expect (f.settings.getLastProjectFile() == savedAs);

        beginTest ("first save of a new project becomes the session, and shutdown writes the actual open path");
        expect (ReopenLastProjectTestAccess::saveAs (*f.main, savedAs));
        expect (f.persistedSession() == savedAs);
        f.settings.setLastSessionProject (original);
        f.main.reset();
        expect (f.persistedSession() == savedAs);

        beginTest ("shutdown of an unsaved new project clears the session");
        f.createMain();
        f.main.reset();
        expect (f.persistedSession() == juce::File());

        beginTest ("missing files are cleared on disk even under never or safe mode");
        for (const auto policy : policies)
        {
            f.settings.setReopenLastProjectPolicy (policy);
            f.settings.setLastSessionProject (f.folder.getChildFile ("missing.enqueue"));
            f.createMain();
            f.main->reopenLastProjectOnStartup (false, false, true);
            expect (f.persistedSession() == juce::File());
            expect (juce::Component::getCurrentlyModalComponent() == nullptr);
            f.main.reset();
        }

        beginTest ("always reopens with normal auto-start; the quiet launch gate still suppresses it");
        expect (f.makeProject (original, true));
        for (const bool allowed : { true, false })
        {
            f.settings.setReopenLastProjectPolicy (Policy::always);
            f.settings.setLastSessionProject (original);
            f.createMain();
            f.main->setAutoStartOnOpenAllowed (allowed);
            f.main->reopenLastProjectOnStartup (false, false, false);
            expect (f.main->getProjectFile() == original);
            expect (ReopenLastProjectTestAccess::pendingAutoStart (*f.main) == allowed);
            expect (juce::Component::getCurrentlyModalComponent() == nullptr);
            f.main.reset();
        }
    }

    void testMenu()
    {
        beginTest ("the three commands match UI scale category/scope, have no default keys, and tick the saved policy");
        Fixture f;
        f.createMain();
        const auto& catalog = ShortcutCatalog::get();
        const auto* scale = catalog.find (CommandIDs::uiScale100);
        const std::array<juce::String, 3> names { ko ("묻기"), ko ("항상 열기"), ko ("열지 않음") };
        for (size_t selected = 0; selected < policies.size(); ++selected)
        {
            expect (f.commands.invokeDirectly (commandIDs[selected], false));
            expect (f.settings.getReopenLastProjectPolicy() == policies[selected]);
            for (size_t i = 0; i < commandIDs.size(); ++i)
            {
                const auto* definition = catalog.find (commandIDs[i]);
                expect (definition != nullptr && scale != nullptr);
                if (definition == nullptr || scale == nullptr) continue;
                expect (definition->category == scale->category && definition->scope == scale->scope);
                expect (definition->defaultKeys.isEmpty());
                juce::ApplicationCommandInfo info (commandIDs[i]);
                f.main->getCommandInfo (commandIDs[i], info);
                expectEquals (info.shortName, names[i]);
                expect (((info.flags & juce::ApplicationCommandInfo::isTicked) != 0) == (i == selected));
                expectEquals (info.flags & juce::ApplicationCommandInfo::isDisabled, 0);
            }
        }

        beginTest ("submenu sits directly below UI scale; ticks persist and show mode disables every choice");
        for (const bool showMode : { false, true })
        {
            if (showMode) f.commands.invokeDirectly (CommandIDs::toggleShowMode, false);
            const auto menu = f.main->getMenuForIndex (4, ko ("설정"));
            juce::PopupMenu::MenuItemIterator items (menu);
            juce::String previous;
            bool found = false;
            while (items.next())
            {
                const auto& item = items.getItem();
                if (item.text == ko ("시작할 때 최근 프로젝트 (이 PC)"))
                {
                    found = true;
                    expectEquals (previous, ko ("글씨·화면 크기 (이 PC)"));
                    expect (item.isEnabled == ! showMode);
                    expect (item.subMenu != nullptr);
                    if (item.subMenu == nullptr) continue;
                    juce::PopupMenu::MenuItemIterator children (*item.subMenu);
                    size_t count = 0;
                    while (children.next() && count < names.size())
                    {
                        const auto& child = children.getItem();
                        expectEquals (child.text, names[count]);
                        expect (child.isEnabled == ! showMode);
                        expect (child.isTicked == (count == 2));
                        ++count;
                    }
                    expectEquals (static_cast<int> (count), 3);
                }
                previous = item.text;
            }
            expect (found);
        }
        for (const auto id : commandIDs)
        {
            juce::ApplicationCommandInfo info (id);
            f.main->getCommandInfo (id, info);
            expect ((info.flags & juce::ApplicationCommandInfo::isDisabled) != 0);
        }
    }

    void testSaveFailures()
    {
        beginTest ("policy save failure restores the raw value and dirty state for a later flush");
        for (const auto* raw : { static_cast<const char*> (nullptr), "future", "always" })
            for (const bool dirty : { false, true })
            {
                Fixture f (true); // same PropertiesFile failure injection as the shortcut rollback tests
                if (raw != nullptr) f.storage.setValue ("reopenLastProjectPolicy", raw);
                f.storage.setNeedsToBeSaved (false);
                if (dirty) f.settings.setWindowState ("unrelated pending state");
                expect (! f.settings.setReopenLastProjectPolicy (Policy::never));
                expect (f.storage.containsKey ("reopenLastProjectPolicy") == (raw != nullptr));
                expectEquals (f.storage.getValue ("reopenLastProjectPolicy"), juce::String (raw));
                expect (f.storage.needsToBeSaved() == dirty);
                expectEquals (f.settings.getWindowState(), juce::String (dirty ? "unrelated pending state" : ""));
                f.settings.flush();
                expectEquals (f.storage.getValue ("reopenLastProjectPolicy"), juce::String (raw));
            }

        beginTest ("a later successful flush cannot commit a rejected policy");
        {
            Fixture f;
            expect (f.storage.getFile().createDirectory().wasOk());
            f.storage.setValue ("reopenLastProjectPolicy", "always");
            f.settings.setWindowState ("unrelated pending state");
            expect (! f.settings.setReopenLastProjectPolicy (Policy::never));
            expect (f.settings.getReopenLastProjectPolicy() == Policy::always);
            expect (f.storage.needsToBeSaved());
            expect (f.storage.getFile().deleteFile());
            expect (f.settings.saveNow());
            juce::PropertiesFile disk (f.storage.getFile(), Fixture::options());
            AppSettings restarted (disk);
            expect (restarted.getReopenLastProjectPolicy() == Policy::always);
            expectEquals (restarted.getWindowState(), juce::String ("unrelated pending state"));
        }

        beginTest ("menu policy save failure restores the saved choice and shows one notice; restart uses disk values");
        {
            Fixture f (true);
            const auto previous = f.folder.getChildFile ("previous.enqueue");
            expect (f.makeProject (previous, true));
            expect (f.seedSession (previous, Policy::always));
            f.createMain();
            expect (f.commands.invokeDirectly (CommandIDs::reopenLastProjectNever, false));
            expect (f.settings.getReopenLastProjectPolicy() == Policy::always);
            juce::ApplicationCommandInfo info (CommandIDs::reopenLastProjectAlways);
            f.main->getCommandInfo (info.commandID, info);
            expect ((info.flags & juce::ApplicationCommandInfo::isTicked) != 0);
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 1);
            auto* notice = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
            expect (notice != nullptr);
            if (notice != nullptr)
            {
                expectEquals (notice->getName(), ko ("최근 프로젝트 설정 저장 실패"));
                expect (notice->getDescription().contains (ko ("이전 설정")));
            }
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
            drainMessages();
            f.main.reset(); // a failed shutdown save must not issue another notice
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 0);
            juce::PropertiesFile disk (f.storage.getFile(), Fixture::options());
            AppSettings restarted (disk);
            expect (restarted.getReopenLastProjectPolicy() == Policy::always);
            expect (restarted.getLastSessionProject() == previous);
            expect (decideReopenLastProject (restarted.getReopenLastProjectPolicy(),
                { false, false, false, true, restarted.getLastSessionProject().existsAsFile(), false }).action == Action::open);
        }

        beginTest ("session save failure is reported once apart from project save success; shutdown is quiet and disk stays authoritative");
        {
            Fixture f (true);
            const auto previous = f.folder.getChildFile ("previous.enqueue");
            const auto incoming = f.folder.getChildFile ("incoming.enqueue");
            const auto savedAs = f.folder.getChildFile ("saved-as.enqueue");
            expect (f.makeProject (previous, true) && f.makeProject (incoming));
            expect (f.seedSession (previous, Policy::always));
            f.createMain();
            f.main->openProjectFile (incoming);
            expect (f.main->getProjectFile() == incoming);
            expect (f.settings.getLastSessionProject() == incoming);
            expect (f.persistedSession() == previous);
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 1);
            auto* notice = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
            expect (notice != nullptr);
            if (notice != nullptr)
            {
                expectEquals (notice->getName(), ko ("최근 프로젝트 경로 저장 실패"));
                expect (notice->getDescription().contains (ko ("프로젝트 파일")));
                expect (notice->getDescription().contains (ko ("이전 프로젝트")));
                expect (notice->getDescription().contains (ko ("시작 큐")));
            }
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
            drainMessages();
            expect (ReopenLastProjectTestAccess::saveAs (*f.main, savedAs));
            ProjectDocument saved;
            expect (saved.load (savedAs).wasOk());
            expect (f.main->getProjectFile() == savedAs);
            expect (f.settings.getLastSessionProject() == savedAs);
            f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::newProject));
            expect (f.settings.getLastSessionProject() == juce::File());
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 0);
            f.main.reset();
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 0);
            expect (f.persistedSession() == previous);
            juce::PropertiesFile disk (f.storage.getFile(), Fixture::options());
            AppSettings restarted (disk);
            expect (restarted.getReopenLastProjectPolicy() == Policy::always);
            expect (decideReopenLastProject (restarted.getReopenLastProjectPolicy(),
                { false, false, false, true, restarted.getLastSessionProject().existsAsFile(), false }).action == Action::open);
        }

        beginTest ("session persistence retries on the next change and a later failure can notify again");
        {
            Fixture f;
            const auto project = f.folder.getChildFile ("session.enqueue");
            expect (f.makeProject (project));
            expect (f.storage.getFile().createDirectory().wasOk()); // directory blocks the settings write
            f.createMain();
            f.main->openProjectFile (project);
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 1);
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
            drainMessages();
            expect (f.storage.getFile().deleteFile());
            expect (ReopenLastProjectTestAccess::saveAs (*f.main, project)); // unchanged path still retries
            expect (f.persistedSession() == project);
            expect (! f.storage.needsToBeSaved());
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 0);
            expect (f.storage.getFile().deleteFile());
            expect (f.storage.getFile().createDirectory().wasOk());
            f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::newProject));
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 1);
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
            drainMessages();
            f.main.reset();
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 0);
            expect (f.storage.getFile().deleteFile());
        }

        beginTest ("policy save recovery allows a later session save failure to notify again");
        {
            Fixture f;
            const auto project = f.folder.getChildFile ("session.enqueue");
            expect (f.makeProject (project, true));
            expect (f.storage.getFile().createDirectory().wasOk());
            f.createMain();
            f.main->openProjectFile (project);
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 1);
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
            drainMessages();

            expect (f.storage.getFile().deleteFile());
            expect (f.commands.invokeDirectly (CommandIDs::reopenLastProjectAlways, false));
            expect (f.settings.getReopenLastProjectPolicy() == Policy::always);
            expect (f.persistedSession() == project);
            expect (! f.storage.needsToBeSaved());
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 0);

            expect (f.storage.getFile().deleteFile());
            expect (f.storage.getFile().createDirectory().wasOk());
            f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::newProject));
            expect (f.settings.getLastSessionProject() == juce::File());
            expect (f.storage.needsToBeSaved());
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 1);
            if (auto* notice = juce::Component::getCurrentlyModalComponent())
                expectEquals (notice->getName(), ko ("최근 프로젝트 경로 저장 실패"));
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
            drainMessages();
            f.main.reset();
            drainMessages();
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 0);
            expect (f.storage.getFile().deleteFile());
        }
    }

    void testPrompt()
    {
        beginTest ("ask presents exact buttons; normal keyboard/MIDI actions are blocked while panic is retained");
        for (int button = 0; button < 4; ++button)
        {
            Fixture f;
            const auto project = f.folder.getChildFile (ko ("최근 공연.enqueue"));
            expect (f.makeProject (project, true));
            f.settings.setLastSessionProject (project);
            f.createMain();
            f.main->reopenLastProjectOnStartup (false, false, false);
            auto* alert = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
            expect (alert != nullptr);
            if (alert == nullptr) continue;
            expectEquals (alert->getName(), ko ("최근 프로젝트 다시 열기"));
            expectEquals (alert->getNumButtons(), 3);
            const std::array<juce::String, 3> names { ko ("열기"), ko ("항상 열기"), ko ("새 프로젝트") };
            for (int i = 0; i < 3; ++i) expectEquals (alert->getButton (i)->getButtonText(), names[static_cast<size_t> (i)]);
            expect (alert->getButton (2)->isRegisteredForShortcut (juce::KeyPress (juce::KeyPress::escapeKey)));
            auto& service = f.main->getShortcutService();
            auto& keyboard = ReopenLastProjectTestAccess::keyboard (*f.main);
            const juce::KeyPress go (juce::KeyPress::spaceKey), panic (juce::KeyPress::escapeKey);
            expect (service.resolveKeyOwner (go, keyboard.contextFor (alert, go)).kind != ShortcutKeyOwner::Kind::command);
            expectEquals (service.resolveKeyOwner (panic, keyboard.contextFor (alert, panic)).commandID, juce::CommandID (CommandIDs::panicAll));
            const auto midi = currentMidiContext (*f.main);
            expect (midi.modal);
            expect (service.resolveMidiOwner ({ "transport.go", {}, CommandIDs::go }, midi).kind == MidiOwner::Kind::blocked);
            expect (service.resolveMidiOwner ({ "transport.panicAll", {}, CommandIDs::panicAll }, midi).kind == MidiOwner::Kind::panic);
            expect (f.main->getProjectFile() == juce::File());
            if (button == 3)
                expect (static_cast<juce::Component*> (alert)->keyPressed (panic));
            else
                alert->triggerButtonClick (names[static_cast<size_t> (button)]);
            drainMessages();
            expect (juce::Component::getCurrentlyModalComponent() == nullptr);
            const bool opened = button < 2;
            expect (f.main->getProjectFile() == (opened ? project : juce::File()));
            expect (f.persistedSession() == (opened ? project : juce::File()));
            expect (f.settings.getReopenLastProjectPolicy() == (button == 1 ? Policy::always : Policy::ask));
            expect (ReopenLastProjectTestAccess::pendingAutoStart (*f.main) == opened);
        }

        beginTest ("always-open prompt save failure keeps ask and reports once while opening the requested project");
        {
            Fixture f (true);
            const auto previous = f.folder.getChildFile ("previous.enqueue");
            expect (f.makeProject (previous));
            expect (f.seedSession (previous, Policy::ask));
            f.createMain();
            f.main->reopenLastProjectOnStartup (false, false, false);
            auto* prompt = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
            expect (prompt != nullptr);
            if (prompt != nullptr) prompt->triggerButtonClick (ko ("항상 열기"));
            drainMessages();
            expect (f.main->getProjectFile() == previous);
            expect (f.settings.getReopenLastProjectPolicy() == Policy::ask);
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 1);
            auto* notice = juce::Component::getCurrentlyModalComponent();
            expect (notice != nullptr);
            if (notice != nullptr) expectEquals (notice->getName(), ko ("최근 프로젝트 설정 저장 실패"));
        }

        beginTest ("another open dismisses the prompt before auto-start and restores keyboard/MIDI without a response");
        for (const bool commandLine : { true, false })
        {
            Fixture f;
            const auto previous = f.folder.getChildFile ("previous.enqueue");
            const auto incoming = f.folder.getChildFile ("incoming.enqueue");
            expect (f.makeProject (previous) && f.makeProject (incoming, true));
            f.settings.setLastSessionProject (previous);
            f.createMain();
            f.main->reopenLastProjectOnStartup (false, false, false);
            juce::Component::SafePointer<juce::Component> prompt (juce::Component::getCurrentlyModalComponent());
            expect (prompt != nullptr);
            if (commandLine)
                f.main->openProjectFromCommandLine ("\"" + incoming.getFullPathName() + "\"");
            else
                f.main->openProjectFile (incoming); // the menu chooser's completion uses the same path
            expect (prompt == nullptr);
            expectEquals (juce::Component::getNumCurrentlyModalComponents(), 0);
            expect (ReopenLastProjectTestAccess::pendingAutoStart (*f.main));
            auto& keyboard = ReopenLastProjectTestAccess::keyboard (*f.main);
            keyboard.applicationActiveChanged (true);
            const juce::KeyPress go (juce::KeyPress::spaceKey);
            auto* origin = juce::Component::getCurrentlyModalComponent();
            const auto keyContext = keyboard.contextFor (origin != nullptr ? origin : f.main.get(), go);
            expectEquals (f.main->getShortcutService().resolveKeyOwner (go, keyContext).commandID, juce::CommandID (CommandIDs::go));
            auto midi = currentMidiContext (*f.main);
            expect (! midi.modal);
            midi.applicationActive = true; // independent of whether the test console owns the foreground
            midi.window = ShortcutKeyContext::Window::main;
            expect (f.main->getShortcutService().resolveMidiOwner ({ "transport.go", {}, CommandIDs::go }, midi).kind == MidiOwner::Kind::command);
            drainMessages(); // cancellation's queued callback must be inert too
            expect (f.main->getProjectFile() == incoming);
            expect (f.persistedSession() == incoming);
            expect (f.settings.getReopenLastProjectPolicy() == Policy::ask);
        }

        beginTest ("a queued old prompt response cannot change the project or dismiss a replacement prompt");
        Fixture f;
        const auto previous = f.folder.getChildFile ("previous.enqueue");
        const auto incoming = f.folder.getChildFile ("incoming.enqueue");
        expect (f.makeProject (previous) && f.makeProject (incoming));
        f.settings.setLastSessionProject (previous);
        f.createMain();
        f.main->reopenLastProjectOnStartup (false, false, false);
        auto* alert = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
        expect (alert != nullptr);
        if (alert != nullptr)
        {
            alert->exitModalState (2); // response queued, but not dispatched yet
            f.main->openProjectFromCommandLine ("\"" + incoming.getFullPathName() + "\"");
            f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::newProject));
            f.settings.setLastSessionProject (previous);
            f.main->reopenLastProjectOnStartup (false, false, false);
            juce::Component::SafePointer<juce::Component> replacement (juce::Component::getCurrentlyModalComponent());
            expect (replacement != nullptr);
            drainMessages();
            expect (replacement != nullptr);
            expect (juce::Component::getCurrentlyModalComponent() == replacement.getComponent());
            expect (f.main->getProjectFile() == juce::File());
            expect (f.persistedSession() == previous);
            expect (f.settings.getReopenLastProjectPolicy() == Policy::ask);
        }

        beginTest ("closing the owner while the async prompt is open dismisses it safely");
        f.main.reset();
        f.settings.setReopenLastProjectPolicy (Policy::ask);
        f.settings.setLastSessionProject (previous);
        f.createMain();
        f.main->reopenLastProjectOnStartup (false, false, false);
        juce::Component::SafePointer<juce::Component> lifetime (juce::Component::getCurrentlyModalComponent());
        expect (lifetime != nullptr);
        f.main.reset();
        drainMessages();
        expect (lifetime == nullptr);
        expect (f.persistedSession() == juce::File());
    }
};

static ReopenLastProjectTests reopenLastProjectTests;
} // namespace gocue::tests
