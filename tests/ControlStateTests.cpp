#include "ControlDispatcher.h"
#include "MixDocument.h"
#include "MuteGroups.h"

namespace gocue::tests
{

using namespace gocue::livemix;

class ControlStateTests : public juce::UnitTest
{
public:
    using P = ControlProtocol;
    ControlStateTests() : juce::UnitTest ("LiveMix control state", "LiveMix") {}

    struct Fixture
    {
        MixEngine engine;
        MixDocument document { engine };
        MuteGroups groups { document };
        juce::Uuid generation = document.getSessionGeneration();
        int values = 0, structures = 0;
        Fixture()
        {
            engine.prepare (48000.0, 256); document.applyToEngine();
            document.onValueChanged = [this] { ++values; groups.apply(); };
            document.onStructureChanged = [this]
            {
                ++structures;
                if (generation != document.getSessionGeneration()) { generation = document.getSessionGeneration(); groups.reset(); }
                else groups.apply();
            };
        }
        juce::Uuid channel() const { return document.getSession().channels[0].id; }
        juce::Uuid fx() const { return document.getSession().fx[0].id; }
        ControlState::Snapshot capture (bool running = false) { return ControlState::capture (document, groups, running); }
    };

    // A small client-side merge for the wire contract: channel/FX entries merge by id, nested scalar objects
    // merge by key, and all other arrays replace. Comparing its result with a fresh full state tests the schema.
    void merge (juce::var& target, const juce::var& patch, bool root = true)
    {
        const auto* object = patch.getDynamicObject();
        expect (object != nullptr);
        if (object == nullptr) return;
        for (const auto& property : object->getProperties())
        {
            const auto key = property.name;
            const auto& value = property.value;
            if (root && (key == juce::Identifier ("channels") || key == juce::Identifier ("fx")))
            {
                for (const auto& item : *value.getArray())
                {
                    bool found = false;
                    for (auto& existing : *target[key].getArray())
                        if (existing["id"].toString() == item["id"].toString()) { merge (existing, item, false); found = true; break; }
                    expect (found);
                }
            }
            else if (value.getDynamicObject() != nullptr)   // JUCE isObject() is also true for arrays
            {
                auto child = target[key]; merge (child, value, false);
            }
            else target.getDynamicObject()->setProperty (key, value.clone());
        }
    }

