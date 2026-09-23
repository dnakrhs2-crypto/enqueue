#include "ControlState.h"
#include "GlobalHotkeys.h"
#include "MixDocument.h"
#include "PluginPreset.h"
#include "TestGainPlugin.h"
#include "../livemix/src/ui/MainComponent.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <thread>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace gocue::tests
{
namespace
{
using namespace gocue::livemix;

// The AuditL1 access technique, with TU-local names: real UI operations,
// read-only notice observations, and the chain lock for a contention check.
struct AuditFix0923ApplyPreset
{
    using Type = void (ChainDrawer::*) (const PluginPreset&, bool);
    friend Type auditMember (AuditFix0923ApplyPreset);
};
struct AuditFix0923RemoveSlot
{
    using Type = void (ChainDrawer::*) (int);
    friend Type auditMember (AuditFix0923RemoveSlot);
};
struct AuditFix0923OpenChain
{
    using Type = void (MainComponent::*) (PluginChain*, const juce::String&);
    friend Type auditMember (AuditFix0923OpenChain);
};
struct AuditFix0923Status
{
    using Type = juce::Label MainComponent::*;
    friend Type auditMember (AuditFix0923Status);
};
struct AuditFix0923Notice
{
    using Type = juce::TextEditor MainComponent::*;
    friend Type auditMember (AuditFix0923Notice);
};
struct AuditFix0923RegisterHotkeys
{
    using Type = void (MainComponent::*) ();
    friend Type auditMember (AuditFix0923RegisterHotkeys);
};
struct AuditFix0923ChainLock
{
    using Type = juce::CriticalSection PluginChain::*;
    friend Type auditMember (AuditFix0923ChainLock);
};
template <typename Tag, typename Tag::Type value>
struct AuditFix0923Access
{
    friend typename Tag::Type auditMember (Tag) { return value; }
};
template struct AuditFix0923Access<AuditFix0923ApplyPreset, &ChainDrawer::applyPreset>;
template struct AuditFix0923Access<AuditFix0923RemoveSlot, &ChainDrawer::removeSlot>;
template struct AuditFix0923Access<AuditFix0923OpenChain, &MainComponent::openChainFor>;
template struct AuditFix0923Access<AuditFix0923Status, &MainComponent::statusLeft>;
template struct AuditFix0923Access<AuditFix0923Notice, &MainComponent::noticeText>;
template struct AuditFix0923Access<AuditFix0923RegisterHotkeys, &MainComponent::registerHotkeys>;
template struct AuditFix0923Access<AuditFix0923ChainLock, &PluginChain::lock>;

// Same TestGainPlugin/PluginHost format as AuditL1Tests, confined to this TU.
class AuditFix0923GainFormat final : public juce::AudioPluginFormat
{
public:
    juce::String getName() const override { return "Test"; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String&) override {}
    bool fileMightContainThisPluginType (const juce::String& id) override { return id == "test://gain"; }
    juce::String getNameOfPluginFromIdentifier (const juce::String&) override { return "TestGain"; }
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }
private:
    void createPluginInstance (const juce::PluginDescription&, double, int, PluginCreationCallback callback) override
    {
        callback (std::make_unique<TestGainPlugin> (1.0f), {});
    }
};

struct AuditFix0923Directory
{
    const juce::File parent = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File path = parent.getChildFile ("livemix-auditfix0923-" + juce::Uuid().toString());
    AuditFix0923Directory() { path.createDirectory(); }
    ~AuditFix0923Directory()
    {
        // Delete only this test's generated child, never a settings/session root.
        if (path.isAChildOf (parent) && path.getFileName().startsWith ("livemix-auditfix0923-"))
            path.deleteRecursively();
    }
};

// TestMain has no outer event loop. Like ControlServerTests, dispatch real OS
// messages on the JUCE message thread so async removal, clicks and timers run.
template <typename Predicate>
bool auditUntil (Predicate done, int timeoutMs = 2000)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    do
    {
        if (done()) return true;
       #if JUCE_WINDOWS
        MSG message;
        for (int i = 0; i < 32 && juce::Time::getMillisecondCounterHiRes() < deadline
             && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++i)
        {
            if (message.message == WM_QUIT) return false;
            TranslateMessage (&message);
            DispatchMessageW (&message);
        }
       #endif
        juce::Thread::sleep (2);
    } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    return done();
}

