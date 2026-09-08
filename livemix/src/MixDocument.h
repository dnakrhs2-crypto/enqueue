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
    /** Runtime identity of this opening of the session. Message thread only; never written to the session file. */
    juce::Uuid getSessionGeneration() const noexcept { return sessionGeneration; }

    /** A blank session (one mic channel, one FX channel). */
    void newSession();
    /** Puts the session into the engine's graph. The constructor does not: nothing reaches the outputs until the
        saved session (or a new one) is really in. */
    void applyToEngine();
    /** A session went into the engine at least once (a startup whose every load failed still needs one). */
    bool hasAppliedGraph() const noexcept { return graphApplied; }
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
    void setChannelMuteGroup (const juce::Uuid& id, bool inGroup);   // the mark; MuteGroups applies the state
    void setChannelInput (const juce::Uuid& id, int first, bool stereo);
    void setChannelOutput (const juce::Uuid& id, const MixOutput& output);
    void setChannelPan (const juce::Uuid& id, double pan);
    void setSend (const juce::Uuid& channelId, const juce::Uuid& fxId, double amount, bool pre);
    void renameFx (const juce::Uuid& id, const juce::String& name);
    void setFxReturn (const juce::Uuid& id, double amount);
    void setFxMono (const juce::Uuid& id, bool mono);
    void setFxMuteGroup (const juce::Uuid& id, bool inGroup);
    void setFxOutput (const juce::Uuid& id, const MixOutput& output);
    void setMasterOutput (int first);

    /** A mic channel's plugin groups (MixPluginGroup): made, dropped, their members picked among the chain's slots
        (by slot id), and switched off / on - off bypasses every member in the live chain at once. */
    int addPluginGroup (const juce::Uuid& channelId);   // the new group's index, or -1 with maxPluginGroups there already
    void removePluginGroup (const juce::Uuid& channelId, int group);   // an OFF group's members come back on
    void setPluginGroupMember (const juce::Uuid& channelId, int group, const juce::Uuid& slotId, bool member);   // joining an OFF group switches the plugin off at once, leaving it switches it on
    void setPluginGroupOff (const juce::Uuid& channelId, int group, bool off);

    void setSessionName (const juce::String& name);
    void setDeviceInfo (const juce::String& name, int bufferSize, double sampleRate);

    /** A plugin chain was edited live (the engine is the truth): the file is out of date. 'refreshViews' false is for
        a parameter turned in a plugin editor: the views need no refresh, only the dirty state (announced once). */
    void markDirty (bool refreshViews = true);
    /** The operator discarded unsaved changes (a save that failed on quit, then "discard and continue"): the session
        is no longer dirty, so the shutdown save does not bring the discarded edits back. */
    void discardUnsavedChanges() noexcept { dirty.store (false, std::memory_order_release); }
    /** Asks every chain whether a plugin reported a parameter / state change since the last poll; true (and dirty)
        when one did. The timer polls; anything that decides on the dirty state asks first. */
    bool pollPluginEdits();

    std::function<void()> onStructureChanged;
    std::function<void()> onValueChanged;

private:
    void bypassSlot (const juce::Uuid& channelId, const juce::Uuid& slotId, bool bypass);   // the live chain's slot with that id (none: nothing)
    /** Another OFF group of the channel (not 'exceptGroup') holds the slot: switching one group on must not run it. */
    static bool heldOffElsewhere (const MixChannel& channel, const juce::Uuid& slotId, int exceptGroup);
    bool liveChainHas (const juce::Uuid& channelId, const juce::Uuid& slotId) const;
    void structureChanged();   // an edit: dirty, then announced
    void valueChanged();
    void notifyStructure();    // announced only (a load / a new session are clean)
    void notifyValue();

    MixEngine& engine;
    MixSession session;
    juce::Uuid sessionGeneration;   // Uuid's default constructor creates a fresh non-null identity
    juce::File file;
    std::atomic<bool> dirty { false };
    bool graphApplied = false;
};

} // namespace gocue::livemix
