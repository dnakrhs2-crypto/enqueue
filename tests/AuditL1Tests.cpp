#include "ControlDispatcher.h"
#include "MixDocument.h"
#include "MuteGroups.h"
#include "PluginPreset.h"
#include "TestGainPlugin.h"
#include "ui/ChainDrawer.h"

#include <cmath>
#include <thread>

namespace gocue::tests
{
namespace audit_l1
{
using namespace gocue::livemix;
using P = ControlProtocol;

// Explicit-instantiation access is confined to this test translation unit. Call
// the real drawer operation without changing production visibility or copying
// its append loop (so a fix to applyPreset would also fix this reproduction).
struct ApplyPreset
{
    using Type = void (ChainDrawer::*) (const PluginPreset&, bool);
    friend Type member (ApplyPreset);
};
template <typename Tag, typename Tag::Type method>
struct PrivateMethod
{
    friend typename Tag::Type member (Tag) { return method; }
};
template struct PrivateMethod<ApplyPreset, &ChainDrawer::applyPreset>;

// Let the real PluginHost recreate the existing TestGainPlugin fixture.
class GainFormat final : public juce::AudioPluginFormat
{
public:
    std::function<std::unique_ptr<juce::AudioPluginInstance>()> create;
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
        callback (create ? create() : std::make_unique<TestGainPlugin> (1.0f), {});
    }
};

struct Block
{
    float peak = 0.0f, last = 0.0f;
    int peakSample = -1;
    bool finite = true;
};

// Same real engine/document/dispatcher setup and refresh callbacks as
// ControlDispatcherTests::Fixture; the renderer examines complete offline blocks.
struct Fixture
{
    MixEngine engine;
    MixDocument document { engine };
    MuteGroups groups { document };
    juce::Uuid instance;
    ControlState state { ControlState::capture (document, groups, false) };
    ControlDispatcher dispatcher { document, groups, state, instance, [] { return false; } };
    PluginWindowManager windows;
    ChainDrawer drawer { document, windows };
    GainFormat* format = nullptr;
    juce::int64 nextId = 1;

    Fixture()
    {
        engine.prepare (48000.0, 256);
        document.applyToEngine();
        document.onValueChanged = [this] { groups.apply(); };
        document.onStructureChanged = [this] { groups.apply(); };
        windows.onChainChanged = [this] (PluginChain&) { document.markDirty(); };
        engine.forEachChain ([this] (PluginChain& chain) { chain.setListener (&windows); });
        drawer.onChainEdited = [this] { groups.apply(); };
        auto gainFormat = std::make_unique<GainFormat>();
        format = gainFormat.get();
        engine.getPluginHost().getFormatManager().addFormat (std::move (gainFormat));
    }
    ~Fixture()
    {
        engine.forEachChain ([] (PluginChain& chain) { chain.setListener (nullptr); });
    }
    juce::Uuid channel() const { return document.getSession().channels[0].id; }
    ControlDispatcher::Result dispatch (P::CommandArgs args)
    {
        return dispatcher.dispatch ({ juce::String (nextId++), instance,
                                      document.getSessionGeneration(), {}, std::move (args) });
    }
    void apply (PluginChain& chain, const PluginPreset& preset, bool replace)
    {
        drawer.setChain (&chain, "Audit");
        (drawer.*member (ApplyPreset {})) (preset, replace);
    }
    Block render (float amplitude, int blocks = 1)
    {
        juce::AudioBuffer<float> input (2, 256), output (2, 256);
        input.clear();
        juce::FloatVectorOperations::fill (input.getWritePointer (0), amplitude, 256);
        Block result;
        for (int b = 0; b < blocks; ++b)
        {
            engine.renderBlock (input.getArrayOfReadPointers(), 2, output.getArrayOfWritePointers(), 2, 256);
            for (int ch = 0; ch < output.getNumChannels(); ++ch)
                for (int i = 0; i < output.getNumSamples(); ++i)
                {
                    const auto value = output.getSample (ch, i);
                    result.finite = result.finite && std::isfinite (value);
                    if (std::abs (value) > result.peak)
                    {
                        result.peak = std::abs (value);
                        result.peakSample = b * 256 + i;
                    }
                }
            result.last = output.getSample (0, 255);
        }
        return result;
    }
};

struct PrepareGate
{
    juce::WaitableEvent entered, release;
    bool timedOut = false;
};
class SlowPrepareGain final : public TestGainPlugin
{
public:
    explicit SlowPrepareGain (PrepareGate& g) : TestGainPlugin (1.0f), gate (g) {}
    void prepareToPlay (double rate, int size) override
    {
        TestGainPlugin::prepareToPlay (rate, size);
        gate.entered.signal();
        // No timing guess: preparation cannot return until the audio worker has
        // captured the intermediate block. The timeout only guards a broken test.
        gate.timedOut = ! gate.release.wait (5000);
    }
private:
    PrepareGate& gate;
};
} // namespace audit_l1