template <typename T, typename Predicate>
T* auditChild (juce::Component& component, Predicate matches)
{
    if (auto* found = dynamic_cast<T*> (&component); found != nullptr && matches (*found)) return found;
    for (auto* child : component.getChildren())
        if (auto* found = auditChild<T> (*child, matches)) return found;
    return nullptr;
}

juce::String auditStatus (MainComponent& main) { return (main.*auditMember (AuditFix0923Status {})).getText(); }
juce::String auditNotice (MainComponent& main) { return (main.*auditMember (AuditFix0923Notice {})).getText(); }

struct AuditFix0923Fixture
{
    AuditFix0923Directory directory;
    std::unique_ptr<LiveMixSettings> settings = std::make_unique<LiveMixSettings> (directory.path.getChildFile ("settings"));
    MixEngine engine;
    MixDocument document { engine };
    std::unique_ptr<MainComponent> main;

    AuditFix0923Fixture()
    {
        engine.prepare (48000.0, 256);
        document.applyToEngine();
        engine.getPluginHost().getFormatManager().addFormat (std::make_unique<AuditFix0923GainFormat>());
        main = std::make_unique<MainComponent> (document, *settings);
        main->setSize (900, 800);
    }
    juce::Uuid channel() const { return document.getSession().channels[0].id; }
    PluginChain& chain() { return *engine.getChannelChain (channel()); }
    juce::AudioBuffer<float> render (float amplitude, int blocks = 1)
    {
        juce::AudioBuffer<float> input (2, 256), output (2, 256);
        input.clear();
        juce::FloatVectorOperations::fill (input.getWritePointer (0), amplitude, 256);
        for (int b = 0; b < blocks; ++b)
            engine.renderBlock (input.getArrayOfReadPointers(), 2, output.getArrayOfWritePointers(), 2, 256);
        return output;
    }
};

#if JUCE_WINDOWS
// A real registration conflict on an invisible message-only HWND. No keyboard
// events are generated and no desktop window or OS focus is needed.
class AuditFix0923HotkeyOwner
{
public:
    AuditFix0923HotkeyOwner()
    {
        instance = (HINSTANCE) juce::Process::getCurrentModuleInstanceHandle();
        className = ("AuditFix0923Hotkey_" + juce::Uuid().toString()).toWideCharPointer();
        WNDCLASSW wc {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = className.c_str();
        if (RegisterClassW (&wc) == 0) return;
        window = CreateWindowExW (0, className.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        if (window == nullptr) return;
        for (int number = 24; number >= 13; --number)
        {
            const auto candidate = "F" + juce::String (number);
            unsigned int modifiers = 0, vk = 0;
            if (GlobalHotkeys::toWindowsHotkey (juce::KeyPress::createFromDescription (candidate), modifiers, vk)
                && RegisterHotKey (window, id, modifiers | MOD_NOREPEAT, vk))
            {
                key = candidate;
                registered = true;
                break;
            }
        }
    }
    ~AuditFix0923HotkeyOwner()
    {
        release();
        if (window != nullptr) DestroyWindow (window);
        UnregisterClassW (className.c_str(), instance);
    }
    void release()
    {
        if (registered) UnregisterHotKey (window, id);
        registered = false;
    }
    juce::String key;
private:
    static constexpr int id = 9230;
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    bool registered = false;
    std::wstring className;
};
#endif
} // namespace

class AuditFix0923LiveMixTests final : public juce::UnitTest
{
public:
    AuditFix0923LiveMixTests() : UnitTest ("AuditFix0923 LiveMix", "LiveMix") {}
    void runTest() override
    {
        appendDeletedOffPlugin();
        for (const bool muteGroup : { false, true })
            for (const bool skip : { true, false })
                resumeWithBypassedGain (skip, muteGroup);
        for (const int latency : { 128, 640 })
        {
            for (const bool changeGroup : { false, true })
                resumeWithDryHistory (latency, changeGroup);
            for (const bool enableWhileOff : { false, true })
                resumeWithWetOutput (latency, enableWhileOff);
        }
        resumeWithBusyChain();
        startupHotkeyConflict();
    }
private:
    bool prerequisite (bool ok, const juce::String& reason)
    {
        if (! ok) logMessage ("REPRODUCTION UNAVAILABLE: " + reason);
        expect (ok, "Scenario prerequisite: " + reason);
        return ok;
    }

