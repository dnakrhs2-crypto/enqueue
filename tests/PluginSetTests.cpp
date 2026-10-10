#include "PluginSet.h"
#include "MixDocument.h"
#include "TestGainPlugin.h"

#include <algorithm>

namespace gocue::tests
{

using namespace gocue::livemix;

namespace
{
    juce::String k (const char* text) { return juce::String::fromUTF8 (text); }

    struct SetTestFolder
    {
        const juce::File temp = juce::File::getSpecialLocation (juce::File::tempDirectory);
        const juce::File root = temp.getChildFile ("LiveMixPluginSetTests_" + juce::Uuid().toString());
        ~SetTestFolder()
        {
            if (root.getParentDirectory() == temp && root.getFileName().startsWith ("LiveMixPluginSetTests_"))
                root.deleteRecursively();
        }
    };

    PluginSet sampleSet()
    {
        PluginSet set;
        set.number = 2;
        set.source = k ("보컬 마이크");
        set.savedAt = "2026-09-28T12:34:56Z";
        for (const auto* name : { "EQ", "Comp", "Gate" })
        {
            PluginSlotState state;
            state.name = name;
            state.format = "PluginSetTestMissing";   // never scans/opens a real plugin or touches a plugin cache
            state.fileOrIdentifier = "set-test://" + state.name;
            state.uniqueId = 73 + (int) set.plugins.size();
            state.stateBase64 = "AAECAw==";
            state.descriptionXml = "<test/>";
            set.plugins.push_back (state);
        }
        set.plugins[1].bypassed = true;
        return set;
    }

    MixFx fxNamed (const juce::String& name)
    {
        MixFx fx;
        fx.name = name;
        return fx;
    }

    struct DirtyChainListener : PluginChain::Listener
    {
        explicit DirtyChainListener (MixDocument& d) : document (d) {}
        void chainChanged (PluginChain&) override { ++calls; document.markDirty(); }
        MixDocument& document;
        int calls = 0;
    };
}

class PluginSetTests : public juce::UnitTest
{
public:
    PluginSetTests() : juce::UnitTest ("LiveMix plugin sets", "LiveMix") {}

