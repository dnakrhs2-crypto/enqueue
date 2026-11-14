#include "Localisation.h"
#include "MixModel.h"

#include "model/ProjectSerializer.h"
#include "model/SafeFileWrite.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace gocue::livemix
{

namespace
{
    juce::var outputToVar (const MixOutput& o)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("master", o.master);
        obj->setProperty ("direct", o.direct);
        obj->setProperty ("directFirst", o.directFirst);
        return obj;
    }

    MixOutput outputFromVar (const juce::var& v, const MixOutput& fallback)
    {
        MixOutput o = fallback;

        if (v.getDynamicObject() == nullptr)
            return o;

        o.master = (bool) v.getProperty ("master", fallback.master);
        o.direct = (bool) v.getProperty ("direct", fallback.direct);
        o.directFirst = (int) v.getProperty ("directFirst", fallback.directFirst);
        return o;
    }

    juce::String idText (const juce::Uuid& id) { return id.isNull() ? juce::String() : id.toString(); }

    juce::var pluginGroupsToVar (const std::vector<MixPluginGroup>& pluginGroups)
    {
        juce::Array<juce::var> groups;

        for (const auto& g : pluginGroups)
        {
            auto* group = new juce::DynamicObject();
            juce::Array<juce::var> members;

            for (const auto& slotId : g.slots)
                members.add (idText (slotId));

            group->setProperty ("slots", members);
            group->setProperty ("off", g.off);
            groups.add (group);
        }

        return groups;
    }

    std::vector<MixPluginGroup> pluginGroupsFromVar (const juce::var& value)
    {
        std::vector<MixPluginGroup> result;
        if (const auto* groups = value.getArray())
            for (const auto& gv : *groups)
                if (gv.getDynamicObject() != nullptr)
                {
                    MixPluginGroup g;
                    g.off = (bool) gv.getProperty ("off", false);
                    if (const auto* members = gv.getProperty ("slots", juce::var()).getArray())
                        for (const auto& mv : *members)
                            g.slots.push_back (juce::Uuid (mv.toString()));
                    result.push_back (std::move (g));
                }
        return result;
    }

    void cleanGroupMembers (std::vector<MixPluginGroup>& groups, const std::vector<PluginSlotState>& chain)
    {
        for (auto& group : groups)
        {
            std::vector<juce::Uuid> kept;
            for (const auto& id : group.slots)
                if (! id.isNull() && std::find (kept.begin(), kept.end(), id) == kept.end()
                    && std::any_of (chain.begin(), chain.end(), [&] (const auto& slot) { return slot.slotId == id; }))
                    kept.push_back (id);
            group.slots = std::move (kept);
        }
    }

    double finiteOr (double value, double fallback) { return std::isfinite (value) ? value : fallback; }
}

juce::String MixDevice::readSampleFormat (const juce::var& value)
{
    if (value.isString())
    {
        const auto text = value.toString();
        if (text == "int16" || text == "int24" || text == "int32" || text == "float32") return text;
    }
    return {};
}

MixChannel* MixSession::findChannel (const juce::Uuid& id) noexcept
{
    for (auto& c : channels)
        if (c.id == id)
            return &c;

    return nullptr;
}

const MixChannel* MixSession::findChannel (const juce::Uuid& id) const noexcept
{
    return const_cast<MixSession*> (this)->findChannel (id);
}

MixAux* MixSession::findAux (const juce::Uuid& id) noexcept
{
    for (auto& a : auxes)
        if (a.id == id)
            return &a;

    return nullptr;
}

const MixAux* MixSession::findAux (const juce::Uuid& id) const noexcept
{
    return const_cast<MixSession*> (this)->findAux (id);
}

MixFx* MixSession::findFx (const juce::Uuid& id) noexcept
{
    for (auto& f : fx)
        if (f.id == id)
            return &f;

    return nullptr;
}

const MixFx* MixSession::findFx (const juce::Uuid& id) const noexcept
{
    return const_cast<MixSession*> (this)->findFx (id);
}

int MixSession::indexOfFx (const juce::Uuid& id) const noexcept
{
    for (size_t i = 0; i < fx.size(); ++i)
        if (fx[i].id == id)
            return (int) i;

    return -1;
}