    void appendDeletedOffPlugin()
    {
        beginTest ("audit0923 LA-1: append deleted preset member while group is OFF");
        AuditFix0923Fixture f;
        auto& chain = f.chain();
        chain.addPlugin (std::make_unique<TestGainPlugin> (2.0f)); // A only
        const auto aId = chain.getSlot (0).state.slotId;
        PluginPreset saved, preset;
        saved.name = "P";
        bool complete = true;
        saved.plugins = chain.getStates (&complete);
        for (auto& slot : saved.plugins) slot.bypassed = false; // saveChainAsPreset policy, AuditL1 fixture
        const auto file = f.directory.path.getChildFile ("P.livemixpreset");
        if (! prerequisite (complete && saved.save (file).wasOk() && PluginPreset::load (file, preset).wasOk(),
                            "save/reload A-only preset in the isolated directory")) return;
        chain.addPlugin (std::make_unique<TestGainPlugin> (1.0f)); // B remains after deleting A
        const auto bId = chain.getSlot (1).state.slotId;
        expectEquals (f.document.addPluginGroup (f.channel()), 0);
        f.document.setPluginGroupMember (f.channel(), 0, aId, true);
        f.document.setPluginGroupOff (f.channel(), 0, true);
        expectWithinAbsoluteError (f.render (0.25f, 8).getSample (0, 255), 0.25f, 1.0e-6f);

        (f.main.get()->*auditMember (AuditFix0923OpenChain {})) (&chain, "AuditFix0923");
        auto* drawer = auditChild<ChainDrawer> (*f.main, [] (const ChainDrawer&) { return true; });
        if (! prerequisite (drawer != nullptr, "production chain drawer exists")) return;
        (drawer->*auditMember (AuditFix0923RemoveSlot {})) (0);
        if (! prerequisite (auditUntil ([&] { return chain.getNumSlots() == 1; }), "drawer actually deleted A")) return;
        expect (chain.getSlot (0).state.slotId == bId);
        const auto generation = f.document.getSessionGeneration();
        (drawer->*auditMember (AuditFix0923ApplyPreset {})) (preset, false); // actual 'append' branch
        if (! prerequisite (chain.getNumSlots() == 2 && chain.getSlot (1).plugin != nullptr
                            && ! chain.getSlot (1).faulted.load(), "A appended through PluginHost without fault")) return;
        expect (chain.getSlot (0).state.slotId == bId);
        expect (chain.getSlot (1).state.slotId == aId);
        expect (f.document.getSessionGeneration() == generation, "No intervening session reopen");
        const auto& group = f.document.getSession().channels[0].pluginGroups[0];
        expect (group.off && std::find (group.slots.begin(), group.slots.end(), aId) != group.slots.end());
        const auto projection = ControlState::capture (f.document, f.main->getMuteGroups(), false);
        expect (projection.projection.channels[0].pluginGroups[0].off);
        f.render (0.25f, 8); // past all ramps; inspect another 32 complete blocks
        float low = 100.0f, high = -100.0f, error = 0.0f;
        for (int b = 0; b < 32; ++b)
        {
            const auto block = f.render (0.25f);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                {
                    const auto sample = block.getSample (ch, i);
                    low = juce::jmin (low, sample);
                    high = juce::jmax (high, sample);
                    error = juce::jmax (error, std::abs (sample - 0.25f));
                }
        }
        logMessage ("LA-1 input=0.25, A=x2, B=x1; group/control OFF=1; restored ID=" + aId.toString()
                    + "; bypassed=" + juce::String ((int) chain.getSlot (1).bypassed.load())
                    + "; settled 32-block min/max=" + juce::String (low, 6) + "/" + juce::String (high, 6));
        expect (chain.getSlot (1).bypassed.load() && chain.getSlot (1).state.bypassed,
                "An appended member of an OFF group must remain bypassed");
        expectLessThan (error, 1.0e-6f, "OFF group must keep every settled sample at 0.25");

        // Appending the same preset again creates a new ID, outside the OFF group.
        // Only the returning member above should inherit the group's bypass.
        (drawer->*auditMember (AuditFix0923ApplyPreset {})) (preset, false);
        if (! prerequisite (chain.getNumSlots() == 3, "The duplicate preset was appended")) return;
        const auto& duplicate = chain.getSlot (2);
        expect (duplicate.state.slotId != aId && duplicate.state.slotId != bId);
        expect (! duplicate.bypassed.load() && ! duplicate.state.bypassed,
                "A duplicate with a fresh ID keeps the preset bypass instead of joining the OFF group");
        expect (chain.getSlot (1).bypassed.load());
        expect (! chain.getSlot (0).bypassed.load(), "The pre-existing non-member keeps its bypass state");
        expectWithinAbsoluteError (f.render (0.25f, 8).getSample (0, 255), 0.5f, 1.0e-6f);
        expect (! preset.plugins[0].bypassed, "Applying the preset must not change the source preset");
    }