    void runTest() override
    {
        beginTest ("JSON round trips plugins alone, sends, groups, and both without losing any slot data");
        for (int flags = 0; flags < 4; ++flags)
        {
            auto set = sampleSet();
            if ((flags & 1) != 0)
                set.sends = std::vector<PluginSetSend> { { juce::Uuid(), k ("리버브"), 0.625, true }, { juce::Uuid(), "Delay", 0.0, false } };
            if ((flags & 2) != 0)
                set.groups = std::vector<MixPluginGroup> { { { set.plugins[0].slotId, set.plugins[2].slotId }, true }, { {}, false } };

            const auto json = set.toJson();
            const auto root = juce::JSON::parse (json);
            expectEquals (root["app"].toString(), juce::String ("LiveMix"));
            expectEquals (root["kind"].toString(), juce::String ("pluginSet"));
            expectEquals ((int) root["version"], 1);
            expect (root.hasProperty ("sends") == ((flags & 1) != 0));
            expect (root.hasProperty ("groups") == ((flags & 2) != 0));
            PluginSet back;
            const auto result = PluginSet::fromJson (json, back);
            expect (result.wasOk(), result.getErrorMessage());
            expectEquals (back.toJson(), json);
            expectEquals (back.source, set.source);
            expectEquals (back.savedAt, set.savedAt);
            expectEquals ((int) back.plugins.size(), 3);
            if (back.plugins.size() == 3)
                for (size_t i = 0; i < set.plugins.size(); ++i)
                {
                    expect (back.plugins[i].slotId == set.plugins[i].slotId);
                    expect (back.plugins[i].bypassed == set.plugins[i].bypassed);
                    expectEquals (back.plugins[i].name, set.plugins[i].name);
                    expectEquals (back.plugins[i].stateBase64, set.plugins[i].stateBase64);
                    expectEquals (back.plugins[i].uniqueId, set.plugins[i].uniqueId);
                }
        }

        beginTest ("foreign app/kind, future or malformed versions, malformed JSON and plugin records are refused");
        {
            const auto json = sampleSet().toJson();
            PluginSet unchanged;
            unchanged.number = 5;
            auto rejects = [&] (const juce::String& text)
            {
                expect (PluginSet::fromJson (text, unchanged).failed());
                expectEquals (unchanged.number, 5, "failed reads leave the caller's set intact");
            };
            for (const auto* text : { "", "not JSON", "[]", "{", "{\"app\":\"LiveMix\"}" })
                rejects (text);
            rejects (json + " trailing");
            rejects (json + " {}");
            const auto badProperty = [&] (const juce::Identifier& key, const juce::var& value)
            {
                auto root = juce::JSON::parse (json);
                root.getDynamicObject()->setProperty (key, value);
                rejects (juce::JSON::toString (root));
            };
            badProperty ("app", "Enqueue");
            badProperty ("kind", "pluginPreset");
            for (const auto& version : { juce::var (2), juce::var (0), juce::var (1.5), juce::var ("1"), juce::var() })
                badProperty ("version", version);
            for (const auto& number : { juce::var (0), juce::var (6), juce::var (2.5), juce::var ("2") })
                badProperty ("number", number);
            badProperty ("source", 1);
            badProperty ("savedAt", juce::var());
            badProperty ("plugins", juce::var());
            badProperty ("plugins", juce::Array<juce::var> { juce::var (3) });

            auto set = sampleSet();
            set.plugins[0].stateBase64 = "@@@@";
            rejects (set.toJson());
            set = sampleSet();
            set.plugins[1].slotId = set.plugins[0].slotId;
            rejects (set.toJson());
            set.plugins[1].slotId = juce::Uuid::null();
            rejects (set.toJson());
            auto root = juce::JSON::parse (json);
            root.getDynamicObject()->setProperty ("version", 1.0);
            expect (PluginSet::fromJson (juce::JSON::toString (root), unchanged).wasOk());
        }

        beginTest ("optional metadata has a strict shape, with absent and empty groups kept distinct");
        {
            auto set = sampleSet();
            set.sends.emplace();
            set.groups.emplace();
            PluginSet back;
            expect (PluginSet::fromJson (set.toJson(), back).wasOk());
            expect (back.sends.has_value() && back.sends->empty());
            expect (back.groups.has_value() && back.groups->empty());
            const auto json = set.toJson();
            for (const auto* field : { "sends", "groups" })
            {
                auto root = juce::JSON::parse (json);
                root.getDynamicObject()->setProperty (field, juce::var());
                expect (PluginSet::fromJson (juce::JSON::toString (root), back).failed());
                root.getDynamicObject()->setProperty (field, juce::Array<juce::var> { juce::var (false) });
                expect (PluginSet::fromJson (juce::JSON::toString (root), back).failed());
            }
            set.groups->resize (MixSession::maxPluginGroups);
            expect (PluginSet::fromJson (set.toJson(), back).wasOk());
            set.groups->push_back ({});
            expect (PluginSet::fromJson (set.toJson(), back).failed());
            set.groups->resize (1);
            set.groups->front().slots = { juce::Uuid::null() };
            expect (PluginSet::fromJson (set.toJson(), back).failed());
            set.groups.reset();
            set.sends->push_back ({ juce::Uuid(), "FX", 1.1, false });
            expect (PluginSet::fromJson (set.toJson(), back).failed());
            set.sends->front().amount = -0.1;
            expect (PluginSet::fromJson (set.toJson(), back).failed());
            set.sends->front().amount = 0.0;
            expect (PluginSet::fromJson (set.toJson(), back).wasOk());
            set.sends->push_back (set.sends->front());
            expect (PluginSet::fromJson (set.toJson(), back).failed());
        }

        beginTest ("16 plugins and 8 MB are inclusive limits; failed saves preserve the previous file");
        {
            SetTestFolder temp;
            const auto file = PluginSet::fileFor (2, PluginSet::folderIn (temp.root));
            auto set = sampleSet();
            PluginSet back;
            while ((int) set.plugins.size() < PluginSet::maxPlugins)
                set.plugins.emplace_back();
            expect (set.save (file).wasOk());
            expect (PluginSet::load (file, back).wasOk());
            expectEquals ((int) back.plugins.size(), 16);
            const auto previous = file.loadFileAsString();
            set.plugins.emplace_back();
            expect (PluginSet::fromJson (set.toJson(), back).failed());
            expect (set.save (file).failed());
            expectEquals (file.loadFileAsString(), previous);

            set = sampleSet();
            set.source.clear();
            const auto padding = (int) (PluginSet::maxFileBytes - (juce::int64) set.toJson().getNumBytesAsUTF8());
            set.source = juce::String::repeatedString ("x", padding);
            expectEquals ((juce::int64) set.toJson().getNumBytesAsUTF8(), PluginSet::maxFileBytes);
            expect (set.save (file).wasOk());
            expect (PluginSet::load (file, back).wasOk());
            expectEquals (file.getSize(), PluginSet::maxFileBytes);
            set.source += "x";
            expect (PluginSet::fromJson (set.toJson(), back).failed());
            const auto refused = set.save (file);
            expect (refused.failed());
            expect (refused.getErrorMessage().contains (k ("세트가 너무 커서 저장하지 않았습니다")));
            expectEquals (file.getSize(), PluginSet::maxFileBytes);
            expect (PluginSet::load (file, back).wasOk());
            expectEquals (back.source.length(), padding);
            const auto oversized = temp.root.getChildFile ("oversized.livemixset");
            expect (oversized.replaceWithText (set.toJson()));
            expect (PluginSet::load (oversized, back).failed());
        }

        beginTest ("temporary documents path, exact five filenames, missing/broken slots, and atomic overwrite");
        {
            SetTestFolder temp;
            const auto folder = PluginSet::folderIn (temp.root);
            expect (folder == temp.root.getChildFile ("LiveMix").getChildFile (k ("플러그인 세트")));
            for (int n = 1; n <= PluginSet::numSets; ++n)
                expectEquals (PluginSet::fileFor (n, folder).getFileName(), k ("세트 ") + juce::String (n) + ".livemixset");
            const auto empty = listPluginSets (folder);
            expectEquals ((int) empty.size(), 5);
            for (int i = 0; i < 5; ++i)
            {
                expect (empty[(size_t) i].state == PluginSetEntry::State::empty);
                expectEquals (empty[(size_t) i].set.number, i + 1);
            }
            expect (! folder.exists(), "reading the menu never creates folders");
            auto set = sampleSet();
            set.number = 1;
            expect (set.save (PluginSet::fileFor (1, folder)).wasOk());
            expect (PluginSet::fileFor (2, folder).replaceWithText ("broken"));
            expect (set.save (PluginSet::fileFor (3, folder)).wasOk());   // mismatched embedded slot number
            expect (PluginSet::fileFor (4, folder).createDirectory().wasOk());
            const auto listed = listPluginSets (folder);
            expect (listed[0].state == PluginSetEntry::State::ready);
            for (int i = 1; i < 4; ++i)
            {
                expect (listed[(size_t) i].state == PluginSetEntry::State::unreadable);
                expect (listed[(size_t) i].error.isNotEmpty());
            }
            expect (listed[4].state == PluginSetEntry::State::empty);
            expectEquals (listed[0].menuText (false), k ("세트 1 — EQ → Comp → Gate (3개)"));
            expectEquals (listed[0].menuText (true), k ("세트 1 — EQ → Comp → Gate (3개) (덮어쓰기)"));
            expectEquals (listed[1].menuText (false), k ("세트 2 — 읽을 수 없는 파일"));
            expectEquals (listed[1].menuText (true), k ("세트 2 — 읽을 수 없는 파일 (덮어쓰기)"));
            expectEquals (listed[4].menuText (true), k ("세트 5 — 비어 있음"));
            set.number = 2;
            expect (set.save (PluginSet::fileFor (2, folder)).wasOk());
            expect (listPluginSets (folder)[1].state == PluginSetEntry::State::ready);
            expect (set.save (PluginSet::fileFor (4, folder)).failed());
            expect (PluginSet::fileFor (4, folder).isDirectory());
            PluginSet missing;
            expect (PluginSet::load (PluginSet::fileFor (5, folder), missing).failed());
        }

        beginTest ("summary truncates only plugin names, retaining count and all metadata indicators");
        {
            auto set = sampleSet();
            expectEquals (set.summary(), k ("EQ → Comp → Gate (3개)"));
            set.sends.emplace();
            expectEquals (set.summary(), k ("EQ → Comp → Gate (3개) · 센드"));
            set.groups.emplace();
            expectEquals (set.summary(), k ("EQ → Comp → Gate (3개) · 센드·그룹"));
            set.sends.reset();
            expectEquals (set.summary(), k ("EQ → Comp → Gate (3개) · 그룹"));
            set.plugins[0].name = juce::String::repeatedString (k ("한"), 60);
            expectEquals (set.summary(), juce::String::repeatedString (k ("한"), 48) + k ("… (3개) · 그룹"));
        }

        beginTest ("capture releases only OFF-group bypasses when groups are omitted, includes all sends even at zero");
        {
            auto sample = sampleSet();
            sample.plugins[0].bypassed = true;
            MixChannel channel;
            channel.pluginGroups = { { { sample.plugins[0].slotId }, true }, { { sample.plugins[1].slotId }, false } };
            const std::vector<MixFx> fx { fxNamed (k ("리버브")), fxNamed ("Delay"), fxNamed ("Chorus") };
            channel.sends = { { fx[0].id, 0.75, true }, { fx[1].id, 0.0, true } };
            const auto without = capturePluginSet (3, sample.source, sample.savedAt, sample.plugins, &channel, fx, true, false);
            expect (! without.groups);
            expect (! without.plugins[0].bypassed);
            expect (without.plugins[1].bypassed);
            expect (! without.plugins[2].bypassed);
            expect (sample.plugins[0].bypassed, "pure capture does not modify its input");
            expectEquals ((int) without.sends->size(), 3);
            expect ((*without.sends)[0].fx == fx[0].id && (*without.sends)[0].pre);
            expectEquals ((*without.sends)[0].name, fx[0].name);
            expectWithinAbsoluteError ((*without.sends)[0].amount, 0.75, 1e-12);
            expect ((*without.sends)[1].amount == 0.0 && (*without.sends)[1].pre);
            expect ((*without.sends)[2].amount == 0.0 && ! (*without.sends)[2].pre);
            const auto with = capturePluginSet (4, sample.source, sample.savedAt, sample.plugins, &channel, fx, false, true);
            expect (! with.sends);
            expect (with.plugins[0].bypassed && with.plugins[1].bypassed);
            expect (with.groups->front().off && with.groups->front().slots == channel.pluginGroups[0].slots);
            const auto bus = capturePluginSet (5, "FX", sample.savedAt, sample.plugins, nullptr, fx, true, true);
            expect (! bus.groups && ! bus.sends);
            expect (bus.plugins[0].bypassed && bus.plugins[1].bypassed);
        }

        beginTest ("complete live state and bypass restore together; incomplete reads are reported to the save caller");
        {
            PluginChain chain;
            chain.prepare (48000.0, 256);
            auto* plugin = new TestGainPlugin (0.375f);
            chain.addPlugin (std::unique_ptr<juce::AudioPluginInstance> (plugin));
            chain.setBypassed (0, true);
            bool complete = true;   // getStates only clears the caller's aggregate completeness flag
            auto states = chain.getStates (&complete);
            expect (complete);
            const auto set = capturePluginSet (1, "Master", "now", states, nullptr, {}, false, false);
            PluginChain restored;
            restored.prepare (48000.0, 256);
            const auto errors = restored.restore (set.plugins, [] (const PluginSlotState&, juce::String&)
            {
                return std::make_unique<TestGainPlugin> (1.0f);
            });
            expect (errors.isEmpty());
            expect (restored.getSlot (0).bypassed.load());
            expect (restored.getSlot (0).state.slotId == states[0].slotId);
            expectWithinAbsoluteError (static_cast<TestGainPlugin*> (restored.getSlot (0).plugin.get())->gain, 0.375f, 1e-6f);
            plugin->throwOnGetState = true;
            states = chain.getStates (&complete);
            expect (! complete);
        }

        beginTest ("send matching reserves IDs before exact names; duplicate FX names consume one destination each");
        {
            const std::vector<MixFx> fx { fxNamed ("Reverb"), fxNamed ("Reverb"), fxNamed ("Renamed"), fxNamed ("Untouched") };
            const std::vector<PluginSetSend> sends {
                { juce::Uuid(), "Reverb", 0.1, true },
                { fx[0].id, "Old name", 0.2, false },
                { fx[2].id, "Reverb", 0.3, true },
                { juce::Uuid(), "Gone", 0.4, false },
                { juce::Uuid(), "Silent", 0.0, true },
                { juce::Uuid(), "reverb", 0.5, false }
            };
            const auto matched = matchPluginSetSends (sends, fx);
            expectEquals ((int) matched.sends.size(), 3);
            if (matched.sends.size() == 3)
            {
                expect (matched.sends[0].fx == fx[1].id && matched.sends[0].pre);
                expect (matched.sends[1].fx == fx[0].id && ! matched.sends[1].pre);
                expect (matched.sends[2].fx == fx[2].id);
            }
            expectEquals (matched.warnings.size(), 2);
            expectEquals (matched.warnings[0], k ("센드: 'Gone' FX 채널이 이 세션에 없어 건너뛰었습니다"));
            const auto names = matchPluginSetSends ({ { juce::Uuid(), "Reverb", 0.0, false }, { juce::Uuid(), "Reverb", 0.8, true } }, fx);
            expectEquals ((int) names.sends.size(), 2);
            expect (names.sends[0].fx == fx[0].id && names.sends[1].fx == fx[1].id);
            const auto exactCase = matchPluginSetSends ({ { juce::Uuid(), "reverb", 0.5, false } }, fx);
            expect (exactCase.sends.empty() && exactCase.warnings.size() == 1);
        }

        beginTest ("pure group plans replace only when present, filter members, cap at five and bypass before publication");
        {
            auto set = sampleSet();
            const auto first = set.plugins[0].slotId, last = set.plugins[2].slotId;
            MixChannel channel;
            channel.pluginGroups = { { { last, juce::Uuid() }, true } };
            auto plan = planPluginSet (set, &channel, {});
            expect (! plan.groups);
            expect (plan.plugins[2].bypassed && ! plan.plugins[0].bypassed);
            expect (! set.plugins[2].bypassed);
            set.groups = std::vector<MixPluginGroup> { { { first, first, juce::Uuid(), juce::Uuid::null() }, true } };
            set.groups->resize (7);
            plan = planPluginSet (set, &channel, {});
            expectEquals ((int) plan.groups->size(), 5);
            expectEquals ((int) plan.groups->front().slots.size(), 1);
            expect (plan.groups->front().slots[0] == first);
            expect (plan.plugins[0].bypassed && plan.plugins[1].bypassed && ! plan.plugins[2].bypassed);
            set.groups->clear();
            plan = planPluginSet (set, &channel, {});
            expect (plan.groups && plan.groups->empty());
            expect (! plan.plugins[2].bypassed);
            plan = planPluginSet (set, nullptr, {});
            expect (! plan.groups && plan.sends.sends.empty());
            expect (! plan.plugins[0].bypassed && plan.plugins[1].bypassed);

            // FX / master: groups do not come along, so an OFF group's hold on its members is released there
            auto fxSet = sampleSet();
            fxSet.plugins[0].bypassed = true;
            fxSet.groups = std::vector<MixPluginGroup> { { { fxSet.plugins[0].slotId }, true } };
            const auto fxPlan = planPluginSet (fxSet, nullptr, {});
            expect (! fxPlan.plugins[0].bypassed, "an OFF-group bypass is released where the groups stay behind");
            expect (fxPlan.plugins[1].bypassed, "a plugin switched off on its own stays off");
            expect (fxSet.plugins[0].bypassed, "the set itself is untouched");
        }

        beginTest ("document replaces groups and matched sends in one notification; missing plugins retain slots and state");
        {
            MixEngine engine;
            engine.prepare (48000.0, 256);
            MixDocument document (engine);
            document.newSession();
            const auto channelId = document.getSession().channels[0].id;
            const auto secondFx = document.addFx();
            auto& channel = *document.getSession().findChannel (channelId);
            auto* chain = engine.getChannelChain (channelId);
            chain->addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            channel.pluginGroups = { { { chain->getSlot (0).state.slotId }, true } };
            document.setSend (channelId, secondFx, 0.9, true);
            auto set = sampleSet();
            set.groups = std::vector<MixPluginGroup> { { { set.plugins[0].slotId, juce::Uuid() }, true }, { { set.plugins[2].slotId }, false } };
            const auto firstFx = document.getSession().fx[0].id;
            set.sends = std::vector<PluginSetSend> { { firstFx, "old FX name", 0.0, true }, { juce::Uuid(), "Gone", 0.7, false } };
            int values = 0, structures = 0;
            document.onValueChanged = [&] { ++values; };
            document.onStructureChanged = [&] { ++structures; };
            DirtyChainListener listener (document);
            chain->setListener (&listener);
            document.discardUnsavedChanges();
            const auto errors = document.applyPluginSet (*chain, set);
            chain->setListener (nullptr);
            expect (document.isDirty());
            expectEquals (values, 1);
            expectEquals (structures, 0);
            expect (listener.calls > 0);
            expectEquals (errors.size(), 4, "three missing plugins plus one send warning in the same result");
            expect (errors.joinIntoString ("\n").contains (k ("센드: 'Gone'")));
            expectEquals (chain->getNumSlots(), 3);
            expectEquals ((int) channel.pluginGroups.size(), 2);
            expectEquals ((int) channel.pluginGroups[0].slots.size(), 1);
            expect (channel.pluginGroups[0].off && channel.pluginGroups[0].slots[0] == set.plugins[0].slotId);
            for (int i = 0; i < 3; ++i)
            {
                expect (chain->getSlot (i).isMissing());
                expect (chain->getSlot (i).state.slotId == set.plugins[(size_t) i].slotId);
                expectEquals (chain->getSlot (i).state.stateBase64, set.plugins[(size_t) i].stateBase64);
                expect (chain->getSlot (i).bypassed.load() == (i != 2));
            }
            const auto& first = document.getSession().sendFor (channel, firstFx);
            const auto& untouched = document.getSession().sendFor (channel, secondFx);
            expect (first.amount == 0.0 && first.pre);
            expect (untouched.amount == 0.9 && untouched.pre);
        }

        beginTest ("sets without groups retain existing OFF memberships; present empty groups clear them");
        {
            MixEngine engine;
            engine.prepare (48000.0, 256);
            MixDocument document (engine);
            document.newSession();
            auto& channel = document.getSession().channels[0];
            auto* chain = engine.getChannelChain (channel.id);
            auto set = sampleSet();
            const auto ghost = juce::Uuid();
            channel.pluginGroups = { { { set.plugins[2].slotId, ghost }, true } };
            document.applyPluginSet (*chain, set);
            expectEquals ((int) channel.pluginGroups.size(), 1);
            expect (channel.pluginGroups[0].off);
            expect (channel.pluginGroups[0].slots == std::vector<juce::Uuid> { set.plugins[2].slotId, ghost });
            expect (chain->getSlot (2).bypassed.load());
            set.groups.emplace();
            document.applyPluginSet (*chain, set);
            expect (channel.pluginGroups.empty());
            expect (! chain->getSlot (2).bypassed.load());
            expect (chain->getSlot (1).bypassed.load(), "an independently bypassed plugin stays off");
        }

        beginTest ("FX and master take plugins only; loading into a second session matches sends by name");
        {
            auto set = sampleSet();
            set.groups = std::vector<MixPluginGroup> { { { set.plugins[0].slotId }, true } };
            set.sends = std::vector<PluginSetSend> { { juce::Uuid(), "Reverb", 0.25, true } };
            MixEngine engine;
            engine.prepare (48000.0, 256);
            MixDocument document (engine);
            document.newSession();
            auto& channel = document.getSession().channels[0];
            const auto fxId = document.getSession().fx[0].id;
            document.renameFx (fxId, "Reverb");
            channel.pluginGroups = { { {}, false } };
            document.setSend (channel.id, fxId, 0.8, false);
            for (auto* chain : { engine.getFxChain (fxId), &engine.getMasterChain() })
            {
                const auto errors = document.applyPluginSet (*chain, set);
                expectEquals (errors.size(), 3);
                expectEquals (chain->getNumSlots(), 3);
                expect (! chain->getSlot (0).bypassed.load());
                expect (chain->getSlot (1).bypassed.load());
                expectEquals ((int) channel.pluginGroups.size(), 1);
                expect (! channel.pluginGroups[0].off && channel.pluginGroups[0].slots.empty());
                expect (channel.sends[0].amount == 0.8 && ! channel.sends[0].pre);
            }
            document.newSession();
            auto& next = document.getSession().channels[0];
            const auto newFx = document.getSession().fx[0].id;
            document.renameFx (newFx, "Reverb");
            auto* chain = engine.getChannelChain (next.id);
            document.applyPluginSet (*chain, set);
            expect (next.sends[0].fx == newFx && next.sends[0].amount == 0.25 && next.sends[0].pre);
            expect (next.pluginGroups[0].off && chain->getSlot (0).bypassed.load());
            expectEquals ((int) juce::JSON::parse (document.getSession().toJson())["version"], MixSession::currentVersion);
        }
    }
};

static PluginSetTests pluginSetTests;

} // namespace gocue::tests