int MixSession::addChannel (const juce::String& newName)
{
    if ((int) channels.size() >= maxChannels)
        return -1;

    MixChannel c;
    c.name = newName.isNotEmpty() ? newName : newMicName ((int) channels.size() + 1);

    // the next free input after the last channel's
    if (! channels.empty())
    {
        const auto& last = channels.back();
        c.inputFirst = juce::jlimit (0, maxDeviceChannels - 1, last.inputFirst + (last.stereo ? 2 : 1));
    }

    for (const auto& f : fx)
        c.sends.push_back ({ f.id, 0.0, false });

    channels.push_back (std::move (c));
    return (int) channels.size() - 1;
}

int MixSession::addAux (const juce::String& newName)
{
    if ((int) auxes.size() >= maxAux)
        return -1;

    MixAux a;
    a.name = newName;
    if (a.name.isEmpty())   // like a new mic channel: its place in the list (the card's AUX badge), the next number if taken
        for (int number = (int) auxes.size() + 1;; ++number)
        {
            a.name = trf ("억스 {0}", number);
            if (std::none_of (auxes.begin(), auxes.end(), [&] (const MixAux& existing) { return existing.name == a.name; }))
                break;
        }

    auxes.push_back (std::move (a));
    return (int) auxes.size() - 1;
}

int MixSession::addFx (const juce::String& newName)
{
    if ((int) fx.size() >= maxFx)
        return -1;

    MixFx f;
    f.name = newName.isNotEmpty() ? newName : "FX " + juce::String ((int) fx.size() + 1);
    fx.push_back (std::move (f));

    for (auto& c : channels)
        c.sends.push_back ({ fx.back().id, 0.0, false });

    return (int) fx.size() - 1;
}

void MixSession::removeChannel (const juce::Uuid& id)
{
    channels.erase (std::remove_if (channels.begin(), channels.end(), [&] (const MixChannel& c) { return c.id == id; }), channels.end());
}

void MixSession::removeAux (const juce::Uuid& id)
{
    const auto removed = id;   // the caller may pass an id from inside auxes
    auxes.erase (std::remove_if (auxes.begin(), auxes.end(), [&] (const MixAux& a) { return a.id == removed; }), auxes.end());
    for (auto& c : channels)
        c.auxSends.erase (std::remove (c.auxSends.begin(), c.auxSends.end(), removed), c.auxSends.end());
}

void MixSession::removeFx (const juce::Uuid& id)
{
    fx.erase (std::remove_if (fx.begin(), fx.end(), [&] (const MixFx& f) { return f.id == id; }), fx.end());

    for (auto& c : channels)
        c.sends.erase (std::remove_if (c.sends.begin(), c.sends.end(), [&] (const MixSend& s) { return s.fx == id; }), c.sends.end());
}

MixSend& MixSession::sendFor (MixChannel& channel, const juce::Uuid& fxId)
{
    for (auto& s : channel.sends)
        if (s.fx == fxId)
            return s;

    channel.sends.push_back ({ fxId, 0.0, false });
    return channel.sends.back();
}

