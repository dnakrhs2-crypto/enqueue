#include "ShortcutTestHarness.h"
#include "app/Commands.h"
#include "ui/ShortcutRouter.h"
#include "ui/CueTable.h"
#include "ui/LevelMatrixComponent.h"
#include "ui/GoCueLookAndFeel.h"
#include "app/CueController.h"
#include "audio/AudioEngine.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
namespace
{
using K = juce::KeyPress;
using M = juce::ModifierKeys;
using Window = ShortcutKeyContext::Window;

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

struct FixedComponent : juce::Component
{
    explicit FixedComponent (ShortcutScope scope) { ShortcutRouter::setComponentScope (*this, scope); }
    bool keyPressed (const K&) override { ++attempts; if (enabledAction) ++actions; return enabledAction; }
    bool enabledAction = true;
    int attempts = 0, actions = 0;
};

struct InputHarness : shortcut_test::Harness
{
    InputHarness()
    {
        panic = std::make_unique<PanicKeyHook> (*service, [this] (double time, bool hard)
        { panicTimes.push_back (time); hardPanics.push_back (hard); });
        ShortcutRouter::Callbacks callbacks;
        callbacks.keyDown = [this] (int code) { return physical.count (code) != 0; };
        callbacks.nativeKeyDown = [this] (int vk) { return nativePhysical.count (vk) != 0; };
        callbacks.applicationActive = [this] { return foreground; };
        callbacks.requireGoKeyUp = [this] { return requireKeyUp; };
        callbacks.cueHotkey = [this] (const K& key, bool repeat)
        {
            if (repeat) ++cueRepeats;
            else { ++cueFires; cueKeys.push_back (key); }
        };
        callbacks.panic = [this] (double time) { panic->fromJuce (time); };
        router = std::make_unique<ShortcutRouter> (*service, manager, std::move (callbacks));
    }
    bool press (const K& key, ShortcutKeyContext context = {}, juce::Component* origin = nullptr, bool repeat = false)
    {
        physical.insert (key.getKeyCode());
        return router->route (key, origin, context, now, repeat);
    }
    void release (int code) { physical.erase (code); router->pollKeyState(); }
    void releaseAll() { physical.clear(); nativePhysical.clear(); router->pollKeyState(); }
    int downs (juce::CommandID id) const
    {
        return static_cast<int> (std::count_if (target.invocations.begin(), target.invocations.end(), [id] (const auto& i)
        { return i.commandID == id && i.isKeyDown; }));
    }
    int ups (juce::CommandID id) const
    {
        return static_cast<int> (std::count_if (target.invocations.begin(), target.invocations.end(), [id] (const auto& i)
        { return i.commandID == id && ! i.isKeyDown; }));
    }
    void native (int vk, int mods = 0, bool down = true, bool repeat = false)
    {
        if (const auto event = panic->observe (vk, mods, down, repeat, now, foreground))
            panic->dispatch (*event);
    }
    std::set<int> physical;
    std::set<int> nativePhysical; // VK state is independent of queued down/up observations
    std::unique_ptr<PanicKeyHook> panic;
    std::unique_ptr<ShortcutRouter> router;
    bool requireKeyUp = false, foreground = true;
    double now = 1000.0;
    int cueFires = 0, cueRepeats = 0;
    std::vector<K> cueKeys;
    std::vector<double> panicTimes;
    std::vector<bool> hardPanics;
};
}

