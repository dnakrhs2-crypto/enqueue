#include "PluginSet.h"

#include "model/ProjectSerializer.h"
#include "model/SafeFileWrite.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace gocue::livemix
{

namespace
{
    juce::String k (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    bool numberIn (const juce::var& value, double low, double high, bool whole = false)
    {
        if (! (value.isInt() || value.isInt64() || value.isDouble()))
            return false;
        const double n = (double) value;
        return std::isfinite (n) && n >= low && n <= high && (! whole || n == std::floor (n));
    }

    bool validId (const juce::var& value)
    {
        const auto text = value.toString().removeCharacters ("-");
        return value.isString() && text.length() == 32 && text.containsOnly ("0123456789abcdefABCDEF")
            && ! juce::Uuid (text).isNull();
    }

    bool hasSlot (const MixPluginGroup& group, const juce::Uuid& id)
    {
        return std::find (group.slots.begin(), group.slots.end(), id) != group.slots.end();
    }

    bool heldOff (const std::vector<MixPluginGroup>& groups, const juce::Uuid& id)
    {
        return std::any_of (groups.begin(), groups.end(), [&] (const auto& group) { return group.off && hasSlot (group, id); });
    }
}

juce::String PluginSet::toJson() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("app", "LiveMix");
    root->setProperty ("kind", "pluginSet");
    root->setProperty ("version", currentVersion);
    root->setProperty ("number", number);
    root->setProperty ("source", source);
    root->setProperty ("savedAt", savedAt);
    root->setProperty ("plugins", ProjectSerializer::pluginSlotsToVar (plugins));

    if (sends)
    {
        juce::Array<juce::var> array;
        for (const auto& send : *sends)
        {
            auto* item = new juce::DynamicObject();
            item->setProperty ("fx", send.fx.toString());
            item->setProperty ("name", send.name);
            item->setProperty ("amount", send.amount);
            item->setProperty ("pre", send.pre);
            array.add (juce::var (item));
        }
        root->setProperty ("sends", array);
    }

    if (groups)
    {
        juce::Array<juce::var> array;
        for (const auto& group : *groups)
        {
            auto* item = new juce::DynamicObject();
            juce::Array<juce::var> slots;
            for (const auto& id : group.slots)
                slots.add (id.toString());
            item->setProperty ("slots", slots);
            item->setProperty ("off", group.off);
            array.add (juce::var (item));
        }
        root->setProperty ("groups", array);
    }

    return juce::JSON::toString (juce::var (root), false);
}

juce::Result PluginSet::fromJson (const juce::String& json, PluginSet& out)
{
    if ((juce::int64) json.getNumBytesAsUTF8() > maxFileBytes)
        return juce::Result::fail (k ("세트 파일이 너무 큽니다 (최대 8 MB)."));

    const auto root = juce::JSON::parse (json);
    if (root.getDynamicObject() == nullptr || ProjectSerializer::hasTrailingJsonData (json))
        return juce::Result::fail (k ("플러그인 세트 파일의 JSON을 읽을 수 없습니다."));
    if (root["app"].toString() != "LiveMix" || root["kind"].toString() != "pluginSet")
        return juce::Result::fail (k ("LiveMix 플러그인 세트 파일이 아닙니다."));
    if (! numberIn (root["version"], 1.0, 1.0e6, true))
        return juce::Result::fail (k ("세트 파일의 버전 표시가 잘못됐습니다."));
    if ((int) root["version"] > currentVersion)
        return juce::Result::fail (k ("더 새로운 LiveMix로 저장한 세트입니다. LiveMix를 업데이트하세요."));
    if (! numberIn (root["number"], 1.0, numSets, true))
        return juce::Result::fail (k ("세트 번호가 잘못됐습니다 (1~5)."));
    if (! root["source"].isString() || ! root["savedAt"].isString())
        return juce::Result::fail (k ("세트 파일의 저장 정보가 잘못됐습니다."));

    const auto* pluginsArray = root["plugins"].getArray();
    if (pluginsArray == nullptr)
        return juce::Result::fail (k ("세트 파일에 플러그인 목록이 없습니다."));
    if (pluginsArray->size() > maxPlugins)
        return juce::Result::fail (k ("한 세트에는 플러그인을 16개까지 저장할 수 있습니다."));

    std::set<juce::Uuid> ids;
    for (const auto& item : *pluginsArray)
    {
        if (item.getDynamicObject() == nullptr || ! validId (item["slotId"])
            || ! ids.insert (juce::Uuid (item["slotId"].toString())).second || ! item["bypassed"].isBool()
            || ! item["state"].isString())
            return juce::Result::fail (k ("세트 파일의 플러그인 항목이 잘못됐습니다."));

        juce::MemoryOutputStream decoded;
        const auto state = item["state"].toString();
        if (state.isNotEmpty() && ! juce::Base64::convertFromBase64 (decoded, state))
            return juce::Result::fail (k ("세트 파일의 플러그인 설정 데이터를 읽을 수 없습니다: ") + item["name"].toString());
    }

    PluginSet set;
    set.number = (int) root["number"];
    set.source = root["source"].toString();
    set.savedAt = root["savedAt"].toString();
    set.plugins = ProjectSerializer::pluginSlotsFromVar (root["plugins"]);

    if (root.hasProperty ("sends"))
    {
        const auto* array = root["sends"].getArray();
        if (array == nullptr)
            return juce::Result::fail (k ("세트 파일의 FX 센드 목록이 잘못됐습니다."));
        set.sends.emplace();
        ids.clear();
        for (const auto& item : *array)
        {
            if (item.getDynamicObject() == nullptr || ! validId (item["fx"]) || ! item["name"].isString()
                || ! numberIn (item["amount"], 0.0, 1.0) || ! item["pre"].isBool()
                || ! ids.insert (juce::Uuid (item["fx"].toString())).second)
                return juce::Result::fail (k ("세트 파일의 FX 센드 항목이 잘못됐습니다."));
            set.sends->push_back ({ juce::Uuid (item["fx"].toString()), item["name"].toString(), (double) item["amount"], (bool) item["pre"] });
        }
    }

    if (root.hasProperty ("groups"))
    {
        const auto* array = root["groups"].getArray();
        if (array == nullptr || array->size() > MixSession::maxPluginGroups)
            return juce::Result::fail (k ("세트 파일의 플러그인 그룹 목록이 잘못됐습니다 (최대 5개)."));
        set.groups.emplace();
        for (const auto& item : *array)
        {
            const auto* slots = item["slots"].getArray();
            if (item.getDynamicObject() == nullptr || slots == nullptr || ! item["off"].isBool())
                return juce::Result::fail (k ("세트 파일의 플러그인 그룹 항목이 잘못됐습니다."));
            MixPluginGroup group;
            group.off = (bool) item["off"];
            for (const auto& id : *slots)
            {
                if (! validId (id))
                    return juce::Result::fail (k ("세트 파일의 플러그인 그룹 멤버가 잘못됐습니다."));
                const juce::Uuid slot (id.toString());
                if (! hasSlot (group, slot))
                    group.slots.push_back (slot);
            }
            set.groups->push_back (std::move (group));
        }
    }

    out = std::move (set);
    return juce::Result::ok();
}

juce::Result PluginSet::save (const juce::File& target) const
{
    const auto json = toJson();
    if ((juce::int64) json.getNumBytesAsUTF8() > maxFileBytes)
        return juce::Result::fail (k ("세트가 너무 커서 저장하지 않았습니다 (최대 8 MB)."));

    PluginSet check;
    if (const auto valid = fromJson (json, check); valid.failed())
        return valid;   // never truncate a chain or overwrite a good set with an unreadable one

    return SafeFileWrite::writeTextVerified (target, json, [] (const juce::String& readBack)
    {
        PluginSet verified;
        return fromJson (readBack, verified);
    });
}

juce::Result PluginSet::load (const juce::File& file, PluginSet& out)
{
    if (! file.existsAsFile())
        return juce::Result::fail (k ("세트 파일을 읽을 수 없습니다: ") + file.getFullPathName());
    if (file.getSize() > maxFileBytes)
        return juce::Result::fail (k ("세트 파일이 너무 큽니다 (최대 8 MB)."));
    return fromJson (file.loadFileAsString(), out);
}

juce::File PluginSet::defaultFolder()
{
    return folderIn (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
}

juce::File PluginSet::folderIn (const juce::File& documents)
{
    return documents.getChildFile ("LiveMix").getChildFile (k ("플러그인 세트"));
}

juce::File PluginSet::fileFor (int number, const juce::File& folder)
{
    jassert (number >= 1 && number <= numSets);
    return folder.getChildFile (k ("세트 ") + juce::String (number) + fileExtension);
}

juce::String PluginSet::summary() const
{
    juce::StringArray names;
    for (const auto& plugin : plugins)
        names.add (plugin.name);
    auto text = names.joinIntoString (k (" → "));
    if (text.length() > 48)
        text = text.substring (0, 48) + k ("…");
    text += " (" + juce::String ((int) plugins.size()) + k ("개)");
    if (sends || groups)
        text += k (" · ") + (sends ? k ("센드") : juce::String())
            + (sends && groups ? k ("·") : juce::String()) + (groups ? k ("그룹") : juce::String());
    return text;
}

juce::String PluginSetEntry::description() const
{
    return state == State::empty ? k ("비어 있음") : state == State::unreadable ? k ("읽을 수 없는 파일") : set.summary();
}

juce::String PluginSetEntry::menuText (bool saving) const
{
    return k ("세트 ") + juce::String (set.number) + k (" — ") + description()
        + (saving && state != State::empty ? k (" (덮어쓰기)") : juce::String());
}

std::array<PluginSetEntry, PluginSet::numSets> listPluginSets (const juce::File& folder)
{
    std::array<PluginSetEntry, PluginSet::numSets> entries;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        auto& entry = entries[i];
        entry.set.number = (int) i + 1;
        const auto file = PluginSet::fileFor (entry.set.number, folder);
        if (! file.exists())
            continue;
        PluginSet loaded;
        const auto result = PluginSet::load (file, loaded);
        if (result.failed() || loaded.number != entry.set.number)
        {
            entry.state = PluginSetEntry::State::unreadable;
            entry.error = result.failed() ? result.getErrorMessage() : k ("파일 이름과 세트 번호가 다릅니다.");
            continue;
        }
        entry.state = PluginSetEntry::State::ready;
        entry.set = std::move (loaded);
    }
    return entries;
}

PluginSet capturePluginSet (int number, const juce::String& source, const juce::String& savedAt,
                            std::vector<PluginSlotState> states, const MixChannel* channel,
                            const std::vector<MixFx>& fx, bool includeSends, bool includeGroups)
{
    PluginSet set;
    set.number = number;
    set.source = source;
    set.savedAt = savedAt;
    set.plugins = std::move (states);
    if (channel == nullptr)
        return set;

    if (includeGroups)
        set.groups = channel->pluginGroups;
    else
        for (auto& state : set.plugins)
            if (heldOff (channel->pluginGroups, state.slotId))
                state.bypassed = false;

    if (includeSends)
    {
        set.sends.emplace();
        for (const auto& target : fx)
        {
            PluginSetSend send { target.id, target.name, 0.0, false };
            for (const auto& existing : channel->sends)
                if (existing.fx == target.id)
                {
                    send.amount = existing.amount;
                    send.pre = existing.pre;
                    break;
                }
            set.sends->push_back (std::move (send));
        }
    }
    return set;
}

PluginSetSendMatches matchPluginSetSends (const std::vector<PluginSetSend>& sends, const std::vector<MixFx>& fx)
{
    std::vector<int> targets (sends.size(), -1);
    std::vector<bool> used (fx.size(), false);
    // Name fallbacks must not steal an FX that a later send identifies exactly.
    for (size_t i = 0; i < sends.size(); ++i)
        for (size_t j = 0; j < fx.size(); ++j)
            if (! used[j] && sends[i].fx == fx[j].id)
            {
                targets[i] = (int) j;
                used[j] = true;
                break;
            }

    PluginSetSendMatches result;
    for (size_t i = 0; i < sends.size(); ++i)
    {
        const auto& send = sends[i];
        if (targets[i] < 0)
            for (size_t j = 0; j < fx.size(); ++j)
                if (! used[j] && send.name == fx[j].name)
                {
                    targets[i] = (int) j;
                    used[j] = true;
                    break;
                }
        if (targets[i] >= 0)
            result.sends.push_back ({ fx[(size_t) targets[i]].id, send.amount, send.pre });
        else if (send.amount > 0.0)
            result.warnings.add (k ("센드: '") + send.name + k ("' FX 채널이 이 세션에 없어 건너뛰었습니다"));
    }
    return result;
}

PluginSetApplication planPluginSet (const PluginSet& set, const MixChannel* channel, const std::vector<MixFx>& fx)
{
    PluginSetApplication result;
    result.plugins = set.plugins;
    if ((int) result.plugins.size() > PluginSet::maxPlugins)
        result.plugins.resize ((size_t) PluginSet::maxPlugins);
    if (channel == nullptr)
    {
        // FX / master: the groups stay behind, so a plugin only an OFF group had switched off comes back on
        // (the rule capture uses when groups are left out)
        if (set.groups)
            for (auto& state : result.plugins)
                if (heldOff (*set.groups, state.slotId))
                    state.bypassed = false;
        return result;
    }

    if (set.groups)
    {
        result.groups.emplace();
        for (const auto& saved : *set.groups)
        {
            if ((int) result.groups->size() == MixSession::maxPluginGroups)
                break;
            MixPluginGroup group;
            group.off = saved.off;
            for (const auto& id : saved.slots)
                if (! id.isNull() && ! hasSlot (group, id)
                    && std::any_of (result.plugins.begin(), result.plugins.end(), [&] (const auto& state) { return state.slotId == id; }))
                    group.slots.push_back (id);
            result.groups->push_back (std::move (group));
        }
    }

    const auto& groups = result.groups ? *result.groups : channel->pluginGroups;
    for (auto& state : result.plugins)
        if (heldOff (groups, state.slotId))
            state.bypassed = true;
    if (set.sends)
        result.sends = matchPluginSetSends (*set.sends, fx);
    return result;
}

} // namespace gocue::livemix
