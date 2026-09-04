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

    double finiteOr (double value, double fallback) { return std::isfinite (value) ? value : fallback; }
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
    c.name = newName.isNotEmpty() ? newName : juce::String::fromUTF8 ("마이크 ") + juce::String ((int) channels.size() + 1);

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
    if ((int) channels.size() > maxChannels)
        channels.resize ((size_t) maxChannels);

    if ((int) fx.size() > maxFx)
        fx.resize ((size_t) maxFx);

    auto capChain = [] (std::vector<PluginSlotState>& chain)
    {
        if ((int) chain.size() > maxChainSlots)
            chain.resize ((size_t) maxChainSlots);
    };

    for (auto& c : channels)
        capChain (c.chain);

    for (auto& f : fx)
        capChain (f.chain);

    capChain (master.chain);

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
            c.name = juce::String::fromUTF8 ("마이크 ") + juce::String ((int) i + 1);

        c.inputFirst = juce::jlimit (0, maxDeviceChannels - 1, c.inputFirst);

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

    master.outputFirst = juce::jlimit (0, maxDeviceChannels - 2, master.outputFirst);
    device.bufferSize = juce::jlimit (16, 8192, device.bufferSize);
    device.sampleRate = juce::jlimit (8000.0, 384000.0, finiteOr (device.sampleRate, 48000.0));
}

juce::String MixSession::toJson() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("app", "LiveMix");
    root->setProperty ("version", currentVersion);
    root->setProperty ("name", name);

    auto* dev = new juce::DynamicObject();
    dev->setProperty ("name", device.name);
    dev->setProperty ("bufferSize", device.bufferSize);
    dev->setProperty ("sampleRate", device.sampleRate);
    root->setProperty ("device", dev);

    juce::Array<juce::var> channelArray;

    for (const auto& c : channels)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id", idText (c.id));
        obj->setProperty ("name", c.name);
        obj->setProperty ("on", c.on);
        obj->setProperty ("muteGroup", c.muteGroup);
        auto* input = new juce::DynamicObject();
        input->setProperty ("first", c.inputFirst);
        input->setProperty ("stereo", c.stereo);
        obj->setProperty ("input", input);
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
        channelArray.add (obj);
    }

    root->setProperty ("channels", channelArray);

    juce::Array<juce::var> fxArray;

    for (const auto& f : fx)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id", idText (f.id));
        obj->setProperty ("name", f.name);
        obj->setProperty ("chain", ProjectSerializer::pluginSlotsToVar (f.chain));
        obj->setProperty ("returnAmount", f.returnAmount);
        obj->setProperty ("mono", f.mono);
        obj->setProperty ("muteGroup", f.muteGroup);
        obj->setProperty ("output", outputToVar (f.output));
        fxArray.add (obj);
    }

    root->setProperty ("fx", fxArray);

    auto* m = new juce::DynamicObject();
    m->setProperty ("chain", ProjectSerializer::pluginSlotsToVar (master.chain));
    m->setProperty ("outputFirst", master.outputFirst);
    root->setProperty ("master", m);

    return juce::JSON::toString (juce::var (root), false);
}