void MixSession::sanitise()
{
    name = name.trim();

    if (name.isEmpty())
        nameChosen = false;   // nothing to have chosen: the session goes by its file

    if ((int) channels.size() > maxChannels)
        channels.resize ((size_t) maxChannels);

    if ((int) auxes.size() > maxAux)
        auxes.resize ((size_t) maxAux);

    if ((int) fx.size() > maxFx)
        fx.resize ((size_t) maxFx);

    auto capChain = [] (std::vector<PluginSlotState>& chain)
    {
        if ((int) chain.size() > maxChainSlots)
            chain.resize ((size_t) maxChainSlots);
    };

    for (auto& c : channels)
        capChain (c.chain);

    for (auto& a : auxes)
        capChain (a.chain);

    for (auto& f : fx)
        capChain (f.chain);

    capChain (master.chain);
    if ((int) master.pluginGroups.size() > maxPluginGroups)
        master.pluginGroups.resize ((size_t) maxPluginGroups);

    std::set<juce::String> seen;

    auto uniqueId = [&seen] (juce::Uuid& id)
    {
        if (id.isNull() || seen.count (id.toString()) != 0)
            id = juce::Uuid();

        seen.insert (id.toString());
    };

    auto clampOutput = [] (MixOutput& o)
    {
        o.directFirst = juce::jlimit (0, maxDeviceChannels - 2, o.directFirst);
    };

    for (size_t i = 0; i < fx.size(); ++i)
    {
        auto& f = fx[i];
        uniqueId (f.id);

        if (f.name.isEmpty())
            f.name = "FX " + juce::String ((int) i + 1);

        f.returnAmount = juce::jlimit (0.0, 1.0, finiteOr (f.returnAmount, 1.0));
        clampOutput (f.output);
    }

    for (size_t i = 0; i < channels.size(); ++i)
    {
        auto& c = channels[i];
        uniqueId (c.id);

        if (c.name.isEmpty())
            c.name = newMicName ((int) i + 1);

        if ((int) c.pluginGroups.size() > maxPluginGroups)
            c.pluginGroups.resize ((size_t) maxPluginGroups);

        c.inputFirst = juce::jlimit (0, maxDeviceChannels - 1, c.inputFirst);
        c.pan = juce::jlimit (-1.0, 1.0, finiteOr (c.pan, 0.0));

        if (c.stereo && c.inputFirst > maxDeviceChannels - 2)
            c.inputFirst = maxDeviceChannels - 2;

        clampOutput (c.output);

        // exactly one send per FX channel, in FX order
        std::vector<MixSend> sends;

        for (const auto& f : fx)
        {
            MixSend s { f.id, 0.0, false };

            for (const auto& old : c.sends)
                if (old.fx == f.id)
                {
                    s.amount = juce::jlimit (0.0, 1.0, finiteOr (old.amount, 0.0));
                    s.pre = old.pre;
                    break;
                }

            sends.push_back (s);
        }

        c.sends = std::move (sends);
    }

    for (size_t i = 0; i < auxes.size(); ++i)
    {
        auto& a = auxes[i];
        uniqueId (a.id);   // shared namespace with mic and FX ids
        if (a.name.isEmpty())
            a.name = trf ("억스 {0}", (int) i + 1);
        a.pan = juce::jlimit (-1.0, 1.0, finiteOr (a.pan, 0.0));
        clampOutput (a.output);
    }

    for (auto& c : channels)
    {
        std::vector<juce::Uuid> kept;
        for (const auto& id : c.auxSends)
            if (findAux (id) != nullptr && std::find (kept.begin(), kept.end(), id) == kept.end())
                kept.push_back (id);
        c.auxSends = std::move (kept);
    }

    master.outputFirst = juce::jlimit (0, maxDeviceChannels - 2, master.outputFirst);

    if (device.type.isEmpty())
        device.type = "ASIO";

    if (device.isAsio())
        device.output = device.input;

    device.bufferSize = juce::jlimit (16, 8192, device.bufferSize);
    device.sampleRate = juce::jlimit (8000.0, 384000.0, finiteOr (device.sampleRate, 48000.0));
}

