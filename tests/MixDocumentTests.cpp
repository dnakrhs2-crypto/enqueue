#include "MixDocument.h"
#include "TestGainPlugin.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

using namespace gocue::livemix;

/** The document's dirty state: clean after new / load / save, dirty after any edit, so the title and the save on quit see only what changed. */
class MixDocumentTests : public juce::UnitTest
{
public:
    MixDocumentTests() : juce::UnitTest ("LiveMix document", "LiveMix") {}

    void runTest() override
    {
        beginTest ("the graph stays empty until the document applies a session (no raw mic before the saved session is in)");
        {
            MixEngine quiet;
            quiet.prepare (48000.0, 256);
            MixDocument fresh (quiet);
            expect (fresh.getSession().channels.size() == 1);
            expect (quiet.getChannelChain (fresh.getSession().channels[0].id) == nullptr);   // not in the graph yet
            fresh.applyToEngine();
            expect (quiet.getChannelChain (fresh.getSession().channels[0].id) != nullptr);
        }

        beginTest ("new, load and save leave the document clean; edits, chain edits and plugin tweaks make it dirty");
        {
            MixEngine engine;
            engine.prepare (48000.0, 256);
            MixDocument doc (engine);
            expect (! doc.isDirty());
            expect (! doc.hasFile());

            int structure = 0, value = 0;
            doc.onStructureChanged = [&structure] { ++structure; };
            doc.onValueChanged = [&value] { ++value; };

            expect (! doc.addChannel().isNull());
            expect (doc.isDirty());
            expectEquals (structure, 1);

            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("livemix_doc_" + juce::Uuid().toString());
            expect (dir.createDirectory().wasOk());
            const auto file = dir.getChildFile ("one.livemix");
            expect (doc.save (file).wasOk());
            expect (! doc.isDirty());   // saved: clean, and the views were told
            expect (doc.hasFile());
            expectEquals (value, 1);

            doc.setSessionName ("one");
            expect (doc.isDirty());
            expect (doc.saveIfPossible().wasOk());
            expect (! doc.isDirty());

            doc.markDirty (false);   // a knob turned in a plugin editor
            expect (doc.isDirty());
            const int announced = value;
            doc.markDirty (false);   // more turns: no more announcements
            expectEquals (value, announced);
            doc.markDirty();         // a chain edit always refreshes the views
            expectEquals (value, announced + 1);

            doc.setSessionName ("two");   // unsaved, then the file is opened again
            juce::StringArray warnings;
            expect (doc.load (file, &warnings).wasOk());
            expect (! doc.isDirty());
            expectEquals (doc.getSession().name, juce::String ("one"));
            expectEquals ((int) doc.getSession().channels.size(), 2);   // the blank session's channel plus the added one

            doc.newSession();
            expect (! doc.isDirty());
            expect (! doc.hasFile());
            expect (dir.deleteRecursively());
        }

        beginTest ("pan edits clamp, dirty and notify once, ignore equal values and unknown ids, and reach the engine");
        {
            MixEngine engine;
            MixDocument doc (engine);
            doc.applyToEngine();
            const auto id = doc.getSession().channels[0].id;
            int values = 0, structures = 0;
            doc.onValueChanged = [&] { ++values; };
            doc.onStructureChanged = [&] { ++structures; };
            doc.setChannelPan (id, 0.0);
            doc.setChannelPan (juce::Uuid(), 1.0);
            expect (! doc.isDirty());
            expectEquals (values, 0);

            doc.setChannelPan (id, 0.3);
            expect (doc.isDirty());
            expectEquals (values, 1);
            expectWithinAbsoluteError (doc.getSession().channels[0].pan, 0.3, 1e-12);
            doc.discardUnsavedChanges();
            doc.setChannelPan (id, 0.3);
            expect (! doc.isDirty());
            expectEquals (values, 1);

            doc.setChannelPan (id, 8.0);
            expectWithinAbsoluteError (doc.getSession().channels[0].pan, 1.0, 1e-12);
            doc.discardUnsavedChanges();
            doc.setChannelPan (id, 2.0);   // equal after clamping is also a no-op
            expect (! doc.isDirty());
            expectEquals (values, 2);
            juce::AudioBuffer<float> input (1, 256), output (2, 256);
            juce::FloatVectorOperations::fill (input.getWritePointer (0), 0.5f, 256);
            for (int i = 0; i < 3; ++i)
                engine.renderBlock (input.getArrayOfReadPointers(), 1, output.getArrayOfWritePointers(), 2, 256);
            expectWithinAbsoluteError (output.getSample (0, 255), 0.0f, 1e-6f);
            expectWithinAbsoluteError (output.getSample (1, 255), std::sqrt (0.5f), 1e-6f);

            doc.setChannelPan (id, -8.0);
            expectWithinAbsoluteError (doc.getSession().channels[0].pan, -1.0, 1e-12);
            doc.setChannelPan (id, std::numeric_limits<double>::quiet_NaN());
            expectWithinAbsoluteError (doc.getSession().channels[0].pan, 0.0, 1e-12);
            expectEquals (values, 4);
            expectEquals (structures, 0);
        }

        beginTest ("plugin groups: members switch off together, a removed plugin drops out, five groups at most, the file keeps them");
        {
            MixEngine engine;
            engine.prepare (48000.0, 256);
            MixDocument doc (engine);
            doc.newSession();
            const auto channelId = doc.getSession().channels[0].id;
            auto* chain = engine.getChannelChain (channelId);
            expect (chain != nullptr);
            chain->addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            chain->addPlugin (std::make_unique<TestGainPlugin> (0.25f));
            expectEquals (chain->getNumSlots(), 2);
            const auto first = chain->getSlot (0).state.slotId, second = chain->getSlot (1).state.slotId;
            expect (! first.isNull() && first != second);

            expectEquals (doc.addPluginGroup (channelId), 0);
            doc.setPluginGroupMember (channelId, 0, first, true);
            doc.setPluginGroupOff (channelId, 0, true);
            expect (chain->getSlot (0).bypassed.load());
            expect (! chain->getSlot (1).bypassed.load());
            expect (doc.getSession().channels[0].pluginGroups[0].off);
            doc.setPluginGroupMember (channelId, 0, second, true);    // joining an OFF group: off at once
            expect (chain->getSlot (1).bypassed.load());
            doc.setPluginGroupMember (channelId, 0, second, false);   // leaving it: back on
            expect (! chain->getSlot (1).bypassed.load());
            doc.setPluginGroupOff (channelId, 0, false);
            expect (! chain->getSlot (0).bypassed.load());

            doc.setPluginGroupOff (channelId, 0, true);
            doc.removePluginGroup (channelId, 0);   // an OFF group dropped: its plugin comes back on
            expect (! chain->getSlot (0).bypassed.load());
            expectEquals ((int) doc.getSession().channels[0].pluginGroups.size(), 0);

            expectEquals (doc.addPluginGroup (channelId), 0);
            doc.setPluginGroupMember (channelId, 0, first, true);
            chain->removePlugin (0);   // the member is gone: the group switches what is left (nothing) without complaint
            doc.setPluginGroupOff (channelId, 0, true);
            expect (! chain->getSlot (0).bypassed.load());

            // a plugin in two OFF groups runs only when both are on; a slot the chain does not have is not a member
            expectEquals (doc.addPluginGroup (channelId), 1);
            const auto remaining = chain->getSlot (0).state.slotId;
            doc.setPluginGroupMember (channelId, 0, remaining, true);
            doc.setPluginGroupMember (channelId, 1, remaining, true);
            doc.setPluginGroupMember (channelId, 1, juce::Uuid(), true);   // a stranger
            expectEquals ((int) doc.getSession().channels[0].pluginGroups[1].slots.size(), 1);
            doc.setPluginGroupOff (channelId, 0, true);
            doc.setPluginGroupOff (channelId, 1, true);
            expect (chain->getSlot (0).bypassed.load());
            doc.setPluginGroupOff (channelId, 0, false);
            expect (chain->getSlot (0).bypassed.load());      // group 1 still holds it off
            doc.setPluginGroupMember (channelId, 1, remaining, false);   // leaving the OFF group: on, group 0 is on too
            expect (! chain->getSlot (0).bypassed.load());
            doc.setPluginGroupOff (channelId, 1, false);
            doc.setPluginGroupOff (channelId, 0, true);   // off again: the file check below wants an OFF group with its member

            for (int i = 2; i < MixSession::maxPluginGroups; ++i)
                expectEquals (doc.addPluginGroup (channelId), i);

            expectEquals (doc.addPluginGroup (channelId), -1);
            expect (doc.isDirty());

            // the session written and read back keeps the groups and the slots' ids (the chain captured live)
            MixSession copy = doc.getSession();
            expect (engine.captureLivePluginStates (copy));
            MixSession back;
            expect (MixSession::fromJson (copy.toJson(), back, nullptr).wasOk());
            expectEquals ((int) back.channels[0].pluginGroups.size(), MixSession::maxPluginGroups);
            expect (back.channels[0].chain[0].slotId == chain->getSlot (0).state.slotId);
            expect (back.channels[0].pluginGroups[0].off);
            expectEquals ((int) back.channels[0].pluginGroups[0].slots.size(), 1);   // the removed member is not in the file, the remaining one is
            expect (copy.toJson().contains ("\"version\": 3"));   // older LiveMix refuses it instead of losing the saved pan
        }

        beginTest ("a plugin's own state change is picked up on demand and settled by a save");
        {
            MixEngine engine;
            engine.prepare (48000.0, 256);
            MixDocument doc (engine);
            doc.applyToEngine();   // the constructor leaves the graph empty (see the first test)
            auto* chain = engine.getChannelChain (doc.getSession().channels[0].id);
            expect (chain != nullptr);
            auto* plugin = new TestGainPlugin (0.5f);
            chain->addPlugin (std::unique_ptr<juce::AudioPluginInstance> (plugin));
            expect (! doc.pollPluginEdits());   // adding is a chain edit (markDirty by the UI), not a plugin state change

            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("livemix_doc2_" + juce::Uuid().toString());
            expect (dir.createDirectory().wasOk());
            expect (doc.save (dir.getChildFile ("p.livemix")).wasOk());
            expect (! doc.isDirty());

            plugin->updateHostDisplay();      // what a knob turned in the editor does
            expect (doc.pollPluginEdits());   // asked before the timer got there: dirty now
            expect (doc.isDirty());
            expect (! doc.pollPluginEdits());

            plugin->updateHostDisplay();
            expect (doc.saveIfPossible().wasOk());   // the save captured that state
            expect (! doc.isDirty());
            expect (! doc.pollPluginEdits());        // and settled the flag: the next tick does not dirty a saved file
            expect (! doc.isDirty());

            plugin->updateHostDisplay();
            expect (doc.save (dir).failed());        // a write that cannot succeed (the target is a directory)
            expect (doc.isDirty());                  // keeps the edit on record
            expect (dir.deleteRecursively());
        }
    }
};

static MixDocumentTests mixDocumentTests;

} // namespace gocue::tests