class AuditL1Tests final : public juce::UnitTest
{
public:
    AuditL1Tests() : UnitTest ("Audit L1 live mix reproductions", "LiveMix") {}
    void runTest() override
    {
        testRestoreOffGroup();
        testAppend();
        testGroupCrossfade();
    }
private:
    void testRestoreOffGroup()
    {
        using namespace audit_l1;
        beginTest ("audit L1-1: preset reload then explicit OFF");
        Fixture f;
        auto* chain = f.engine.getChannelChain (f.channel());
        expect (chain != nullptr);
        if (chain == nullptr) return;
        chain->addPlugin (std::make_unique<TestGainPlugin> (0.5f));
        expectWithinAbsoluteError (f.render (0.5f, 3).last, 0.25f, 1.0e-6f);

        PluginPreset saved, preset;
        saved.name = "Audit same-slot preset";
        bool complete = true;
        saved.plugins = chain->getStates (&complete);
        expect (complete);
        for (auto& slot : saved.plugins) slot.bypassed = false; // saveChainAsPreset policy
        expect (PluginPreset::fromJson (saved.toJson(), preset).wasOk());
        const auto slotId = chain->getSlot (0).state.slotId;
        expectEquals (f.document.addPluginGroup (f.channel()), 0);
        f.document.setPluginGroupMember (f.channel(), 0, slotId, true);
        f.document.setPluginGroupOff (f.channel(), 0, true);
        expectWithinAbsoluteError (f.render (0.5f, 3).last, 0.5f, 1.0e-6f);

        f.apply (*chain, preset, true);
        expectEquals (chain->getNumSlots(), 1);
        expect (chain->getSlot (0).plugin != nullptr && ! chain->getSlot (0).faulted.load());
        expect (chain->getSlot (0).state.slotId == slotId);
        expect (f.document.getSession().channels[0].pluginGroups[0].off);
        const auto restored = f.render (0.5f, 3).last;

        // One microphone: no other ON group can force the everywhere setter.
        const auto everywhereReply = f.dispatch (P::SetPluginGroupOffEverywhere { 1, true });
        const auto* everywhere = std::get_if<P::Ack> (&everywhereReply);
        expect (everywhere != nullptr);
        if (everywhere == nullptr) return;
        const auto* group = std::get_if<P::PluginGroupEverywhereResult> (&everywhere->result);
        expect (group != nullptr && group->off && group->count == 1);
        const auto afterEverywhere = f.render (0.5f, 3).last;
        const auto individualReply = f.dispatch (P::SetPluginGroupOff { f.channel(), 1, true });
        const auto* individual = std::get_if<P::Ack> (&individualReply);
        expect (individual != nullptr);
        if (individual == nullptr) return;
        const auto* individualGroup = std::get_if<P::PluginGroupResult> (&individual->result);
        expect (individualGroup != nullptr && individualGroup->off);
        const auto afterIndividual = f.render (0.5f, 3).last;
        const auto projection = ControlState::capture (f.document, f.groups, false);
        expect (projection.projection.channels[0].pluginGroups[0].off);
        logMessage ("L1-1 input=0.5, gain=0.5, OFF before reload=0.5; restored=" + juce::String (restored, 6)
            + ", after everywhere OFF=" + juce::String (afterEverywhere, 6)
            + ", after individual OFF=" + juce::String (afterIndividual, 6)
            + ", slot bypassed=" + juce::String ((int) chain->getSlot (0).bypassed.load())
            + ", model/control OFF=1, ACK changed=" + juce::String ((int) everywhere->changed)
            + "/" + juce::String ((int) individual->changed));
        // Healthy behavior is asserted: the reported defect must FAIL this test.
        expectWithinAbsoluteError (restored, 0.5f, 1.0e-6f, "An OFF group must stay audibly bypassed after replacement");
        expectWithinAbsoluteError (afterEverywhere, 0.5f, 1.0e-6f, "Explicit everywhere OFF must bypass the actual effect");
        expectWithinAbsoluteError (afterIndividual, 0.5f, 1.0e-6f, "Explicit microphone OFF must bypass the actual effect");
    }

