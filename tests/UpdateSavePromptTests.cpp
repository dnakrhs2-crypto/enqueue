#include "MainComponentTestAccess.h"
#include "TestGainPlugin.h"
#include "app/Commands.h"
#include "app/Updater.h"
#include "ui/GoCueLookAndFeel.h"
#include "ui/UiUtils.h"
#include "../livemix/src/ui/MainComponent.h"
#include "../livemix/src/ui/LiveMixLookAndFeel.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
namespace
{
void dispatchMessages()
{
   #if JUCE_MODAL_LOOPS_PERMITTED
    juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
   #elif JUCE_WINDOWS
    // The apps disable nested JUCE modal loops. Use the same pump as the existing UI tests.
    MSG message {};
    for (int i = 0; i < 2000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++i)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
   #endif
}

struct Scratch
{
    Scratch() { folder.createDirectory(); }
    ~Scratch() { if (folder.isAChildOf (base)) folder.deleteRecursively(); }
    const juce::File base = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File folder = base.getChildFile ("UpdateSavePrompt-" + juce::Uuid().toString());
};

struct EnqueueFixture
{
    static juce::PropertiesFile::Options options()
    {
        juce::PropertiesFile::Options result;
        result.millisecondsBeforeSaving = -1;
        return result;
    }
    EnqueueFixture()
    {
        main->setLookAndFeel (&theme);
        document().settings.autoBackup = false;
        document().settings.backupBeforeSave = false;
        document().settings.copyFilesIntoProject = false;
        document().markClean();
    }
    ~EnqueueFixture() { main.reset(); dispatchMessages(); }
    ProjectDocument& document() { return ReopenLastProjectTestAccess::document (*main); }
    void edit()
    {
        Cue cue;
        cue.type = CueType::control;
        cue.control.kind = ControlKind::memo;
        cue.name = ko ("업데이트 전 편집");
        document().cues.add (cue);
    }
    Scratch scratch;
    juce::File file = scratch.folder.getChildFile ("project.enqueue");
    juce::PropertiesFile storage { scratch.folder.getChildFile ("test.settings"), options() };
    AppSettings settings { storage };
    AudioEngine engine;
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main = std::make_unique<MainComponent> (engine, settings, commands);
};

struct LiveMixFixture
{
    LiveMixFixture()
    {
        engine.prepare (48000.0, 64);
        document.applyToEngine();
        main = std::make_unique<livemix::MainComponent> (document, settings);
        main->setLookAndFeel (&theme);
    }
    ~LiveMixFixture() { main.reset(); dispatchMessages(); }
    Scratch scratch;
    juce::File file = scratch.folder.getChildFile ("session.livemix");
    livemix::LiveMixSettings settings { scratch.folder };
    livemix::MixEngine engine;
    livemix::MixDocument document { engine };
    livemix::LiveMixLookAndFeel theme;
    std::unique_ptr<livemix::MainComponent> main;
};

struct Reply
{
    int calls = 0;
    bool proceed = false;
};

template <typename Main>
std::shared_ptr<Reply> ask (Main& main)
{
    auto reply = std::make_shared<Reply>();
    main.askSaveBeforeUpdate ([reply] (bool proceed) { ++reply->calls; reply->proceed = proceed; });
    return reply;
}

juce::AlertWindow* currentAlert()
{
    return dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
}

template <typename T> T* findChild (juce::Component& root)
{
    if (auto* found = dynamic_cast<T*> (&root)) return found;
    for (auto* child : root.getChildren())
        if (auto* found = findChild<T> (*child)) return found;
    return nullptr;
}

void dismiss (int result)
{
    if (auto* alert = currentAlert()) alert->exitModalState (result);
    dispatchMessages();
}

class QuestionThread : public juce::Thread
{
public:
    explicit QuestionThread (bool answer) : Thread ("UpdateSavePrompt"), expected (answer) {}
    void run() override
    {
        result = Updater::askOnMessageThread ([this] (auto reply)
        {
            onMessageThread = juce::MessageManager::getInstance()->isThisTheMessageThread();
            // A later message, like a modal callback, must still unblock the waiting caller.
            juce::MessageManager::callAsync ([reply, answer = expected] { reply (answer); });
        });
    }
    bool expected, result = false, onMessageThread = false;
};
}

class UpdateSavePromptTests : public juce::UnitTest
{
public:
    UpdateSavePromptTests() : UnitTest ("UpdateSavePrompt", "Enqueue") {}

