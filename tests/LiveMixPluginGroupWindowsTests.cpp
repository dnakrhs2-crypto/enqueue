#include "TestGainPlugin.h"
#include "../livemix/src/ui/MainComponent.h"
#include "ui/SettingsDialog.h"

#include <stdexcept>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
using namespace gocue::livemix;

namespace
{
    // Same private-member access used by the production UI screenshot tests; no test API on MainComponent.
    template<class Tag, typename Tag::Type value> struct Access { friend typename Tag::Type member (Tag) { return value; } };
    struct Windows { using Type = PluginWindowManager MainComponent::*; friend Type member (Windows); };
    struct Status { using Type = juce::Label MainComponent::*; friend Type member (Status); };
    struct Held { using Type = int MainComponent::*; friend Type member (Held); };
    struct Register { using Type = void (MainComponent::*)(); friend Type member (Register); };
    struct Settings { using Type = void (MainComponent::*)(); friend Type member (Settings); };
    struct Positions
    {
        using Type = std::map<const juce::AudioPluginInstance*, juce::Point<int>> PluginWindowManager::*;
        friend Type member (Positions);
    };
    template struct Access<Windows, &MainComponent::windows>;
    template struct Access<Status, &MainComponent::statusLeft>;
    template struct Access<Held, &MainComponent::hotkeysHeld>;
    template struct Access<Register, &MainComponent::registerHotkeys>;
    template struct Access<Settings, &MainComponent::showSettingsDialog>;
    template struct Access<Positions, &PluginWindowManager::positions>;

    struct TemporaryFolder
    {
        juce::File path = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("lm-group-windows-" + juce::Uuid().toString());
        TemporaryFolder() { path.createDirectory(); }
        ~TemporaryFolder() { path.deleteRecursively(); }
    };