    void runTest() override
    {
        beginTest ("capture owns the complete public projection in UI order and does not mutate or expose private state");
        {
            Fixture f;
            const auto second = f.document.addChannel(), secondFx = f.document.addFx();
            f.document.setSessionName (juce::String::fromUTF8 ("곰 방송"));
            f.document.renameChannel (f.channel(), "Mic A"); f.document.renameChannel (second, "Mic B");
            f.document.renameFx (f.fx(), "Reverb"); f.document.renameFx (secondFx, "Delay");
            f.document.setChannelOn (f.channel(), false); f.document.setChannelMuteGroup (f.channel(), true);
            f.document.addPluginGroup (f.channel()); f.document.addPluginGroup (f.channel());
            f.document.setPluginGroupOff (f.channel(), 1, true);
            f.document.setSend (f.channel(), f.fx(), 0.35, true); f.document.setSend (f.channel(), secondFx, 0.7, false);
            f.document.setFxMuteGroup (f.fx(), true); f.document.setFxReturn (f.fx(), 0.8);
            f.groups.set (MuteGroups::Group::mic, true); f.groups.set (MuteGroups::Group::fx, true);
            const auto snapshot = f.capture (true);
            const auto& p = snapshot.projection;
            expect (snapshot.sessionId == f.document.getSessionGeneration());
            expectEquals (p.session.name, f.document.getDisplayName()); expect (p.session.dirty && p.audioRunning && p.micMuted && p.fxMuted);
            expectEquals ((int) p.channels.size(), 2); expectEquals ((int) p.fx.size(), 2);
            expect (p.channels[0].id == f.channel() && p.channels[1].id == second);
            expectEquals (p.channels[0].name, juce::String ("Mic A")); expect (! p.channels[0].on && p.channels[0].muteGroup);
            expectEquals ((int) p.channels[0].pluginGroups.size(), 2);
            expectEquals (p.channels[0].pluginGroups[0].index, 1); expectEquals (p.channels[0].pluginGroups[1].index, 2);
            expect (! p.channels[0].pluginGroups[0].off && p.channels[0].pluginGroups[1].off);
            expectEquals ((int) p.channels[0].sends.size(), 2);
            expect (p.channels[0].sends[0].fxId == f.fx() && p.channels[0].sends[1].fxId == secondFx);
            expectWithinAbsoluteError (p.channels[0].sends[0].amount, 0.35, 1.0e-12); expect (p.channels[0].sends[0].pre);
            expect (p.fx[0].id == f.fx() && p.fx[0].muteGroup); expectEquals (p.fx[0].name, juce::String ("Reverb"));
            expectWithinAbsoluteError (p.fx[0].returnAmount, 0.8, 1.0e-12);
            const auto json = P::toVar (p);
            expectEquals (json.getDynamicObject()->getProperties().size(), 5);
            expectEquals (json["channels"][0].getDynamicObject()->getProperties().size(), 6);
            expectEquals (json["fx"][0].getDynamicObject()->getProperties().size(), 4);
            expect (! json.hasProperty ("file") && ! json.hasProperty ("device") && ! json.hasProperty ("master"));
            expect (! json["channels"][0].hasProperty ("chain"));
            f.document.renameChannel (f.channel(), "Later"); f.document.removeFx (secondFx);
            expectEquals (p.channels[0].name, juce::String ("Mic A")); expectEquals ((int) p.fx.size(), 2);   // owned snapshot
            f.document.getSession().channels[0].sends.clear();
            f.document.getSession().channels[0].sends.push_back ({ juce::Uuid(), 0.9, true });   // invalid stale send, not projected
            const auto beforeValues = f.values;
            const auto missing = f.capture();
            expectEquals ((int) missing.projection.channels[0].sends.size(), 1);
            expect (missing.projection.channels[0].sends[0].fxId == f.fx());
            expectWithinAbsoluteError (missing.projection.channels[0].sends[0].amount, 0.0, 1.0e-12);
            expect (! missing.projection.channels[0].sends[0].pre);
            expectEquals ((int) f.document.getSession().channels[0].sends.size(), 1); expectEquals (f.values, beforeValues);
        }

        beginTest ("scalar deltas merge by field; pluginGroups/sends replace the complete channel array");
        {
            Fixture f;
            const auto second = f.document.addChannel(), secondFx = f.document.addFx();
            f.document.addPluginGroup (f.channel()); f.document.addPluginGroup (f.channel());
            f.document.discardUnsavedChanges();
            ControlState state (f.capture());
            const auto published = state.getCurrent();
            f.document.setSessionName ("Renamed session"); f.document.renameChannel (f.channel(), "Renamed mic");
            f.document.setChannelOn (f.channel(), false); f.document.setChannelMuteGroup (f.channel(), true);
            f.document.setPluginGroupOff (f.channel(), 1, true); f.document.setSend (f.channel(), secondFx, 0.6, true);
            f.document.renameFx (f.fx(), "Renamed FX"); f.document.setFxMuteGroup (f.fx(), true); f.document.setFxReturn (f.fx(), 0.5);
            f.groups.set (MuteGroups::Group::mic, true);
            expect (state.update (f.capture (true)));   // one completed observation of several UI edits
            const auto delta = ControlState::diff (published, state.getCurrent());
            expect (delta.kind == ControlState::ChangeKind::values); expect (! delta.needsFullState());
            expectEquals (delta.baseRevision, published.revision); expectEquals (delta.revision, published.revision + 1);
            const auto changes = P::toVar (delta.changes);
            expect (changes["session"].hasProperty ("name") && changes["session"].hasProperty ("dirty"));
            expect ((bool) changes["audio"]["running"]); expect ((bool) changes["muteGroups"]["mic"]);
            expect (! changes["muteGroups"].hasProperty ("fx"));
            expectEquals (changes["channels"].size(), 1); expectEquals (changes["fx"].size(), 1);
            expectEquals (changes["channels"][0]["pluginGroups"].size(), 2); expectEquals (changes["channels"][0]["sends"].size(), 2);
            auto client = P::toVar (published.projection);
            merge (client, changes);
            expectEquals (juce::JSON::toString (client, true), juce::JSON::toString (P::toVar (state.getCurrent().projection), true));
            expectEquals (client["channels"][1]["id"].toString(), second.toString());
            expect (! (bool) client["muteGroups"]["fx"]);
            const auto cleanBase = state.getCurrent();
            f.document.discardUnsavedChanges(); expect (state.update (f.capture (true)));
            const auto clean = P::toVar (ControlState::diff (cleanBase, state.getCurrent()).changes);
            expect (clean["session"]["dirty"].isBool() && ! (bool) clean["session"]["dirty"]);
            expect (! clean["session"].hasProperty ("name")); expect (! clean.hasProperty ("channels"));
        }

        beginTest ("same projection is silent, polls catch audio/dirty without callbacks, unrelated fields do not advance revision");
        {
            Fixture f;
            ControlState state (f.capture());
            const auto initial = state.getCurrent();
            expect (state.update (f.capture()));
            expect (ControlState::diff (initial, state.getCurrent()).kind == ControlState::ChangeKind::none);
            expectEquals (state.getCurrent().revision, initial.revision);
            expect (state.update (f.capture (true)));
            const auto audio = ControlState::diff (initial, state.getCurrent());
            expect (audio.changes.audioRunning == std::optional<bool> (true)); expect (! audio.changes.sessionDirty);
            f.document.markDirty(); expect (state.update (f.capture (true)));
            const auto dirty = state.getCurrent();
            const auto calls = f.values;
            f.document.discardUnsavedChanges(); expectEquals (f.values, calls); expect (state.update (f.capture (true)));
            const auto clean = ControlState::diff (dirty, state.getCurrent());
            expect (clean.changes.sessionDirty == std::optional<bool> (false));
            f.document.markDirty(); expect (state.update (f.capture (true)));
            const auto before = state.getCurrent();
            f.document.setChannelInput (f.channel(), 4, true); expect (state.update (f.capture (true)));
            expectEquals (state.getCurrent().revision, before.revision);   // input routing is not in the projection; already dirty
        }

        beginTest ("channel/FX identities and order, plugin group counts and transient structural edits require a full state");
        {
            Fixture f;
            f.document.markDirty();
            ControlState state (f.capture());
            const auto initial = state.getCurrent();
            const auto structureCalls = f.structures;
            f.document.addPluginGroup (f.channel()); expect (state.update (f.capture()));
            expectEquals (f.structures, structureCalls); expect (f.values > 0);
            expect (ControlState::diff (initial, state.getCurrent()).kind == ControlState::ChangeKind::structure);
            f.document.removePluginGroup (f.channel(), 0); expect (state.update (f.capture()));
            expect (initial.projection == state.getCurrent().projection);
            expect (ControlState::diff (initial, state.getCurrent()).needsFullState());   // do not lose an observed structural edit
            auto base = state.getCurrent();
            const auto second = f.document.addChannel(); expect (state.update (f.capture()));
            expect (ControlState::diff (base, state.getCurrent()).needsFullState());
            base = state.getCurrent();
            std::swap (f.document.getSession().channels[0], f.document.getSession().channels[1]); expect (state.update (f.capture()));
            expect (ControlState::diff (base, state.getCurrent()).needsFullState());
            base = state.getCurrent(); f.document.removeChannel (second); expect (state.update (f.capture()));
            expect (ControlState::diff (base, state.getCurrent()).needsFullState());
            base = state.getCurrent(); const auto secondFx = f.document.addFx(); expect (state.update (f.capture()));
            expect (ControlState::diff (base, state.getCurrent()).needsFullState());
            base = state.getCurrent(); std::swap (f.document.getSession().fx[0], f.document.getSession().fx[1]); expect (state.update (f.capture()));
            expect (ControlState::diff (base, state.getCurrent()).needsFullState());
            base = state.getCurrent(); f.document.removeFx (secondFx); expect (state.update (f.capture()));
            expect (ControlState::diff (base, state.getCurrent()).needsFullState());
            base = state.getCurrent(); f.document.applyToEngine(); expect (state.update (f.capture()));
            expect (ControlState::diff (base, state.getCurrent()).kind == ControlState::ChangeKind::none);   // structure callback is not a session change
        }

        beginTest ("two toggles inside one publish window send an empty delta with advanced revision; ack does not move the client base");
        {
            Fixture f;
            f.document.markDirty();   // both toggles can return the complete projection to its published value
            ControlState state (f.capture());
            const auto published = state.getCurrent();
            const juce::Uuid instance;
            ControlDispatcher dispatcher (f.document, f.groups, state, instance, [] { return false; });
            const P::Command first { "1", instance, published.sessionId, {}, P::ToggleChannel { f.channel() } };
            auto second = first; second.id = "2";
            const auto a = dispatcher.dispatch (first);
            expect (std::holds_alternative<P::Ack> (a));
            const auto afterFirst = state.getCurrent();
            const auto b = dispatcher.dispatch (second);
            expect (std::holds_alternative<P::Ack> (b));
            if (const auto* ack = std::get_if<P::Ack> (&a)) expectEquals (ack->context.revision, published.revision + 1);
            if (const auto* ack = std::get_if<P::Ack> (&b)) expectEquals (ack->context.revision, published.revision + 2);
            const auto delta = ControlState::diff (published, state.getCurrent());
            expect (delta.kind == ControlState::ChangeKind::values && delta.changes.empty());
            expectEquals (delta.baseRevision, published.revision); expectEquals (delta.revision, published.revision + 2);
            const auto otherClient = ControlState::diff (afterFirst, state.getCurrent());
            expectEquals (otherClient.baseRevision, published.revision + 1); expect (! otherClient.changes.empty());
            auto received = published;
            expect (received.revision == delta.baseRevision);
            mergeProjection (received, delta, state.getCurrent());
            expect (ControlState::diff (received, state.getCurrent()).kind == ControlState::ChangeKind::none);
            const P::Command same { "3", instance, received.sessionId, received.revision, P::SetChannelOn { f.channel(), true } };
            const auto noOp = dispatcher.dispatch (same);
            expect (std::holds_alternative<P::Ack> (noOp));
            if (const auto* ack = std::get_if<P::Ack> (&noOp)) expect (! ack->changed && ack->context.revision <= received.revision);
            // A client that missed a delta must ask for a full state instead of applying a mismatched base.
            expect (published.revision != otherClient.baseRevision);
            const auto requested = dispatcher.dispatch ({ "4", instance, {}, {}, P::RequestState {} });
            expect (std::holds_alternative<P::Ack> (requested));
            if (const auto* ack = std::get_if<P::Ack> (&requested))
            {
                const auto* result = std::get_if<P::RequestStateResult> (&ack->result);
                expect (result != nullptr);
                if (result != nullptr) expectEquals (result->snapshotRevision, state.getCurrent().revision);
            }
        }

        beginTest ("constructor/new/every successful reload have fresh generations, save/failed loads retain them and version remains 2");
        {
            Fixture f;
            Fixture other;
            const auto original = f.document.getSessionGeneration();
            expect (! original.isNull() && original != other.document.getSessionGeneration());
            ControlState state (f.capture());
            f.groups.set (MuteGroups::Group::mic, true);
            f.document.newSession(); expect (! f.groups.isMuted (MuteGroups::Group::mic));
            expect (original != f.document.getSessionGeneration()); expect (f.generation == f.document.getSessionGeneration());
            const auto initial = state.getCurrent(); expect (state.update (f.capture()));
            expect (ControlState::diff (initial, state.getCurrent()).kind == ControlState::ChangeKind::session);
            const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory);
            const auto dir = temp.getChildFile ("livemix_control_state_" + juce::Uuid().toString());
            expect (dir.createDirectory().wasOk());
            const auto file = dir.getChildFile ("session.livemix");
            f.document.setSessionName ("Saved session"); expect (state.update (f.capture()));
            const auto dirty = state.getCurrent();
            const auto newGeneration = f.document.getSessionGeneration();
            expect (f.document.save (file).wasOk()); expect (f.document.getSessionGeneration() == newGeneration);
            expect (state.update (f.capture()));
            expect (ControlState::diff (dirty, state.getCurrent()).changes.sessionDirty == std::optional<bool> (false));
            const auto fileJson = juce::JSON::parse (file.loadFileAsString());
            expectEquals ((int) fileJson["version"], 2);
            expect (! fileJson.hasProperty ("sessionGeneration") && ! fileJson.hasProperty ("sessionId"));
            const auto channel = f.channel();
            for (int i = 0; i < 2; ++i)
            {
                const auto before = state.getCurrent(); const auto generation = f.document.getSessionGeneration();
                expect (f.document.load (file).wasOk()); expect (f.channel() == channel);
                expect (f.document.getSessionGeneration() != generation && f.generation == f.document.getSessionGeneration());
                expect (state.update (f.capture()));
                expect (ControlState::diff (before, state.getCurrent()).kind == ControlState::ChangeKind::session);
                expectEquals (state.getCurrent().revision, before.revision + 1);
            }
            const auto good = state.getCurrent(); const auto calls = f.structures;
            expect (f.document.load (dir.getChildFile ("missing.livemix")).failed());
            const auto broken = dir.getChildFile ("broken.livemix"); expect (broken.replaceWithText ("not JSON"));
            expect (f.document.load (broken).failed());
            expect (broken.replaceWithText ("{\"version\":999}")); expect (f.document.load (broken).failed());
            expect (f.document.getSessionGeneration() == good.sessionId); expectEquals (f.structures, calls);
            expect (state.update (f.capture())); expectEquals (state.getCurrent().revision, good.revision);
            expect (f.capture().projection == good.projection);
            expect (dir.isAChildOf (temp));
            if (dir.isAChildOf (temp)) expect (dir.deleteRecursively());
        }
    }

    void mergeProjection (ControlState::Snapshot& received, const ControlState::Difference& delta, const ControlState::Snapshot& current)
    {
        auto json = P::toVar (received.projection);
        merge (json, P::toVar (delta.changes));
        expectEquals (juce::JSON::toString (json, true), juce::JSON::toString (P::toVar (current.projection), true));
        received = current;
    }
};

static ControlStateTests controlStateTests;

} // namespace gocue::tests