juce::String MixSession::toJson() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("app", "LiveMix");
    root->setProperty ("version", currentVersion);
    root->setProperty ("name", name);
    root->setProperty ("nameChosen", nameChosen);

    auto* dev = new juce::DynamicObject();
    dev->setProperty ("type", device.type);
    dev->setProperty ("input", device.input);
    dev->setProperty ("output", device.output);
    dev->setProperty ("bufferSize", device.bufferSize);
    dev->setProperty ("sampleRate", device.sampleRate);
    dev->setProperty ("sampleFormat", device.sampleFormat);
    root->setProperty ("device", dev);

    juce::Array<juce::var> channelArray;

    for (const auto& c : channels)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id", idText (c.id));
        obj->setProperty ("name", c.name);
        obj->setProperty ("on", c.on);
        obj->setProperty ("muteGroup", c.muteGroup);
        obj->setProperty ("pan", c.pan);
        auto* input = new juce::DynamicObject();
        input->setProperty ("first", c.inputFirst);
        input->setProperty ("stereo", c.stereo);
        obj->setProperty ("input", input);
        juce::Array<juce::var> auxSends;
        for (const auto& id : c.auxSends)
            auxSends.add (idText (id));
        obj->setProperty ("auxSends", auxSends);
        obj->setProperty ("chain", ProjectSerializer::pluginSlotsToVar (c.chain));

        juce::Array<juce::var> sends;

        for (const auto& s : c.sends)
        {
            auto* send = new juce::DynamicObject();
            send->setProperty ("fx", idText (s.fx));
            send->setProperty ("amount", s.amount);
            send->setProperty ("pre", s.pre);
            sends.add (send);
        }

        obj->setProperty ("sends", sends);
        obj->setProperty ("output", outputToVar (c.output));

        obj->setProperty ("pluginGroups", pluginGroupsToVar (c.pluginGroups));
        channelArray.add (obj);
    }

    root->setProperty ("channels", channelArray);

    juce::Array<juce::var> auxArray;
    for (const auto& a : auxes)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id", idText (a.id));
        obj->setProperty ("name", a.name);
        obj->setProperty ("on", a.on);
        obj->setProperty ("pan", a.pan);
        obj->setProperty ("output", outputToVar (a.output));
        obj->setProperty ("chain", ProjectSerializer::pluginSlotsToVar (a.chain));
        auxArray.add (obj);
    }
    root->setProperty ("auxes", auxArray);

    juce::Array<juce::var> fxArray;

    for (const auto& f : fx)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id", idText (f.id));
        obj->setProperty ("name", f.name);
        obj->setProperty ("chain", ProjectSerializer::pluginSlotsToVar (f.chain));
        obj->setProperty ("returnAmount", f.returnAmount);
        obj->setProperty ("mono", f.mono);
        obj->setProperty ("on", f.on);
        obj->setProperty ("muteGroup", f.muteGroup);
        obj->setProperty ("output", outputToVar (f.output));
        fxArray.add (obj);
    }

    root->setProperty ("fx", fxArray);

    auto* m = new juce::DynamicObject();
    m->setProperty ("chain", ProjectSerializer::pluginSlotsToVar (master.chain));
    m->setProperty ("pluginGroups", pluginGroupsToVar (master.pluginGroups));
    m->setProperty ("outputFirst", master.outputFirst);
    m->setProperty ("sendToObs", master.sendToObs);
    root->setProperty ("master", m);

    return juce::JSON::toString (juce::var (root), false);
}

