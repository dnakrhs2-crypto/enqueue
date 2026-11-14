#include "MixModel.h"
#include "LiveMixSettings.h"
#include "Localisation.h"

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
        beginTest ("aux mono round trips, missing mono stays stereo, and mono direct output keeps the last device channel");
        {
            MixSession s;
            s.addAux ("Mono");
            s.addAux ("Stereo");
            expect (! s.auxes[0].mono && ! s.auxes[1].mono);
            s.auxes[0].mono = true;
            s.auxes[0].output = { false, true, MixSession::maxDeviceChannels - 1 };
            s.auxes[1].output = { false, true, MixSession::maxDeviceChannels - 1 };
            MixSession loaded;
            expect (MixSession::fromJson (s.toJson(), loaded).wasOk());
            expect (loaded.auxes[0].mono && ! loaded.auxes[1].mono);
            expectEquals (loaded.auxes[0].output.directFirst, MixSession::maxDeviceChannels - 1);
            expectEquals (loaded.auxes[1].output.directFirst, MixSession::maxDeviceChannels - 2);
            expect (! loaded.auxes[0].output.master && loaded.auxes[0].output.direct);
            const auto json = juce::JSON::parse (loaded.toJson());
            expect ((bool) json["auxes"][0]["mono"] && ! (bool) json["auxes"][1]["mono"]);
            expectEquals ((int) json["version"], 4);
            for (const bool legacyInput : { false, true })
            {
                auto legacy = json.clone();
                for (auto& aux : *legacy["auxes"].getArray())
                {
                    aux.getDynamicObject()->removeProperty ("mono");
                    if (legacyInput)
                        aux.getDynamicObject()->setProperty ("input", juce::JSON::parse (R"({"first":3,"stereo":false})"));
                }
                expect (MixSession::fromJson (juce::JSON::toString (legacy), loaded).wasOk());
                expect (! loaded.auxes[0].mono && ! loaded.auxes[1].mono);
                expectEquals (loaded.auxes[0].output.directFirst, MixSession::maxDeviceChannels - 2);
            }
            s.auxes[0].output.directFirst = 999;
            s.auxes[1].output.directFirst = -1;
            s.sanitise();
            expectEquals (s.auxes[0].output.directFirst, MixSession::maxDeviceChannels - 1);
            expectEquals (s.auxes[1].output.directFirst, 0);
        }

        beginTest ("aux defaults, free names, limits and removal of sends");
        {
            MixSession s;
            expectEquals (s.addAux(), 0);
            expectEquals (s.auxes[0].name, trf ("억스 {0}", 1));
            expect (s.auxes[0].on && s.auxes[0].output.master && ! s.auxes[0].output.direct);
            expectWithinAbsoluteError (s.auxes[0].pan, 0.0, 1e-9);
            s.addChannel();
            s.addChannel();
            expectEquals (s.addAux(), 1);
            const auto removed = s.auxes[0].id, kept = s.auxes[1].id;
            s.channels[0].auxSends = { removed, kept };
            s.channels[1].auxSends = { removed };
            s.removeAux (s.auxes[0].id);
            expect (s.findAux (removed) == nullptr);
            expect (s.channels[0].auxSends == std::vector<juce::Uuid> { kept });
            expect (s.channels[1].auxSends.empty());
            expectEquals (s.addAux(), 1);
            expectEquals (s.auxes[1].name, trf ("억스 {0}", 3));   // second place, but number 2 is still there
            expectEquals (s.addAux ("Room"), 2);
            expectEquals (s.auxes[2].name, juce::String ("Room"));
            expectEquals (s.addAux(), 3);
            expectEquals (s.addAux(), -1);

            MixSession renamed;
            renamed.addAux();
            renamed.auxes[0].name = "Talk";
            expectEquals (renamed.addAux(), 1);
            expectEquals (renamed.auxes[1].name, trf ("억스 {0}", 2));
        }

        beginTest ("aux JSON round trip keeps sends, chains, balance and outputs without changing v4");
        {
            MixSession s;
            s.addChannel();
            s.addChannel();
            s.addAux ("Room");
            s.addAux ("Hall");
            auto& a = s.auxes[0];
            a.on = false;
            a.pan = -0.5;
            a.output = { false, true, 6 };
            PluginSlotState slot;
            slot.name = "Aux plugin";
            slot.fileOrIdentifier = "test://aux";
            slot.stateBase64 = "AAEC";
            slot.bypassed = true;
            a.chain = { slot };
            s.channels[0].inputFirst = 5;
            s.channels[0].stereo = true;
            s.channels[0].auxSends = { a.id, s.auxes[1].id };
            s.sanitise();
            const auto json = juce::JSON::parse (s.toJson());
            expectEquals ((int) json["version"], 4);
            expectEquals ((int) json["channels"][0]["input"]["first"], 5);
            expect ((bool) json["channels"][0]["input"]["stereo"]);
            expect (! json["channels"][0]["input"].hasProperty ("aux"));
            expect (! json["auxes"][0].hasProperty ("input"));
            expectEquals (json["channels"][0]["auxSends"].size(), 2);
            MixSession loaded;
            expect (MixSession::fromJson (s.toJson(), loaded).wasOk());
            expectEquals ((int) loaded.auxes.size(), 2);
            const auto& back = loaded.auxes[0];
            expect (back.id == a.id && loaded.channels[0].auxSends == s.channels[0].auxSends);
            expectEquals (back.name, a.name);
            expect (! back.on && ! back.output.master && back.output.direct);
            expectEquals (back.output.directFirst, 6);
            expectWithinAbsoluteError (back.pan, -0.5, 1e-9);
            expectEquals ((int) back.chain.size(), 1);
            expect (back.chain[0].slotId == slot.slotId && back.chain[0].bypassed);
            expectEquals (back.chain[0].stateBase64, slot.stateBase64);
            expectEquals (back.chain[0].fileOrIdentifier, slot.fileOrIdentifier);
            expect (loaded.channels[1].auxSends.empty());
        }

        beginTest ("0.24.0 aux input and mic input.aux are ignored; copied device input, aux name, switch and chain survive");
        {
            const juce::String legacy = R"json({
                "app":"LiveMix","version":4,
                "channels":[{"id":"11111111111111111111111111111111","name":"Mic",
                    "input":{"first":5,"stereo":true,"aux":"22222222222222222222222222222222"}}],
                "auxes":[{"id":"22222222222222222222222222222222","name":"Input","on":false,
                    "input":{"first":1,"stereo":false},
                    "chain":[{"name":"Aux plugin","fileOrIdentifier":"test://aux","state":"AAEC","bypassed":true}]}]
            })json";
            MixSession loaded;
            expect (MixSession::fromJson (legacy, loaded).wasOk());
            expectEquals (loaded.channels[0].inputFirst, 5);
            expect (loaded.channels[0].stereo && loaded.channels[0].auxSends.empty());
            expectEquals (loaded.auxes[0].name, juce::String ("Input"));
            expect (! loaded.auxes[0].on);
            expectEquals ((int) loaded.auxes[0].chain.size(), 1);
            expectEquals (loaded.auxes[0].chain[0].name, juce::String ("Aux plugin"));
            expectEquals (loaded.auxes[0].chain[0].stateBase64, juce::String ("AAEC"));
            expect (loaded.auxes[0].chain[0].bypassed);
            expect (loaded.auxes[0].output.master && ! loaded.auxes[0].output.direct);
            expectWithinAbsoluteError (loaded.auxes[0].pan, 0.0, 1e-9);
            loaded.sanitise();
            expectEquals (loaded.channels[0].inputFirst, 5);
            expect (loaded.channels[0].stereo && loaded.channels[0].auxSends.empty());
            expect (MixSession::fromJson (R"({"app":"LiveMix","version":4,"channels":[{"input":{"first":3}}]})", loaded).wasOk());
            expect (loaded.auxes.empty() && loaded.channels[0].auxSends.empty());
            expectEquals (loaded.channels[0].inputFirst, 3);
        }

        beginTest ("aux sanitise caps counts and chains, repairs shared ids, clamps outputs and removes missing or duplicate sends");
        {
            MixSession s;
            s.addChannel();
            s.addChannel();
            s.addFx();
            s.auxes.resize (MixSession::maxAux + 2);
            s.auxes[0].id = s.channels[0].id;
            s.auxes[1].id = s.fx[0].id;
            s.auxes[2].id = s.auxes[3].id;
            s.auxes[0].pan = -10.0;
            s.auxes[1].pan = 10.0;
            s.auxes[0].output.directFirst = -10;
            s.auxes[1].output.directFirst = 100;
            s.auxes[3].chain.resize (MixSession::maxChainSlots + 1);
            const auto missing = s.auxes.back().id;
            s.channels[0].auxSends = { missing, juce::Uuid::null(), juce::Uuid(), s.auxes[2].id, s.auxes[2].id };
            MixSession loaded;
            juce::StringArray warnings;
            expect (MixSession::fromJson (s.toJson(), loaded, &warnings).wasOk());
            expectEquals ((int) loaded.auxes.size(), MixSession::maxAux);
            expectEquals (warnings.size(), 2);
            expectEquals ((int) loaded.auxes[3].chain.size(), MixSession::maxChainSlots);
            expectWithinAbsoluteError (loaded.auxes[0].pan, -1.0, 1e-9);
            expectWithinAbsoluteError (loaded.auxes[1].pan, 1.0, 1e-9);
            expectEquals (loaded.auxes[0].output.directFirst, 0);
            expectEquals (loaded.auxes[1].output.directFirst, MixSession::maxDeviceChannels - 2);
            expect (loaded.channels[0].auxSends == std::vector<juce::Uuid> { loaded.auxes[2].id });
            for (size_t i = 0; i < loaded.auxes.size(); ++i)
            {
                expect (! loaded.auxes[i].id.isNull());
                expect (loaded.findChannel (loaded.auxes[i].id) == nullptr && loaded.findFx (loaded.auxes[i].id) == nullptr);
                for (size_t j = 0; j < i; ++j)
                    expect (loaded.auxes[i].id != loaded.auxes[j].id);
            }
            loaded.auxes[0].id = juce::Uuid::null();
            loaded.auxes[1].pan = std::numeric_limits<double>::quiet_NaN();
            loaded.auxes[2].pan = std::numeric_limits<double>::infinity();
            loaded.sanitise();
            expect (! loaded.auxes[0].id.isNull());
            expectWithinAbsoluteError (loaded.auxes[1].pan, 0.0, 1e-9);
            expectWithinAbsoluteError (loaded.auxes[2].pan, 0.0, 1e-9);
        }

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

        beginTest ("master groups survive JSON, cap at five and clean members without changing the file version");
        {
            MixSession source;
            source.addChannel();
            PluginSlotState slot;
            source.master.chain = source.channels[0].chain = { slot };
            for (int i = 0; i < 7; ++i)
                source.channels[0].pluginGroups.push_back ({ { slot.slotId, slot.slotId, juce::Uuid() }, true });
            auto json = juce::JSON::parse (source.toJson());
            json["master"].getDynamicObject()->setProperty ("pluginGroups", json["channels"][0]["pluginGroups"]);
            MixSession loaded;
            expect (MixSession::fromJson (juce::JSON::toString (json), loaded).wasOk());
            const auto saved = juce::JSON::parse (loaded.toJson());
            const auto groups = saved["master"]["pluginGroups"];
            expectEquals (groups.size(), MixSession::maxPluginGroups);
            if (groups.size() > 0)
            {
                expect ((bool) groups[0]["off"]);
                expectEquals (groups[0]["slots"].size(), 1);
                if (groups[0]["slots"].size() > 0)
                    expectEquals (groups[0]["slots"][0].toString(), slot.slotId.toString());
            }
            expectEquals ((int) saved["version"], 4);
            json["master"].getDynamicObject()->removeProperty ("pluginGroups");
            expect (MixSession::fromJson (juce::JSON::toString (json), loaded).wasOk());
            expectEquals (juce::JSON::parse (loaded.toJson())["master"]["pluginGroups"].size(), 0);
            expectEquals ((int) loaded.master.chain.size(), 1);
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
