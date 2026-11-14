#include "MixDocument.h"
#include "TestGainPlugin.h"
#include "PluginSet.h"

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
        beginTest ("setAuxMono is a live value edit, unchanged values are inert, and stereo returns to the containing pair");
        {
            MixEngine engine;
            MixDocument doc (engine);
            doc.applyToEngine();
            const auto mic = doc.getSession().channels[0].id, aux = doc.addAux();
            doc.setChannelInput (mic, 0, true);
            doc.setChannelOutput (mic, { false, false, 2 });
            doc.setChannelAuxSend (mic, aux, true);
            doc.setAuxOutput (aux, { false, true, 2 });
            auto* chain = engine.getAuxChain (aux);
            int values = 0, structures = 0;
            doc.onValueChanged = [&] { ++values; };
            doc.onStructureChanged = [&] { ++structures; };
            doc.discardUnsavedChanges();
            doc.setAuxMono (aux, false);
            doc.setAuxMono (juce::Uuid(), true);
            expect (! doc.isDirty());
            expectEquals (values, 0);
            juce::AudioBuffer<float> input (2, 256), output (MixSession::maxDeviceChannels, 256);
            juce::FloatVectorOperations::fill (input.getWritePointer (0), 0.8f, 256);
            juce::FloatVectorOperations::fill (input.getWritePointer (1), 0.4f, 256);
            const auto render = [&]
            {
                for (int i = 0; i < 3; ++i)
                    engine.renderBlock (input.getArrayOfReadPointers(), 2, output.getArrayOfWritePointers(), output.getNumChannels(), 256);
            };
            doc.setAuxMono (aux, true);
            expect (doc.isDirty() && doc.getSession().findAux (aux)->mono);
            expectEquals (values, 1);
            expectEquals (doc.getSession().findAux (aux)->output.directFirst, 2);
            render();
            expectWithinAbsoluteError (output.getSample (2, 255), 0.6f, 1e-6f);
            expectWithinAbsoluteError (output.getSample (3, 255), 0.0f, 1e-6f);
            doc.discardUnsavedChanges();
            doc.setAuxMono (aux, true);
            expect (! doc.isDirty());
            expectEquals (values, 1);
            doc.setAuxOutput (aux, { false, true, 3 });
            render();
            expectWithinAbsoluteError (output.getSample (3, 255), 0.6f, 1e-6f);
            doc.setAuxMono (aux, false);
            expectEquals (values, 3);
            expectEquals (doc.getSession().findAux (aux)->output.directFirst, 2);
            render();
            expectWithinAbsoluteError (output.getSample (2, 255), 0.8f, 1e-6f);
            expectWithinAbsoluteError (output.getSample (3, 255), 0.4f, 1e-6f);
            doc.setAuxMono (aux, true);
            doc.setAuxOutput (aux, { false, true, 999 });
            expectEquals (doc.getSession().findAux (aux)->output.directFirst, MixSession::maxDeviceChannels - 1);
            render();
            expectWithinAbsoluteError (output.getSample (MixSession::maxDeviceChannels - 1, 255), 0.6f, 1e-6f);
            expectWithinAbsoluteError (output.getSample (MixSession::maxDeviceChannels - 2, 255), 0.0f, 1e-6f);
            expect (engine.getAuxChain (aux) == chain);
            expectEquals (structures, 0);
            const auto file = juce::File::createTempFile (".livemix");
            expect (doc.save (file).wasOk());
            expect (doc.load (file).wasOk());
            expect (doc.getSession().findAux (aux)->mono);
            expectEquals (doc.getSession().findAux (aux)->output.directFirst, MixSession::maxDeviceChannels - 1);
            render();
            expectWithinAbsoluteError (output.getSample (MixSession::maxDeviceChannels - 1, 255), 0.6f, 1e-6f);
            doc.setAuxMono (aux, false);
            expectEquals (doc.getSession().findAux (aux)->output.directFirst, MixSession::maxDeviceChannels - 2);
            doc.setAuxOutput (aux, { false, true, 999 });
            expectEquals (doc.getSession().findAux (aux)->output.directFirst, MixSession::maxDeviceChannels - 2);
            expect (file.deleteFile());
        }

        beginTest ("aux sends, pan and output are live value edits; only add, rename and remove announce structure");
        {
            MixEngine engine;
            MixDocument doc (engine);
            doc.applyToEngine();
            const auto mic = doc.getSession().channels[0].id;
            auto* micChain = engine.getChannelChain (mic);
            micChain->addPlugin (std::make_unique<TestGainPlugin> (0.25f));
            doc.setChannelInput (mic, 2, true);
            doc.discardUnsavedChanges();
            int structures = 0, values = 0;
            doc.onStructureChanged = [&] { ++structures; expect (doc.isDirty()); };
            doc.onValueChanged = [&] { ++values; };
            const auto aux = doc.addAux();
            expect (! aux.isNull() && doc.isDirty());
            expectEquals (structures, 1);
            auto* chain = engine.getAuxChain (aux);
            chain->addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            const auto* plugin = chain->getSlot (0).plugin.get();
            doc.setChannelAuxSend (mic, aux, true);
            expectEquals (structures, 1);
            expectEquals (values, 1);
            expect (doc.getSession().findChannel (mic)->auxSends == std::vector<juce::Uuid> { aux });
            expect (engine.getChannelChain (mic) == micChain && engine.getAuxChain (aux) == chain);
            doc.discardUnsavedChanges();
            doc.setChannelAuxSend (mic, juce::Uuid(), true);
            doc.setChannelAuxSend (juce::Uuid(), aux, true);
            doc.setChannelAuxSend (mic, aux, true);
            doc.setAuxPan (juce::Uuid(), 0.5);
            doc.setAuxOutput (juce::Uuid(), { false, true, 4 });
            expect (! doc.isDirty());
            expectEquals (values, 1);
            expectEquals (structures, 1);
            doc.renameAux (aux, " Room ");
            expectEquals (doc.getSession().findAux (aux)->name, juce::String ("Room"));
            expectEquals (structures, 2);
            doc.setAuxPan (aux, -10.0);
            doc.setAuxOutput (aux, { false, true, 999 });
            expectWithinAbsoluteError (doc.getSession().findAux (aux)->pan, -1.0, 1e-9);
            expectEquals (doc.getSession().findAux (aux)->output.directFirst, MixSession::maxDeviceChannels - 2);
            expectEquals (doc.getSession().findChannel (mic)->inputFirst, 2);
            expect (doc.getSession().findChannel (mic)->stereo);
            doc.setAuxOn (aux, false);
            expect (! doc.getSession().findAux (aux)->on);
            expectEquals (values, 4);
            expectEquals (structures, 2);
            doc.setChannelAuxSend (mic, aux, false);
            expect (doc.getSession().findChannel (mic)->auxSends.empty());
            doc.setChannelAuxSend (mic, aux, true);
            doc.setChannelInput (mic, 1, true);
            expect (doc.getSession().findChannel (mic)->auxSends == std::vector<juce::Uuid> { aux });
            expectEquals (doc.getSession().findChannel (mic)->inputFirst, 1);
            expectEquals (values, 7);
            expectEquals (structures, 2);
            expect (chain->getSlot (0).plugin.get() == plugin);
            expectEquals (doc.addPluginGroup (aux), -1);
            const auto otherMic = doc.addChannel();
            doc.setChannelAuxSend (otherMic, aux, true);
            for (int i = 1; i < MixSession::maxAux; ++i)
                expect (! doc.addAux().isNull());
            doc.discardUnsavedChanges();
            expect (doc.addAux().isNull());
            expect (! doc.isDirty());
            const int before = structures;
            doc.removeAux (aux);
            expectEquals (structures, before + 1);
            expect (doc.isDirty() && doc.getSession().findAux (aux) == nullptr);
            expect (doc.getSession().findChannel (mic)->auxSends.empty());
            expect (doc.getSession().findChannel (otherMic)->auxSends.empty());
            expect (engine.getAuxChain (aux) == nullptr && engine.getChannelChain (mic) == micChain);
            doc.removeAux (aux);
            expectEquals (structures, before + 1);
        }

        beginTest ("aux chains participate in listeners, live capture, plugin-state checks, snapshot saves and session restore");
        {
            const auto file = juce::File::createTempFile (".livemix");
            MixEngine engine;
            MixDocument doc (engine);
            doc.applyToEngine();
            const auto aux = doc.addAux();
            struct Listener : PluginChain::Listener
            {
                explicit Listener (MixDocument& d) : document (d) {}
                void chainChanged (PluginChain&) override { document.markDirty(); }
                MixDocument& document;
            } listener (doc);
            const auto mic = doc.getSession().channels[0].id;
            doc.setChannelAuxSend (mic, aux, true);
            doc.setAuxPan (aux, 0.4);
            doc.setAuxOutput (aux, { false, true, 4 });
            int chains = 0;
            engine.forEachChain ([&] (PluginChain& chain) { ++chains; chain.setListener (&listener); });
            expectEquals (chains, 4);   // mic, aux, FX, master
            expect (doc.save (file).wasOk());
            auto* chain = engine.getAuxChain (aux);
            auto plugin = std::make_unique<TestGainPlugin> (0.5f);
            auto* gain = plugin.get();
            chain->addPlugin (std::move (plugin));
            expect (doc.isDirty());
            expect (doc.save (file).wasOk());
            expect (! doc.isDirty() && ! doc.checkPluginStates());
            const auto firstState = doc.getSession().findAux (aux)->chain[0].stateBase64;
            expect (firstState.isNotEmpty());
            gain->gain = 0.25f;
            gain->updateHostDisplay();
            expect (doc.pollPluginEdits() && doc.isDirty());
            expect (doc.saveIfPossible().wasOk());
            expect (! doc.isDirty());
            expect (doc.getSession().findAux (aux)->chain[0].stateBase64 != firstState);
            gain->gain = 0.125f;   // silent plugin edit, learned on editor close
            expect (doc.checkPluginState (*gain) && doc.isDirty());
            std::optional<MixSession> captured;
            expect (doc.checkPluginStates (&captured) && captured.has_value());
            const auto capturedState = captured->findAux (aux)->chain[0].stateBase64;
            const int reads = gain->stateReads;
            expect (doc.saveIfPossible (&*captured).wasOk());
            expectEquals (gain->stateReads, reads);
            expectEquals (doc.getSession().findAux (aux)->chain[0].stateBase64, capturedState);
            expect (! doc.isDirty() && ! doc.checkPluginStates());
            MixSession saved;
            expect (MixSession::load (file, saved).wasOk());
            expectEquals (saved.findAux (aux)->chain[0].stateBase64, capturedState);
            expect (saved.findChannel (mic)->auxSends == std::vector<juce::Uuid> { aux });
            expectWithinAbsoluteError (saved.findAux (aux)->pan, 0.4, 1e-9);
            expect (! saved.findAux (aux)->output.master && saved.findAux (aux)->output.direct);
            expectEquals (saved.findAux (aux)->output.directFirst, 4);
            engine.forEachChain ([] (PluginChain& c) { c.setListener (nullptr); });
            juce::StringArray errors;
            expect (doc.load (file, nullptr, &errors).wasOk());
            chain = engine.getAuxChain (aux);
            expect (chain != nullptr);
            expectEquals (chain->getNumSlots(), 1);   // unavailable test plugin is restored as a state-preserving slot
            expectEquals (chain->getSlot (0).state.stateBase64, capturedState);
            expect (doc.getSession().findChannel (mic)->auxSends == std::vector<juce::Uuid> { aux });
            expectWithinAbsoluteError (doc.getSession().findAux (aux)->pan, 0.4, 1e-9);
            expect (! doc.getSession().findAux (aux)->output.master && doc.getSession().findAux (aux)->output.direct);
            expectEquals (doc.getSession().findAux (aux)->output.directFirst, 4);
            expectEquals (errors.size(), 1);
            expect (! doc.isDirty() && ! doc.checkPluginStates());
            PluginSet set;
            set.groups = std::vector<MixPluginGroup> { { {}, true } };
            set.sends = std::vector<PluginSetSend> { { doc.getSession().fx[0].id, "", 1.0, true } };
            expect (doc.applyPluginSet (*chain, set).isEmpty());
            expectEquals (chain->getNumSlots(), 0);
            expect (doc.getSession().channels[0].pluginGroups.empty() && doc.getSession().master.pluginGroups.empty());
            expectWithinAbsoluteError (doc.getSession().channels[0].sends[0].amount, 0.0, 1e-12);
            expect (doc.isDirty());
            expect (file.deleteFile());
        }

        beginTest ("master groups bypass the live master only, respect overlapping OFF groups and survive reopening");
        {
            MixEngine engine;
            engine.prepare (48000.0, 64);
            MixDocument doc (engine);
            doc.applyToEngine();
            auto& chain = engine.getMasterChain();
            chain.addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            chain.addPlugin (std::make_unique<TestGainPlugin> (0.25f));
            const auto first = chain.getSlot (0).state.slotId, second = chain.getSlot (1).state.slotId;
            const auto mic = doc.getSession().channels[0].id;
            doc.addPluginGroup (mic);
            int values = 0;
            doc.onValueChanged = [&] { ++values; };
            expectEquals (doc.addMasterPluginGroup(), 0);
            doc.setMasterPluginGroupMember (0, first, true);
            values = 0;
            doc.setMasterPluginGroupOff (0, true);
            expectEquals (values, 1);
            expect (chain.getSlot (0).bypassed.load());
            expect (! chain.getSlot (1).bypassed.load());
            expect (! doc.getSession().channels[0].pluginGroups[0].off);
            doc.setMasterPluginGroupMember (0, second, true);
            expect (chain.getSlot (1).bypassed.load());
            doc.setMasterPluginGroupMember (0, second, false);
            expect (! chain.getSlot (1).bypassed.load());
            expectEquals (doc.addMasterPluginGroup(), 1);
            doc.setMasterPluginGroupMember (1, first, true);
            doc.setMasterPluginGroupOff (1, true);
            doc.setMasterPluginGroupOff (0, false);
            expect (chain.getSlot (0).bypassed.load());
            doc.setMasterPluginGroupOff (0, true);
            doc.removeMasterPluginGroup (0);
            expect (chain.getSlot (0).bypassed.load());
            doc.setMasterPluginGroupMember (0, first, false);
            expect (! chain.getSlot (0).bypassed.load());
            doc.setMasterPluginGroupMember (0, first, true);
            doc.removeMasterPluginGroup (0);
            expect (! chain.getSlot (0).bypassed.load());
            doc.addMasterPluginGroup();
            doc.setMasterPluginGroupMember (0, first, true);
            doc.setMasterPluginGroupMember (0, second, true);
            doc.setMasterPluginGroupOff (0, true);
            chain.removePlugin (0);
            doc.markDirty();   // the chain listener's existing edit notification
            expect (doc.getSession().master.pluginGroups[0].slots == std::vector<juce::Uuid> { first, second });
            for (int i = 1; i < MixSession::maxPluginGroups; ++i) expectEquals (doc.addMasterPluginGroup(), i);
            expectEquals (doc.addMasterPluginGroup(), -1);
            doc.setGroupOffOnEveryChannel (0, false);
            expect (doc.getSession().master.pluginGroups[0].off);
            const auto file = juce::File::createTempFile (".livemix");
            expect (doc.save (file).wasOk());
            expect (doc.getSession().master.pluginGroups[0].slots == std::vector<juce::Uuid> { first, second });
            expect (doc.load (file).wasOk());
            expect (doc.getSession().master.pluginGroups[0].slots == std::vector<juce::Uuid> { second });
            expect (doc.getSession().master.pluginGroups[0].off);
            expect (engine.getMasterChain().getSlot (0).bypassed.load());
            expectEquals ((int) doc.getSession().master.pluginGroups.size(), 5);
            expect (file.deleteFile());
        }

        beginTest ("Send to OBS is a value edit, notifies once, and persists through save and load");
        {
            MixEngine engine ("Local\\LiveMix.DocumentObsTest." + juce::Uuid().toString());
            engine.prepare (48000.0, 256);
            MixDocument document (engine);
            int values = 0, structures = 0;
            bool dirtyAtNotification = false;
            document.onValueChanged = [&]
            {
                ++values;
                dirtyAtNotification = document.isDirty();
            };
            document.onStructureChanged = [&] { ++structures; };
            expect (! document.getSession().master.sendToObs);
            document.setSendToObs (false);
            expect (! document.isDirty());
            expectEquals (values, 0);
            document.setSendToObs (true);
            expect (document.isDirty());
            expect (dirtyAtNotification);
            expect (document.getSession().master.sendToObs);
            expect (engine.getObsSender().isEnabled());
            expectEquals (values, 1);
            expectEquals (structures, 0);
            document.setSendToObs (true);
            expectEquals (values, 1);

            const auto file = juce::File::createTempFile (".livemix");
            expect (document.save (file).wasOk());
            expect (! document.isDirty());
            values = 0;
            document.setSendToObs (true);
            expectEquals (values, 0);
            expect (! document.isDirty());
            document.setSendToObs (false);
            expectEquals (values, 1);
            expect (document.isDirty());
            expect (document.load (file).wasOk());
            expect (document.getSession().master.sendToObs);
            expect (engine.getObsSender().isEnabled());
            expect (! document.isDirty());
            document.newSession();
            expect (! document.getSession().master.sendToObs);
            expect (! engine.getObsSender().isEnabled());
            expect (! document.isDirty());
            {
                const bool wasSafe = PluginHost::isSafeMode();
                PluginHost::setSafeMode (true);
                expect (document.load (file).wasOk());
                expect (! document.getSession().master.sendToObs);
                expect (! engine.getObsSender().isEnabled());
                document.getSession().master.sendToObs = true;
                document.applyToEngine();
                expect (! document.getSession().master.sendToObs);
                expect (! engine.getObsSender().isEnabled());
                PluginHost::setSafeMode (wasSafe);
            }
            expect (file.deleteFile());
        }

        beginTest ("device info compares every v4 field and batches value announcements");
        {
            MixEngine engine ("Local\\LiveMix.DocumentObsTest." + juce::Uuid().toString());
            engine.prepare (48000.0, 256);
            MixDocument document (engine);
            int values = 0, structures = 0;
            document.onValueChanged = [&] { ++values; };
            document.onStructureChanged = [&] { ++structures; };
            MixDevice device { "Windows Audio", "USB Microphone", "Headphones", 480, 48000.0 };
            document.setDeviceInfo (device);
            expect (document.isDirty());
            expectEquals (values, 1);
            expectEquals (structures, 0);
            document.discardUnsavedChanges();
            document.setDeviceInfo (device);
            expect (! document.isDirty());
            expectEquals (values, 1);

            device.output.clear();
            document.setDeviceInfo (device);
            expectEquals (values, 2);
            expect (document.isDirty());
            expect (document.getSession().device.output.isEmpty());
            device.type = "Windows Audio (Low Latency Mode)";
            document.setDeviceInfo (device);
            expectEquals (values, 3);
            device.input = "Other Microphone";
            document.setDeviceInfo (device);
            expectEquals (values, 4);
            device.bufferSize = 128;
            document.setDeviceInfo (device);
            expectEquals (values, 5);
            device.sampleRate = 44100.0;
            document.setDeviceInfo (device);
            expectEquals (values, 6);
            const auto& stored = document.getSession().device;
            expectEquals (stored.type, device.type);
            expectEquals (stored.input, device.input);
            expectEquals (stored.output, device.output);
            expectEquals (stored.bufferSize, device.bufferSize);
            expectWithinAbsoluteError (stored.sampleRate, device.sampleRate, 1.0e-9);

            {
                const MixDocument::ValueBatch batch (document);
                device.output = "New Headphones";
                document.setDeviceInfo (device);
                document.setSendToObs (true);
                expectEquals (values, 6);
            }
            expectEquals (values, 7);
            expectEquals (structures, 0);
        }

        beginTest ("a bit depth choice alone dirties the device and repeating it does not notify");
        {
            MixEngine engine;
            MixDocument document (engine);
            int values = 0;
            document.onValueChanged = [&] { ++values; };
            auto device = document.getSession().device;
            device.sampleFormat = "int24";
            document.setDeviceInfo (device);
            expect (document.isDirty());
            expectEquals (values, 1);
            expectEquals (document.getSession().device.sampleFormat, juce::String ("int24"));
            document.discardUnsavedChanges();
            document.setDeviceInfo (device);
            expect (! document.isDirty());
            expectEquals (values, 1);
        }

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

            doc.setSessionName (juce::String::fromUTF8 ("쇼 하나"));   // a name of the operator's own (not the file's)
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

            doc.setSessionName (juce::String::fromUTF8 ("쇼 둘"));   // unsaved, then the file is opened again
            juce::StringArray warnings;
            expect (doc.load (file, &warnings).wasOk());
            expect (! doc.isDirty());
            expectEquals (doc.getSession().name, juce::String::fromUTF8 ("쇼 하나"));
            expectEquals ((int) doc.getSession().channels.size(), 2);   // the blank session's channel plus the added one

            doc.newSession();
            expect (! doc.isDirty());
            expect (! doc.hasFile());
            expect (dir.deleteRecursively());
        }

        beginTest ("setting a plugin group everywhere counts matching channels and batches their chain announcements");
        {
            MixEngine engine;
            engine.prepare (48000.0, 256);
            MixDocument doc (engine);
            doc.applyToEngine();
            const auto first = doc.getSession().channels[0].id;
            const auto second = doc.addChannel();
            const auto third = doc.addChannel();
            int values = 0;
            doc.onValueChanged = [&values] { ++values; };
            doc.discardUnsavedChanges();
            expectEquals (doc.setGroupOffOnEveryChannel (0, true), 0);
            expectEquals (doc.setGroupOffOnEveryChannel (-1, true), 0);
            expectEquals (doc.setGroupOffOnEveryChannel (MixSession::maxPluginGroups, true), 0);
            expectEquals (values, 0); expect (! doc.isDirty());

            expectEquals (doc.addPluginGroup (first), 0);
            expectEquals (doc.addPluginGroup (first), 1);
            expectEquals (doc.addPluginGroup (second), 0);
            for (const auto& id : { first, second, third })
            {
                auto* chain = engine.getChannelChain (id);
                expect (chain != nullptr);
                if (chain != nullptr)
                {
                    chain->addPlugin (std::make_unique<TestGainPlugin> (0.5f));
                    if (id != third) doc.setPluginGroupMember (id, 0, chain->getSlot (0).state.slotId, true);
                }
            }
            doc.discardUnsavedChanges(); values = 0;
            expectEquals (doc.setGroupOffOnEveryChannel (0, true), 2);
            expectEquals (values, 1); expect (doc.isDirty());
            const auto& channels = doc.getSession().channels;
            expect (channels[0].pluginGroups[0].off && channels[1].pluginGroups[0].off);
            expect (! channels[0].pluginGroups[1].off); expect (channels[2].pluginGroups.empty());
            for (const auto& id : { first, second, third })
                if (const auto* chain = engine.getChannelChain (id)) expect (chain->getSlot (0).bypassed.load() == (id != third));

            expectEquals (doc.setGroupOffOnEveryChannel (0, false), 2);
            expectEquals (values, 2);
            expect (! channels[0].pluginGroups[0].off && ! channels[1].pluginGroups[0].off);
            for (const auto& id : { first, second, third })
                if (const auto* chain = engine.getChannelChain (id)) expect (! chain->getSlot (0).bypassed.load());
            expectEquals (doc.setGroupOffOnEveryChannel (1, true), 1);
            expectEquals (values, 3); expect (channels[0].pluginGroups[1].off);
            expectEquals ((int) channels[1].pluginGroups.size(), 1); expect (channels[2].pluginGroups.empty());
            expect (! channels[0].pluginGroups[0].off && ! channels[1].pluginGroups[0].off);
            doc.discardUnsavedChanges(); values = 0;
            expectEquals (doc.setGroupOffOnEveryChannel (2, true), 0);
            expectEquals (values, 0); expect (! doc.isDirty());
        }

        beginTest ("the plugin group hotkey switches that numbered group on every mic channel at once");
        {
            MixEngine engine;
            engine.prepare (48000.0, 256);
            MixDocument doc (engine);
            doc.applyToEngine();
            const auto first = doc.getSession().channels[0].id;
            const auto second = doc.addChannel();

            int values = 0;
            doc.onValueChanged = [&values] { ++values; };

            bool off = false;
            expectEquals (doc.toggleGroupOnEveryChannel (0, off), 0);   // no groups made yet: nothing to switch
            expect (! off);
            expectEquals (doc.toggleGroupOnEveryChannel (-1, off), 0);
            expectEquals (doc.toggleGroupOnEveryChannel (MixSession::maxPluginGroups, off), 0);

            expectEquals (doc.addPluginGroup (first), 0);
            expectEquals (doc.addPluginGroup (second), 0);
            values = 0;
            expectEquals (doc.toggleGroupOnEveryChannel (0, off), 2);   // both are on: the key switches them off
            expectEquals (values, 1);   // one keypress is one edit, however many channels it touched
            expect (off);
            expect (doc.getSession().channels[0].pluginGroups[0].off);
            expect (doc.getSession().channels[1].pluginGroups[0].off);

            expectEquals (doc.toggleGroupOnEveryChannel (0, off), 2);   // both off: back on
            expect (! off);
            expect (! doc.getSession().channels[0].pluginGroups[0].off);
            expect (! doc.getSession().channels[1].pluginGroups[0].off);

            // one on and one off: the key takes them all off, so the first press always does something
            doc.setPluginGroupOff (second, 0, true);
            expectEquals (doc.toggleGroupOnEveryChannel (0, off), 2);
            expect (off);
            expect (doc.getSession().channels[0].pluginGroups[0].off);
            expect (doc.getSession().channels[1].pluginGroups[0].off);

            // a value announcement made while a batch is open is the batch's one announcement, not an extra
            {
                values = 0;
                const MixDocument::ValueBatch batch (doc);
                doc.renameChannel (first, "held");
                doc.renameChannel (second, "held too");
                expectEquals (values, 0);
            }
            expectEquals (values, 1);

            // a group only one channel has: only that channel is counted and touched
            expectEquals (doc.addPluginGroup (first), 1);
            expectEquals (doc.toggleGroupOnEveryChannel (1, off), 1);
            expect (off);
            expect (doc.getSession().channels[0].pluginGroups[1].off);
            expectEquals ((int) doc.getSession().channels[1].pluginGroups.size(), 1);
        }

        beginTest ("the session goes by its file name unless the operator chose one of its own");
        {
            MixEngine engine;
            engine.prepare (48000.0, 256);
            MixDocument doc (engine);
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("livemix_name_" + juce::Uuid().toString());
            expect (dir.createDirectory().wasOk());

            // the session LiveMix makes on the first run: the file names it, nothing of the operator's is stored
            const auto first = dir.getChildFile (juce::String::fromUTF8 ("기본 세션.livemix"));
            expect (doc.save (first).wasOk());
            expectEquals (doc.getDisplayName(), juce::String::fromUTF8 ("기본 세션"));
            expect (doc.getSession().name.isEmpty());

            // "다른 이름으로 저장": the session is called what the operator saved it as
            const auto second = dir.getChildFile (juce::String::fromUTF8 ("방송용.livemix"));
            expect (doc.save (second).wasOk());
            expectEquals (doc.getDisplayName(), juce::String::fromUTF8 ("방송용"));

            // a name the operator typed is theirs: another file name does not take it away
            doc.setSessionName (juce::String::fromUTF8 ("리붕후 방송"));
            const auto third = dir.getChildFile ("show.livemix");
            expect (doc.save (third).wasOk());
            expectEquals (doc.getDisplayName(), juce::String::fromUTF8 ("리붕후 방송"));

            MixEngine other;
            other.prepare (48000.0, 256);
            MixDocument reopened (other);
            expect (reopened.load (third).wasOk());
            expectEquals (reopened.getDisplayName(), juce::String::fromUTF8 ("리붕후 방송"));   // and it comes back with the file

            // a file renamed outside LiveMix: the name on disk is the session's
            const auto renamed = dir.getChildFile (juce::String::fromUTF8 ("새 이름.livemix"));
            expect (second.copyFileTo (renamed));
            expect (reopened.load (renamed).wasOk());
            expectEquals (reopened.getDisplayName(), juce::String::fromUTF8 ("새 이름"));

            // a chosen name that happens to read like its own file is still the operator's, through a save and a reopen
            doc.setSessionName (juce::String::fromUTF8 ("방송용"));
            expect (doc.save (second).wasOk());   // 방송용.livemix, chosen name 방송용
            const auto fourth = dir.getChildFile (juce::String::fromUTF8 ("다른.livemix"));
            expect (doc.save (fourth).wasOk());
            expectEquals (doc.getDisplayName(), juce::String::fromUTF8 ("방송용"));
            expect (reopened.load (second).wasOk());
            expectEquals (reopened.getDisplayName(), juce::String::fromUTF8 ("방송용"));
            expect (reopened.getSession().nameChosen);
            expect (reopened.save (dir.getChildFile ("after.livemix")).wasOk());
            expectEquals (reopened.getDisplayName(), juce::String::fromUTF8 ("방송용"));   // and a save-as does not take it

            // a file from before 0.8.0 has no "nameChosen" at all, so the fixtures take it back out again
            auto olderFile = [this] (const juce::File& file, const juce::String& storedName)
            {
                MixSession older;
                older.name = storedName;
                older.addChannel();
                expect (older.save (file).wasOk());
                auto parsed = juce::JSON::parse (file.loadFileAsString());
                expect (parsed.getDynamicObject() != nullptr);
                parsed.getDynamicObject()->removeProperty ("nameChosen");
                expect (file.replaceWithText (juce::JSON::toString (parsed)));
                expect (! file.loadFileAsString().contains ("nameChosen"));
            };

            // one of LiveMix's own placeholder names in an older file: still not the operator's
            const auto legacy = dir.getChildFile (juce::String::fromUTF8 ("옛 세션.livemix"));
            olderFile (legacy, juce::String::fromUTF8 ("기본 세션"));
            expect (reopened.load (legacy).wasOk());
            expectEquals (reopened.getDisplayName(), juce::String::fromUTF8 ("옛 세션"));
            expect (! reopened.getSession().nameChosen);

            // an older file carrying a name only the operator could have typed: kept, and marked as theirs
            const auto legacyChosen = dir.getChildFile (juce::String::fromUTF8 ("어제 세션.livemix"));
            olderFile (legacyChosen, juce::String::fromUTF8 ("리허설 세팅"));
            expect (reopened.load (legacyChosen).wasOk());
            expectEquals (reopened.getDisplayName(), juce::String::fromUTF8 ("리허설 세팅"));
            expect (reopened.getSession().nameChosen);

            // and once saved again it says so itself, so opening it later leaves the name alone
            expect (reopened.save (legacyChosen).wasOk());
            expect (legacyChosen.loadFileAsString().contains ("nameChosen"));
            expect (reopened.load (legacyChosen).wasOk());
            expect (reopened.getSession().nameChosen);

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
            expectEquals ((int) juce::JSON::parse (copy.toJson())["version"], MixSession::currentVersion);
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
