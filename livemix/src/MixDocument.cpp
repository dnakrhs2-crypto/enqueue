#include "MixDocument.h"

namespace gocue::livemix
{

MixDocument::MixDocument (MixEngine& e) : engine (e)
{
    newSession();
}

juce::String MixDocument::getDisplayName() const
{
    if (session.name.isNotEmpty())
        return session.name;

    return hasFile() ? file.getFileNameWithoutExtension() : juce::String::fromUTF8 ("새 세션");
}

void MixDocument::newSession()
{
    MixSession fresh;
    fresh.name = juce::String::fromUTF8 ("새 세션");
    fresh.addFx (juce::String::fromUTF8 ("리버브"));
    fresh.addChannel();
    session = std::move (fresh);
    file = juce::File();
    dirty = false;
    engine.applySession (session, nullptr, true);
    notifyStructure();
}

juce::Result MixDocument::load (const juce::File& newFile, juce::StringArray* warnings)
{
    MixSession loaded;
    const auto result = MixSession::load (newFile, loaded, warnings);

    if (result.failed())
        return result;

    session = std::move (loaded);
    file = newFile;
    dirty = false;
    juce::StringArray pluginErrors;
    engine.applySession (session, &pluginErrors, true);

    if (warnings != nullptr)
        warnings->addArray (pluginErrors);

    notifyStructure();
    return juce::Result::ok();
}

juce::Result MixDocument::save (const juce::File& newFile)
{
    engine.captureLivePluginStates (session);
    session.sanitise();
    const auto result = session.save (newFile);

    if (result.failed())
        return result;

    file = newFile;
    dirty = false;
    notifyValue();   // the title and the views: clean now
    return juce::Result::ok();
}

juce::Result MixDocument::saveIfPossible()
{
    if (! hasFile())
        return juce::Result::fail ("no file");

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
    const bool wasDirty = dirty;
    dirty = true;

    if (refreshViews || ! wasDirty)
        notifyValue();
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
