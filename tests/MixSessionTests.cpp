#include "MixModel.h"
#include "LiveMixSettings.h"

#include <juce_core/juce_core.h>
#include <limits>

namespace gocue::tests
{

using namespace gocue::livemix;

class MixSessionTests : public juce::UnitTest
{
public:
    MixSessionTests() : juce::UnitTest ("LiveMix session model", "LiveMix") {}

    void runTest() override
    {
        beginTest ("bit depth choices survive v4 JSON and isolated last-device settings; invalid choices become automatic");
        {
            const auto directory = juce::File::createTempFile ("-bitdepth-settings");
            expect (directory.deleteFile());
            expect (directory.createDirectory().wasOk());
            for (const auto* value : { "\"\"", "\"int16\"", "\"int24\"", "\"int32\"", "\"float32\"",
                                      "\"int20\"", "24", "true", "null", "{}", "[]", "" })
            {
                const juce::String raw (value);
                const auto expected = raw == "\"int16\"" || raw == "\"int24\"" || raw == "\"int32\"" || raw == "\"float32\""
                    ? juce::JSON::fromString (raw).toString() : juce::String();
                const auto deviceJson = juce::String (R"json({"type":"Windows Audio (Exclusive Mode)","input":"Capture","output":"Headphones")json")
                    + (raw.isEmpty() ? juce::String() : ",\"sampleFormat\":" + raw) + "}";
                MixSession session;
                expect (MixSession::fromJson ("{\"app\":\"LiveMix\",\"version\":4,\"device\":" + deviceJson + "}", session).wasOk());
                expectEquals (session.device.sampleFormat, expected);
                MixSession reloaded;
                expect (MixSession::fromJson (session.toJson(), reloaded).wasOk());
                expectEquals (reloaded.device.sampleFormat, expected);
                auto saved = juce::JSON::parse (session.toJson());
                expectEquals ((int) saved["version"], 4);
                expect (saved["device"].hasProperty ("sampleFormat"));
                expectEquals (saved["device"]["sampleFormat"].toString(), expected);
                {
                    juce::PropertiesFile::Options options;
                    options.storageFormat = juce::PropertiesFile::storeAsXML;
                    juce::PropertiesFile file (directory.getChildFile ("LiveMix.settings"), options);
                    file.setValue ("lastDevice", deviceJson);
                    expect (file.save());
                }
                {
                    LiveMixSettings settings (directory);
                    const auto last = settings.getLastDevice();
                    expect (last.has_value());
                    if (last.has_value())
                    {
                        session.device = *last;
                        expectEquals (juce::JSON::parse (session.toJson())["device"]["sampleFormat"].toString(), expected);
                    }
                    settings.setLastDevice (session.device);
                    settings.saveIfNeeded();
                }
                {
                    LiveMixSettings settings (directory);
                    const auto last = settings.getLastDevice();
                    expect (last.has_value());
                    if (last.has_value())
                    {
                        session.device = *last;
                        expectEquals (juce::JSON::parse (session.toJson())["device"]["sampleFormat"].toString(), expected);
                    }
                }
            }
            expect (directory.deleteRecursively());
        }

        beginTest ("v1-v3 devices migrate to ASIO and OBS sending always starts disabled");
        for (int version : { 1, 2, 3 })
        {
            const auto legacy = "{\"app\":\"LiveMix\",\"version\":" + juce::String (version)
                + R"(,"device":{"name":"Focusrite USB ASIO","bufferSize":128,"sampleRate":44100,
                     "type":"Windows Audio","input":"ignored","output":"ignored"},"master":{"sendToObs":true}})";
            MixSession loaded;
            juce::StringArray warnings;
            expect (MixSession::fromJson (legacy, loaded, &warnings).wasOk());
            expectEquals (warnings.size(), 0);
            expectEquals (loaded.device.type, juce::String ("ASIO"));
            expect (loaded.device.isAsio());
            expectEquals (loaded.device.input, juce::String ("Focusrite USB ASIO"));
            expectEquals (loaded.device.output, loaded.device.input);
            expectEquals (loaded.device.bufferSize, 128);
            expectWithinAbsoluteError (loaded.device.sampleRate, 44100.0, 1.0e-9);
            expect (! loaded.master.sendToObs);