    void resumeWithBypassedGain (bool skip, bool muteGroup)
    {
        const juce::String condition = juce::String (muteGroup ? "mute-group" : "mic-OFF")
                                       + ", skipChainWhenOff=" + (skip ? "true" : "false");
        beginTest ("audit0923 LA-2: resume with OFF gain (" + condition + ")");
        AuditFix0923Fixture f;
        expect (f.settings->getSkipPluginsWhenOff() && f.engine.getSkipChainWhenOff(), "Default skips OFF chains");
        f.engine.setSkipChainWhenOff (skip);
        f.document.setChannelPan (f.channel(), 0.0);
        for (const auto& fx : f.document.getSession().fx) f.document.setSend (f.channel(), fx.id, 0.0, false);
        expectEquals (f.engine.getMasterChain().getNumSlots(), 0);
        f.document.setChannelMuteGroup (f.channel(), true);
        const auto setOff = [&] (bool off)
        {
            if (muteGroup) f.main->getMuteGroups().set (MuteGroups::Group::mic, off);
            else f.document.setChannelOn (f.channel(), ! off);
        };
        f.render (0.4f, 3);
        setOff (true);
        expectWithinAbsoluteError (f.render (0.4f, 8).getMagnitude (0, 256), 0.0f, 1.0e-7f);
        auto gain = std::make_unique<TestGainPlugin> (1.0f);
        auto* processor = gain.get();
        f.chain().addPlugin (std::move (gain)); // added only after complete silence
        processor->gain = 10.0f; // +20 dB; no latency, no tail
        f.document.markDirty();
        expectEquals (f.chain().getLatencySamples(), 0);
        expectEquals (f.document.addPluginGroup (f.channel()), 0);
        f.document.setPluginGroupMember (f.channel(), 0, f.chain().getSlot (0).state.slotId, true);
        f.document.setPluginGroupOff (f.channel(), 0, true);
        expectWithinAbsoluteError (f.render (0.4f, 400).getMagnitude (0, 256), 0.0f, 1.0e-7f); // 2.13 s of OFF audio
        const auto wetBefore = f.chain().getSlot (0).wetMix; // offline: no concurrent audio thread
        const auto callsBefore = processor->processCount;
        expectEquals (callsBefore, skip ? 0 : 400);
        expect (f.chain().getSlot (0).bypassed.load());
        expect (f.document.getSession().channels[0].pluginGroups[0].off);
        setOff (false);
        const auto first = f.render (0.4f);
        float peak = 0.0f;
        int peakIndex = -1;
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
            {
                const auto value = first.getSample (ch, i);
                finite = finite && std::isfinite (value);
                if (std::abs (value) > peak) { peak = std::abs (value); peakIndex = i; }
            }
        const auto settled = f.render (0.4f, 8).getSample (0, 255);
        logMessage ("LA-2 " + condition + "; off processing calls=" + juce::String (callsBefore)
                    + "; wet before resume=" + juce::String (wetBefore, 6)
                    + "; first-block peak=" + juce::String (peak, 6) + " at sample " + juce::String (peakIndex)
                    + "; first-block last=" + juce::String (first.getSample (0, 255), 6)
                    + "; settled=" + juce::String (settled, 6));
        expect (finite);
        expectWithinAbsoluteError (settled, 0.4f, 1.0e-6f);
        expect (peak <= 0.40001f, "An OFF +20 dB effect must not amplify ANY sample of the resumed block; peak=" + juce::String (peak, 6));
    }

