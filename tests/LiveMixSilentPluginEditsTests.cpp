#include "MixDocument.h"
#include "LiveMixSettings.h"
#include "TestGainPlugin.h"
#include "../livemix/src/ui/MainComponent.h"
#include "ui/MasterCard.h"

namespace gocue::tests
{
using namespace gocue::livemix;

namespace
{
    class ChangingStatePlugin final : public TestGainPlugin
    {
    public:
        ChangingStatePlugin() : TestGainPlugin (1.0f) {}
        void getStateInformation (juce::MemoryBlock& block) override
        {
            if (changeOnRead) gain += 0.01f;
            TestGainPlugin::getStateInformation (block);
            if (notifyOnRead) updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
        }
        bool changeOnRead = false, notifyOnRead = false;
    };

    struct SilentEditFixture
    {
        juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("lm-silent-edits-" + juce::Uuid().toString());
        juce::File file = directory.getChildFile ("session.livemix");
        MixEngine engine;
        MixDocument document { engine };
        TestGainPlugin* plugin = nullptr;

        SilentEditFixture()
        {
            directory.createDirectory();
            engine.prepare (48000.0, 64);
            document.applyToEngine();
            auto p = std::make_unique<TestGainPlugin> (1.0f);
            plugin = p.get();
            engine.getMasterChain().addPlugin (std::move (p));
        }
        ~SilentEditFixture() { directory.deleteRecursively(); }
    };

    PluginEditorWindow* editorFor (juce::AudioPluginInstance& plugin)
    {
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* window = dynamic_cast<PluginEditorWindow*> (desktop.getComponent (i)))
                if (&window->getPlugin() == &plugin) return window;
        return nullptr;
    }
}

class LiveMixSilentPluginEditsTests : public juce::UnitTest
{
public:
    LiveMixSilentPluginEditsTests() : UnitTest ("LiveMix silent plugin edits", "LiveMix") {}