            const auto upgraded = juce::JSON::parse (loaded.toJson());
            expectEquals ((int) upgraded["version"], 4);
            expect (! upgraded["device"].hasProperty ("name"));
            expectEquals (upgraded["device"]["input"].toString(), loaded.device.input);
            expectEquals (upgraded["device"]["output"].toString(), loaded.device.input);
            expect (! (bool) upgraded["master"]["sendToObs"]);
        }

        beginTest ("v4 preserves Windows type, capture and render endpoints, output none, and OBS sending");
        for (const auto* type : { "Windows Audio", "Windows Audio (Low Latency Mode)", "Windows Audio (Exclusive Mode)" })
        {
            for (const auto* output : { "Headphones", "" })
            {
                for (bool send : { false, true })
                {
                    MixSession original, loaded;
                    original.device = { type, "USB Microphone", output, 480, 48000.0 };
                    original.master.sendToObs = send;
                    const auto json = original.toJson();
                    const auto root = juce::JSON::parse (json);
                    expectEquals ((int) root["version"], 4);
                    expectEquals (root["device"]["type"].toString(), original.device.type);
                    expectEquals (root["device"]["input"].toString(), original.device.input);
                    expectEquals (root["device"]["output"].toString(), original.device.output);
                    expectEquals (root["device"].getDynamicObject()->getProperties().size(), 6);
                    expect (root["master"].hasProperty ("sendToObs"));
                    expect ((bool) root["master"]["sendToObs"] == send);
                    expect (MixSession::fromJson (json, loaded).wasOk());
                    expectEquals (loaded.device.type, original.device.type);
                    expect (! loaded.device.isAsio());
                    expectEquals (loaded.device.input, original.device.input);
                    expectEquals (loaded.device.output, original.device.output);
                    expectEquals (loaded.device.bufferSize, 480);
                    expectWithinAbsoluteError (loaded.device.sampleRate, 48000.0, 1.0e-9);
                    expect (loaded.master.sendToObs == send);
                }
            }
        }

        beginTest ("device defaults and ASIO sanitisation retain one device for both directions");
        {
            MixSession session;
            expectEquals (session.device.type, juce::String ("ASIO"));
            expect (session.device.input.isEmpty() && session.device.output.isEmpty());
            expect (! session.master.sendToObs);
            session.device = { "asio", "Focusrite USB ASIO", "Another device", 128, 44100.0 };
            session.sanitise();
            expect (session.device.isAsio());
            expectEquals (session.device.output, session.device.input);

            session.device.type = "Windows Audio";
            session.device.output.clear();
            session.sanitise();
            expect (session.device.output.isEmpty());
            expect (MixSession::fromJson (R"({"app":"LiveMix","version":4,"device":{"input":"ASIO device","output":"wrong"}})", session).wasOk());
            expectEquals (session.device.type, juce::String ("ASIO"));
            expectEquals (session.device.output, juce::String ("ASIO device"));
            expect (! session.master.sendToObs);
        }

        beginTest ("v5 is refused with the newer LiveMix message and leaves the open session intact");
        {
            MixSession session;
            session.device = { "Windows Audio", "Mic", "", 480, 48000.0 };
            session.master.sendToObs = true;
            const auto before = session.toJson();
            const auto result = MixSession::fromJson (R"({"app":"LiveMix","version":5})", session);
            expect (result.failed());
            expect (result.getErrorMessage().contains (juce::String::fromUTF8 ("더 새로운 LiveMix로 저장한 세션입니다")));
            expectEquals (session.toJson(), before);
        }

        beginTest ("a file beyond the size limit is refused before it is read; a chain is capped at 16 plugins");
        {
            expect (MixSession::checkFileSize (MixSession::maxFileBytes).wasOk());
            expect (MixSession::checkFileSize (MixSession::maxFileBytes + 1).failed());
            MixSession big;
            big.addChannel ("A");

            for (int i = 0; i < 20; ++i)
                big.channels[0].chain.push_back ({});

            big.sanitise();
            expectEquals ((int) big.channels[0].chain.size(), MixSession::maxChainSlots);
        }

        beginTest ("channels and FX are added with names, inputs and one send per FX");
        {
            MixSession s;
            expectEquals (s.addFx(), 0);
            expectEquals (s.addChannel(), 0);
            expectEquals (s.addChannel(), 1);
            expectEquals (s.channels[0].name, juce::String::fromUTF8 ("마이크 1"));
            expectEquals (s.channels[1].name, juce::String::fromUTF8 ("마이크 2"));
            expectEquals (s.channels[1].inputFirst, 1);   // the next free input
            expectWithinAbsoluteError (s.channels[0].pan, 0.0, 1e-12);
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
            s.channels[0].pan = -4.0;
            s.channels[1].pan = 4.0;
            s.fx[0].returnAmount = -1.0;
            s.master.outputFirst = 500;
            s.sanitise();

            expect (s.channels[0].id != s.channels[1].id);
            expectEquals ((int) s.channels[0].sends.size(), 1);
            expectWithinAbsoluteError (s.channels[0].sends[0].amount, 1.0, 1e-12);
            expectEquals ((int) s.channels[1].sends.size(), 1);
            expect (s.channels[1].sends[0].fx == s.fx[0].id);
            expectEquals (s.channels[1].inputFirst, MixSession::maxDeviceChannels - 1);
            expectWithinAbsoluteError (s.channels[0].pan, -1.0, 1e-12);
            expectWithinAbsoluteError (s.channels[1].pan, 1.0, 1e-12);
            expectWithinAbsoluteError (s.fx[0].returnAmount, 0.0, 1e-12);
            expectEquals (s.master.outputFirst, MixSession::maxDeviceChannels - 2);

            for (const double invalid : { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
            {
                s.channels[0].pan = invalid;
                s.sanitise();
                expectWithinAbsoluteError (s.channels[0].pan, 0.0, 1e-12);
            }
        }

        beginTest ("JSON round trip keeps every field");
        {
            MixSession s;
            s.name = juce::String::fromUTF8 ("방송 세팅 A");
            s.device.input = s.device.output = "Focusrite USB ASIO";
            s.device.bufferSize = 128;
            s.device.sampleRate = 44100.0;
            s.addFx (juce::String::fromUTF8 ("리버브"));
            s.addChannel (juce::String::fromUTF8 ("곰 마이크"));
            auto& c = s.channels[0];
            c.on = false;
            c.inputFirst = 3;
            c.stereo = true;
            c.pan = -0.3;
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
            c.pluginGroups.push_back ({ { slot.slotId }, true });
            s.fx[0].returnAmount = 0.5;
            s.fx[0].mono = true;
            s.fx[0].output.direct = true;
            s.fx[0].output.directFirst = 6;
            s.fx[0].chain.push_back (slot);
            s.master.chain.push_back (slot);
            s.master.outputFirst = 2;

            const auto json = s.toJson();
            expectEquals ((int) juce::JSON::parse (json)["version"], 4);
            expectWithinAbsoluteError ((double) juce::JSON::parse (json)["channels"][0]["pan"], -0.3, 1e-12);
            expect (json.contains ("\"app\": \"LiveMix\"") || json.contains ("\"app\":\"LiveMix\""));

            MixSession q;
            juce::StringArray warnings;
            expect (MixSession::fromJson (json, q, &warnings).wasOk());
            expectEquals (warnings.size(), 0);
            expectEquals (q.name, s.name);
            expectEquals (q.device.type, s.device.type);
            expectEquals (q.device.input, s.device.input);
            expectEquals (q.device.output, s.device.output);
            expectEquals (q.device.bufferSize, 128);
            expectWithinAbsoluteError (q.device.sampleRate, 44100.0, 1e-9);
            expectEquals ((int) q.channels.size(), 1);
            expectEquals ((int) q.fx.size(), 1);
            expect (q.channels[0].id == c.id);
            expect (q.fx[0].id == s.fx[0].id);
            expect (q.fx[0].mono);
            expectEquals (q.channels[0].name, c.name);
            expect (! q.channels[0].on);
            expectEquals (q.channels[0].inputFirst, 3);
            expect (q.channels[0].stereo);
            expectWithinAbsoluteError (q.channels[0].pan, -0.3, 1e-12);
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
            expect (q.channels[0].chain[0].slotId == slot.slotId);   // the slot's own id survives the file
            expectEquals ((int) q.channels[0].pluginGroups.size(), 1);
            expect (q.channels[0].pluginGroups[0].off);
            expectEquals ((int) q.channels[0].pluginGroups[0].slots.size(), 1);
            expect (q.channels[0].pluginGroups[0].slots[0] == slot.slotId);
            expectWithinAbsoluteError (q.fx[0].returnAmount, 0.5, 1e-12);
            expectEquals (q.fx[0].output.directFirst, 6);
            expectEquals ((int) q.fx[0].chain.size(), 1);
            expectEquals ((int) q.master.chain.size(), 1);
            expectEquals (q.master.outputFirst, 2);
        }

        beginTest ("legacy sessions load at centre; v3 pan is optional and clamped; newer files are refused without replacing the session");
        {
            for (int version : { 1, 2, 3 })
            {
                MixSession s;
                const auto prefix = "{\"app\":\"LiveMix\",\"version\":" + juce::String (version);
                expect (MixSession::fromJson (prefix + ",\"channels\":[{}]}", s).wasOk());
                expectWithinAbsoluteError (s.channels[0].pan, 0.0, 1e-12);
                expect (MixSession::fromJson (prefix + ",\"channels\":[{\"pan\":-9},{\"pan\":9}]}", s).wasOk());
                expectWithinAbsoluteError (s.channels[0].pan, version >= 3 ? -1.0 : 0.0, 1e-12);
                expectWithinAbsoluteError (s.channels[1].pan, version >= 3 ? 1.0 : 0.0, 1e-12);
                const auto before = s.toJson();
                expect (MixSession::fromJson ("{\"app\":\"LiveMix\",\"version\":5}", s).failed());
                expectEquals (s.toJson(), before);
            }
        }

        beginTest ("plugin groups: at most five a channel, a member once and only of a slot the chain has; a fresh slot has an id of its own");
        {
            PluginSlotState a, b;
            a.name = "A";
            b.name = "B";
            expect (! a.slotId.isNull() && a.slotId != b.slotId);

            MixSession s;
            s.addChannel ("A");
            auto& c = s.channels[0];
            c.chain = { a, b };

            for (int i = 0; i < 7; ++i)
                c.pluginGroups.push_back ({ { a.slotId, a.slotId, juce::Uuid(), b.slotId }, i == 0 });

            MixSession q;
            expect (MixSession::fromJson (s.toJson(), q, nullptr).wasOk());
            expectEquals ((int) q.channels[0].pluginGroups.size(), MixSession::maxPluginGroups);
            expect (q.channels[0].pluginGroups[0].off);
            expectEquals ((int) q.channels[0].pluginGroups[0].slots.size(), 2);   // a once, the stranger dropped, b
            expect (q.channels[0].pluginGroups[0].slots[0] == a.slotId && q.channels[0].pluginGroups[0].slots[1] == b.slotId);

            // an older file without slot ids: every slot gets one, and they differ
            const auto older = s.toJson().replace ("\"slotId\"", "\"wasSlotId\"");
            MixSession o;
            expect (MixSession::fromJson (older, o, nullptr).wasOk());
            expect (! o.channels[0].chain[0].slotId.isNull() && o.channels[0].chain[0].slotId != o.channels[0].chain[1].slotId);
            expectEquals ((int) o.channels[0].pluginGroups[0].slots.size(), 0);   // its members named ids no slot has now
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
