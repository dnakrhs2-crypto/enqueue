#include "ControlDispatcher.h"
#include "MixDocument.h"
#include "MuteGroups.h"
#include "TestGainPlugin.h"
#include "../livemix/src/ui/MainComponent.h"

#include <cmath>

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

}

class LiveMixPluginTransitionTests final : public juce::UnitTest
{
public:
    LiveMixPluginTransitionTests() : UnitTest ("LiveMix group OFF re-sync", "LiveMix") {}

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

        beginTest ("equal OFF commands repair actual bypass without dirty, callback or revision changes");
        for (const bool everywhere : { false, true })
        for (const int slots : { 1, 2 })
        {
            TransitionFixture f;
            for (int i = 0; i < slots; ++i)
                f.chain().addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            f.addGroup (slots == 1 ? std::initializer_list<int> { 0 } : std::initializer_list<int> { 0, 1 });
            f.document.setPluginGroupOff (f.channel(), 0, true);
            expectUnity (f.render (8));
            for (int i = 0; i < slots; ++i)
                f.chain().setBypassed (i, false); // simulate runtime drift while the document still says OFF
            expectWithinAbsoluteError (f.render (8).getSample (0, 63), slots == 1 ? 0.2f : 0.1f, 1.0e-6f);
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
            for (int i = 0; i < slots; ++i)
            {
                expect (f.chain().getSlot (i).bypassed.load());
                expect (f.chain().getSlot (i).state.bypassed);
            }
            expect (! f.document.isDirty());
            expectEquals (f.valueCalls, 0);
            expectUnity (f.render (8));
        }
    }
};

static LiveMixPluginTransitionTests liveMixPluginTransitionTests;
} // namespace gocue::tests