    void resumeWithDryHistory (int latency, bool changeGroup)
    {
        const auto condition = "latency=" + juce::String (latency)
                               + ", group change=" + (changeGroup ? "true" : "false");
        beginTest ("audit0923 LA-2: resumed delayed dry output (" + condition + ")");
        AuditFix0923Fixture f;
        if (changeGroup)
            f.chain().addPlugin (std::make_unique<TestGainPlugin> (10.0f));
        auto delayed = std::make_unique<TestGainPlugin> (1.0f);
        delayed->latencySamples = latency;
        auto* processor = delayed.get();
        PluginSlotState bypassed;
        bypassed.bypassed = true;
        f.chain().addPlugin (std::move (delayed), bypassed);
        expectWithinAbsoluteError (f.render (0.4f, 8).getSample (0, 255), changeGroup ? 4.0f : 0.4f, 1.0e-6f);
        if (changeGroup)
        {
            expectEquals (f.document.addPluginGroup (f.channel()), 0);
            f.document.setPluginGroupMember (f.channel(), 0, f.chain().getSlot (0).state.slotId, true);
        }
        f.document.setChannelOn (f.channel(), false);
        expectWithinAbsoluteError (f.render (0.4f, 8).getMagnitude (0, 256), 0.0f, 1.0e-7f);
        const auto callsBefore = processor->processCount;
        f.render (0.4f, 2);
        expectEquals (processor->processCount, callsBefore, "The silent mic actually skipped its chain");

        // Change bypass after the final skipped block, then resume immediately.
        // The downstream bypassed slot's dry ring still contains the old x10 input.
        if (changeGroup)
            f.document.setPluginGroupOff (f.channel(), 0, true);
        f.document.setChannelOn (f.channel(), true);
        float peak = 0.0f;
        float maxStep = 0.0f;
        float previous[2] { 0.0f, 0.0f }; // include the OFF/ON and every block boundary
        bool finite = true;
        for (int block = 0; block < 4; ++block)
        {
            const auto output = f.render (0.4f);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                {
                    const float value = output.getSample (ch, i);
                    finite = finite && std::isfinite (value);
                    peak = juce::jmax (peak, std::abs (value));
                    maxStep = juce::jmax (maxStep, std::abs (value - previous[ch]));
                    previous[ch] = value;
                    if (block * 256 + i < latency)
                        expectWithinAbsoluteError (value, 0.0f, 1.0e-7f, "No pre-mute audio may leave a stale dry ring");
                }
        }
        logMessage ("LA-2 " + condition + "; resumed four-block peak=" + juce::String (peak, 6)
                    + "; max adjacent step=" + juce::String (maxStep, 6));
        // At 48 kHz the 5 ms bypass ramp is 240 samples: 0.4 / 240 = 0.001667 per sample.
        // Even overlapping the mic's 256-sample ON ramp adds at most 0.4 / 256 = 0.001563.
        // 0.05 leaves ample rounding/ramp headroom but rejects the old 0.2 / 0.4 onset jumps.
        expectLessThan (maxStep, 0.05f, "The first fresh delayed samples must fade in without a one-sample jump");
        expect (finite && peak <= 0.40001f, "A downstream bypassed slot must not replay the old amplified signal");
        expectEquals (processor->processCount, callsBefore + 4);
        expectWithinAbsoluteError (f.render (0.4f).getSample (0, 255), 0.4f, 1.0e-6f);
    }

