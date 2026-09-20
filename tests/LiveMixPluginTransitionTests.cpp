#include "ControlDispatcher.h"
#include "MixDocument.h"
#include "MuteGroups.h"
#include "TestGainPlugin.h"

#include <cmath>
#include <thread>

namespace gocue::tests
{
namespace
{
using namespace gocue::livemix;
using P = ControlProtocol;

struct TransitionFixture
{
    MixEngine engine;
    MixDocument document { engine };
    MuteGroups groups { document };
    juce::Uuid instance;
    ControlState state { ControlState::capture (document, groups, false) };
    ControlDispatcher dispatcher { document, groups, state, instance, [] { return false; } };
    int valueCalls = 0;

    TransitionFixture()
    {
        engine.prepare (48000.0, 64);
        document.applyToEngine();
        document.onValueChanged = [this] { ++valueCalls; groups.apply(); };
    }
    juce::Uuid channel() const { return document.getSession().channels[0].id; }
    PluginChain& chain() { return *engine.getChannelChain (channel()); }
    void addGroup (std::initializer_list<int> members)
    {
        const auto group = document.addPluginGroup (channel());
        for (const int index : members)
            document.setPluginGroupMember (channel(), group, chain().getSlot (index).state.slotId, true);
    }
    juce::AudioBuffer<float> render (int blocks = 1)
    {
        juce::AudioBuffer<float> input (2, 64), output (2, 64);
        input.clear();
        juce::FloatVectorOperations::fill (input.getWritePointer (0), 0.4f, 64);
        for (int i = 0; i < blocks; ++i)
            engine.renderBlock (input.getArrayOfReadPointers(), 2, output.getArrayOfWritePointers(), 2, 64);
        return output;
    }
};

class GatedGain final : public TestGainPlugin
{
public:
    GatedGain() : TestGainPlugin (0.1f) {}
    std::atomic<bool> armed { false };
    juce::WaitableEvent entered, release;
    bool timedOut = false;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        TestGainPlugin::processBlock (buffer, midi);
        if (armed.exchange (false))
        {
            entered.signal();
            timedOut = ! release.wait (5000); // deterministic test barrier, never used by production plugins
        }
    }
};
}

class LiveMixPluginTransitionTests final : public juce::UnitTest
{
public:
    LiveMixPluginTransitionTests() : UnitTest ("LiveMix plugin transitions", "LiveMix") {}

    void expectUnity (const juce::AudioBuffer<float>& block)
    {
        float worst = 0.0f;
        bool finite = true;
        for (int ch = 0; ch < block.getNumChannels(); ++ch)
            for (int i = 0; i < block.getNumSamples(); ++i)
            {
                const float value = block.getSample (ch, i);
                finite = finite && std::isfinite (value);
                worst = juce::jmax (worst, std::abs (value - 0.4f));
            }
        expect (finite);
        expectLessThan (worst, 1.0e-5f);
    }