    void runTest() override
    {
        beginTest ("silent edits stay clean until checked; equality and reads of the chain cache do not move the baseline");
        {
            SilentEditFixture f;
            expect (f.document.save (f.file).wasOk());
            expect (! f.document.checkPluginStates());
            expect (! f.document.isDirty());
            f.plugin->gain = 0.25f; // no listener notification: learned profile / the plugin's own preset browser
            expect (! f.document.pollPluginEdits());
            expect (! f.document.isDirty());
            f.engine.getMasterChain().getStates();
            expect (f.document.checkPluginStates());
            expect (f.document.isDirty());
            expect (f.document.save (f.file).wasOk());
            expect (! f.document.checkPluginStates());
            expect (! f.document.isDirty());
            expect (f.document.load (f.file).wasOk()); // absent Test plugin retains exactly the loaded baseline
            expect (! f.document.checkPluginStates());
            f.document.newSession();
            expect (! f.document.checkPluginStates());
        }

        beginTest ("incomplete capture changes nothing; failed write does not advance the saved baseline");
        {
            SilentEditFixture f;
            auto second = std::make_unique<TestGainPlugin> (1.0f);
            auto* other = second.get();
            f.engine.getMasterChain().addPlugin (std::move (second));
            expect (f.document.save (f.file).wasOk());
            f.plugin->gain = 0.5f;
            other->throwOnGetState = true;
            const auto modelBefore = f.document.getSession().master.chain[0].stateBase64;
            expect (! f.document.checkPluginStates());
            expect (! f.document.isDirty());
            expectEquals (f.document.getSession().master.chain[0].stateBase64, modelBefore);
            other->throwOnGetState = false;
            expect (f.document.save (f.directory).failed()); // directory cannot be replaced by a session file
            expect (! f.document.isDirty());
            expect (f.document.getSession().master.chain[0].stateBase64 != modelBefore); // save's mutable model is not the baseline
            expect (f.document.checkPluginStates());
            expect (f.document.isDirty());
        }

        beginTest ("changing bytes are checked once; silent changes do not cause retries and notified changes retain the three-write limit");
        {
            SilentEditFixture f;
            f.engine.getMasterChain().clear();
            auto plugin = std::make_unique<ChangingStatePlugin>();
            auto& p = *plugin;
            f.engine.getMasterChain().addPlugin (std::move (plugin));
            expect (f.document.save (f.file).wasOk());
            p.changeOnRead = true;
            int reads = p.stateReads;
            expect (f.document.checkPluginState (p));
            expectEquals (p.stateReads, reads + 1);
            reads = p.stateReads;
            expect (f.document.save (f.file).wasOk());
            expectEquals (p.stateReads, reads + 1);
            expect (f.document.checkPluginState (p));
            p.notifyOnRead = true;
            reads = p.stateReads;
            expect (f.document.save (f.file).failed());
            expectEquals (p.stateReads, reads + 3);
            expect (f.document.isDirty());
            p.notifyOnRead = p.changeOnRead = false;
            expect (! f.document.checkPluginStates()); // baseline is the third successful write, despite the reported failure
        }

        beginTest ("one-slot check is keyed by slot identity and never serialises another plugin");
        {
            SilentEditFixture f;
            auto second = std::make_unique<TestGainPlugin> (1.0f);
            auto* other = second.get();
            f.engine.getMasterChain().addPlugin (std::move (second));
            expect (f.document.save (f.file).wasOk());
            other->gain = 0.1f;
            other->throwOnGetState = true;
            const auto reads = other->stateReads;
            expect (! f.document.checkPluginState (*f.plugin));
            expectEquals (other->stateReads, reads);
            expect (! f.document.isDirty());
            f.plugin->gain = 0.3f;
            expect (f.document.checkPluginState (*f.plugin));
            expectEquals (other->stateReads, reads);
            expect (f.document.save (f.file).failed());
            other->throwOnGetState = false;
            expect (f.document.save (f.file).wasOk());
            f.plugin->throwOnGetState = true;
            expect (! f.document.checkPluginState (*f.plugin));
            expect (! f.document.isDirty());
            f.plugin->throwOnGetState = false;
            f.engine.getMasterChain().getSlot (0).state.slotId = juce::Uuid();
            expect (f.document.checkPluginState (*f.plugin));
        }

        beginTest ("the same preset slot id in different chains has an independent saved baseline");
        {
            SilentEditFixture f;
            const auto state = f.engine.getMasterChain().getStates()[0];
            auto* chain = f.engine.getChannelChain (f.document.getSession().channels[0].id);
            chain->addPlugin (std::make_unique<TestGainPlugin> (1.0f), state);
            auto& other = static_cast<TestGainPlugin&> (*chain->getSlot (0).plugin);
            other.gain = 0.2f;
            expect (f.document.save (f.file).wasOk());
            expect (! f.document.checkPluginStates());
            expect (! f.document.checkPluginState (other));
            other.gain = 0.6f;
            expect (f.document.checkPluginState (other));
        }

        beginTest ("silent structure count and plugin identity changes are compared, independently of bytes");
        {
            SilentEditFixture f;
            expect (f.document.save (f.file).wasOk());
            auto state = f.engine.getMasterChain().getStates()[0];
            f.engine.getMasterChain().clear();
            expect (f.document.checkPluginStates());
            f.engine.getMasterChain().addMissingSlot (state);
            expect (f.document.save (f.file).wasOk());
            f.engine.getMasterChain().getSlot (0).state.uniqueId += 1;
            expect (f.document.checkPluginStates());
        }

        beginTest ("saveIfDirty captures silent edits and writes the temporary file; discard survives shutdown and late notifications");
        {
            SilentEditFixture f;
            LiveMixSettings settings (f.directory);
            MainComponent main (f.document, settings);
            expect (f.document.save (f.file).wasOk());
            const auto original = f.file.loadFileAsString();
            f.plugin->gain = 0.5f;
            expect (! f.document.isDirty());
            const int beforeSave = f.plugin->stateReads;
            expect (main.saveIfDirty());
            expectEquals (f.plugin->stateReads, beforeSave + 1); // the complete boundary capture is reused by the immediate save
            const auto saved = f.file.loadFileAsString();
            expect (saved != original);
            expect (! f.document.isDirty());
            f.plugin->gain = 0.2f;
            expect (f.document.checkPluginStates());
            f.document.discardUnsavedChanges();
            f.plugin->updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
            f.document.pollPluginEdits();
            const int reads = f.plugin->stateReads;
            expect (main.saveIfDirty());
            expect (! f.document.isDirty());
            expectEquals (f.plugin->stateReads, reads);
            expectEquals (f.file.loadFileAsString(), saved);
        }

        beginTest ("a discard whose session open then fails leaves the session tracked again");
        {
            SilentEditFixture f;
            expect (f.document.save (f.file).wasOk());
            f.plugin->gain = 0.3f;
            f.document.discardUnsavedChanges();
            expect (! f.document.checkPluginStates());   // discarded: the transition it was for must not bring it back
            expect (f.document.load (f.directory.getChildFile ("missing.livemix")).failed());
            expect (f.document.checkPluginStates());     // still this session: its live plugin state is compared again
            expect (f.document.isDirty());
            f.document.discardUnsavedChanges();
            expect (f.document.load (f.directory.getChildFile ("missing.livemix")).failed());
            f.plugin->updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
            expect (f.document.pollPluginEdits());
            expect (f.document.isDirty());               // a notified edit after the failed open counts too
        }

        beginTest ("withSessionSecured captures before deciding, preserving the silent save and unsaved question");
        {
            SilentEditFixture f;
            LiveMixSettings settings (f.directory);
            MainComponent main (f.document, settings);
            expect (f.document.save (f.file).wasOk());
            const auto original = f.file.loadFileAsString();
            f.plugin->gain = 0.6f;
            bool proceeded = false;
            const int beforeLeave = f.plugin->stateReads;
            main.withSessionSecured ([&] { proceeded = true; });
            expectEquals (f.plugin->stateReads, beforeLeave + 1);
            expect (proceeded && ! f.document.isDirty());
            expect (f.file.loadFileAsString() != original);
            f.document.newSession();
            f.engine.getMasterChain().addPlugin (std::make_unique<TestGainPlugin> (0.4f));
            proceeded = false;
            main.withSessionSecured ([&] { proceeded = true; });
            expect (! proceeded);
            auto* alert = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
            expect (alert != nullptr);
            if (alert != nullptr)
            {
                expectEquals (alert->getName(), juce::String::fromUTF8 ("저장하지 않은 세션"));
                alert->exitModalState (0);
            }
        }

        beginTest ("optional window-close callback runs before the generic editor disappears; LiveMix wires the single-slot check");
        {
            TestGainPlugin plugin (1.0f);
            PluginWindowManager windows;
            expect (! windows.onWindowClosed); // Enqueue leaves the hook unset
            int closes = 0;
            windows.onWindowClosed = [&] (juce::AudioPluginInstance& p)
            {
                expect (&p == &plugin);
                expectEquals (windows.getNumOpenWindows(), 1);
                expect (editorFor (plugin) != nullptr);
                ++closes;
            };
            windows.open (plugin, "Test close");
            auto* window = editorFor (plugin);
            expect (window != nullptr);
            if (window != nullptr) window->closeButtonPressed();
            expectEquals (closes, 1);
            expectEquals (windows.getNumOpenWindows(), 0);
        }
        {
            SilentEditFixture f;
            LiveMixSettings settings (f.directory);
            MainComponent main (f.document, settings);
            expect (f.document.save (f.file).wasOk());
            for (auto* child : main.getChildren())
                if (auto* master = dynamic_cast<MasterCard*> (child)) master->onOpenPluginEditor (0);
            auto* window = editorFor (*f.plugin);
            expect (window != nullptr);
            f.plugin->gain = 0.7f;
            expect (! f.document.isDirty());
            if (window != nullptr) window->closeButtonPressed();
            expect (f.document.isDirty());
        }
    }
};
static LiveMixSilentPluginEditsTests liveMixSilentPluginEditsTests;
}