juce::Result MixSession::fromJson (const juce::String& json, MixSession& out, juce::StringArray* warnings)
{
    if (json.trim().isEmpty())
        return juce::Result::fail (tr ("세션 파일이 비어 있습니다"));

    // JUCE's parser stops after the first object: corruption behind it would be read as a good file and then written over
    if (! json.trimEnd().endsWithChar ('}') || ProjectSerializer::hasTrailingJsonData (json))
        return juce::Result::fail (tr ("세션 파일이 손상됐습니다 (세션이 끝난 뒤에 데이터가 더 있습니다)"));

    juce::var root;
    const auto parsed = juce::JSON::parse (json, root);

    if (parsed.failed())
        return juce::Result::fail (trf ("세션 파일을 읽을 수 없습니다 (JSON): {0}", parsed.getErrorMessage()));

    if (root.getDynamicObject() == nullptr)
        return juce::Result::fail (tr ("세션 파일 형식이 아닙니다 (최상위가 객체가 아닙니다)"));

    const auto app = root.getProperty ("app", "").toString();

    if (app != "LiveMix")
        return juce::Result::fail (app.isEmpty() ? tr ("LiveMix 세션 파일이 아닙니다")
                                                 : trf ("다른 프로그램(\"{0}\")이 쓴 파일입니다. LiveMix 세션이 아닙니다", app));

    const int version = (int) root.getProperty ("version", 1);

    if (version > currentVersion)
        return juce::Result::fail (trf ("더 새로운 LiveMix로 저장한 세션입니다 (파일 버전 {0}, 이 버전은 {1}까지 읽습니다). LiveMix를 업데이트하세요.", juce::String (version), juce::String (currentVersion)));

    MixSession s;
    s.name = root.getProperty ("name", "").toString();
    s.nameChosen = (bool) root.getProperty ("nameChosen", false);   // no such key before 0.8.0: MixDocument::load decides

    if (const auto dev = root.getProperty ("device", juce::var()); dev.getDynamicObject() != nullptr)
    {
        if (version >= 4)
        {
            s.device.type = dev.getProperty ("type", "ASIO").toString();
            s.device.input = dev.getProperty ("input", "").toString();
            s.device.output = dev.getProperty ("output", "").toString();
        }
        else
        {
            s.device.type = "ASIO";
            s.device.input = s.device.output = dev.getProperty ("name", "").toString();
        }
        s.device.bufferSize = (int) dev.getProperty ("bufferSize", 256);
        s.device.sampleRate = (double) dev.getProperty ("sampleRate", 48000.0);
        s.device.sampleFormat = MixDevice::readSampleFormat (dev.getProperty ("sampleFormat", {}));
    }

    if (const auto* auxArray = root.getProperty ("auxes", juce::var()).getArray())
        for (const auto& item : *auxArray)
        {
            if (item.getDynamicObject() == nullptr)
            {
                if (warnings != nullptr)
                    warnings->add ("Skipped a malformed aux channel entry");
                continue;
            }

            MixAux a;
            a.id = juce::Uuid (item.getProperty ("id", "").toString());
            a.name = item.getProperty ("name", "").toString();
            a.on = (bool) item.getProperty ("on", true);
            // 0.24.0's aux device input is intentionally ignored; without mic sends the bus is silent.
            a.pan = (double) item.getProperty ("pan", 0.0);
            a.output = outputFromVar (item.getProperty ("output", juce::var()), MixOutput());
            a.chain = ProjectSerializer::pluginSlotsFromVar (item.getProperty ("chain", juce::var()));
            s.auxes.push_back (std::move (a));
        }

    if (const auto* fxArray = root.getProperty ("fx", juce::var()).getArray())
    {
        for (const auto& item : *fxArray)
        {
            if (item.getDynamicObject() == nullptr)
            {
                if (warnings != nullptr)
                    warnings->add ("Skipped a malformed FX channel entry");

                continue;
            }

            MixFx f;
            f.id = juce::Uuid (item.getProperty ("id", "").toString());
            f.name = item.getProperty ("name", "").toString();
            f.chain = ProjectSerializer::pluginSlotsFromVar (item.getProperty ("chain", juce::var()));
            f.returnAmount = (double) item.getProperty ("returnAmount", 1.0);
            f.mono = (bool) item.getProperty ("mono", false);
            f.on = (bool) item.getProperty ("on", true);   // absent from files before 0.14.0: an FX was always on
            f.muteGroup = (bool) item.getProperty ("muteGroup", false);
            f.output = outputFromVar (item.getProperty ("output", juce::var()), MixOutput());
            s.fx.push_back (std::move (f));
        }
    }

    if (const auto* channelArray = root.getProperty ("channels", juce::var()).getArray())
    {
        for (const auto& item : *channelArray)
        {
            if (item.getDynamicObject() == nullptr)
            {
                if (warnings != nullptr)
                    warnings->add ("Skipped a malformed channel entry");

                continue;
            }

            MixChannel c;
            c.id = juce::Uuid (item.getProperty ("id", "").toString());
            c.name = item.getProperty ("name", "").toString();
            c.on = (bool) item.getProperty ("on", true);
            c.muteGroup = (bool) item.getProperty ("muteGroup", false);
            c.pan = version >= 3 ? (double) item.getProperty ("pan", 0.0) : 0.0;

            if (const auto input = item.getProperty ("input", juce::var()); input.getDynamicObject() != nullptr)
            {
                c.inputFirst = (int) input.getProperty ("first", 0);
                c.stereo = (bool) input.getProperty ("stereo", false);
                // Ignore 0.24.0's input.aux and keep its copied device input, just like a 0.23.0 reader.
            }

            if (const auto* sends = item.getProperty ("auxSends", juce::var()).getArray())
                for (const auto& id : *sends)
                    c.auxSends.emplace_back (id.toString());

            c.chain = ProjectSerializer::pluginSlotsFromVar (item.getProperty ("chain", juce::var()));

            if (const auto* sends = item.getProperty ("sends", juce::var()).getArray())
                for (const auto& sv : *sends)
                    if (sv.getDynamicObject() != nullptr)
                        c.sends.push_back ({ juce::Uuid (sv.getProperty ("fx", "").toString()), (double) sv.getProperty ("amount", 0.0), (bool) sv.getProperty ("pre", false) });

            c.output = outputFromVar (item.getProperty ("output", juce::var()), MixOutput());

            c.pluginGroups = pluginGroupsFromVar (item.getProperty ("pluginGroups", juce::var()));

            s.channels.push_back (std::move (c));
        }
    }

    if (const auto m = root.getProperty ("master", juce::var()); m.getDynamicObject() != nullptr)
    {
        s.master.chain = ProjectSerializer::pluginSlotsFromVar (m.getProperty ("chain", juce::var()));
        s.master.pluginGroups = pluginGroupsFromVar (m.getProperty ("pluginGroups", juce::var()));
        s.master.outputFirst = (int) m.getProperty ("outputFirst", 0);
        s.master.sendToObs = version >= 4 && (bool) m.getProperty ("sendToObs", false);
    }

    const auto channelsBefore = s.channels.size();
    const auto auxesBefore = s.auxes.size();
    const auto fxBefore = s.fx.size();
    bool chainOverLimit = (int) s.master.chain.size() > maxChainSlots;

    for (const auto& c : s.channels)
        chainOverLimit = chainOverLimit || (int) c.chain.size() > maxChainSlots;

    for (const auto& a : s.auxes)
        chainOverLimit = chainOverLimit || (int) a.chain.size() > maxChainSlots;

    for (const auto& f : s.fx)
        chainOverLimit = chainOverLimit || (int) f.chain.size() > maxChainSlots;

    s.sanitise();

    for (auto& c : s.channels)
        cleanGroupMembers (c.pluginGroups, c.chain);
    cleanGroupMembers (s.master.pluginGroups, s.master.chain);

    if (warnings != nullptr && chainOverLimit)
        warnings->add (tr ("체인에 플러그인이 16개보다 많아 뒤의 것은 뺐습니다"));

    if (warnings != nullptr && (s.channels.size() != channelsBefore || s.auxes.size() != auxesBefore || s.fx.size() != fxBefore))
        warnings->add ("The session had more channels than LiveMix supports: the extra ones were dropped");

    out = std::move (s);
    return juce::Result::ok();
}

