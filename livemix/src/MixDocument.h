#pragma once

#include "MixEngine.h"
#include "MixModel.h"

#include <atomic>
#include <functional>

namespace gocue::livemix
{

/** The open session: the model, its file, the dirty flag, and every edit (each one also reaches the engine live).
    Structure changes (channels / FX added, removed, reordered, a file loaded) fire onStructureChanged; value changes
    fire onValueChanged. Both on the message thread. */
class MixDocument
{
public:
    explicit MixDocument (MixEngine& engine);

    MixSession& getSession() noexcept { return session; }
    const MixSession& getSession() const noexcept { return session; }
    MixEngine& getEngine() noexcept { return engine; }

    const juce::File& getFile() const noexcept { return file; }
    bool hasFile() const noexcept { return file != juce::File(); }
    bool isDirty() const noexcept { return dirty.load (std::memory_order_acquire); }   // any thread (the updater asks from its own)
    juce::String getDisplayName() const;

    /** A blank session (one mic channel, one FX channel). */
    void newSession();
    /** Puts the session into the engine's graph. The constructor does not: nothing reaches the outputs until the
        saved session (or a new one) is really in. */
    void applyToEngine();
    /** 'warnings' gets what the parser had to skip; plugin restore errors go to 'pluginErrors' (or to 'warnings' when null). */
    juce::Result load (const juce::File& file, juce::StringArray* warnings = nullptr, juce::StringArray* pluginErrors = nullptr);
    juce::Result save (const juce::File& file);
    juce::Result saveIfPossible();   // to the current file (no-op without one)

    // structure
    juce::Uuid addChannel();
    void removeChannel (const juce::Uuid& id);
    juce::Uuid addFx();
    void removeFx (const juce::Uuid& id);

    // values (applied to the engine at once)
    void renameChannel (const juce::Uuid& id, const juce::String& name);
    void setChannelOn (const juce::Uuid& id, bool on);
    void setAllChannelsOn (bool on);
    void setChannelInput (const juce::Uuid& id, int first, bool stereo);
    void setChannelOutput (const juce::Uuid& id, const MixOutput& output);
    void setSend (const juce::Uuid& channelId, const juce::Uuid& fxId, double amount, bool pre);
    void renameFx (const juce::Uuid& id, const juce::String& name);
    void setFxReturn (const juce::Uuid& id, double amount);
    void setFxMono (const juce::Uuid& id, bool mono);
    void setFxOutput (const juce::Uuid& id, const MixOutput& output);
    void setMasterOutput (int first);
    void setSessionName (const juce::String& name);
    void setDeviceInfo (const juce::String& name, int bufferSize, double sampleRate);

    /** A plugin chain was edited live (the engine is the truth): the file is out of date. 'refreshViews' false is for
        a parameter turned in a plugin editor: the views need no refresh, only the dirty state (announced once). */
    void markDirty (bool refreshViews = true);
    /** Asks every chain whether a plugin reported a parameter / state change since the last poll; true (and dirty)
        when one did. The timer polls; anything that decides on the dirty state asks first. */
    bool pollPluginEdits();

    std::function<void()> onStructureChanged;
    std::function<void()> onValueChanged;

private:
    void structureChanged();   // an edit: dirty, then announced
    void valueChanged();
    void notifyStructure();    // announced only (a load / a new session are clean)
    void notifyValue();

    MixEngine& engine;
    MixSession session;
    juce::File file;
    std::atomic<bool> dirty { false };
};

} // namespace gocue::livemix