    void testAppend()
    {
        using namespace audit_l1;
        beginTest ("audit L1-2: append during slow preparation");
        for (const bool master : { false, true })
        {
            PrepareGate gate;
            Fixture f;
            auto* chain = master ? &f.engine.getMasterChain() : f.engine.getChannelChain (f.channel());
            expect (chain != nullptr);
            if (chain == nullptr) continue;
            chain->addPlugin (std::make_unique<TestGainPlugin> (1.0f)); // nonempty destination
            const auto before = f.render (0.1f, 3).last;
            expectWithinAbsoluteError (before, 0.1f, 1.0e-6f);

            PluginChain source;
            source.prepare (48000.0, 256);
            source.addPlugin (std::make_unique<TestGainPlugin> (4.0f));
            source.addPlugin (std::make_unique<TestGainPlugin> (0.25f));
            PluginPreset saved, preset;
            saved.name = "Audit unity pair";
            saved.plugins = source.getStates();
            expect (PluginPreset::fromJson (saved.toJson(), preset).wasOk());
            int created = 0;
            f.format->create = [&]() -> std::unique_ptr<juce::AudioPluginInstance>
            {
                if (++created == 2) return std::make_unique<SlowPrepareGain> (gate);
                return std::make_unique<TestGainPlugin> (1.0f);
            };

            Block during;
            bool observed = false;
            int publishedSlots = -1;
            std::thread audio ([&]
            {
                observed = gate.entered.wait (5000);
                if (observed)
                {
                    publishedSlots = chain->getNumSlots();
                    during = f.render (0.1f);
                }
                gate.release.signal();
            });
            f.apply (*chain, preset, false); // actual ChainDrawer createInstance/addPlugin loop
            audio.join();
            expect (observed && ! gate.timedOut, "Reproduction prerequisite: audio captured before preparation returned");
            expectEquals (created, 2);
            expectEquals (chain->getNumSlots(), 3);
            for (int i = 0; i < chain->getNumSlots(); ++i)
                expect (chain->getSlot (i).plugin != nullptr && ! chain->getSlot (i).faulted.load());
            const auto after = f.render (0.1f, 3).last;
            expectWithinAbsoluteError (after, 0.1f, 1.0e-6f);
            expect (during.finite);
            logMessage (juce::String ("L1-2 ") + (master ? "master" : "microphone")
                + ": before=" + juce::String (before, 6) + ", during peak=" + juce::String (during.peak, 6)
                + ", after=" + juce::String (after, 6) + ", published slots during prepare=" + juce::String (publishedSlots));
            if (observed && ! gate.timedOut)
                expectWithinAbsoluteError (during.peak, 0.1f, 1.0e-6f,
                    juce::String (master ? "Master" : "Microphone") + " must not output a partially appended unity preset");
        }
    }

    void testGroupCrossfade()
    {
        using namespace audit_l1;
        beginTest ("audit L1-3: full first-block bypass peak");
        Fixture f;
        auto* chain = f.engine.getChannelChain (f.channel());
        expect (chain != nullptr);
        if (chain == nullptr) return;
        chain->addPlugin (std::make_unique<TestGainPlugin> (0.1f));
        chain->addPlugin (std::make_unique<TestGainPlugin> (10.0f));
        expectEquals (f.document.addPluginGroup (f.channel()), 0);
        for (int i = 0; i < chain->getNumSlots(); ++i)
            f.document.setPluginGroupMember (f.channel(), 0, chain->getSlot (i).state.slotId, true);
        expectWithinAbsoluteError (f.render (0.4f, 3).last, 0.4f, 1.0e-6f);

        for (const bool off : { true, false })
        {
            // Both bits change before rendering even the first sample. No edit
            // races, block skipping or tail-only measurements are involved.
            f.document.setPluginGroupOff (f.channel(), 0, off);
            expect (chain->getSlot (0).bypassed.load() == off && chain->getSlot (1).bypassed.load() == off);
            const auto first = f.render (0.4f);
            const auto settled = f.render (0.4f, 3).last;
            expect (first.finite);
            expectWithinAbsoluteError (first.last, 0.4f, 1.0e-5f);
            expectWithinAbsoluteError (settled, 0.4f, 1.0e-6f);
            logMessage (juce::String ("L1-3 ") + (off ? "ON->OFF" : "OFF->ON")
                + ": first 256-sample block peak=" + juce::String (first.peak, 6)
                + " at sample " + juce::String (first.peakSample)
                + ", block last=" + juce::String (first.last, 6) + ", settled=" + juce::String (settled, 6));
            expect (first.peak <= 0.40001f, "Unity endpoints must not overshoot during a group transition; peak=" + juce::String (first.peak, 6));
        }
    }
};

static AuditL1Tests auditL1Tests;
} // namespace gocue::tests