    void resumeWithWetOutput (int latency, bool enableWhileOff)
    {
        beginTest ("audit0923 LA-2: resumed wet output is unchanged (latency=" + juce::String (latency)
                   + ", enabled while OFF=" + (enableWhileOff ? "true" : "false") + ")");
        AuditFix0923Fixture f, reference;
        auto delayed = std::make_unique<TestGainPlugin> (2.0f);
        delayed->latencySamples = latency;
        auto* processor = delayed.get();
        PluginSlotState state;
        state.bypassed = enableWhileOff;
        f.chain().addPlugin (std::move (delayed), state);
        expectWithinAbsoluteError (f.render (0.4f, 8).getSample (0, 255), enableWhileOff ? 0.4f : 0.8f, 1.0e-6f);
        reference.render (0.8f, 8);
        f.document.setChannelOn (f.channel(), false);
        reference.document.setChannelOn (reference.channel(), false);
        f.render (0.4f, 8);
        reference.render (0.8f, 8);
        const auto callsBefore = processor->processCount;
        const auto resetsBefore = processor->resetCount;
        if (enableWhileOff)
            f.chain().setBypassed (0, false);
        f.document.setChannelOn (f.channel(), true);
        reference.document.setChannelOn (reference.channel(), true);
        for (int block = 0; block < 4; ++block)
        {
            const auto output = f.render (0.4f);
            const auto expected = reference.render (0.8f);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    expectWithinAbsoluteError (output.getSample (ch, i), expected.getSample (ch, i), 1.0e-6f,
                                               "An enabled effect keeps its wet history and only the existing mic ON ramp");
        }
        expectEquals (processor->processCount, callsBefore + 4);
        expectEquals (processor->resetCount, resetsBefore, "Resume must not reset plugin state");
    }

    void resumeWithBusyChain()
    {
        beginTest ("audit0923 LA-2: resume synchronization survives a busy chain try-lock");
        AuditFix0923Fixture f;
        f.document.setChannelOn (f.channel(), false);
        f.render (0.4f, 8);
        auto gain = std::make_unique<TestGainPlugin> (10.0f);
        auto* processor = gain.get();
        f.chain().addPlugin (std::move (gain));
        expectEquals (f.document.addPluginGroup (f.channel()), 0);
        f.document.setPluginGroupMember (f.channel(), 0, f.chain().getSlot (0).state.slotId, true);
        f.document.setPluginGroupOff (f.channel(), 0, true);
        f.render (0.4f, 8);
        f.document.setChannelOn (f.channel(), true);
        juce::WaitableEvent locked, release;
        std::thread editor ([&]
        {
            const juce::ScopedLock held (f.chain().*auditMember (AuditFix0923ChainLock {}));
            locked.signal();
            release.wait (2000);
        });
        const bool acquired = locked.wait (2000);
        expect (acquired, "The competing thread holds the chain lock");
        if (acquired)
        {
            expect (f.render (0.4f).getMagnitude (0, 256) <= 0.40001f);
            expectEquals (processor->processCount, 0, "A busy chain passes dry without waiting for the editor");
        }
        release.signal();
        editor.join();
        if (! acquired) return;
        const auto resumed = f.render (0.4f);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                expectWithinAbsoluteError (resumed.getSample (ch, i), 0.4f, 1.0e-6f,
                                           "The first processed block still honors the OFF group after lock contention");
        expectEquals (processor->processCount, 1);
    }

