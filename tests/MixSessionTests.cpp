#include "MixModel.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

using namespace gocue::livemix;

class MixSessionTests : public juce::UnitTest
{
public:
    MixSessionTests() : juce::UnitTest ("LiveMix session model", "LiveMix") {}

    void runTest() override
    {
        beginTest ("channels and FX are added with names, inputs and one send per FX");
        {
            MixSession s;
            expectEquals (s.addFx(), 0);
            expectEquals (s.addChannel(), 0);
            expectEquals (s.addChannel(), 1);
            expectEquals (s.channels[0].name, juce::String::fromUTF8 ("마이크 1"));
            expectEquals (s.channels[1].name, juce::String::fromUTF8 ("마이크 2"));
            expectEquals (s.channels[1].inputFirst, 1);   // the next free input
            expectEquals (s.fx[0].name, juce::String ("FX 1"));
            expectEquals ((int) s.channels[0].sends.size(), 1);
            expect (s.channels[0].sends[0].fx == s.fx[0].id);

            expectEquals (s.addFx (juce::String::fromUTF8 ("딜레이")), 1);
            expectEquals ((int) s.channels[1].sends.size(), 2);   // every channel got the new send

            s.channels[0].stereo = true;
            expectEquals (s.addChannel(), 2);
            expectEquals (s.channels[2].inputFirst, 2);   // after channel 2's mono input

            for (int i = 3; i < MixSession::maxChannels; ++i)
                expectEquals (s.addChannel(), i);

            expectEquals (s.addChannel(), -1);   // the limit

            s.removeFx (s.fx[0].id);
            expectEquals ((int) s.fx.size(), 1);

            for (const auto& c : s.channels)
                expectEquals ((int) c.sends.size(), 1);
        }

        beginTest ("sanitise: clamps, drops sends to unknown FX, adds missing sends, replaces duplicate ids");
        {
            MixSession s;
            s.addFx();
            s.addChannel();
            s.addChannel();
            s.channels[1].id = s.channels[0].id;   // a duplicate
            s.channels[0].sends.push_back ({ juce::Uuid(), 0.7, true });   // to nothing
            s.channels[0].sends[0].amount = 4.0;
            s.channels[1].sends.clear();
            s.channels[1].inputFirst = 999;
            s.fx[0].returnAmount = -1.0;
            s.master.outputFirst = 500;
            s.sanitise();

            expect (s.channels[0].id != s.channels[1].id);
            expectEquals ((int) s.channels[0].sends.size(), 1);
            expectWithinAbsoluteError (s.channels[0].sends[0].amount, 1.0, 1e-12);
            expectEquals ((int) s.channels[1].sends.size(), 1);
            expect (s.channels[1].sends[0].fx == s.fx[0].id);
            expectEquals (s.channels[1].inputFirst, MixSession::maxDeviceChannels - 1);
            expectWithinAbsoluteError (s.fx[0].returnAmount, 0.0, 1e-12);
            expectEquals (s.master.outputFirst, MixSession::maxDeviceChannels - 2);
        }

        beginTest ("JSON round trip keeps every field");
        {
            MixSession s;
            s.name = juce::String::fromUTF8 ("방송 세팅 A");
            s.device.name = "Focusrite USB ASIO";
            s.device.bufferSize = 128;
            s.device.sampleRate = 44100.0;
            s.addFx (juce::String::fromUTF8 ("리버브"));
            s.addChannel (juce::String::fromUTF8 ("곰 마이크"));
            auto& c = s.channels[0];
            c.on = false;
            c.inputFirst = 3;
            c.stereo = true;
            c.sends[0].amount = 0.35;
            c.sends[0].pre = true;
            c.output.master = false;
            c.output.direct = true;
            c.output.directFirst = 4;
            PluginSlotState slot;
            slot.name = "Test Plugin";
            slot.fileOrIdentifier = "C:/plugins/test.vst3";
            slot.uniqueId = 1234;
            slot.stateBase64 = "AAEC";
            slot.bypassed = true;
            c.chain.push_back (slot);
            s.fx[0].returnAmount = 0.5;
            s.fx[0].output.direct = true;
            s.fx[0].output.directFirst = 6;
            s.fx[0].chain.push_back (slot);
            s.master.chain.push_back (slot);
            s.master.outputFirst = 2;

            const auto json = s.toJson();
            expect (json.contains ("\"app\": \"LiveMix\"") || json.contains ("\"app\":\"LiveMix\""));

            MixSession q;
            juce::StringArray warnings;
            expect (MixSession::fromJson (json, q, &warnings).wasOk());
            expectEquals (warnings.size(), 0);
            expectEquals (q.name, s.name);
            expectEquals (q.device.name, s.device.name);
            expectEquals (q.device.bufferSize, 128);
            expectWithinAbsoluteError (q.device.sampleRate, 44100.0, 1e-9);
            expectEquals ((int) q.channels.size(), 1);
            expectEquals ((int) q.fx.size(), 1);
            expect (q.channels[0].id == c.id);
            expect (q.fx[0].id == s.fx[0].id);
            expectEquals (q.channels[0].name, c.name);
            expect (! q.channels[0].on);
            expectEquals (q.channels[0].inputFirst, 3);
            expect (q.channels[0].stereo);
            expectWithinAbsoluteError (q.channels[0].sends[0].amount, 0.35, 1e-12);
            expect (q.channels[0].sends[0].pre);
            expect (q.channels[0].sends[0].fx == s.fx[0].id);
            expect (! q.channels[0].output.master && q.channels[0].output.direct);
            expectEquals (q.channels[0].output.directFirst, 4);
            expectEquals ((int) q.channels[0].chain.size(), 1);
            expectEquals (q.channels[0].chain[0].name, juce::String ("Test Plugin"));
            expectEquals (q.channels[0].chain[0].uniqueId, 1234);
            expectEquals (q.channels[0].chain[0].stateBase64, juce::String ("AAEC"));
            expect (q.channels[0].chain[0].bypassed);
            expectWithinAbsoluteError (q.fx[0].returnAmount, 0.5, 1e-12);
            expectEquals (q.fx[0].output.directFirst, 6);
            expectEquals ((int) q.fx[0].chain.size(), 1);
            expectEquals ((int) q.master.chain.size(), 1);
            expectEquals (q.master.outputFirst, 2);
        }

        beginTest ("broken, foreign and future files are refused; save is verified and atomic");
        {
            MixSession q;
            expect (MixSession::fromJson ("", q).failed());
            expect (MixSession::fromJson ("[1,2]", q).failed());
            expect (MixSession::fromJson ("{\"version\": 1, \"channels\": []}", q).failed());   // no app marker
            expect (MixSession::fromJson ("{\"app\": \"Enqueue\", \"version\": 1}", q).failed());
            expect (MixSession::fromJson ("{\"app\": \"LiveMix\", \"version\": 99}", q).failed());
            expect (MixSession::fromJson ("{\"app\": \"LiveMix\", \"version\": 1}", q).wasOk());   // an empty session is fine
            expect (MixSession::fromJson ("{\"app\": \"LiveMix\", \"version\": 1} trailing", q).failed());   // corruption behind the object is not hidden
            expect (MixSession::fromJson ("{\"app\": \"LiveMix\", \"version\": 1}{\"x\": 1}", q).failed());

            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("livemix_session_" + juce::Uuid().toString());
            expect (dir.createDirectory().wasOk());
            const auto file = dir.getChildFile ("a.livemix");

            MixSession s;
            s.name = "one";
            s.addChannel();
            expect (s.save (file).wasOk());
            expectEquals (dir.getNumberOfChildFiles (juce::File::findFiles), 1);   // no temp file left behind

            MixSession loaded;
            expect (MixSession::load (file, loaded).wasOk());
            expectEquals (loaded.name, juce::String ("one"));
            expectEquals ((int) loaded.channels.size(), 1);

            s.name = "two";
            expect (s.save (file).wasOk());
            expect (MixSession::load (file, loaded).wasOk());
            expectEquals (loaded.name, juce::String ("two"));

            const auto blocker = dir.getChildFile ("blocked");
            expect (blocker.replaceWithText ("x"));
            expect (s.save (blocker.getChildFile ("b.livemix")).failed());   // a file where the directory should be

            dir.deleteRecursively();
        }
    }
};

static MixSessionTests mixSessionTests;

} // namespace gocue::tests