juce::Result MixSession::save (const juce::File& file) const
{
    const auto json = toJson();
    const auto bytes = (juce::int64) json.getNumBytesAsUTF8();

    if (bytes > maxFileBytes)   // what load() refuses must not be written: the file would never open again
        return juce::Result::fail (trf ("세션이 너무 커서 저장하지 않았습니다 ({0}): 플러그인 설정이 LiveMix가 다시 열 수 있는 크기(32 MB)를 넘습니다", juce::File::descriptionOfSizeInBytes (bytes)));

    return SafeFileWrite::writeTextVerified (file, json, [] (const juce::String& readBack) -> juce::Result
    {
        MixSession check;
        return fromJson (readBack, check);
    });
}

juce::Result MixSession::checkFileSize (juce::int64 bytes)
{
    if (bytes > maxFileBytes)
        return juce::Result::fail (trf ("세션 파일이 너무 큽니다 ({0}): 세션 파일이 아닙니다", juce::File::descriptionOfSizeInBytes (bytes)));

    return juce::Result::ok();
}

juce::Result MixSession::load (const juce::File& file, MixSession& out, juce::StringArray* warnings)
{
    if (! file.existsAsFile())
        return juce::Result::fail (trf ("파일이 없습니다: {0}", file.getFullPathName()));

    if (const auto size = checkFileSize (file.getSize()); size.failed())
        return size;   // not read into memory at all

    return fromJson (file.loadFileAsString(), out, warnings);
}

} // namespace gocue::livemix
