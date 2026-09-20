#include "ControlDispatcher.h"
#include "MixDocument.h"
#include "MuteGroups.h"
#include "PluginPreset.h"
#include "TestGainPlugin.h"
#include "ui/ChainDrawer.h"

namespace gocue::tests
{
namespace audit_l1
{
using namespace gocue::livemix;
using P = ControlProtocol;

// Explicit-instantiation access is confined to this test translation unit. Call
// the real drawer operation without changing production visibility or copying
// its preset replacement logic.
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

// Same real engine/document/dispatcher setup and refresh callbacks as
// ControlDispatcherTests::Fixture; the renderer checks the settled audible output.
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
        engine.getPluginHost().getFormatManager().addFormat (std::make_unique<GainFormat>());
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
    void apply (PluginChain& chain, const PluginPreset& preset)
    {
        drawer.setChain (&chain, "Audit");
        (drawer.*member (ApplyPreset {})) (preset, true);
    }
    float render (float amplitude, int blocks = 1)
    {
        juce::AudioBuffer<float> input (2, 256), output (2, 256);
        input.clear();
        juce::FloatVectorOperations::fill (input.getWritePointer (0), amplitude, 256);
        for (int b = 0; b < blocks; ++b)
            engine.renderBlock (input.getArrayOfReadPointers(), 2, output.getArrayOfWritePointers(), 2, 256);
        return output.getSample (0, 255);
    }
};
} // namespace audit_l1

class AuditL1Tests final : public juce::UnitTest
{
public:
    AuditL1Tests() : UnitTest ("Audit L1 live mix reproductions", "LiveMix") {}
    void runTest() override
    {
        testRestoreOffGroup();
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
        expectWithinAbsoluteError (f.render (0.5f, 3), 0.25f, 1.0e-6f);

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
        expectWithinAbsoluteError (f.render (0.5f, 3), 0.5f, 1.0e-6f);

        f.apply (*chain, preset);
        expectEquals (chain->getNumSlots(), 1);
        expect (chain->getSlot (0).plugin != nullptr && ! chain->getSlot (0).faulted.load());
        expect (chain->getSlot (0).state.slotId == slotId);
        expect (f.document.getSession().channels[0].pluginGroups[0].off);
        const auto restored = f.render (0.5f, 3);

        // One microphone: no other ON group can force the everywhere setter.
        const auto everywhereReply = f.dispatch (P::SetPluginGroupOffEverywhere { 1, true });
        const auto* everywhere = std::get_if<P::Ack> (&everywhereReply);
        expect (everywhere != nullptr);
        if (everywhere == nullptr) return;
        const auto* group = std::get_if<P::PluginGroupEverywhereResult> (&everywhere->result);
        expect (group != nullptr && group->off && group->count == 1);
        const auto afterEverywhere = f.render (0.5f, 3);
        const auto individualReply = f.dispatch (P::SetPluginGroupOff { f.channel(), 1, true });
        const auto* individual = std::get_if<P::Ack> (&individualReply);
        expect (individual != nullptr);
        if (individual == nullptr) return;
        const auto* individualGroup = std::get_if<P::PluginGroupResult> (&individual->result);
        expect (individualGroup != nullptr && individualGroup->off);
        const auto afterIndividual = f.render (0.5f, 3);
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
};

static AuditL1Tests auditL1Tests;
} // namespace gocue::tests