class ShortcutRouterTests : public juce::UnitTest
{
public:
    ShortcutRouterTests() : juce::UnitTest ("Shortcut router and panic input", "Enqueue") {}
    void runTest() override
    {
        beginTest ("commands preserve fromKeyPress, actual key, origin and down/up; repeat is opt-in");
        {
            InputHarness h;
            juce::Component origin;
            const K key (K::F13Key, M::ctrlModifier, 0);
            expect (h.service->setKeys ("transport.go", { key }).wasOk());
            expect (h.press (key, {}, &origin));
            expect (h.press (key, {}, &origin));
            expectEquals (h.downs (CommandIDs::go), 1);
            expect (h.target.invocations.front().keyPress == key);
            expect (h.target.invocations.front().originatingComponent == &origin);
            expect (h.target.invocations.front().invocationMethod == juce::ApplicationCommandTarget::InvocationInfo::fromKeyPress);
            h.releaseAll();
            expectEquals (h.ups (CommandIDs::go), 1);
            expect (h.target.invocations.back().keyPress == key);
            h.press (K ('S', M::ctrlModifier, 0));
            h.press (K ('S', M::ctrlModifier, 0));
            expectEquals (h.downs (CommandIDs::saveProject), 1);
            h.releaseAll();
            h.press (K (K::F3Key));
            h.press (K (K::F3Key));
            expectEquals (h.downs (CommandIDs::findNext), 2);
        }

        beginTest ("fixed owner is exclusive even if disabled or its key handler returns false");
        {
            InputHarness h;
            FixedComponent wave (ShortcutScope::waveform);
            wave.enabledAction = false;
            ShortcutKeyContext context;
            context.focus = ShortcutScope::waveform;
            context.cueHotkeys.push_back ({ "cue", K (K::deleteKey) });
            expect (h.press (K (K::deleteKey), context, &wave));
            expectEquals (wave.attempts, 1);
            expectEquals (wave.actions, 0);
            expectEquals (h.downs (CommandIDs::removeCue), 0);
            expectEquals (h.cueFires, 0);
            h.press (K (K::deleteKey), context, &wave);
            expectEquals (wave.attempts, 1);
            expectEquals (h.cueRepeats, 0);
            h.releaseAll();
            context.componentCanHandle = [] (const juce::String&) { return false; };
            h.press (K (K::deleteKey), context, &wave);
            expectEquals (wave.attempts, 1);
            expectEquals (h.cueFires, 0);
        }

        beginTest ("disabled commands and out-of-scope commands never fall through to cues");
        {
            InputHarness h;
            const K key (K::F14Key);
            expect (h.service->setKeys ("cue.remove", { key }).wasOk());
            ShortcutKeyContext context;
            context.cueHotkeys.push_back ({ "cue", key });
            h.target.disabledCommands.insert (CommandIDs::removeCue); // show mode / no selection
            h.press (key, context);
            expectEquals (h.downs (CommandIDs::removeCue), 0);
            expectEquals (h.cueFires, 0);
            h.releaseAll();
            h.target.disabledCommands.clear();
            context.window = Window::auxiliary;
            h.press (key, context);
            expectEquals (h.downs (CommandIDs::removeCue), 0);
            expectEquals (h.cueFires, 0);
            h.releaseAll();
            expect (h.service->setKeys ("cue.remove", {}).wasOk());
            context.window = Window::main;
            h.press (key, context);
            h.press (key, context);
            expectEquals (h.cueFires, 1);
            expectEquals (h.cueRepeats, 1);
        }

        beginTest ("window scopes use remapped playback, retain standard controls and exclude plugin/outside app");
        {
            InputHarness h;
            const K key (K::F13Key);
            expect (h.service->setKeys ("transport.go", { key }).wasOk());
            ShortcutKeyContext context;
            for (const auto window : { Window::main, Window::auxiliary, Window::modal, Window::nativePlugin, Window::outsideApp })
            {
                context.window = window;
                h.press (key, context);
                h.releaseAll();
            }
            expectEquals (h.downs (CommandIDs::go), 2);
            context.window = Window::auxiliary;
            h.press (K (K::spaceKey), context);
            expectEquals (h.downs (CommandIDs::go), 2); // old Space gone
            h.releaseAll();
            context.standardUiConsumesKey = true;
            expect (! h.press (key, context));
            expectEquals (h.downs (CommandIDs::go), 2);
        }

        beginTest ("actual text editing keys win after remapping; function keys and unused command chords work");
        {
            InputHarness h;
            juce::TextEditor editor;
            h.router->attach (editor, Window::main);
            const std::vector<K> editing { K ('G', 0, 'g'), K ('G', M::shiftModifier, 'G'), K (K::spaceKey, 0, ' '),
                K (K::returnKey), K (K::tabKey), K (K::deleteKey), K (K::leftKey), K ('C', M::ctrlModifier, 0),
                K ('V', M::ctrlModifier, 0), K ('X', M::ctrlModifier, 0), K ('Z', M::ctrlModifier, 0), K ('A', M::ctrlModifier, 0) };
            for (const auto& key : editing)
            {
                h.releaseAll();
                expect (h.service->setKeys ("transport.go", { key }, ShortcutService::ConflictPolicy::move).wasOk());
                expect (! h.press (key, h.router->contextFor (&editor, key), &editor));
            }
            expectEquals (h.downs (CommandIDs::go), 0);
            for (const K key : { K (K::F13Key), K ('G', M::ctrlModifier | M::altModifier, 0) })
            {
                h.releaseAll();
                expect (h.service->setKeys ("transport.go", { key }).wasOk());
                h.press (key, h.router->contextFor (&editor, key), &editor);
            }
            expectEquals (h.downs (CommandIDs::go), 2);
            h.releaseAll();
            auto context = h.router->contextFor (&editor, K (K::F24Key));
            context.cueHotkeys.push_back ({ "cue", K (K::F24Key) });
            h.press (K (K::F24Key), context, &editor);
            expectEquals (h.cueFires, 0);
            h.releaseAll();
            editor.setReadOnly (true);
            expect (h.service->setKeys ("transport.go", { K ('G') }).wasOk());
            context = h.router->contextFor (&editor, K ('G', 0, 'g'));
            expect (! context.textEditing);
            h.press (K ('G', 0, 'g'), context, &editor);
            expectEquals (h.downs (CommandIDs::go), 3);
        }

        beginTest ("GO aliases need every physical key released, including the suppressed second alias");
        {
            InputHarness h;
            h.requireKeyUp = true;
            expect (h.service->setKeys ("transport.go", { K (K::spaceKey), K (K::F13Key) }).wasOk());
            h.press (K (K::spaceKey));
            h.press (K (K::F13Key));
            expectEquals (h.downs (CommandIDs::go), 1);
            h.release (K::spaceKey);
            expect (h.router->anyGoKeyHeld());
            h.press (K (K::spaceKey));
            expectEquals (h.downs (CommandIDs::go), 1);
            h.release (K::F13Key);
            expect (h.router->anyGoKeyHeld());
            h.releaseAll();
            expect (! h.router->anyGoKeyHeld());
            h.press (K (K::F13Key));
            expectEquals (h.downs (CommandIDs::go), 2);
        }

        beginTest ("keypad GO keeps VK_ADD held through polling and blocks a second GO alias");
        {
            InputHarness h;
            h.requireKeyUp = true;
            expect (h.service->setKeys ("transport.go", { K ('+'), K (K::F13Key) }).wasOk());
            juce::Component origin;
            h.router->attach (origin, Window::main);
            h.nativePhysical.insert (0x6b); // VK_ADD; neither '+' nor VK_OEM_PLUS is down
            h.router->prepareNativeEvent (0x6b, 0, true, false);
            expect (h.router->keyPressed (K ('+', 0, '+'), &origin));
            h.router->pollKeyState();
            expect (h.router->anyGoKeyHeld());
            expectEquals (h.ups (CommandIDs::go), 0);
            h.press (K (K::F13Key));
            expectEquals (h.downs (CommandIDs::go), 1);
            h.release (K::F13Key);
            h.router->pollKeyState();
            h.press (K (K::F13Key));
            expectEquals (h.downs (CommandIDs::go), 1);
            h.releaseAll(); // physical polling recovers a missing native up
            expect (! h.router->anyGoKeyHeld());
            h.press (K (K::F13Key));
            expectEquals (h.downs (CommandIDs::go), 2);
        }

        beginTest ("a keypad held before capture keeps its VK identity until physical release");
        {
            InputHarness h;
            expect (h.service->setKeys ("transport.go", { K ('+') }).wasOk());
            h.nativePhysical.insert (0x6b);
            int token = 0, candidates = 0;
            h.service->beginCapture (&token, [&] (const K&) { ++candidates; });
            juce::Component origin;
            h.router->attach (origin, Window::main);
            h.router->keyPressed (K ('+', 0, '+'), &origin);
            expectEquals (candidates, 0);
            h.service->endCapture (&token);
            h.router->keyPressed (K ('+', 0, '+'), &origin);
            expectEquals (h.downs (CommandIDs::go), 0);
            h.releaseAll();
            h.nativePhysical.insert (0x6b);
            h.router->keyPressed (K ('+', 0, '+'), &origin);
            expectEquals (h.downs (CommandIDs::go), 1);
        }

        beginTest ("queued native up ends the first GO even when the second press is physically down");
        {
            InputHarness h;
            expect (! h.requireKeyUp);
            juce::Component origin;
            h.router->attach (origin, Window::main);
            h.nativePhysical.insert (0x20);
            h.router->prepareNativeEvent (0x20, 0, true, false);
            h.router->keyStateChanged (true, &origin);
            h.router->keyPressed (K (K::spaceKey), &origin);
            // The device already released and pressed again; the queue is behind it.
            h.router->prepareNativeEvent (0x20, 0, false, false);
            h.router->keyStateChanged (false, &origin);
            expectEquals (h.ups (CommandIDs::go), 1);
            expect (! h.router->anyGoKeyHeld());
            h.router->prepareNativeEvent (0x20, 0, true, false);
            h.router->keyStateChanged (true, &origin);
            h.router->keyPressed (K (K::spaceKey), &origin);
            expectEquals (h.downs (CommandIDs::go), 2);
            h.router->prepareNativeEvent (0x20, 0, true, true);
            h.router->keyPressed (K (K::spaceKey), &origin);
            expectEquals (h.downs (CommandIDs::go), 2);
        }

        beginTest ("delayed character downs retain their own native up and modifier observations");
        {
            InputHarness h;
            expect (h.service->setKeys ("transport.go", { K ('G'), K ('G', M::ctrlModifier, 0) },
                                       ShortcutService::ConflictPolicy::move).wasOk());
            juce::Component origin;
            h.router->attach (origin, Window::main);
            h.nativePhysical.insert ('G');
            h.router->prepareNativeEvent ('G', M::ctrlModifier, true, false);
            h.router->prepareNativeEvent ('H', 0, true, false); // an unrelated down cannot overwrite G
            h.router->prepareNativeEvent ('G', M::ctrlModifier, false, false);
            h.router->prepareNativeEvent ('G', 0, true, false);
            h.router->keyPressed (K ('G', 0, 'g'), &origin); // first WM_CHAR arrives after its up
            expectEquals (h.downs (CommandIDs::go), 1);
            expectEquals (h.ups (CommandIDs::go), 1);
            if (! h.target.invocations.empty())
                expect (h.target.invocations.front().keyPress.getModifiers().isCtrlDown());
            h.router->keyPressed (K ('G', 0, 'g'), &origin);
            expectEquals (h.downs (CommandIDs::go), 2);
            expectEquals (h.ups (CommandIDs::go), 1);
            expect (h.router->anyGoKeyHeld());
        }

        beginTest ("GO focus/modifier changes and map replacement cannot manufacture a fresh down");
        {
            InputHarness h;
            const K plain (K::F13Key), modified (K::F13Key, M::shiftModifier, 0);
            expect (h.service->setKeys ("transport.go", { plain, modified }).wasOk());
            h.press (plain);
            ShortcutKeyContext auxiliary;
            auxiliary.window = Window::auxiliary;
            h.press (modified, auxiliary);
            expectEquals (h.downs (CommandIDs::go), 1);
            h.router->applicationActiveChanged (false);
            h.router->applicationActiveChanged (true);
            h.press (plain);
            expectEquals (h.downs (CommandIDs::go), 1);
            expect (h.service->setKeys ("transport.go", { plain }).wasOk());
            h.press (plain);
            expectEquals (h.downs (CommandIDs::go), 1);
            h.releaseAll();
            h.press (plain);
            expectEquals (h.downs (CommandIDs::go), 2);
            h.releaseAll();
            h.press (plain, {}, nullptr, true); // missed first down in another app, native repeat bit survives
            expectEquals (h.downs (CommandIDs::go), 2);
        }

        beginTest ("without requireKeyUp each GO alias fires once, never on OS repeat");
        {
            InputHarness h;
            expect (h.service->setKeys ("transport.go", { K (K::spaceKey), K (K::F13Key) }).wasOk());
            for (int i = 0; i < 5; ++i)
            {
                h.press (K (K::spaceKey));
                h.press (K (K::F13Key));
            }
            expectEquals (h.downs (CommandIDs::go), 2);
            h.releaseAll();
            expectEquals (h.ups (CommandIDs::go), 2);
        }

        beginTest ("GO aliases pressed in a settings window keep the group held through a return to main");
        {
            InputHarness h;
            h.requireKeyUp = true;
            expect (h.service->setKeys ("transport.go", { K (K::spaceKey), K (K::F13Key) }).wasOk());
            h.press (K (K::spaceKey));
            ShortcutKeyContext modal;
            modal.window = Window::modal;
            h.press (K (K::F13Key), modal);
            h.release (K::spaceKey);
            expect (h.router->anyGoKeyHeld());
            h.press (K (K::spaceKey));
            expectEquals (h.downs (CommandIDs::go), 1);
            h.releaseAll();
            h.press (K (K::spaceKey));
            expectEquals (h.downs (CommandIDs::go), 2);
        }

        beginTest ("capture exclusively receives one candidate, suppressing command/cue/component/default button paths");
        {
            InputHarness h;
            int token = 0, candidates = 0;
            FixedComponent component (ShortcutScope::waveform);
            ShortcutKeyContext context;
            context.focus = ShortcutScope::waveform;
            context.cueHotkeys.push_back ({ "cue", K (K::F24Key) });
            h.service->beginCapture (&token, [&] (const K&) { ++candidates; });
            for (const auto key : { K (K::spaceKey), K (K::F24Key), K (K::deleteKey), K (K::returnKey), K (K::tabKey), K (K::escapeKey) })
            {
                h.press (key, context, &component);
                h.press (key, context, &component);
            }
            expectEquals (candidates, 6);
            expectEquals (h.downs (CommandIDs::go), 0);
            expectEquals (h.cueFires, 0);
            expectEquals (component.actions, 0);
            expectEquals (static_cast<int> (h.panicTimes.size()), 0);
            expect (h.router->keyStateChanged (true, &component));
            h.service->endCapture (&token);
            h.press (K (K::spaceKey), context, &component);
            h.press (K (K::F24Key), context, &component);
            expectEquals (h.downs (CommandIDs::go), 0);
            expectEquals (h.cueFires, 0);
            h.releaseAll();
            h.press (K (K::F24Key), context, &component);
            expectEquals (h.cueFires, 1);
        }

        beginTest ("keyboard capture activation waits for release; stale tokens, lock and held registration are safe");
        {
            InputHarness h;
            int token = 0, stale = 0, candidates = 0, cancelled = 0;
            h.physical.insert (K::returnKey);
            h.service->beginCapture (&token, [&] (const K&) { ++candidates; }, [&] { ++cancelled; });
            h.service->endCapture (&stale);
            expect (h.service->isCapturing());
            h.press (K (K::returnKey));
            h.press (K (K::spaceKey));
            expectEquals (candidates, 0);
            h.release (K::returnKey);
            h.press (K (K::spaceKey)); // this key was already pressed while activation was held
            expectEquals (candidates, 0);
            h.releaseAll();
            h.press (K (K::spaceKey));
            expectEquals (candidates, 1);
            h.service->setEditingLocked (true);
            expectEquals (cancelled, 1);
            expect (! h.service->isCapturing());
            h.press (K (K::spaceKey));
            expectEquals (h.downs (CommandIDs::go), 0);
            h.releaseAll();
            h.press (K (K::spaceKey));
            expectEquals (h.downs (CommandIDs::go), 1);
        }

        beginTest ("a GO alias captured while another GO is held keeps requireKeyUp closed until both release");
        {
            InputHarness h;
            h.requireKeyUp = true;
            expect (h.service->setKeys ("transport.go", { K (K::spaceKey), K (K::F13Key) }).wasOk());
            h.press (K (K::spaceKey));
            int token = 0;
            h.service->beginCapture (&token);
            h.press (K (K::F13Key));
            h.service->endCapture (&token);
            h.release (K::spaceKey);
            expect (h.router->anyGoKeyHeld());
            h.press (K (K::spaceKey));
            expectEquals (h.downs (CommandIDs::go), 1);
            h.releaseAll();
            expect (! h.router->anyGoKeyHeld());
            h.press (K (K::spaceKey));
            expectEquals (h.downs (CommandIDs::go), 2);
        }

        beginTest ("real JUCE peer input cannot activate a default button during/after capture");
        {
            InputHarness h;
            juce::TextButton button;
            int clicks = 0, token = 0, candidates = 0;
            button.onClick = [&] { ++clicks; };
            button.addToDesktop (0);
            h.router->attach (button, Window::modal);
            h.service->beginCapture (&token, [&] (const K&) { ++candidates; h.service->endCapture (&token); });
            h.physical.insert (K::returnKey);
            button.getPeer()->handleKeyPress (K (K::returnKey));
            button.getPeer()->handleKeyPress (K (K::returnKey));
            button.getPeer()->handleKeyUpOrDown (true);
            drainMessages();
            expectEquals (candidates, 1);
            expectEquals (clicks, 0);
            h.releaseAll();
            h.router->attach (button, Window::main);
            h.physical.insert (K::spaceKey);
            button.getPeer()->handleKeyPress (K (K::spaceKey));
            expectEquals (h.downs (CommandIDs::go), 1); // an unused button key returns to normal routing after release
            button.removeFromDesktop();
        }

        beginTest ("a dynamically added TextEditor receives routing before JUCE can consume its first key");
        {
            InputHarness h;
            juce::Component root;
            h.router->attach (root, Window::main);
            juce::TextEditor editor;
            root.addAndMakeVisible (editor); // synchronous child listener installation
            editor.addToDesktop (0); // hidden peer; dispatch to this component without stealing OS focus
            ShortcutRouter::setWindowScope (editor, Window::main);
            const K functionKey (K::F13Key);
            expect (h.service->setKeys ("transport.go", { functionKey }).wasOk());
            h.physical.insert (K::F13Key);
            editor.getPeer()->handleKeyPress (functionKey);
            expectEquals (h.downs (CommandIDs::go), 1);
            h.releaseAll();
            int token = 0, captured = 0;
            h.service->beginCapture (&token, [&] (const K&) { ++captured; });
            h.physical.insert ('G');
            editor.getPeer()->handleKeyPress (K ('G', 0, 'g'));
            expectEquals (captured, 1);
            expect (editor.getText().isEmpty());
            h.service->endCapture (&token);
            editor.getPeer()->handleKeyPress (K ('G', 0, 'g'));
            expect (editor.getText().isEmpty());
            h.releaseAll();
            h.physical.insert ('G');
            editor.getPeer()->handleKeyPress (K ('G', 0, 'g'));
            expectEquals (editor.getText(), juce::String ("g"));
            editor.removeFromDesktop();
        }

        beginTest ("window factories and popup preparation attach before the first key without a native hook");
        {
            InputHarness h;
            h.router->activateDesktopRouting();
            expect (h.service->setKeys ("transport.go", { K (K::F13Key) }).wasOk());
            juce::Component auxiliary;
            ShortcutRouter::setWindowScope (auxiliary, Window::auxiliary);
            auxiliary.addToDesktop (0);
            h.physical.insert (K::F13Key);
            auxiliary.getPeer()->handleKeyPress (K (K::F13Key));
            expectEquals (h.downs (CommandIDs::go), 1);
            h.releaseAll();
            auxiliary.removeFromDesktop();
            GoCueLookAndFeel look;
            std::unique_ptr<juce::AlertWindow> dialog (look.createAlertWindow ("test", "test", "OK", "Cancel", {},
                juce::MessageBoxIconType::NoIcon, 2, nullptr));
            dialog->addToDesktop (0);
            int token = 0, candidates = 0;
            h.service->beginCapture (&token, [&] (const K&) { ++candidates; });
            h.physical.insert (K::returnKey);
            dialog->getPeer()->handleKeyPress (K (K::returnKey));
            expectEquals (candidates, 1);
            h.service->endCapture (&token);
            h.releaseAll();
            dialog->removeFromDesktop();
            juce::Component popup;
            look.preparePopupMenuWindow (popup);
            popup.addToDesktop (0);
            h.physical.insert (K::escapeKey);
            popup.getPeer()->handleKeyPress (K (K::escapeKey));
            expectEquals (static_cast<int> (h.panicTimes.size()), 1);
            popup.removeFromDesktop();
        }

        beginTest ("native VK conversion is explicit for F1-F24, letters, modifiers, navigation and numpad");
        {
            const std::vector<std::pair<int, int>> keys { { K::F13Key, 0x7c }, { K::F24Key, 0x87 }, { K::F1Key, 0x70 },
                { 'A', 0x41 }, { '9', 0x39 }, { K::leftKey, 0x25 }, { K::deleteKey, 0x2e }, { K::returnKey, 0x0d },
                { K::numberPad0, 0x60 }, { K::numberPad9, 0x69 }, { K::numberPadAdd, 0x6b },
                { K::numberPadDivide, 0x6f }, { K::numberPadDecimalPoint, 0x6e }, { K::numberPadEquals, 0x92 } };
            for (const auto [code, vk] : keys)
                for (const int mods : { 0, static_cast<int> (M::ctrlModifier), M::altModifier | M::shiftModifier, M::ctrlModifier | M::altModifier | M::shiftModifier })
                {
                    const auto c = PanicKeyHook::convert (K (code, mods, 0));
                    expect (c.binding.has_value());
                    if (c.binding)
                    {
                        expectEquals (c.binding->virtualKey, vk);
                        expectEquals (c.binding->modifiers, mods);
                    }
                }
            expect (! PanicKeyHook::convert (K (0x2007c)).binding); // low byte must not turn into F13
            expect (! PanicKeyHook::convert (K (0)).binding);
            expect (! PanicKeyHook::convert (K (0x1234)).binding);
        }

        beginTest ("OEM uses the current layout resolver, rejects unrepresentable keys and keeps modifiers exact");
        {
            const auto oem = [] (juce::juce_wchar ch) { return ch == ';' ? 0xba : ch == '+' ? 0x1bb : -1; };
            const auto semicolon = PanicKeyHook::convert (K (';', M::ctrlModifier, 0), oem);
            expect (semicolon.binding.has_value());
            if (semicolon.binding) expectEquals (semicolon.binding->virtualKey, 0xba);
            expect (! PanicKeyHook::convert (K ('+'), oem).binding);
            const auto plus = PanicKeyHook::convert (K ('+', M::shiftModifier, 0), oem);
            expect (plus.binding.has_value());
            if (plus.binding) expectEquals (plus.binding->modifiers, static_cast<int> (M::shiftModifier));
            expect (! PanicKeyHook::convert (K ('['), oem).binding);
            expect (PanicKeyHook::convert (K ('['), oem).reason.isNotEmpty());
            const auto realLayout = PanicKeyHook::convert (K (';'));
            expect (realLayout.binding.has_value() || realLayout.reason.isNotEmpty());
        }

        beginTest ("panic first/repeat/up/alias and inclusive 500ms boundary use event times");
        {
            InputHarness h;
            expect (h.service->setKeys ("transport.panicAll", { K (K::escapeKey), K (K::F12Key) }).wasOk());
            h.native (0x1b);
            h.now += 50; h.native (0x1b, 0, true, true);
            h.native (0x1b, 0, false);
            h.now = 1500; h.native (0x7b); // another alias, exactly 500 ms
            h.now = 2000.001; h.native (0x1b);
            drainMessages();
            expectEquals (static_cast<int> (h.hardPanics.size()), 3);
            if (h.hardPanics.size() == 3)
            {
                expect (! h.hardPanics[0]);
                expect (h.hardPanics[1]);
                expect (! h.hardPanics[2]);
                expectEquals (h.panicTimes[1], 1500.0);
            }
            h.now += 100; h.native (0x1b, M::shiftModifier);
            h.native (0x7b, M::ctrlModifier);
            drainMessages();
            expectEquals (static_cast<int> (h.panicTimes.size()), 3);
        }

        beginTest ("message creation times survive hook-entry delay and both Windows clock boundaries");
        {
            for (const uint32_t firstTick : { uint32_t (1000), uint32_t (0x7fffff00), uint32_t (0xffffff00) })
            {
                InputHarness h;
                const uint32_t secondTick = firstTick + 601u;
                const uint32_t hookTick = firstTick + 5000u;
                // Both messages enter WH_KEYBOARD at once, seconds after creation.
                const auto firstTime = PanicKeyHook::messageTimeToHiRes (firstTick, hookTick, 10000.0);
                const auto secondTime = PanicKeyHook::messageTimeToHiRes (secondTick, hookTick, 10000.0);
                expectEquals (firstTime, 5000.0);
                expectEquals (secondTime - firstTime, 601.0);
                const auto first = h.panic->observeWindowsEvent (0x1b, 0, 1, firstTime);
                const auto second = h.panic->observeWindowsEvent (0x1b, 0, 1, secondTime);
                expect (first.has_value() && second.has_value());
                if (first) h.panic->dispatch (*first);
                if (second) h.panic->dispatch (*second);
                drainMessages();
                expectEquals (static_cast<int> (h.hardPanics.size()), 2);
                if (h.hardPanics.size() == 2)
                {
                    expect (! h.hardPanics[0]);
                    expect (! h.hardPanics[1]);
                }
                h.panic->fromUi (secondTime + 500.0); // same time axis as UI/fallback input
                expectEquals (static_cast<int> (h.hardPanics.size()), 3);
                expect (h.hardPanics.back());
            }
        }

        beginTest ("Windows transition and previous-state bits are independent of the extended bit");
        {
            InputHarness h;
            expect (h.service->setKeys ("transport.panicAll", { K (K::F13Key), K (K::numberPadDivide) }).wasOk());
            const auto first = h.panic->observeWindowsEvent (0x7c, 0, 1, 1000);
            expect (first.has_value());
            expect (! h.panic->observeWindowsEvent (0x7c, 0, 0x40000001, 1100));
            expect (! h.panic->observeWindowsEvent (0x7c, 0, 0xc0000001, 1200));
            const auto extended = h.panic->observeWindowsEvent (0x6f, 0, 0x01000001, 1300);
            expect (extended.has_value());
            if (first) { h.panic->dispatch (*first); h.panic->dispatch (*first); }
            if (extended) h.panic->dispatch (*extended);
            drainMessages();
            expectEquals (static_cast<int> (h.panicTimes.size()), 2);
            if (h.hardPanics.size() == 2) expect (h.hardPanics.back());
        }

        beginTest ("event modifiers are immutable and mapping validation/save failures leave the old panic path live");
        {
            InputHarness h;
            expect (h.service->setKeys ("transport.panicAll", { K (K::F24Key, M::ctrlModifier, 0) }).wasOk());
            h.native (0x87); // missing Ctrl
            h.native (0x87, M::ctrlModifier | M::shiftModifier); // extra Shift
            h.native (0x87, M::ctrlModifier);
            h.failSave = true;
            expect (h.service->setKeys ("transport.panicAll", { K (K::escapeKey) }).failed());
            drainMessages(); // no re-read of modifiers at this point
            expectEquals (static_cast<int> (h.panicTimes.size()), 1);
            h.failSave = false;
            for (const auto& named : ShortcutKeyCodec::allowedKeys())
            {
                const K candidate (named.code);
                if (! PanicKeyHook::convert (candidate).binding)
                {
                    const auto before = h.service->getInputGeneration();
                    const auto result = h.service->setKeys ("transport.panicAll", { candidate }, ShortcutService::ConflictPolicy::move);
                    expect (result.failed());
                    expect (result.getErrorMessage().contains (juce::String::fromUTF8 ("패닉용 등록 불가")));
                    expect (h.service->getInputGeneration() == before);
                    expect (h.service->getKeys (CommandIDs::panicAll).contains (K (K::F24Key, M::ctrlModifier, 0)));
                }
            }
        }

        beginTest ("capture and mapping changes invalidate observed asynchronous panic, even after capture ends");
        {
            InputHarness h;
            int token = 0;
            h.native (0x1b);
            h.service->beginCapture (&token);
            h.native (0x1b);
            h.service->endCapture (&token);
            drainMessages();
            expectEquals (static_cast<int> (h.panicTimes.size()), 0);
            h.native (0x1b);
            expect (h.service->setKeys ("transport.panicAll", { K (K::F12Key) }).wasOk());
            drainMessages();
            expectEquals (static_cast<int> (h.panicTimes.size()), 0);
            h.native (0x1b); // Esc was removed
            h.native (0x7b);
            drainMessages();
            expectEquals (static_cast<int> (h.panicTimes.size()), 1);
            h.now += 10;
            h.service->beginCapture (&token);
            h.service->endCapture (&token);
            h.native (0x7b);
            drainMessages();
            expectEquals (static_cast<int> (h.hardPanics.size()), 2);
            if (h.hardPanics.size() == 2) expect (! h.hardPanics.back());
            expect (h.service->setKeys ("transport.panicAll", {}).wasOk());
            expect (h.service->getPanicBindings().empty());
            h.native (0x7b);
            drainMessages();
            expectEquals (static_cast<int> (h.panicTimes.size()), 2);
            h.panic->fromUi (h.now + 600); // menus/buttons survive an empty list
            expectEquals (static_cast<int> (h.panicTimes.size()), 3);
        }

        beginTest ("the real controller uses the event-time panic gesture despite message processing delay");
        {
            InputHarness h;
            AudioEngine engine (0);
            engine.prepare (44100.0, 512);
            ProjectDocument document;
            document.settings.panicSeconds = 3.0;
            double controllerTime = 100.0;
            Scheduler scheduler ([&] { return controllerTime; });
            CueController controller (engine, document, scheduler);
            h.panic = std::make_unique<PanicKeyHook> (*h.service, [&] (double, bool hard) { controller.panicAll (hard); });
            h.now = 1000; h.native (0x1b);
            h.now = 1500.001; h.native (0x1b);
            drainMessages(); // both callbacks run at controllerTime=100, but they are not a double press
            controllerTime = 100.2;
            expect (controller.isPanicLatched());
            h.now = 1800; h.native (0x1b);
            controllerTime = 200.0;
            drainMessages(); // long callback delay must not turn this double press back into a fade
            controllerTime = 200.2;
            expect (! controller.isPanicLatched());
        }

        beginTest ("Windows hook plus JUCE event executes panic once for remapped F12, never for removed Esc");
        {
            InputHarness h;
            expect (h.panic->install().wasOk());
           #if JUCE_WINDOWS
            expect (h.panic->isInstalled());
           #endif
            expect (h.service->setKeys ("transport.panicAll", { K (K::F12Key) }).wasOk());
            h.native (0x7b);
            h.press (K (K::F12Key));
            drainMessages();
            expectEquals (static_cast<int> (h.panicTimes.size()), 1);
            expectEquals (h.target.invocationCount (CommandIDs::panicAll), 0);
            h.releaseAll();
            h.native (0x1b);
            expect (! h.press (K (K::escapeKey)));
            drainMessages();
            expectEquals (static_cast<int> (h.panicTimes.size()), 1);
        }

        beginTest ("native Ctrl+F12 panic owns a JUCE F12 event whose asynchronous Ctrl is already up");
        {
            InputHarness h;
            expect (h.panic->install().wasOk());
            expect (h.service->setKeys ("transport.panicAll", { K (K::F12Key, M::ctrlModifier, 0) }).wasOk());
            expect (h.service->setKeys ("transport.go", { K (K::F12Key) }).wasOk());
            juce::Component origin;
            h.router->attach (origin, Window::main);
            h.nativePhysical.insert (0x7b);
            h.native (0x7b, M::ctrlModifier);
            h.router->prepareNativeEvent (0x7b, M::ctrlModifier, true, false);
            h.router->prepareNativeEvent (0x11, 0, false, false); // queued Ctrl release
            expect (h.router->keyPressed (K (K::F12Key), &origin));
            drainMessages();
            expectEquals (static_cast<int> (h.panicTimes.size()), 1);
            expectEquals (h.downs (CommandIDs::go), 0);
            expectEquals (h.target.invocationCount (CommandIDs::panicAll), 0);
        }

        beginTest ("JUCE-only fallback is single-shot, supports chords and preserves dialog cancellation");
        {
            InputHarness h;
            expect (! h.panic->isInstalled());
            const K panicKey (K::F12Key, M::ctrlModifier | M::altModifier, 0);
            expect (h.service->setKeys ("transport.panicAll", { panicKey, K (K::escapeKey) }).wasOk());
            ShortcutKeyContext modal;
            modal.window = Window::modal;
            modal.textEditing = true;
            h.press (panicKey, modal);
            h.press (panicKey, modal);
            h.releaseAll();
            h.now += 499;
            expect (! h.press (K (K::escapeKey), modal));
            expectEquals (static_cast<int> (h.panicTimes.size()), 2);
            if (h.hardPanics.size() == 2) expect (h.hardPanics.back());
            expectEquals (h.target.invocationCount (CommandIDs::panicAll), 0);
        }

       #if JUCE_WINDOWS
        beginTest ("installation failure activates only the JUCE fallback; mapping edits never reinstall the hook");
        {
            InputHarness installed, fallback;
            expect (installed.panic->install().wasOk());
            expect (fallback.panic->install().failed());
            expect (! fallback.panic->isInstalled());
            fallback.press (K (K::escapeKey));
            fallback.press (K (K::escapeKey));
            expectEquals (static_cast<int> (fallback.panicTimes.size()), 1);
            expect (installed.service->setKeys ("transport.panicAll", { K (K::F12Key) }).wasOk());
            expect (installed.panic->isInstalled());
            installed.press (K (K::F12Key));
            expectEquals (static_cast<int> (installed.panicTimes.size()), 0);
            installed.native (0x7b);
            drainMessages();
            expectEquals (static_cast<int> (installed.panicTimes.size()), 1);
        }
       #endif

        beginTest ("read-only manual copy/select-all remain standard UI; panic still precedes input/component owners");
        {
            InputHarness h;
            juce::TextEditor manual;
            manual.setReadOnly (true);
            h.router->attach (manual, Window::auxiliary);
            for (const auto key : { K ('C', M::ctrlModifier, 0), K ('A', M::ctrlModifier, 0) })
            {
                expect (! h.press (key, h.router->contextFor (&manual, key), &manual));
                h.releaseAll();
            }
            expect (h.target.invocations.empty());
            FixedComponent wave (ShortcutScope::waveform);
            expect (h.service->setKeys ("transport.panicAll", { K (K::deleteKey) }, ShortcutService::ConflictPolicy::move).wasOk());
            ShortcutKeyContext context;
            context.focus = ShortcutScope::waveform;
            context.cueHotkeys.push_back ({ "cue", K (K::deleteKey) });
            h.press (K (K::deleteKey), context, &wave);
            expectEquals (wave.actions, 0);
            expectEquals (h.cueFires, 0);
            expectEquals (static_cast<int> (h.panicTimes.size()), 1);
        }

       #if JUCE_WINDOWS
        beginTest ("keypad panic character aliases conflict with commands and still belong to panic");
        {
            InputHarness h;
            expect (h.panic->install().wasOk());
            expect (h.service->setKeys ("transport.panicAll", { K (K::numberPadAdd) }).wasOk());
            expect (h.service->setKeys ("transport.go", { K ('+') }).failed());
            juce::Component origin;
            h.router->attach (origin, Window::main);
            origin.addToDesktop (0);
            h.nativePhysical.insert (0x6b);
            h.native (0x6b);
            h.router->prepareNativeEvent (0x6b, 0, true, false);
            origin.getPeer()->handleKeyPress (K ('+', 0, '+'));
            drainMessages();
            expectEquals (h.downs (CommandIDs::go), 0);
            expectEquals (static_cast<int> (h.panicTimes.size()), 1);
            expectEquals (h.target.invocationCount (CommandIDs::panicAll), 0);
            origin.removeFromDesktop();
        }

        beginTest ("failed-hook fallback fires each keypad operator panic once across repeats and polling");
        {
            InputHarness installed;
            expect (installed.panic->install().wasOk());
            struct Operator { int padCode, vk, character; };
            const Operator operators[] {
                { K::numberPadAdd, 0x6b, '+' }, { K::numberPadSubtract, 0x6d, '-' },
                { K::numberPadMultiply, 0x6a, '*' }, { K::numberPadDivide, 0x6f, '/' },
                { K::numberPadSeparator, 0x6c, ',' }, { K::numberPadDecimalPoint, 0x6e, '.' },
                { K::numberPadDecimalPoint, 0x6e, ',' }, { K::numberPadEquals, 0x92, '=' }
            };
            for (const auto& op : operators)
            {
                InputHarness h;
                expect (h.panic->install().failed());
                expect (! h.panic->isInstalled());
                expect (h.service->setKeys ("transport.panicAll", { K (op.padCode) }).wasOk());
                juce::Component origin;
                h.router->attach (origin, Window::main);
                h.nativePhysical.insert (op.vk); // no native observation; real keypad VK only
                for (int repeat = 0; repeat < 3; ++repeat)
                {
                    expect (h.router->keyPressed (K (op.character, 0, static_cast<juce::juce_wchar> (op.character)), &origin));
                    h.router->pollKeyState();
                }
                expectEquals (static_cast<int> (h.panicTimes.size()), 1);
                expectEquals (h.downs (CommandIDs::go), 0);
                expectEquals (h.target.invocationCount (CommandIDs::panicAll), 0);
                h.releaseAll();
                h.nativePhysical.insert (op.vk);
                h.router->keyPressed (K (op.character), &origin);
                expectEquals (static_cast<int> (h.panicTimes.size()), 2);
            }
        }

        beginTest ("keypad alias conflicts share reject, move, import and cue ownership rules");
        {
            InputHarness h;
            expect (h.service->setKeys ("transport.panicAll", { K (K::numberPadAdd) }).wasOk());
            expect (h.service->setKeys ("transport.go", { K ('+') }).failed());
            ShortcutKeyContext context;
            context.cueHotkeys.push_back ({ "cue", K ('+') });
            const auto owner = h.service->resolveKeyOwner (K (K::numberPadAdd), context);
            expectEquals (owner.commandID, static_cast<int> (CommandIDs::panicAll));
            expectEquals (static_cast<int> (owner.conflicts.size()), 1);
            expect (h.service->setKeys ("transport.go", { K ('+') }, ShortcutService::ConflictPolicy::move).wasOk());
            expect (h.service->getKeys (CommandIDs::panicAll).isEmpty());
            expect (h.service->setKeys ("transport.panicAll", { K (K::numberPadAdd) }).failed());
            ShortcutProfile conflicting;
            conflicting.overrides["transport.panicAll"] = { K (K::numberPadAdd) };
            conflicting.overrides["transport.go"] = { K ('+') };
            juce::String xml;
            expect (conflicting.serialise (xml).wasOk());
            expect (h.service->importProfile (xml).failed());
            expect (h.service->setKeys ("transport.panicAll", { K (K::numberPadAdd, M::ctrlModifier, 0) }).wasOk());
            expect (h.service->resolveKeyOwner (K ('+'), {}).commandID == CommandIDs::go);
            expect (h.service->resolveKeyOwner (K ('+', M::ctrlModifier, 0), {}).commandID == CommandIDs::panicAll);
            expect (h.service->setKeys ("transport.panicAll", { K (K::numberPadDecimalPoint) }).wasOk());
            expect (h.service->setKeys ("transport.go", { K (K::numberPadSeparator) }).failed());
            expect (h.service->setKeys ("transport.go", { K ('.') }).failed());
            expect (h.service->setKeys ("transport.go", { K (',') }).failed());
        }
       #endif

        beginTest ("real cue table Delete follows mapping; old Delete/Backspace execute no command");
        {
            InputHarness h;
            CueList cues;
            juce::AudioFormatManager formats;
            CueTable table (cues, formats, h.manager);
            h.router->attach (table, Window::main);
            juce::Component* box = nullptr;
            for (auto* child : table.getChildren())
                if (dynamic_cast<juce::TableListBox*> (child) != nullptr) box = child;
            expect (box != nullptr);
            expect (h.service->setKeys ("cue.remove", { K (K::F13Key) }).wasOk());
            if (box != nullptr)
            {
                for (const auto key : { K (K::deleteKey), K (K::backspaceKey), K (K::F13Key) })
                {
                    const bool consumed = h.press (key, h.router->contextFor (box, key), box);
                    if (! consumed) box->keyPressed (key);
                    h.releaseAll();
                }
                expectEquals (h.downs (CommandIDs::removeCue), 1);
            }
        }
    }
};
static ShortcutRouterTests shortcutRouterTests;
}
