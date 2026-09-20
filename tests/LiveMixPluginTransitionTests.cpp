#include "ControlDispatcher.h"
#include "MixDocument.h"
#include "MuteGroups.h"
#include "TestGainPlugin.h"
#include "../livemix/src/ui/MainComponent.h"

#include <cmath>
#include <thread>

namespace gocue::tests
{
namespace
{
using namespace gocue::livemix;
using P = ControlProtocol;

struct OpenTransitionChain
{
    using Type = void (MainComponent::*) (PluginChain*, const juce::String&);
    friend Type member (OpenTransitionChain);
};
template <typename Tag, typename Tag::Type method>
struct TransitionPrivateMethod
{
    friend typename Tag::Type member (Tag) { return method; }
};
template struct TransitionPrivateMethod<OpenTransitionChain, &MainComponent::openChainFor>;

template <typename T>
T* transitionChild (juce::Component& parent)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* found = dynamic_cast<T*> (child)) return found;
        if (auto* found = transitionChild<T> (*child)) return found;
    }
    return nullptr;
}

class TransitionSquare final : public TestGainPlugin
{
public:
    TransitionSquare() : TestGainPlugin (1.0f) {}
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        TestGainPlugin::processBlock (buffer, midi);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const auto value = buffer.getSample (ch, i);
                buffer.setSample (ch, i, value * value);
            }
    }
};