juce::Result MixSession::fromJson (const juce::String& json, MixSession& out, juce::StringArray* warnings)
{
    if (json.trim().isEmpty())
        return juce::Result::fail (juce::String::fromUTF8 ("세션 파일이 비어 있습니다"));

    // JUCE's parser stops after the first object: corruption behind it would be read as a good file and then written over
    if (! json.trimEnd().endsWithChar ('}') || ProjectSerializer::hasTrailingJsonData (json))
        return juce::Result::fail (juce::String::fromUTF8 ("세션 파일이 손상됐습니다 (세션이 끝난 뒤에 데이터가 더 있습니다)"));

    juce::var root;
    const auto parsed = juce::JSON::parse (json, root);

    if (parsed.failed())
        return juce::Result::fail (juce::String::fromUTF8 ("세션 파일을 읽을 수 없습니다 (JSON): ") + parsed.getErrorMessage());

    if (root.getDynamicObject() == nullptr)
        return juce::Result::fail (juce::String::fromUTF8 ("세션 파일 형식이 아닙니다 (최상위가 객체가 아닙니다)"));

    const auto app = root.getProperty ("app", "").toString();

    if (app != "LiveMix")
        return juce::Result::fail (app.isEmpty() ? juce::String::fromUTF8 ("LiveMix 세션 파일이 아닙니다")
                                                 : juce::String::fromUTF8 ("다른 프로그램(\"") + app + juce::String::fromUTF8 ("\")이 쓴 파일입니다. LiveMix 세션이 아닙니다"));

    const int version = (int) root.getProperty ("version", 1);

    if (version > currentVersion)
        return juce::Result::fail (juce::String::fromUTF8 ("더 새로운 LiveMix로 저장한 세션입니다 (파일 버전 ") + juce::String (version)
                                   + juce::String::fromUTF8 (", 이 버전은 ") + juce::String (currentVersion) + juce::String::fromUTF8 ("까지 읽습니다). LiveMix를 업데이트하세요."));

    MixSession s;
    s.name = root.getProperty ("name", "").toString();

    if (const auto dev = root.getProperty ("device", juce::var()); dev.getDynamicObject() != nullptr)
    {
        s.device.name = dev.getProperty ("name", "").toString();
        s.device.bufferSize = (int) dev.getProperty ("bufferSize", 256);
        s.device.sampleRate = (double) dev.getProperty ("sampleRate", 48000.0);
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

            if (const auto input = item.getProperty ("input", juce::var()); input.getDynamicObject() != nullptr)
            {
                c.inputFirst = (int) input.getProperty ("first", 0);
                c.stereo = (bool) input.getProperty ("stereo", false);
            }

            c.chain = ProjectSerializer::pluginSlotsFromVar (item.getProperty ("chain", juce::var()));

            if (const auto* sends = item.getProperty ("sends", juce::var()).getArray())
                for (const auto& sv : *sends)
                    if (sv.getDynamicObject() != nullptr)
                        c.sends.push_back ({ juce::Uuid (sv.getProperty ("fx", "").toString()), (double) sv.getProperty ("amount", 0.0), (bool) sv.getProperty ("pre", false) });

            c.output = outputFromVar (item.getProperty ("output", juce::var()), MixOutput());
            s.channels.push_back (std::move (c));
        }
    }

    if (const auto m = root.getProperty ("master", juce::var()); m.getDynamicObject() != nullptr)
    {
        s.master.chain = ProjectSerializer::pluginSlotsFromVar (m.getProperty ("chain", juce::var()));
        s.master.outputFirst = (int) m.getProperty ("outputFirst", 0);
    }

    const auto channelsBefore = s.channels.size();
    const auto fxBefore = s.fx.size();
    bool chainOverLimit = (int) s.master.chain.size() > maxChainSlots;

    for (const auto& c : s.channels)
        chainOverLimit = chainOverLimit || (int) c.chain.size() > maxChainSlots;

    for (const auto& f : s.fx)
        chainOverLimit = chainOverLimit || (int) f.chain.size() > maxChainSlots;

    s.sanitise();

    if (warnings != nullptr && chainOverLimit)
        warnings->add (juce::String::fromUTF8 ("체인에 플러그인이 16개보다 많아 뒤의 것은 뺐습니다"));

    if (warnings != nullptr && (s.channels.size() != channelsBefore || s.fx.size() != fxBefore))
        warnings->add ("The session had more channels than LiveMix supports: the extra ones were dropped");

    out = std::move (s);
    return juce::Result::ok();
}

juce::Result MixSession::save (const juce::File& file) const
{
    const auto json = toJson();
    const auto bytes = (juce::int64) json.getNumBytesAsUTF8();

    if (bytes > maxFileBytes)   // what load() refuses must not be written: the file would never open again
        return juce::Result::fail (juce::String::fromUTF8 ("세션이 너무 커서 저장하지 않았습니다 (") + juce::File::descriptionOfSizeInBytes (bytes)
                                   + juce::String::fromUTF8 ("): 플러그인 설정이 LiveMix가 다시 열 수 있는 크기(32 MB)를 넘습니다"));

    return SafeFileWrite::writeTextVerified (file, json, [] (const juce::String& readBack) -> juce::Result
    {
        MixSession check;
        return fromJson (readBack, check);
    });
}

juce::Result MixSession::checkFileSize (juce::int64 bytes)
{
    if (bytes > maxFileBytes)
        return juce::Result::fail (juce::String::fromUTF8 ("세션 파일이 너무 큽니다 (") + juce::File::descriptionOfSizeInBytes (bytes)
                                   + juce::String::fromUTF8 ("): 세션 파일이 아닙니다"));

    return juce::Result::ok();
}

juce::Result MixSession::load (const juce::File& file, MixSession& out, juce::StringArray* warnings)
{
    if (! file.existsAsFile())
        return juce::Result::fail (juce::String::fromUTF8 ("파일이 없습니다: ") + file.getFullPathName());

    if (const auto size = checkFileSize (file.getSize()); size.failed())
        return size;   // not read into memory at all

    return fromJson (file.loadFileAsString(), out, warnings);
}

} // namespace gocue::livemix