    PluginEditorWindow* editorFor (const juce::AudioPluginInstance& plugin)
    {
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* window = dynamic_cast<PluginEditorWindow*> (desktop.getComponent (i)))
                if (&window->getPlugin() == &plugin) return window;
        return nullptr;
    }

   #if JUCE_WINDOWS
    BOOL CALLBACK countHiddenStaticPopups (HWND window, LPARAM result)
    {
        wchar_t className[32] {};
        GetClassNameW (window, className, (int) std::size (className));
        if (juce::String (className).equalsIgnoreCase ("Static")
            && (GetWindowLongPtrW (window, GWL_STYLE) & WS_POPUP) != 0 && ! IsWindowVisible (window))
            ++*reinterpret_cast<int*> (result);
        return TRUE;
    }

    int hiddenStaticPopupCount()
    {
        int count = 0;
        EnumThreadWindows (GetCurrentThreadId(), countHiddenStaticPopups, reinterpret_cast<LPARAM> (&count));
        return count;
    }
   #endif

    struct WindowPlugin : TestGainPlugin
    {
        struct Editor : juce::AudioProcessorEditor
        {
            explicit Editor (juce::AudioProcessor& processor) : AudioProcessorEditor (processor) {}
            ~Editor() override { processor.editorBeingDeleted (this); }   // as JUCE's VST2/VST3 editor wrappers do
        };
        explicit WindowPlugin (juce::String name = "WindowTest") : TestGainPlugin (1.0f), name (std::move (name)) {}
        const juce::String getName() const override { return name; }
        bool hasEditor() const override { return true; }
        juce::AudioProcessorEditor* createEditor() override
        {
            if (onCreate) onCreate();
            auto* editor = new Editor (*this);
            editor->setResizable (resizable, false);
            editor->setSize (width, height);
            return editor;
        }
        juce::String name;
        int width = 340, height = 180;
        bool resizable = false;
        std::function<void()> onCreate;
    };

    struct Rig
    {
        TemporaryFolder folder;
        LiveMixSettings settings { folder.path };
        MixEngine engine { "Local\\LiveMix.GroupWindowsTest." + juce::Uuid().toString() };
        MixDocument document { engine };
        std::unique_ptr<MainComponent> main;
        const juce::AudioPluginInstance* active = nullptr;

        Rig()
        {
            settings.setMicMuteHotkey ({});
            settings.setFxMuteHotkey ({});
            settings.setWindowHotkey ({});
            settings.setMicAllMuteHotkey ({});
            settings.setFxAllMuteHotkey ({});
            engine.prepare (48000.0, 64);
            document.applyToEngine();
            ObsPluginActions actions;
            actions.roots = [path = folder.path] { return ObsPluginInstaller::Roots { path, path, path, [] { return false; }, {} }; };
            actions.elevate = [] (juce::String&) { return ObsPluginInstaller::Result::needsElevation; };
            main = std::make_unique<MainComponent> (document, settings, actions);
            main->setSize (1440, 900);
            windows().isWindowInUse = [this] (const PluginEditorWindow& window) { return &window.getPlugin() == active; };
        }
        PluginWindowManager& windows() { return (*main).*member (Windows()); }
        juce::Label& status() { return (*main).*member (Status()); }
        juce::Uuid mic() const { return document.getSession().channels[0].id; }
        juce::File file() const { return folder.path.getChildFile ("session.livemix"); }
        void press (int group = 0) { main->hotkeyPressed (MainComponent::firstPluginGroupWindowHotkeyId + group); }
        void registerKeys() { ((*main).*member (Register()))(); }
        void showSettings() { ((*main).*member (Settings()))(); }
        WindowPlugin& add (const juce::Uuid& channel, const juce::String& name)
        {
            auto plugin = std::make_unique<WindowPlugin> (name);
            auto& result = *plugin;
            engine.getChannelChain (channel)->addPlugin (std::move (plugin));
            return result;
        }
        void join (const juce::Uuid& channel, int group, int slot)
        {
            document.setPluginGroupMember (channel, group, engine.getChannelChain (channel)->getSlot (slot).state.slotId, true);
        }
    };

    juce::Component* settingsContent()
    {
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* dialog = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)); dialog != nullptr && dialog->getName() == tr ("설정"))
                if (auto* viewport = dynamic_cast<juce::Viewport*> (dialog->getContentComponent()))
                    return viewport->getViewedComponent();
        return nullptr;
    }

    juce::Label* labelWithText (juce::Component& parent, const juce::String& text)
    {
        for (auto* child : parent.getChildren())
            if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getText() == text)
                return label;
        return nullptr;
    }
}

class LiveMixPluginGroupWindowsTests : public juce::UnitTest
{
public:
    LiveMixPluginGroupWindowsTests() : UnitTest ("LiveMix plugin group windows", "LiveMix") {}