    void runTest() override
    {
        beginTest ("equal OFF commands repair actual bypass without dirty, callback or revision changes");
        for (const bool everywhere : { false, true })
        {
            TransitionFixture f;
            f.chain().addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            f.addGroup ({ 0 });
            f.document.setPluginGroupOff (f.channel(), 0, true);
            expectUnity (f.render (8));
            f.chain().setBypassed (0, false); // simulate runtime drift while the document still says OFF
            expectWithinAbsoluteError (f.render (8).getSample (0, 63), 0.2f, 1.0e-6f);
            f.document.discardUnsavedChanges();
            f.valueCalls = 0;
            expect (f.state.update (ControlState::capture (f.document, f.groups, false)));
            const auto revision = f.state.getCurrent().revision;
            const P::CommandArgs args = everywhere ? P::CommandArgs { P::SetPluginGroupOffEverywhere { 1, true } }
                                                   : P::CommandArgs { P::SetPluginGroupOff { f.channel(), 1, true } };
            const auto reply = f.dispatcher.dispatch ({ "1", f.instance, f.document.getSessionGeneration(), {}, args });
            const auto* ack = std::get_if<P::Ack> (&reply);
            expect (ack != nullptr);
            if (ack != nullptr)
            {
                expect (! ack->changed);
                expectEquals (ack->context.revision, revision);
            }
            expect (f.chain().getSlot (0).bypassed.load());
            expect (! f.document.isDirty());
            expectEquals (f.valueCalls, 0);
            expectUnity (f.render (8));
        }

        beginTest ("nonadjacent group members with latency stay aligned through rapid reversals");
        {
            TransitionFixture f;
            const float gains[] { 0.1f, 1.0f, 10.0f };
            const int latencies[] { 17, 43, 97 };
            for (int i = 0; i < 3; ++i)
            {
                auto plugin = std::make_unique<TestGainPlugin> (gains[i]);
                plugin->latencySamples = latencies[i];
                f.chain().addPlugin (std::move (plugin));
            }
            f.addGroup ({ 0, 2 });
            expectUnity (f.render (8));
            for (int i = 0; i < 12; ++i)
            {
                f.document.setPluginGroupOff (f.channel(), 0, i % 2 == 0);
                expectUnity (f.render()); // 64 samples between commands, less than the accumulated latency
                expect (! f.chain().getSlot (1).bypassed.load());
            }
            for (int i = 0; i < 4; ++i)
                expectUnity (f.render());

            // A later single-slot edit still ramps, even after this slot participated in a group transition.
            f.chain().setBypassed (2, true);
            const auto single = f.render();
            expectGreaterThan (single.getSample (0, 0), 0.39f);
            expectLessThan (single.getSample (0, 0), 0.4f);
            expectGreaterThan (single.getSample (0, 63), 0.04f);
            expectLessThan (single.getSample (0, 63), single.getSample (0, 0));
        }

        beginTest ("a group command between two plugin calls cannot split an audio block");
        {
            TransitionFixture f;
            auto plugin = std::make_unique<GatedGain>();
            auto* gate = plugin.get();
            f.chain().addPlugin (std::move (plugin));
            f.chain().addPlugin (std::make_unique<TestGainPlugin> (10.0f));
            f.addGroup ({ 0, 1 });
            f.document.setPluginGroupOff (f.channel(), 0, true);
            expectUnity (f.render (8));
            juce::AudioBuffer<float> during;
            gate->armed.store (true);
            std::thread audio ([&] { during = f.render(); });
            const bool entered = gate->entered.wait (5000);
            if (entered)
                f.document.setPluginGroupOff (f.channel(), 0, false);
            gate->release.signal();
            audio.join();
            expect (entered && ! gate->timedOut);
            expectUnity (during);
            expectUnity (f.render());
        }

        beginTest ("append keeps existing instances and gives repeated or missing slots unique identities");
        {
            PluginChain chain;
            chain.prepare (48000.0, 64);
            chain.addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            const auto original = chain.getSlot (0).plugin.get();
            const auto originalId = chain.getSlot (0).state.slotId;
            auto states = chain.getStates();
            states.push_back (states.front());
            int created = 0;
            const auto errors = chain.append (states, [&] (const PluginSlotState&, juce::String& error)
                -> std::unique_ptr<juce::AudioPluginInstance>
            {
                if (++created == 1) return std::make_unique<TestGainPlugin> (1.0f);
                error = "deliberately missing";
                return {};
            });
            expectEquals (errors.size(), 1);
            expectEquals (chain.getNumSlots(), 3);
            expect (chain.getSlot (0).plugin.get() == original && chain.getSlot (0).state.slotId == originalId);
            expect (chain.getSlot (1).state.slotId != originalId);
            expect (chain.getSlot (2).state.slotId != originalId && chain.getSlot (2).state.slotId != chain.getSlot (1).state.slotId);
            expect (chain.getSlot (2).isMissing());
            expectEquals (chain.getSlot (2).state.stateBase64, states[1].stateBase64);
            expectWithinAbsoluteError (static_cast<TestGainPlugin*> (chain.getSlot (1).plugin.get())->gain, 0.5f, 1.0e-6f);
        }
    }
};

static LiveMixPluginTransitionTests liveMixPluginTransitionTests;
} // namespace gocue::tests
