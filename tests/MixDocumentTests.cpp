#include "MixDocument.h"
#include "TestGainPlugin.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

using namespace gocue::livemix;

/** The document's dirty state: clean after new / load / save, dirty after any edit, so autosave writes only what changed. */
class MixDocumentTests : public juce::UnitTest
{
public:
    MixDocumentTests() : juce::UnitTest ("LiveMix document", "LiveMix") {}

    void runTest() override
    {
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

        beginTest ("a plugin's own state change is picked up on demand and settled by a save");
        {
            MixEngine engine;
            engine.prepare (48000.0, 256);
            MixDocument doc (engine);
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
            expect (dir.deleteRecursively());
        }
    }
};

static MixDocumentTests mixDocumentTests;

} // namespace gocue::tests