    void runTest() override
    {
        for (bool answer : { true, false })
        {
            beginTest (juce::String ("message-thread question returns deferred ") + (answer ? "true" : "false") + " to worker");
            QuestionThread thread (answer);
            expect (thread.startThread());
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
            while (thread.isThreadRunning() && juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                dispatchMessages();
               #if JUCE_WINDOWS && ! JUCE_MODAL_LOOPS_PERMITTED
                MsgWaitForMultipleObjectsEx (0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
               #endif
            }
            expect (thread.waitForThreadToExit (1000));
            expect (thread.onMessageThread);
            expect (thread.result == answer);

            beginTest ("synchronous message-thread caller runs inline");
            expect (Updater::askOnMessageThread ([answer] (auto reply) { reply (answer); }) == answer);
        }

        enqueueTests();
        liveMixTests();
    }

private:
    void checkReply (const std::shared_ptr<Reply>& reply, bool proceed)
    {
        expectEquals (reply->calls, 1);
        expect (reply->proceed == proceed);
    }

    void checkPrompt (const juce::String& message)
    {
        auto* alert = currentAlert();
        expect (alert != nullptr);
        if (alert == nullptr) return;
        expectEquals (alert->getName(), ko ("업데이트 설치"));
        expectEquals (alert->getDescription(), alert->getName() + ". " + message);
        expectEquals (alert->getNumButtons(), 2);
        if (alert->getNumButtons() == 2)
        {
            expectEquals (alert->getButton (0)->getButtonText(), ko ("예"));
            expectEquals (alert->getButton (1)->getButtonText(), ko ("아니오"));
            expect (alert->getButton (0)->isRegisteredForShortcut (juce::KeyPress (juce::KeyPress::returnKey)));
        }
        expect (alert->getButton (ko ("취소")) == nullptr);
    }

    void enqueueTests()
    {
        beginTest ("Enqueue: No stays dirty until installer shutdown; manual quit still asks; decision is consumed once");
        {
            EnqueueFixture f;
            f.edit();
            auto reply = ask (*f.main);
            expectEquals (reply->calls, 0);
            checkPrompt ("\"" + f.document().getDisplayName() + "\""
                + ko ("에 저장하지 않은 변경 사항이 있습니다. 저장하시겠습니까?\n\n예·아니오 어느 쪽을 눌러도 Enqueue를 닫고 업데이트를 설치합니다."));
            auto* original = currentAlert();
            checkReply (ask (*f.main), false);
            expect (currentAlert() == original);
            dismiss (2);
            checkReply (reply, true);
            expect (f.document().isDirty());

            bool quit = false;
            f.main->confirmDiscardChangesThen ([&] { quit = true; });
            expect (currentAlert() != nullptr);
            if (auto* alert = currentAlert()) expectEquals (alert->getName(), ko ("변경 사항 저장"));
            dismiss (0);
            expect (! quit && f.document().isDirty());
            f.main->applyUpdateDecisionBeforeQuit();
            expect (! f.document().isDirty());
            f.document().markDirty();
            f.main->applyUpdateDecisionBeforeQuit();
            expect (f.document().isDirty());
        }

        beginTest ("Enqueue: Yes rewrites the existing temporary project and returns true only after saving");
        {
            EnqueueFixture f;
            expect (f.document().save (f.file).wasOk());
            const auto original = f.file.loadFileAsString();
            f.edit();
            auto reply = ask (*f.main);
            dismiss (1);
            checkReply (reply, true);
            expect (! f.document().isDirty());
            expect (f.file.loadFileAsString() != original);
            ProjectDocument loaded;
            expect (loaded.load (f.file).wasOk());
            expectEquals (loaded.cues.size(), 1);
        }

        beginTest ("Enqueue: an inline name edit is committed before checking dirty state");
        {
            EnqueueFixture f;
            f.edit();
            expect (f.document().save (f.file).wasOk());
            f.main->setSize (1541, 980);
            f.main->addToDesktop (0);
            f.main->setVisible (true);
            auto* table = findChild<CueTable> (*f.main);
            expect (table != nullptr);
            if (table != nullptr)
            {
                table->beginCellEdit (0, CueTable::colName);
                auto* editor = findChild<juce::TextEditor> (*table);
                expect (editor != nullptr);
                if (editor != nullptr)
                {
                    editor->setText (ko ("입력 중인 이름"), false);
                    expect (! f.document().isDirty());
                    auto reply = ask (*f.main);
                    expect (currentAlert() != nullptr);
                    expectEquals (f.document().cues.get (0).name, ko ("입력 중인 이름"));
                    dismiss (0);
                    checkReply (reply, false);
                    expect (f.document().isDirty());
                }
            }
        }

        beginTest ("Enqueue: failed save returns false, keeps changes and retains the existing save error dialog");
        {
            EnqueueFixture f;
            expect (f.document().save (f.file).wasOk());
            const auto original = f.file.loadFileAsString();
            f.edit();
            expect (f.file.setReadOnly (true));
            auto reply = ask (*f.main);
            dismiss (1);
            checkReply (reply, false);
            expect (f.document().isDirty());
            expectEquals (f.file.loadFileAsString(), original);
            expect (currentAlert() != nullptr);
            if (auto* alert = currentAlert()) expectEquals (alert->getName(), ko ("저장 실패"));
            dismiss (0);
            f.main->applyUpdateDecisionBeforeQuit();
            expect (f.document().isDirty());
            expect (f.file.setReadOnly (false));
        }

        for (int cancel : { 0, 1, 2 })
        {
            beginTest ("Enqueue: cancel by result / Escape / window close keeps changes (" + juce::String (cancel) + ")");
            EnqueueFixture f;
            f.edit();
            auto reply = ask (*f.main);
            if (auto* alert = currentAlert())
            {
                if (cancel == 0) alert->exitModalState (0);
                if (cancel == 1) expect (static_cast<juce::Component*> (alert)->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
                if (cancel == 2) static_cast<juce::Component*> (alert)->userTriedToCloseWindow();
            }
            dispatchMessages();
            checkReply (reply, false);
            f.main->applyUpdateDecisionBeforeQuit();
            expect (f.document().isDirty());
        }

        beginTest ("Enqueue: clean project proceeds without a dialog; show mode rejects before asking about changes");
        {
            EnqueueFixture f;
            checkReply (ask (*f.main), true);
            expect (currentAlert() == nullptr);
            f.edit();
            f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::toggleShowMode));
            expect (! f.main->isIdleForInterruptions());
            checkReply (ask (*f.main), false);
            expect (currentAlert() == nullptr);
            expect (f.document().isDirty());
        }

        beginTest ("Enqueue: cancelling a retry clears an earlier discard decision after installer launch failed");
        {
            EnqueueFixture f;
            f.edit();
            auto first = ask (*f.main);
            dismiss (2);
            checkReply (first, true);
            auto retry = ask (*f.main);
            dismiss (0);
            checkReply (retry, false);
            f.main->applyUpdateDecisionBeforeQuit();
            expect (f.document().isDirty());
        }
    }

