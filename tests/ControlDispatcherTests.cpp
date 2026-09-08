#include "ControlDispatcher.h"
#include "MixDocument.h"
#include "MuteGroups.h"
#include "TestGainPlugin.h"

#include <juce_events/juce_events.h>

namespace gocue::tests
{

using namespace gocue::livemix;

class ControlDispatcherTests : public juce::UnitTest
{
public:
    using P = ControlProtocol;
    ControlDispatcherTests() : juce::UnitTest ("LiveMix control dispatcher", "LiveMix") {}

    struct Fixture
    {
        MixEngine engine;
        MixDocument document { engine };
        MuteGroups groups { document };
        juce::Uuid instance;
        bool running = false;
        ControlState state { ControlState::capture (document, groups, running) };
        ControlDispatcher dispatcher { document, groups, state, instance, [this] { return running; } };
        int valueCalls = 0, structureCalls = 0, groupCalls = 0;
        bool callbacksOnMessageThread = true;
        juce::int64 nextId = 1;
        juce::Uuid generation = document.getSessionGeneration();

        Fixture()
        {
            engine.prepare (48000.0, 256);
            document.applyToEngine();
            document.onValueChanged = [this]
            {
                ++valueCalls; groups.apply();
                callbacksOnMessageThread = callbacksOnMessageThread && juce::MessageManager::getInstance()->isThisTheMessageThread();
            };
            document.onStructureChanged = [this]
            {
                ++structureCalls;
                if (generation != document.getSessionGeneration()) { generation = document.getSessionGeneration(); groups.reset(); }
                else groups.apply();
            };
            groups.onChanged = [this]
            {
                ++groupCalls;
                callbacksOnMessageThread = callbacksOnMessageThread && juce::MessageManager::getInstance()->isThisTheMessageThread();
            };
        }
        juce::Uuid channel() const { return document.getSession().channels[0].id; }
        juce::Uuid fx() const { return document.getSession().fx[0].id; }
        P::Command command (P::CommandArgs args) { return { juce::String (nextId++), instance, document.getSessionGeneration(), {}, std::move (args) }; }
        void settle() { state.update (ControlState::capture (document, groups, running)); }
        void clean() { document.discardUnsavedChanges(); settle(); valueCalls = structureCalls = groupCalls = 0; }
        float render()
        {
            juce::AudioBuffer<float> input (2, 256), output (2, 256);
            input.clear(); juce::FloatVectorOperations::fill (input.getWritePointer (0), 0.5f, 256);
            for (int i = 0; i < 3; ++i) engine.renderBlock (input.getArrayOfReadPointers(), 2, output.getArrayOfWritePointers(), 2, 256);
            return output.getSample (0, 255);
        }
    };

    P::Ack ack (Fixture& f, P::CommandArgs args)
    {
        const auto reply = f.dispatcher.dispatch (f.command (std::move (args)));
        const auto* result = std::get_if<P::Ack> (&reply);
        expect (result != nullptr);
        return result != nullptr ? *result : P::Ack();
    }
    template <typename T>
    T result (const P::Ack& a)
    {
        const auto* r = std::get_if<T> (&a.result);
        expect (r != nullptr);
        return r != nullptr ? *r : T();
    }
    void error (Fixture& f, const P::Command& command, P::ErrorCode code)
    {
        const auto before = ControlState::capture (f.document, f.groups, f.running);
        const auto valueCalls = f.valueCalls, groupCalls = f.groupCalls;
        const auto reply = f.dispatcher.dispatch (command);
        const auto* failure = std::get_if<P::ErrorResponse> (&reply);
        expect (failure != nullptr);
        if (failure != nullptr)
        {
            expect (failure->error.code == code);
            expect (failure->context.has_value());
            if (failure->context) expectEquals (failure->context->revision, f.state.getCurrent().revision);
        }
        expect (ControlState::capture (f.document, f.groups, f.running).projection == before.projection);
        expectEquals (f.valueCalls, valueCalls); expectEquals (f.groupCalls, groupCalls);
    }