class TransitionParameterGain final : public TestGainPlugin
{
public:
    TransitionParameterGain() : TestGainPlugin (1.0f)
    {
        juce::AudioProcessor::addParameter (parameter = new juce::AudioParameterFloat (juce::ParameterID { "gain", 1 }, "Gain", 0.0f, 1.0f, 1.0f));
    }
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        gain = parameter->get();
        TestGainPlugin::processBlock (buffer, midi);
    }
    void getStateInformation (juce::MemoryBlock& state) override
    {
        gain = parameter->get();
        TestGainPlugin::getStateInformation (state);
    }
    void setStateInformation (const void* state, int bytes) override
    {
        TestGainPlugin::setStateInformation (state, bytes);
        static_cast<juce::AudioProcessorParameter*> (parameter)->setValue (gain);
    }
    juce::AudioParameterFloat* parameter = nullptr;
};

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
        chain().setGroupBypassFactory ([this] (const PluginSlotState& state, juce::String&)
            -> std::unique_ptr<juce::AudioPluginInstance>
        {
            std::unique_ptr<TestGainPlugin> plugin;
            for (int i = 0; i < chain().getNumSlots(); ++i)
                if (chain().getSlot (i).state.slotId == state.slotId)
                {
                    if (dynamic_cast<TransitionSquare*> (chain().getSlot (i).plugin.get()) != nullptr)
                        plugin = std::make_unique<TransitionSquare>();
                    else if (dynamic_cast<TransitionParameterGain*> (chain().getSlot (i).plugin.get()) != nullptr)
                        plugin = std::make_unique<TransitionParameterGain>();
                    else
                        plugin = std::make_unique<TestGainPlugin> (1.0f);
                    plugin->latencySamples = chain().getSlot (i).latency.load();
                }
            if (plugin == nullptr) plugin = std::make_unique<TestGainPlugin> (1.0f);
            return plugin;
        });
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
        beginTest ("group final crossfade has no first-block click or peak with unequal endpoints");
        {
            TransitionFixture f;
            f.chain().addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            f.chain().addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            f.addGroup ({ 0, 1 });
            float previous = f.render (8).getSample (0, 63);
            expectWithinAbsoluteError (previous, 0.1f, 1.0e-6f);
            for (const bool off : { true, false })
            {
                f.document.setPluginGroupOff (f.channel(), 0, off);
                float peak = 0.0f, delta = 0.0f, firstDelta = 0.0f;
                bool finite = true;
                for (int block = 0; block < 5; ++block)
                {
                    const auto output = f.render();
                    for (int i = 0; i < 64; ++i)
                    {
                        const auto value = output.getSample (0, i);
                        finite = finite && std::isfinite (value);
                        peak = juce::jmax (peak, std::abs (value));
                        const auto step = std::abs (value - previous);
                        delta = juce::jmax (delta, step);
                        if (block == 0) firstDelta = juce::jmax (firstDelta, step);
                        previous = value;
                    }
                    if (block == 0)
                        expect (previous > 0.1f && previous < 0.4f, "The first block must be inside the 5 ms fade");
                }
                logMessage (juce::String ("Group unequal endpoints ") + (off ? "ON->OFF" : "OFF->ON")
                    + ": peak=" + juce::String (peak, 8) + ", max delta=" + juce::String (delta, 8)
                    + ", first-block max delta=" + juce::String (firstDelta, 8));
                expect (finite && peak <= 0.40001f, "The final fade must not overshoot either endpoint");
                expect (delta <= 0.001251f, "A 0.3 change must ramp over 240 samples, including the first sample");
                expectWithinAbsoluteError (previous, off ? 0.4f : 0.1f, 1.0e-6f);
            }
        }

        beginTest ("equal OFF re-sync refreshes the real chain button without a document edit");
        for (const bool everywhere : { false, true })
        {
            TransitionFixture f;
            const auto directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("livemix-transition-ui-" + juce::Uuid().toString());
            {
                LiveMixSettings settings (directory);
                int runtimeCalls = 0;
                MainComponent main (f.document, settings);
                main.setSize (900, 800);
                f.document.onChainRuntimeChanged = [&runtimeCalls, original = f.document.onChainRuntimeChanged] (PluginChain& chain)
                {
                    if (original) original (chain);
                    ++runtimeCalls;
                };
                f.document.onValueChanged = [&f, original = f.document.onValueChanged]
                {
                    if (original) original();
                    ++f.valueCalls;
                };
                f.chain().addPlugin (std::make_unique<TestGainPlugin> (0.5f));
                f.addGroup ({ 0 });
                (main.*member (OpenTransitionChain {})) (&f.chain(), "Transition test");
                auto* drawer = transitionChild<ChainDrawer> (main);
                expect (drawer != nullptr);
                if (drawer == nullptr) continue;
                const auto displayedOn = [&] { return transitionChild<juce::ToggleButton> (*drawer)->getToggleState(); };
                f.document.setPluginGroupOff (f.channel(), 0, true);
                expect (! displayedOn());
                f.chain().setBypassed (0, false); // the same edit as the drawer's manual ON
                expect (displayedOn());
                f.document.discardUnsavedChanges();
                f.valueCalls = 0;
                f.state.update (ControlState::capture (f.document, f.groups, false));
                const auto revision = f.state.getCurrent().revision;
                const P::CommandArgs args = everywhere ? P::CommandArgs { P::SetPluginGroupOffEverywhere { 1, true } }
                                                       : P::CommandArgs { P::SetPluginGroupOff { f.channel(), 1, true } };
                const auto reply = f.dispatcher.dispatch ({ "1", f.instance, f.document.getSessionGeneration(), {}, args });
                const auto* ack = std::get_if<P::Ack> (&reply);
                expect (ack != nullptr && ! ack->changed);
                if (ack != nullptr) expectEquals (ack->context.revision, revision);
                expect (f.chain().getSlot (0).bypassed.load());
                expect (f.chain().getSlot (0).state.bypassed);
                expect (f.document.getSession().channels[0].pluginGroups[0].off);
                expect (! displayedOn(), "A restored OFF slot must be displayed OFF in the open chain drawer");
                expect (! f.document.isDirty());
                expectEquals (f.valueCalls, 0);
                expectEquals (runtimeCalls, 1);
                f.document.setPluginGroupOff (f.channel(), 0, true);
                expect (! displayedOn() && ! f.document.isDirty());
                expectEquals (f.valueCalls, 0);
                expectEquals (runtimeCalls, 1, "An already-correct OFF command needs no refresh");
            }
            directory.deleteRecursively();
        }

        beginTest ("group endpoints align changing stereo input through a delayed nonlinear nonmember");
        {
            TransitionFixture f;
            const int latencies[] { 17, 43, 97 };
            for (int i = 0; i < 3; ++i)
            {
                std::unique_ptr<TestGainPlugin> plugin = i == 1 ? std::unique_ptr<TestGainPlugin> (new TransitionSquare())
                                                               : std::make_unique<TestGainPlugin> (0.5f);
                plugin->latencySamples = latencies[i];
                f.chain().addPlugin (std::move (plugin));
            }
            f.addGroup ({ 0, 2 });
            const auto inputAt = [] (int channel, int sample)
            {
                return (channel == 0 ? 0.4f : -0.3f) + 0.07f * std::sin ((float) sample * 0.031f);
            };
            int frame = 0;
            float worst = 0.0f, peak = 0.0f;
            for (int block = 0; block < 40; ++block)
            {
                if (block == 12) f.document.setPluginGroupOff (f.channel(), 0, true);
                if (block == 26) f.document.setPluginGroupOff (f.channel(), 0, false);
                juce::AudioBuffer<float> audio (2, 64);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 64; ++i)
                        audio.setSample (ch, i, inputAt (ch, frame + i));
                f.chain().process (audio, 64);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 64; ++i)
                    {
                        const int n = frame + i;
                        const float x = inputAt (ch, n - 157);
                        float offWeight = juce::jlimit (0.0f, 1.0f, (float) (n - (12 * 64 + 157) + 1) / 240.0f);
                        if (n >= 26 * 64 + 157)
                            offWeight = 1.0f - juce::jlimit (0.0f, 1.0f, (float) (n - (26 * 64 + 157) + 1) / 240.0f);
                        const float expected = n < 157 ? 0.0f : x * x * (0.125f + 0.875f * offWeight);
                        worst = juce::jmax (worst, std::abs (audio.getSample (ch, i) - expected));
                        peak = juce::jmax (peak, std::abs (audio.getSample (ch, i)));
                    }
                frame += 64;
            }
            logMessage ("Delayed nonlinear group: max aligned endpoint error=" + juce::String (worst, 8)
                + ", peak=" + juce::String (peak, 8));
            expectLessThan (worst, 1.0e-6f);
            expectLessThan (peak, 0.221f);
            expect (! f.chain().getSlot (1).bypassed.load());
        }

        beginTest ("audible group path follows editor parameters and keeps histories on append and removal");
        {
            TransitionFixture f;
            f.chain().addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            auto effect = std::make_unique<TransitionParameterGain>();
            auto* parameter = effect->parameter;
            f.chain().addPlugin (std::move (effect));
            f.chain().addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            f.addGroup ({ 0, 2 });
            f.render (8);
            f.document.setPluginGroupOff (f.channel(), 0, true);
            expectUnity (f.render (8));
            parameter->setValueNotifyingHost (0.5f);
            expectWithinAbsoluteError (f.render().getSample (0, 0), 0.2f, 1.0e-6f);

            PluginChain source;
            source.addPlugin (std::make_unique<TestGainPlugin> (4.0f));
            source.addPlugin (std::make_unique<TestGainPlugin> (0.25f));
            f.chain().append (source.getStates(), [] (const PluginSlotState&, juce::String&)
                { return std::make_unique<TestGainPlugin> (1.0f); });
            expectWithinAbsoluteError (f.render().getSample (0, 0), 0.2f, 1.0e-6f);
            f.chain().removePlugin (4);
            f.chain().removePlugin (3);
            expectWithinAbsoluteError (f.render().getSample (0, 0), 0.2f, 1.0e-6f);

            auto states = f.chain().getStates();
            parameter->setValueNotifyingHost (0.25f);
            expectWithinAbsoluteError (f.render().getSample (0, 0), 0.1f, 1.0e-6f);
            f.chain().applyStates (states);
            expectWithinAbsoluteError (f.render().getSample (0, 0), 0.2f, 1.0e-6f);
            f.document.setPluginGroupOff (f.channel(), 0, false);
            expectWithinAbsoluteError (f.render (8).getSample (0, 63), 0.05f, 1.0e-6f);
            f.chain().resetProcessing();
            expectWithinAbsoluteError (f.render (8).getSample (0, 63), 0.05f, 1.0e-6f);
        }

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