    void startupHotkeyConflict()
    {
        beginTest ("audit0923 LB-1: auto-open preserves actual hotkey registration failure");
       #if JUCE_WINDOWS
        AuditFix0923Directory directory;
        AuditFix0923HotkeyOwner owner;
        if (! prerequisite (owner.key.isNotEmpty(), "RegisterHotKey preemption on a hidden message-only window")) return;
        const auto settingsFolder = directory.path.getChildFile ("settings");
        const auto sessionFile = directory.path.getChildFile ("last.livemix");
        MixSession session;
        session.addChannel();
        session.channels[0].muteGroup = true;
        if (! prerequisite (session.save (sessionFile).wasOk(), "isolated saved last session")) return;
        {
            LiveMixSettings saved (settingsFolder);
            saved.setMicMuteHotkey (owner.key);
            saved.setLastSessionFile (sessionFile);
            saved.saveIfNeeded();
        }
        LiveMixSettings settings (settingsFolder); // a new launch reads persisted preferences
        expectEquals (settings.getMicMuteHotkey(), owner.key);
        MixEngine engine;
        engine.prepare (48000.0, 256);
        MixDocument document (engine);
        MainComponent main (document, settings); // real constructor/RegisterHotKey failure
        main.setSize (900, 800);
        const auto before = auditStatus (main) + "\n" + auditNotice (main);
        if (! prerequisite (before.contains (juce::String::fromUTF8 ("등록 실패")), "real registration failed before auto-open")) return;
        const auto last = settings.getLastSessionFile();
        if (! prerequisite (last.existsAsFile(), "last-session file exists")) return;
        main.openSession (last); // same public call as Main.cpp initialisation; no safe-mode bypass
        if (! prerequisite (document.getFile() == last && document.hasAppliedGraph(), "normal last session loaded")) return;
        const auto after = auditNotice (main);
        logMessage ("LB-1 key=" + owner.key + "; before='" + before + "'; notice after auto-open='" + after
                    + "'; status='" + auditStatus (main) + "'");
        expect (after.contains (juce::String::fromUTF8 ("등록 실패")), "Unresolved startup hotkey failure must survive successful auto-open in the notice bar");
        expect (after.contains (owner.key) && after.contains (juce::String::fromUTF8 ("마이크 뮤트그룹")),
                "The notice must identify the failed key and action");
        expect ((main.*auditMember (AuditFix0923Notice {})).isVisible(), "The persistent notice is enabled in the hidden component tree");

        main.newSession();
        expect (! document.hasFile(), "A clean new session was created without a dialog");
        expectEquals (auditNotice (main), after, "New session must preserve the unresolved registration failure");
        main.hideNotice();
        expect (auditNotice (main).isEmpty());
        expect (! (main.*auditMember (AuditFix0923Notice {})).isVisible());
        main.openSession (last);
        expect (auditNotice (main).isEmpty(), "A dismissed failure stays dismissed across session opens");

        const auto retry = [&] { (main.*auditMember (AuditFix0923RegisterHotkeys {})) (); };
        retry();
        expectEquals (auditNotice (main), after, "An unsuccessful retry reports the still-unavailable key");
        AuditFix0923HotkeyOwner fxOwner;
        if (! prerequisite (fxOwner.key.isNotEmpty(), "A second hidden hotkey registration for the FX conflict")) return;
        settings.setFxMuteHotkey (fxOwner.key);
        retry();
        const auto both = auditNotice (main);
        expect (both.contains (owner.key) && both.contains (fxOwner.key)
                && both.contains (juce::String::fromUTF8 ("마이크 뮤트그룹"))
                && both.contains (juce::String::fromUTF8 ("FX 뮤트그룹")), "Both failed actions and keys are reported");
        expect (! both.containsChar ('\n'), "Multiple registration failures occupy one notice line");
        expect (both.contains (juce::String::fromUTF8 ("설정")), "The failure explains how to choose another key");

        owner.release();
        retry();
        expect (! auditNotice (main).contains (owner.key) && auditNotice (main).contains (fxOwner.key),
                "A successful mic registration clears only its failure; the FX conflict remains");
        main.setSessionNote ("session warning", true);
        main.setStartupNote ("startup warning", true, false);
        settings.setFxMuteHotkey ({});
        retry();
        expectEquals (auditNotice (main), juce::String ("session warning\nstartup warning"),
                      "Removing a conflicted binding clears its failure without clearing other notices");
        settings.setFxMuteHotkey (fxOwner.key);
        retry();
        expect (auditNotice (main).contains (fxOwner.key));
        fxOwner.release();
        retry();
        expectEquals (auditNotice (main), juce::String ("session warning\nstartup warning"),
                      "Successful registration clears the hotkey failure while preserving other notices");
        main.setSessionNote ({}, false);
        main.setStartupNote ({}, false, false);
        expect (auditNotice (main).isEmpty());
       #else
        logMessage ("REPRODUCTION UNAVAILABLE: Windows RegisterHotKey required");
       #endif
    }

};

static AuditFix0923LiveMixTests auditFix0923LiveMixTests;
} // namespace gocue::tests