    void runTest() override
    {
        beginTest ("set/toggle change the real offline model and engine once, through the existing value callback");
        {
            Fixture f;
            expectWithinAbsoluteError (f.render(), 0.5f, 1.0e-6f);
            const auto revision = f.state.getCurrent().revision;
            const auto off = ack (f, P::SetChannelOn { f.channel(), false });
            expect (off.changed); expect (! result<P::OnResult> (off).on);
            expect (! f.document.getSession().channels[0].on); expect (f.document.isDirty());
            expectEquals (f.valueCalls, 1); expectEquals (f.groupCalls, 0); expectEquals (f.structureCalls, 0);
            expectEquals (off.context.revision, revision + 1);
            expectWithinAbsoluteError (f.render(), 0.0f, 1.0e-6f);
            const auto on = ack (f, P::ToggleChannel { f.channel() });
            expect (on.changed && result<P::OnResult> (on).on);
            expect (f.document.getSession().channels[0].on); expectEquals (f.valueCalls, 2);
            expectWithinAbsoluteError (f.render(), 0.5f, 1.0e-6f);
            expect (f.callbacksOnMessageThread); expect (! f.state.getCurrent().projection.audioRunning);
        }

        beginTest ("two clients toggle at dispatch time and competing conditional sets apply exactly once");
        {
            Fixture f;
            auto first = f.command (P::ToggleChannel { f.channel() });
            auto second = f.command (P::ToggleChannel { f.channel() });   // both prepared before either executes
            const auto a = f.dispatcher.dispatch (first), b = f.dispatcher.dispatch (second);
            expect (std::holds_alternative<P::Ack> (a) && std::holds_alternative<P::Ack> (b));
            if (const auto* r = std::get_if<P::Ack> (&a)) expect (! result<P::OnResult> (*r).on);
            if (const auto* r = std::get_if<P::Ack> (&b)) expect (result<P::OnResult> (*r).on);
            expect (f.document.getSession().channels[0].on); expectEquals (f.valueCalls, 2);
            first = f.command (P::SetChannelOn { f.channel(), false }); first.ifRevision = f.state.getCurrent().revision;
            second = f.command (P::SetChannelOn { f.channel(), false }); second.ifRevision = first.ifRevision;
            expect (std::holds_alternative<P::Ack> (f.dispatcher.dispatch (first)));
            error (f, second, P::ErrorCode::revisionConflict);
            expectEquals (f.valueCalls, 3);
        }

        beginTest ("setAllChannelsOn reports count, preserves mute latches/membership and emits one value callback");
        {
            Fixture f;
            f.document.addChannel();
            f.document.setChannelMuteGroup (f.channel(), true); f.document.setFxMuteGroup (f.fx(), true);
            f.groups.set (MuteGroups::Group::mic, true); f.groups.set (MuteGroups::Group::fx, true);
            f.clean();
            const auto off = ack (f, P::SetAllChannelsOn { false });
            expect (off.changed); expectEquals (result<P::AllChannelsResult> (off).count, 2);
            for (const auto& c : f.document.getSession().channels) expect (! c.on);
            const auto on = ack (f, P::SetAllChannelsOn { true });
            expect (on.changed && result<P::AllChannelsResult> (on).on);
            for (const auto& c : f.document.getSession().channels) expect (c.on);
            expect (f.groups.isMuted (MuteGroups::Group::mic)); expect (f.groups.isMuted (MuteGroups::Group::fx));
            expect (f.document.getSession().channels[0].muteGroup); expect (f.document.getSession().fx[0].muteGroup);
            expectEquals (f.valueCalls, 2); expectEquals (f.groupCalls, 0); expectEquals (f.structureCalls, 0);
        }

        beginTest ("both mute commands use MuteGroups callbacks, retain stored sends/returns, and latch empty groups");
        {
            Fixture f;
            f.clean();
            const auto empty = ack (f, P::ToggleMuteGroup { P::MuteGroup::mic });
            expect (empty.changed && result<P::MuteGroupResult> (empty).muted);
            expect (f.groups.isMuted (MuteGroups::Group::mic)); expect (! f.document.isDirty());
            expectEquals (f.groupCalls, 1); expectEquals (f.valueCalls, 0);
            expectWithinAbsoluteError (f.render(), 0.5f, 1.0e-6f);   // no members yet
            f.document.setChannelMuteGroup (f.channel(), true);   // production value callback reapplies membership
            expectWithinAbsoluteError (f.render(), 0.0f, 1.0e-6f);
            const auto released = ack (f, P::SetMuteGroup { P::MuteGroup::mic, false });
            expect (released.changed && ! result<P::MuteGroupResult> (released).muted);
            expectWithinAbsoluteError (f.render(), 0.5f, 1.0e-6f);
            f.document.setSend (f.channel(), f.fx(), 0.5, true); f.document.setFxReturn (f.fx(), 0.8);
            f.document.setFxMuteGroup (f.fx(), true); f.clean();
            expectWithinAbsoluteError (f.render(), 0.7f, 1.0e-6f);
            const auto mutedFx = ack (f, P::SetMuteGroup { P::MuteGroup::fx, true });
            expect (mutedFx.changed && result<P::MuteGroupResult> (mutedFx).group == P::MuteGroup::fx);
            expectWithinAbsoluteError (f.render(), 0.5f, 1.0e-6f);
            expect (ack (f, P::ToggleMuteGroup { P::MuteGroup::fx }).changed);
            expectWithinAbsoluteError (f.render(), 0.7f, 1.0e-6f);
            expectWithinAbsoluteError (f.document.getSession().fx[0].returnAmount, 0.8, 1.0e-12);
            expectWithinAbsoluteError (f.document.getSession().channels[0].sends[0].amount, 0.5, 1.0e-12);
            expect (f.document.getSession().channels[0].sends[0].pre);
            expect (! f.document.isDirty()); expectEquals (f.valueCalls, 0); expectEquals (f.groupCalls, 2);
            expect (f.callbacksOnMessageThread);
        }

        beginTest ("plugin groups convert 1-based indexes and preserve overlapping bypass semantics in the engine");
        {
            Fixture f;
            auto* chain = f.engine.getChannelChain (f.channel());
            expect (chain != nullptr);
            if (chain != nullptr)
            {
                chain->addPlugin (std::make_unique<TestGainPlugin> (0.5f));
                const auto slot = chain->getSlot (0).state.slotId;
                expectEquals (f.document.addPluginGroup (f.channel()), 0); expectEquals (f.document.addPluginGroup (f.channel()), 1);
                f.document.setPluginGroupMember (f.channel(), 0, slot, true); f.document.setPluginGroupMember (f.channel(), 1, slot, true);
                f.clean();
                expectWithinAbsoluteError (f.render(), 0.25f, 1.0e-6f);
                const auto two = ack (f, P::SetPluginGroupOff { f.channel(), 2, true });
                expect (two.changed); expectEquals (result<P::PluginGroupResult> (two).index, 2);
                expect (! f.document.getSession().channels[0].pluginGroups[0].off);
                expect (f.document.getSession().channels[0].pluginGroups[1].off && chain->getSlot (0).bypassed.load());
                expectWithinAbsoluteError (f.render(), 0.5f, 1.0e-6f);
                expect (ack (f, P::SetPluginGroupOff { f.channel(), 1, true }).changed);
                expect (ack (f, P::SetPluginGroupOff { f.channel(), 2, false }).changed);
                expect (chain->getSlot (0).bypassed.load());
                expect (ack (f, P::SetPluginGroupOff { f.channel(), 1, false }).changed);
                expect (! chain->getSlot (0).bypassed.load());
                expectWithinAbsoluteError (f.render(), 0.25f, 1.0e-6f);
                expectEquals (f.valueCalls, 4); expectEquals (f.structureCalls, 0); expectEquals (f.groupCalls, 0);
            }
        }

        beginTest ("setSend preserves the omitted field at dispatch time and never creates a phantom send");
        {
            Fixture f;
            const auto both = ack (f, P::SetSend { f.channel(), f.fx(), 0.35, true });
            expect (both.changed && result<P::SendResult> (both).pre);
            const auto amount = ack (f, P::SetSend { f.channel(), f.fx(), 0.6, {} });
            expect (amount.changed && result<P::SendResult> (amount).pre);
            expectWithinAbsoluteError (f.document.getSession().channels[0].sends[0].amount, 0.6, 1.0e-12);
            auto preCommand = f.command (P::SetSend { f.channel(), f.fx(), {}, false });
            f.document.setSend (f.channel(), f.fx(), 0.7, true);   // a UI edit after receipt, before dispatch
            const auto preReply = f.dispatcher.dispatch (preCommand);
            expect (std::holds_alternative<P::Ack> (preReply));
            if (const auto* pre = std::get_if<P::Ack> (&preReply))
            {
                expect (pre->changed && ! result<P::SendResult> (*pre).pre);
                expectWithinAbsoluteError (result<P::SendResult> (*pre).amount, 0.7, 1.0e-12);
            }
            expect (! f.document.getSession().channels[0].sends[0].pre);
            expectEquals (f.valueCalls, 4); expectEquals (f.groupCalls, 0);
            const auto count = f.document.getSession().channels[0].sends.size();
            error (f, f.command (P::SetSend { f.channel(), juce::Uuid(), 1.0, true }), P::ErrorCode::fxNotFound);
            error (f, f.command (P::SetSend { juce::Uuid(), f.fx(), 1.0, true }), P::ErrorCode::channelNotFound);
            expectEquals ((int) f.document.getSession().channels[0].sends.size(), (int) count);
            // Even an absent entry for a valid FX must not be created by a same-default request/read.
            f.document.getSession().channels[0].sends.clear(); f.clean();
            expect (! ack (f, P::SetSend { f.channel(), f.fx(), 0.0, false }).changed);
            expect (f.document.getSession().channels[0].sends.empty()); expect (! f.document.isDirty());
            expect (ack (f, P::SetSend { f.channel(), f.fx(), {}, true }).changed);
            expectEquals ((int) f.document.getSession().channels[0].sends.size(), 1);
        }

        beginTest ("every equal-value set and requestState skips dirty/callback/revision changes, including empty all");
        {
            Fixture f;
            f.document.addPluginGroup (f.channel()); f.clean();
            const auto revision = f.state.getCurrent().revision;
            const P::CommandArgs commands[] { P::SetChannelOn { f.channel(), true }, P::SetAllChannelsOn { true },
                P::SetMuteGroup { P::MuteGroup::mic, false }, P::SetMuteGroup { P::MuteGroup::fx, false },
                P::SetPluginGroupOff { f.channel(), 1, false }, P::SetSend { f.channel(), f.fx(), 0.0, {} },
                P::SetSend { f.channel(), f.fx(), {}, false }, P::RequestState {} };
            for (const auto& command : commands)
            {
                const auto response = ack (f, command);
                expect (! response.changed); expectEquals (response.context.revision, revision);
                expect (! f.document.isDirty()); expectEquals (f.valueCalls, 0); expectEquals (f.groupCalls, 0);
            }
            auto request = f.command (P::RequestState {}); request.sessionId.reset();
            const auto reply = f.dispatcher.dispatch (request);
            expect (std::holds_alternative<P::Ack> (reply));
            if (const auto* response = std::get_if<P::Ack> (&reply)) expectEquals (result<P::RequestStateResult> (*response).snapshotRevision, revision);
            f.document.removeChannel (f.channel()); f.clean();
            const auto emptyRevision = f.state.getCurrent().revision;
            const auto all = ack (f, P::SetAllChannelsOn { false });
            expect (! all.changed); expectEquals (result<P::AllChannelsResult> (all).count, 0);
            expectEquals (all.context.revision, emptyRevision); expect (! f.document.isDirty()); expectEquals (f.valueCalls, 0);
        }

        beginTest ("invalid typed arguments, missing targets and stale instance/session/revision never invoke setters");
        {
            Fixture f;
            error (f, f.command (P::SetChannelOn { juce::Uuid(), false }), P::ErrorCode::channelNotFound);
            error (f, f.command (P::SetPluginGroupOff { f.channel(), 1, true }), P::ErrorCode::pluginGroupNotFound);
            for (const auto index : { 0, 6 }) error (f, f.command (P::SetPluginGroupOff { f.channel(), index, true }), P::ErrorCode::invalidArgument);
            error (f, f.command (P::SetSend { f.channel(), f.fx(), -0.1, {} }), P::ErrorCode::invalidArgument);
            error (f, f.command (P::SetSend { f.channel(), f.fx(), 1.1, {} }), P::ErrorCode::invalidArgument);
            error (f, f.command (P::SetSend { f.channel(), f.fx(), {}, {} }), P::ErrorCode::invalidArgument);
            error (f, f.command (P::ToggleChannel { juce::Uuid::null() }), P::ErrorCode::invalidArgument);
            error (f, f.command (P::SetMuteGroup { (P::MuteGroup) 99, true }), P::ErrorCode::invalidArgument);
            auto c = f.command (P::ToggleChannel { f.channel() }); c.instanceId = juce::Uuid();
            error (f, c, P::ErrorCode::instanceChanged);
            c = f.command (P::ToggleChannel { f.channel() }); c.sessionId = juce::Uuid();
            error (f, c, P::ErrorCode::sessionChanged);
            c = f.command (P::ToggleChannel { f.channel() }); c.sessionId.reset();
            error (f, c, P::ErrorCode::invalidArgument);
            c = f.command (P::ToggleChannel { f.channel() }); c.ifRevision = f.state.getCurrent().revision;
            f.document.renameChannel (f.channel(), "UI edit before timer");
            error (f, c, P::ErrorCode::revisionConflict);
            expect (f.state.getCurrent().revision > *c.ifRevision);
            auto stale = f.command (P::SetChannelOn { f.channel(), false });
            f.document.newSession();
            error (f, stale, P::ErrorCode::sessionChanged);   // session check precedes missing target
            expect (f.document.getSession().channels[0].on);
        }
    }
};

static ControlDispatcherTests controlDispatcherTests;

} // namespace gocue::tests
