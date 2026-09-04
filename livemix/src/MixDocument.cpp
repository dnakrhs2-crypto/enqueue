#include "MixDocument.h"

namespace gocue::livemix
{

namespace
{
    MixSession defaultSession()
    {
        MixSession fresh;
        fresh.name = juce::String::fromUTF8 ("새 세션");
        fresh.addFx (juce::String::fromUTF8 ("리버브"));
        fresh.addChannel();
        return fresh;
    }
}

MixDocument::MixDocument (MixEngine& e) : engine (e)
{
    // the model only: the graph stays empty until a session is applied, so no raw microphone reaches the outputs
    // while the saved session (and its plugins) is still loading
    session = defaultSession();
}

void MixDocument::applyToEngine()
{
    engine.applySession (session, nullptr, true);
    graphApplied = true;
    notifyStructure();   // the views (and the chain listeners that close a removed plugin's editor) learn of the graph
}

juce::String MixDocument::getDisplayName() const
{
    if (session.name.isNotEmpty())
        return session.name;

    return hasFile() ? file.getFileNameWithoutExtension() : juce::String::fromUTF8 ("새 세션");
}

void MixDocument::newSession()
{
    session = defaultSession();
    file = juce::File();
    dirty = false;
    engine.applySession (session, nullptr, true);
    graphApplied = true;
    notifyStructure();
    notifyValue();   // the window title and the values shown
}

juce::Result MixDocument::load (const juce::File& newFile, juce::StringArray* warnings, juce::StringArray* pluginErrors)
{
    MixSession loaded;
    const auto result = MixSession::load (newFile, loaded, warnings);

    if (result.failed())
        return result;

    session = std::move (loaded);
    file = newFile;
    dirty = false;
    juce::StringArray restoreErrors;
    engine.applySession (session, &restoreErrors, true);
    graphApplied = true;

    if (pluginErrors != nullptr)
        pluginErrors->addArray (restoreErrors);
    else if (warnings != nullptr)
        warnings->addArray (restoreErrors);

    notifyStructure();
    notifyValue();   // the window title (it listens to value changes only) and the values shown
    return juce::Result::ok();
}

juce::Result MixDocument::save (const juce::File& newFile)
{
    for (int attempt = 0;; ++attempt)
    {
        pollPluginEdits();   // edits reported so far are captured below (and keep the document dirty should the write fail)

        if (! engine.captureLivePluginStates (session))
            return juce::Result::fail (juce::String::fromUTF8 ("플러그인 설정을 읽지 못해 저장하지 않았습니다 (플러그인이 오류를 냈습니다). 다시 시도하거나 그 플러그인을 체인에서 빼세요."));

        session.sanitise();
        const auto result = session.save (newFile);

        if (result.failed())
            return result;

        file = newFile;
        dirty = false;

        // an edit that arrived during the capture / write is not in the file: written again, twice at most (a knob
        // being turned right now keeps the document dirty, and the title says so)
        if (! pollPluginEdits())
            break;

        if (attempt >= 2)
        {
            // three writes and a knob is still moving: the file is a moment behind, and the document says so
            dirty = true;
            notifyValue();
            return juce::Result::fail (juce::String::fromUTF8 ("저장하는 동안 플러그인 값이 계속 바뀌어 마지막 변경이 파일에 없습니다. 잠시 뒤 다시 저장해 주세요."));
        }
    }

    notifyValue();   // the title and the views
    return juce::Result::ok();
}

juce::Result MixDocument::saveIfPossible()
{
    if (! hasFile())
        return juce::Result::fail (juce::String::fromUTF8 ("저장할 파일이 정해지지 않았습니다"));

    return save (file);
}

//==============================================================================
juce::Uuid MixDocument::addChannel()
{
    const int index = session.addChannel();

    if (index < 0)
        return juce::Uuid::null();

    engine.applySession (session);
    structureChanged();
    return session.channels[(size_t) index].id;
}

void MixDocument::removeChannel (const juce::Uuid& id)
{
    session.removeChannel (id);
    engine.applySession (session);
    structureChanged();
}

juce::Uuid MixDocument::addFx()
{
    const int index = session.addFx();

    if (index < 0)
        return juce::Uuid::null();

    engine.applySession (session);
    structureChanged();
    return session.fx[(size_t) index].id;
}

void MixDocument::removeFx (const juce::Uuid& id)
{
    session.removeFx (id);
    engine.applySession (session);
    structureChanged();
}

//==============================================================================
void MixDocument::renameChannel (const juce::Uuid& id, const juce::String& name)
{
    if (auto* c = session.findChannel (id))
    {
        c->name = name.trim().isNotEmpty() ? name.trim() : c->name;
        valueChanged();
    }
}

void MixDocument::setChannelOn (const juce::Uuid& id, bool on)
{
    if (auto* c = session.findChannel (id))
    {
        c->on = on;
        engine.setChannelOn (id, on);
        valueChanged();
    }
}

void MixDocument::setAllChannelsOn (bool on)
{
    for (auto& c : session.channels)
    {
        c.on = on;
        engine.setChannelOn (c.id, on);
    }

    valueChanged();
}

void MixDocument::setChannelInput (const juce::Uuid& id, int first, bool stereo)
{
    if (auto* c = session.findChannel (id))
    {
        c->inputFirst = juce::jlimit (0, MixSession::maxDeviceChannels - (stereo ? 2 : 1), first);
        c->stereo = stereo;
        engine.setChannelInput (id, c->inputFirst, stereo);
        valueChanged();
    }
}

void MixDocument::setChannelOutput (const juce::Uuid& id, const MixOutput& output)
{
    if (auto* c = session.findChannel (id))
    {
        c->output = output;
        c->output.directFirst = juce::jlimit (0, MixSession::maxDeviceChannels - 2, output.directFirst);
        engine.setChannelOutput (id, c->output);
        valueChanged();
    }
}

void MixDocument::setSend (const juce::Uuid& channelId, const juce::Uuid& fxId, double amount, bool pre)
{
    if (auto* c = session.findChannel (channelId))
    {
        auto& s = session.sendFor (*c, fxId);
        s.amount = juce::jlimit (0.0, 1.0, amount);
        s.pre = pre;
        engine.setSend (channelId, fxId, s.amount, pre);
        valueChanged();
    }
}

void MixDocument::renameFx (const juce::Uuid& id, const juce::String& name)
{
    if (auto* f = session.findFx (id))
    {
        f->name = name.trim().isNotEmpty() ? name.trim() : f->name;
        valueChanged();
    }
}

void MixDocument::setFxReturn (const juce::Uuid& id, double amount)
{
    if (auto* f = session.findFx (id))
    {
        f->returnAmount = juce::jlimit (0.0, 1.0, amount);
        engine.setFxReturn (id, f->returnAmount);
        valueChanged();
    }
}

void MixDocument::setFxMono (const juce::Uuid& id, bool mono)
{
    if (auto* f = session.findFx (id))
    {
        f->mono = mono;
        engine.setFxMono (id, mono);
        valueChanged();
    }
}

void MixDocument::setChannelMuteGroup (const juce::Uuid& id, bool inGroup)
{
    if (auto* c = session.findChannel (id))
    {
        c->muteGroup = inGroup;
        valueChanged();
    }
}

void MixDocument::setFxMuteGroup (const juce::Uuid& id, bool inGroup)
{
    if (auto* f = session.findFx (id))
    {
        f->muteGroup = inGroup;
        valueChanged();
    }
}

void MixDocument::setFxOutput (const juce::Uuid& id, const MixOutput& output)
{
    if (auto* f = session.findFx (id))
    {
        f->output = output;
        f->output.directFirst = juce::jlimit (0, MixSession::maxDeviceChannels - 2, output.directFirst);
        engine.setFxOutput (id, f->output);
        valueChanged();
    }
}

void MixDocument::setMasterOutput (int first)
{
    session.master.outputFirst = juce::jlimit (0, MixSession::maxDeviceChannels - 2, first);
    engine.setMasterOutput (session.master.outputFirst);
    valueChanged();
}

void MixDocument::setSessionName (const juce::String& name)
{
    session.name = name.trim();
    valueChanged();
}

void MixDocument::setDeviceInfo (const juce::String& name, int bufferSize, double sampleRate)
{
    if (session.device.name == name && session.device.bufferSize == bufferSize && juce::approximatelyEqual (session.device.sampleRate, sampleRate))
        return;

    session.device.name = name;
    session.device.bufferSize = bufferSize;
    session.device.sampleRate = sampleRate;
    valueChanged();
}

void MixDocument::markDirty (bool refreshViews)
{
    const bool wasDirty = dirty.exchange (true, std::memory_order_acq_rel);

    if (refreshViews || ! wasDirty)
        notifyValue();
}

bool MixDocument::pollPluginEdits()
{
    bool edited = false;
    engine.forEachChain ([&edited] (PluginChain& chain) { if (chain.consumeStateChanged()) edited = true; });

    if (edited)
        markDirty (false);

    return edited;
}

void MixDocument::structureChanged()
{
    dirty = true;
    notifyStructure();
}

void MixDocument::valueChanged()
{
    dirty = true;
    notifyValue();
}

void MixDocument::notifyStructure()
{
    if (onStructureChanged)
        onStructureChanged();
}

void MixDocument::notifyValue()
{
    if (onValueChanged)
        onValueChanged();
}

} // namespace gocue::livemix