    void runTest() override
    {
        beginTest ("each group's window key persists separately from its ON/OFF key");
        {
            TemporaryFolder folder;
            {
                LiveMixSettings settings (folder.path);
                for (int group = 1; group <= 5; ++group)
                {
                    expect (settings.getPluginGroupWindowHotkey (group).isEmpty());
                    settings.setPluginGroupWindowHotkey (group, "  Ctrl + Alt + F" + juce::String (group + 12) + "  ");
                    settings.setPluginGroupHotkey (group, "F" + juce::String (group));
                }
                settings.saveIfNeeded();
            }
            LiveMixSettings settings (folder.path);
            for (int group = 1; group <= 5; ++group)
            {
                expectEquals (settings.getPluginGroupWindowHotkey (group), "Ctrl + Alt + F" + juce::String (group + 12));
                expectEquals (settings.getPluginGroupHotkey (group), "F" + juce::String (group));
            }
            settings.setPluginGroupWindowHotkey (3, {});
            expect (settings.getPluginGroupWindowHotkey (3).isEmpty());
            expect (settings.getPluginGroupWindowHotkey (2).isNotEmpty());
            expect (settings.getPluginGroupWindowHotkey (4).isNotEmpty());
        }

        beginTest ("ids 23..27 follow all existing ids; keys register, release during capture, and clear");
        {
            expectEquals (MainComponent::allMicsHotkeyId, 21);
            expectEquals (MainComponent::allFxHotkeyId, 22);
            expectEquals (MainComponent::firstPluginGroupWindowHotkeyId, 23);
            expectEquals (MainComponent::lastHotkeyId, 27);
            expectEquals (MainComponent::lastHotkeyId - MainComponent::firstPluginGroupWindowHotkeyId + 1, MixSession::maxPluginGroups);
            Rig rig;
            for (int group = 1; group <= 5; ++group)
                rig.settings.setPluginGroupWindowHotkey (group, "Ctrl + Alt + Shift + F" + juce::String (group + 12));
            rig.registerKeys();
           #if JUCE_WINDOWS
            GlobalHotkeys probe;
            auto available = [&] (int group)
            {
                juce::String error;
                const bool result = probe.set (1, juce::KeyPress::createFromDescription (rig.settings.getPluginGroupWindowHotkey (group)), error);
                probe.clear (1);
                return result;
            };
            for (int group = 1; group <= 5; ++group) expect (! available (group));
            rig.showSettings();
            auto* content = settingsContent();
            expect (content != nullptr);
            if (content != nullptr)
            {
                auto* key = dynamic_cast<HotkeyButton*> (content->findChildWithID ("hotkey-group-window-5"));
                expect (key != nullptr);
                if (key != nullptr)
                {
                    key->clicked();
                    for (int group = 1; group <= 5; ++group) expect (available (group));
                    key->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
                    for (int group = 1; group <= 5; ++group) expect (! available (group));
                }
            }
            SettingsDialog::closeIfOpen();
            for (int group = 1; group <= 5; ++group)
            {
                const auto oldKey = rig.settings.getPluginGroupWindowHotkey (group);
                rig.settings.setPluginGroupWindowHotkey (group, {});
                rig.registerKeys();
                juce::String error;
                expect (probe.set (1, juce::KeyPress::createFromDescription (oldKey), error), error);
                probe.clear (1);
            }
           #endif
        }

        beginTest ("absent groups and empty or missing members report errors without changing the session");
        {
            Rig rig;
            expect (rig.document.save (rig.file()).wasOk());
            rig.press();
            expectEquals (rig.status().getText(), trf ("플러그인 그룹 {0}이(가) 있는 마이크가 없습니다 (체인 열기 아래 '플러그인 그룹'에서 만듭니다)", "1"));
            expect (rig.status().findColour (juce::Label::textColourId) == livemix::Palette::danger);
            expect (! rig.document.isDirty());
            rig.document.addPluginGroup (rig.mic());
            rig.press();
            expectEquals (rig.status().getText(), trf ("플러그인 그룹 {0}에 넣은 플러그인이 없습니다", "1"));
            PluginSlotState missing;
            missing.slotId = juce::Uuid();
            rig.engine.getChannelChain (rig.mic())->addMissingSlot (missing);
            rig.join (rig.mic(), 0, 0);
            expect (rig.document.save (rig.file()).wasOk());
            rig.press();
            expectEquals (rig.status().getText(), trf ("플러그인 그룹 {0}에 넣은 플러그인이 없습니다", "1"));
            expect (rig.status().findColour (juce::Label::textColourId) == livemix::Palette::danger);
            expectEquals (rig.windows().getNumOpenWindows(), 0);
            expect (! rig.document.isDirty());
        }

        beginTest ("all five ids address their own numbered group");
        {
            Rig rig;
            for (int group = 0; group < 5; ++group)
            {
                rig.document.addPluginGroup (rig.mic());
                rig.add (rig.mic(), juce::String (group));
                rig.join (rig.mic(), group, group);
            }
            expect (rig.document.save (rig.file()).wasOk());
            auto* chain = rig.engine.getChannelChain (rig.mic());
            for (int group = 0; group < 5; ++group)
            {
                rig.press (group);
                expectEquals (rig.windows().getNumOpenWindows(), 1);
                expect (rig.windows().isOpen (*chain->getSlot (group).plugin));
                expectEquals (rig.status().getText(), trf ("플러그인 그룹 {0} 화면 열림", juce::String (group + 1)));
                rig.active = chain->getSlot (group).plugin.get();
                rig.press (group);
                expectEquals (rig.windows().getNumOpenWindows(), 0);
                expect (! rig.document.isDirty());
                rig.active = nullptr;
            }
        }

        beginTest ("members of two mics open in session and chain order, including OFF groups; only their windows close");
        {
            Rig rig;
            const auto secondMic = rig.document.addChannel();
            auto& first = rig.add (rig.mic(), "First");
            auto& outside = rig.add (rig.mic(), "Outside");
            auto& second = rig.add (rig.mic(), "Second");
            auto& third = rig.add (secondMic, "Third");
            second.width = 600;
            second.height = 340;
            rig.document.addPluginGroup (rig.mic());
            rig.document.addPluginGroup (secondMic);
            rig.join (rig.mic(), 0, 2);   // membership insertion order is not chain order
            rig.join (rig.mic(), 0, 0);
            rig.join (secondMic, 0, 0);
            rig.document.setPluginGroupOff (rig.mic(), 0, true);
            rig.document.setChannelOn (secondMic, false);
            juce::StringArray opened;
            for (auto* plugin : { &first, &second, &third })
                plugin->onCreate = [&, plugin]
                {
                    opened.add (plugin->getName());
                    auto* window = editorFor (*plugin);
                    expect (window != nullptr && window->getPeer() != nullptr && ! window->isVisible());
                };
            expect (rig.document.save (rig.file()).wasOk());
            const auto session = rig.document.getSession().toJson();
            int toggleMain = 0, closed = 0;
            rig.main->onToggleWindow = [&] { ++toggleMain; };
            const auto onClosed = rig.windows().onWindowClosed;
            rig.windows().onWindowClosed = [&] (juce::AudioPluginInstance& plugin)
            {
                expect (rig.windows().isOpen (plugin));
                ++closed;
                onClosed (plugin);
            };
            rig.press();
            expectEquals (opened.joinIntoString (","), juce::String ("First,Second,Third"));
            expectEquals (rig.windows().getNumOpenWindows(), 3);
            expect (! rig.windows().isOpen (outside));
            expect (! rig.main->isVisible());
            expectEquals (toggleMain, 0);
            if (auto* window = editorFor (first))
                expectEquals (window->getName(), rig.document.getSession().channels[0].name + " - First");
            else expect (false);
            if (auto* window = editorFor (third))
                expectEquals (window->getName(), rig.document.getSession().channels[1].name + " - Third");
            else expect (false);

            // An unrelated plugin in front must not turn this hotkey into a close action.
            rig.windows().open (outside, "Outside");
            rig.active = &outside;
            rig.press();
            expectEquals (rig.windows().getNumOpenWindows(), 4);
            expectEquals (closed, 0);
            rig.active = &second;
            rig.press();
            expectEquals (rig.windows().getNumOpenWindows(), 1);
            expect (rig.windows().isOpen (outside));
            expectEquals (closed, 3);
            expectEquals (rig.status().getText(), trf ("플러그인 그룹 {0} 화면 닫힘", "1"));
            expectEquals (rig.document.getSession().toJson(), session);
            expect (! rig.document.isDirty());
        }

        beginTest ("covered, minimised and partly open editors are recalled; a hidden or minimised main stays so");
        {
            Rig rig;
            auto& first = rig.add (rig.mic(), "First");
            auto& second = rig.add (rig.mic(), "Second");
            second.resizable = true;
            rig.document.addPluginGroup (rig.mic());
            rig.join (rig.mic(), 0, 0);
            rig.join (rig.mic(), 0, 1);
            expect (rig.document.save (rig.file()).wasOk());
            juce::DocumentWindow host ("Test main", juce::Colours::black, juce::DocumentWindow::allButtons);
            host.setContentNonOwned (rig.main.get(), true);
            host.setVisible (true);
            host.setMinimised (true);
            rig.press();
            expect (host.isMinimised());
            auto* window = editorFor (first);
            expect (window != nullptr);
            if (window != nullptr)
            {
                rig.press();
                expect (editorFor (first) == window);
                window->setMinimised (true);
                expect (window->isMinimised());
                rig.press();
                expect (editorFor (first) == window && ! window->isMinimised());
                expect (rig.windows().closeByUser (second));
                host.setVisible (false);
                rig.press();
                expect (editorFor (first) == window && rig.windows().isOpen (second));
                expect (! host.isVisible());
                expectEquals (rig.windows().getNumOpenWindows(), 2);
                rig.active = &second;
                rig.press();
                expectEquals (rig.windows().getNumOpenWindows(), 0);
                expect (! host.isVisible());
            }
            expect (! rig.document.isDirty());
            host.clearContentComponent();
        }

        beginTest ("keys held during session loading do nothing; closing still checks silent plugin edits");
        {
            Rig rig;
            auto& plugin = rig.add (rig.mic(), "Member");
            auto& other = rig.add (rig.mic(), "Other");
            rig.document.addPluginGroup (rig.mic());
            rig.join (rig.mic(), 0, 0);
            expect (rig.document.save (rig.file()).wasOk());
            const auto before = rig.status().getText();
            (*rig.main).*member (Held()) = 1;
            rig.press();
            expectEquals (rig.status().getText(), before);
            expect (! rig.windows().isOpen (plugin));
            (*rig.main).*member (Held()) = 0;
            rig.press();
            plugin.gain = 0.4f;
            expect (! rig.document.isDirty());
            const int reads = plugin.stateReads, otherReads = other.stateReads;
            rig.active = &plugin;
            (*rig.main).*member (Held()) = 1;
            rig.press();
            expect (rig.windows().isOpen (plugin));
            (*rig.main).*member (Held()) = 0;
            rig.press();
            expect (rig.document.isDirty());
            expectEquals (plugin.stateReads, reads + 1);
            expectEquals (other.stateReads, otherReads);
            expect (! rig.windows().isOpen (plugin));
        }

        beginTest ("isOpen and closeByUser share the close button callback; ordinary open stays centred by default");
        {
            WindowPlugin plugin;
            PluginWindowManager windows;
            expect (! windows.isOpen (plugin));
            expect (! windows.isInUse (plugin));
            expect (! windows.closeByUser (plugin));
            int closed = 0;
            windows.onWindowClosed = [&] (juce::AudioPluginInstance& p)
            {
                expect (&p == &plugin && editorFor (p) != nullptr);
                ++closed;
            };
            windows.open (plugin, "Ordinary");
            auto* window = editorFor (plugin);
            expect (window != nullptr && windows.isOpen (plugin));
            if (window != nullptr)
            {
                const auto centred = window->getPosition();
                window->setTopLeftPosition (centred + juce::Point<int> (50, 40));
                const auto moved = window->getPosition();
                windows.open (plugin, "Renamed");
                expect (editorFor (plugin) == window && window->getPosition() == moved);
                expectEquals (window->getName(), juce::String ("Renamed"));
                expectEquals (windows.getNumOpenWindows(), 1);
                window->closeButtonPressed();
                windows.open (plugin, "Again");
                expect (editorFor (plugin)->getPosition() == centred,
                        "expected centre " + centred.toString() + ", got " + editorFor (plugin)->getBounds().toString());
                expect (windows.closeByUser (plugin));
                expect (! windows.closeByUser (plugin));
                expectEquals (closed, 2);
            }
        }

       #if JUCE_WINDOWS
        beginTest ("fixed and resizable hotkey editors are visible and leave no hidden foreground window after close");
        {
            for (const bool resizable : { false, true })
            {
                const int before = hiddenStaticPopupCount();
                WindowPlugin plugin;
                plugin.resizable = resizable;
                PluginWindowManager windows;
                windows.openInForeground (plugin, "Hotkey editor");
                auto* window = editorFor (plugin);
                expect (window != nullptr && window->isVisible());
                expect (windows.closeByUser (plugin));
                expectEquals (hiddenStaticPopupCount(), before);
            }
        }

        beginTest ("a throwing hotkey editor falls back to the generic editor without leaving a hidden foreground window");
        {
            const int before = hiddenStaticPopupCount();
            WindowPlugin plugin;
            plugin.onCreate = [] { throw std::runtime_error ("editor creation failed"); };
            PluginWindowManager windows;
            windows.openInForeground (plugin, "Fallback editor");
            auto* window = editorFor (plugin);
            expect (window != nullptr && window->isVisible());
            if (window != nullptr)
                expect (dynamic_cast<juce::GenericAudioProcessorEditor*> (window->getContentComponent()) != nullptr);
            expect (windows.closeByUser (plugin));
            expectEquals (hiddenStaticPopupCount(), before);
        }
       #endif

        beginTest ("positions survive both user close paths and open paths; editor size remains the plugin's choice");
        {
            WindowPlugin plugin;
            PluginWindowManager windows;
            windows.setRememberWindowPositions (true);
            windows.open (plugin, "Move");
            auto* window = editorFor (plugin);
            expect (window != nullptr);
            if (window != nullptr)
            {
                const auto centred = window->getPosition();
                window->setTopLeftPosition (centred + juce::Point<int> (45, 35));
                const auto moved = window->getPosition();
                window->closeButtonPressed();
                windows.openInForeground (plugin, "Recall");
                expect (editorFor (plugin)->getPosition() == moved);
                expect (windows.closeByUser (plugin));
                plugin.width += 80;
                plugin.height += 40;
                windows.open (plugin, "Resized editor");
                window = editorFor (plugin);
                expect (window->getPosition() == moved);
                expectEquals (window->getContentComponent()->getWidth(), plugin.width, "window " + window->getBounds().toString());
                expectEquals (window->getContentComponent()->getHeight(), plugin.height, "window " + window->getBounds().toString());
                windows.closeByUser (plugin);
                windows.setRememberWindowPositions (false);
                plugin.width -= 80;
                plugin.height -= 40;
                windows.open (plugin, "Default again");
                expect (editorFor (plugin)->getPosition() == centred,
                        "expected centre " + centred.toString() + ", got " + editorFor (plugin)->getBounds().toString());
                expect ((windows.*member (Positions())).empty());
            }
        }

        beginTest ("an unreachable saved title bar falls back to centre; fresh editors of different sizes cascade");
        {
            WindowPlugin first, second;
            second.width = 600;
            second.height = 340;
            PluginWindowManager windows;
            windows.setRememberWindowPositions (true);
            windows.open (first, "First");
            auto* window = editorFor (first);
            expect (window != nullptr);
            if (window != nullptr)
            {
                const auto centre = window->getPosition();
                window->setTopLeftPosition (-100000, -100000);
                windows.closeByUser (first);
                windows.open (first, "Monitor gone");
                expect (editorFor (first)->getPosition() == centre,
                        "expected centre " + centre.toString() + ", got " + editorFor (first)->getBounds().toString());
                windows.openInForeground (second, "Second");
                auto* other = editorFor (second);
                expect (other != nullptr);
                if (other != nullptr)
                {
                    expect (other->getPosition() != centre);
                    expect (other->getY() > editorFor (first)->getY());
                    expect (other->getX() > editorFor (first)->getX());
                }
            }
        }

        beginTest ("removal, clear, restore and chain destruction forget closed editors before addresses can be reused");
        {
            for (int action = 0; action < 4; ++action)
            {
                PluginWindowManager windows;
                windows.setRememberWindowPositions (true);
                auto chain = std::make_unique<PluginChain>();
                chain->setListener (&windows);
                auto plugin = std::make_unique<WindowPlugin>();
                auto& p = *plugin;
                chain->addPlugin (std::move (plugin));
                windows.open (p, "Retiring");
                windows.closeByUser (p);
                expectEquals ((int) (windows.*member (Positions())).size(), 1);
                if (action == 0) chain->removePlugin (0);
                if (action == 1) chain->clear();
                if (action == 2)
                    chain->restore (chain->getStates(), [] (const PluginSlotState&, juce::String&) { return std::make_unique<WindowPlugin>(); });
                if (action == 3) chain.reset();
                expect ((windows.*member (Positions())).empty());
                expectEquals (windows.getNumOpenWindows(), 0);
                if (chain != nullptr && chain->getNumSlots() > 0)
                {
                    windows.open (*chain->getSlot (0).plugin, "Replacement");
                    chain->clear();
                    expectEquals (windows.getNumOpenWindows(), 0);
                }
            }
        }

        beginTest ("LiveMix channel deletion, session load and new session forget remembered positions");
        {
            for (int action = 0; action < 3; ++action)
            {
                Rig rig;
                auto& plugin = rig.add (rig.mic(), "Retiring mic");
                rig.windows().open (plugin, "Remember");
                rig.windows().closeByUser (plugin);
                rig.engine.getMasterChain().addPlugin (std::make_unique<WindowPlugin>());
                auto& master = *rig.engine.getMasterChain().getSlot (0).plugin;
                rig.windows().open (master, "Master");
                rig.windows().closeByUser (master);
                expectEquals ((int) (rig.windows().*member (Positions())).size(), 2);
                expect (rig.document.save (rig.file()).wasOk());
                if (action == 0)
                {
                    rig.document.removeChannel (rig.mic());
                    expectEquals ((int) (rig.windows().*member (Positions())).size(), 1);
                    rig.engine.getMasterChain().clear();
                }
                if (action == 1) expect (rig.document.load (rig.file()).wasOk());
                if (action == 2) rig.document.newSession();
                expect ((rig.windows().*member (Positions())).empty());
            }
        }

        beginTest ("settings rows follow group 5 in order, capture keys into settings and reject keys used by other rows");
        {
            Rig rig;
            rig.settings.setMicMuteHotkey ("F9");
            rig.showSettings();
            auto* content = settingsContent();
            expect (content != nullptr);
            if (content != nullptr)
            {
                auto* previous = labelWithText (*content, trf ("플러그인 그룹 {0}", "5"));
                expect (previous != nullptr);
                for (int group = 1; group <= 5; ++group)
                {
                    auto* label = labelWithText (*content, trf ("플러그인 그룹 {0} 화면", juce::String (group)));
                    auto* key = dynamic_cast<HotkeyButton*> (content->findChildWithID ("hotkey-group-window-" + juce::String (group)));
                    expect (label != nullptr && key != nullptr);
                    if (label == nullptr || key == nullptr) continue;
                    if (previous != nullptr) expect (previous->getY() < label->getY());
                    previous = label;
                    expect (key->validate (juce::KeyPress::createFromDescription ("F9")).isNotEmpty());
                    const auto description = "Ctrl + Alt + Shift + F" + juce::String (group + 12);
                    const auto chosen = juce::KeyPress::createFromDescription (description);
                    key->clicked();
                    key->keyPressed (chosen);
                    expectEquals (rig.settings.getPluginGroupWindowHotkey (group), chosen.getTextDescription());
                    key->clicked();
                    key->keyPressed (juce::KeyPress::createFromDescription ("F9"));
                    expectEquals (rig.settings.getPluginGroupWindowHotkey (group), chosen.getTextDescription());
                    if (group > 1)
                        expect (key->validate (juce::KeyPress::createFromDescription (rig.settings.getPluginGroupWindowHotkey (1))).isNotEmpty());
                }
                auto* mic = dynamic_cast<HotkeyButton*> (content->findChildWithID ("hotkey-mic-all"));
                expect (mic != nullptr);
                if (mic != nullptr)
                    expect (mic->validate (juce::KeyPress::createFromDescription (rig.settings.getPluginGroupWindowHotkey (5))).isNotEmpty());
            }
            SettingsDialog::closeIfOpen();
        }
    }
};
static LiveMixPluginGroupWindowsTests liveMixPluginGroupWindowsTests;
}