    void liveMixTests()
    {
        beginTest ("LiveMix: unsaved session No stays dirty and manual quit still asks until installer shutdown");
        {
            LiveMixFixture f;
            f.document.setSessionName (ko ("저장하지 않은 세션"));
            auto reply = ask (*f.main);
            expectEquals (reply->calls, 0);
            checkPrompt (ko ("이 세션은 아직 파일로 저장되지 않았습니다. 저장하시겠습니까?\n\n예·아니오 어느 쪽을 눌러도 LiveMix를 닫고 업데이트를 설치합니다."));
            auto* original = currentAlert();
            checkReply (ask (*f.main), false);
            expect (currentAlert() == original);
            dismiss (2);
            checkReply (reply, true);
            expect (f.document.isDirty());
            bool quit = false;
            f.main->withSessionSecured ([&] { quit = true; });
            expect (currentAlert() != nullptr);
            if (auto* alert = currentAlert()) expectEquals (alert->getName(), ko ("저장하지 않은 세션"));
            dismiss (0);
            expect (! quit && f.document.isDirty());
            f.main->applyUpdateDecisionBeforeQuit();
            expect (! f.document.isDirty());
            expect (f.main->saveIfDirty());
            expect (! f.file.existsAsFile());
            f.document.markDirty();
            f.main->applyUpdateDecisionBeforeQuit();
            expect (f.document.isDirty());
        }

        beginTest ("LiveMix: No on a saved session leaves the file unchanged through shutdown's saveIfDirty");
        {
            LiveMixFixture f;
            expect (f.document.save (f.file).wasOk());
            const auto original = f.file.loadFileAsString();
            f.document.setSessionName (ko ("버릴 변경"));
            auto reply = ask (*f.main);
            dismiss (2);
            checkReply (reply, true);
            expect (f.document.isDirty());
            expectEquals (f.file.loadFileAsString(), original);
            f.main->applyUpdateDecisionBeforeQuit();
            expect (! f.document.isDirty());
            expect (f.main->saveIfDirty());
            expectEquals (f.file.loadFileAsString(), original);
        }

        beginTest ("LiveMix: hidden minimised window is restored before the update question");
        {
            LiveMixFixture f;
            juce::DocumentWindow window ("UpdateSavePrompt", juce::Colours::black, juce::DocumentWindow::closeButton);
            window.setContentNonOwned (f.main.get(), true);
            window.setVisible (true);
            window.setMinimised (true);
            window.setVisible (false);
            f.document.setSessionName (ko ("편집 중"));
            auto reply = ask (*f.main);
            expect (window.isVisible());
            expect (! window.isMinimised());
            expect (currentAlert() != nullptr);
            dismiss (0);
            checkReply (reply, false);
        }

        beginTest ("LiveMix: Yes captures silent plugin edits, reuses the capture and rewrites the temporary session");
        {
            LiveMixFixture f;
            auto plugin = std::make_unique<TestGainPlugin> (1.0f);
            auto& gain = *plugin;
            f.engine.getMasterChain().addPlugin (std::move (plugin));
            expect (f.document.save (f.file).wasOk());
            const auto original = f.file.loadFileAsString();
            gain.gain = 0.25f;
            expect (! f.document.isDirty());
            const int reads = gain.stateReads;
            auto reply = ask (*f.main);
            expect (f.document.isDirty());
            checkPrompt ("\"" + f.document.getDisplayName() + "\""
                + ko ("에 저장하지 않은 변경이 있습니다. 저장하시겠습니까?\n\n예·아니오 어느 쪽을 눌러도 LiveMix를 닫고 업데이트를 설치합니다."));
            dismiss (1);
            checkReply (reply, true);
            expect (! f.document.isDirty());
            expectEquals (gain.stateReads, reads + 1);
            expect (f.file.loadFileAsString() != original);
        }

        for (int cancel : { 0, 1, 2 })
        {
            beginTest ("LiveMix: cancel by result / Escape / window close keeps changes (" + juce::String (cancel) + ")");
            LiveMixFixture f;
            f.document.setSessionName (ko ("편집 중"));
            auto reply = ask (*f.main);
            if (auto* alert = currentAlert())
            {
                if (cancel == 0) alert->exitModalState (0);
                if (cancel == 1) expect (static_cast<juce::Component*> (alert)->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
                if (cancel == 2) static_cast<juce::Component*> (alert)->userTriedToCloseWindow();
            }
            dispatchMessages();
            checkReply (reply, false);
            f.main->applyUpdateDecisionBeforeQuit();
            expect (f.document.isDirty());
        }

        beginTest ("LiveMix: clean session proceeds without a dialog");
        {
            LiveMixFixture f;
            expect (f.document.save (f.file).wasOk());
            checkReply (ask (*f.main), true);
            expect (currentAlert() == nullptr);
        }

        beginTest ("LiveMix: failed save keeps changes, reports false and opens no extra dialog");
        {
            LiveMixFixture f;
            expect (f.document.save (f.file).wasOk());
            const auto original = f.file.loadFileAsString();
            f.document.setSessionName (ko ("저장 실패 확인"));
            expect (f.file.setReadOnly (true));
            auto reply = ask (*f.main);
            dismiss (1);
            checkReply (reply, false);
            expect (f.document.isDirty());
            expectEquals (f.file.loadFileAsString(), original);
            expect (currentAlert() == nullptr);
            f.main->applyUpdateDecisionBeforeQuit();
            expect (f.document.isDirty());
            expect (f.file.setReadOnly (false));
        }

        beginTest ("LiveMix: cancelling a retry clears an earlier discard decision after installer launch failed");
        {
            LiveMixFixture f;
            f.document.setSessionName (ko ("편집 중"));
            auto first = ask (*f.main);
            dismiss (2);
            checkReply (first, true);
            auto retry = ask (*f.main);
            dismiss (0);
            checkReply (retry, false);
            f.main->applyUpdateDecisionBeforeQuit();
            expect (f.document.isDirty());
        }
    }
};
static UpdateSavePromptTests updateSavePromptTests;
}
