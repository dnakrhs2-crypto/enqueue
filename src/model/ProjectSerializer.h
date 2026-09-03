#pragma once

#include "model/AudioPatch.h"
#include "model/Cue.h"
#include "model/WorkspaceSettings.h"

namespace gocue
{

/** Everything that goes into a project file. */
/** One cue list or cart (QLab: cue lists and cue carts live side by side in a workspace). */
struct CueContainer
{
    juce::Uuid id;
    juce::String name;
    bool isCart = false;        // a cart: a grid of buttons instead of a list, no playhead / sequences
    int cartRows = 4, cartCols = 4;
    std::vector<Cue> cues;

    static constexpr int maxGrid = 15;

    void sanitise();
};

struct Project
{
    juce::String name;
    std::vector<CueContainer> lists;   // never empty after ensureMainList(): lists[0] is the main cue list
    int activeList = 0;                // the list / cart shown when the project opens
    std::vector<PluginSlotState> masterPlugins;
    std::vector<AudioPatch> patches;   // never empty after fromJson / ensureDefaultPatch: the first one is the default
    WorkspaceSettings settings;

    /** Adds the main list when there is none. Returns it. */
    CueContainer& ensureMainList();
    /** The main list's cues (lists[0]); the non-const version creates the main list when needed. */
    std::vector<Cue>& cues();
    const std::vector<Cue>& cues() const;
    /** Every cue of every list, in list order (a cue id is unique across lists). */
    const Cue* findCue (const juce::Uuid& id) const noexcept;

    /** Adds the default patch when the list is empty. Returns the default (first) patch. */
    AudioPatch& ensureDefaultPatch();
    const AudioPatch* findPatch (const juce::Uuid& id) const noexcept;
    /** The cue's patch, or the default patch when the cue's id is null / unknown. Null only while 'patches' is empty. */
    const AudioPatch* patchForCue (const Cue& cue) const noexcept;
};

/** JSON <-> Project.
    Tolerant by design: unknown fields are ignored, missing fields take their
    defaults, and a file written by a newer version only produces a warning. */
namespace ProjectSerializer
{
    /** 1: cues with fadeInMs / fadeOutMs / gainDb.  2: "audio" object (trim, loops, rate, envelope); fadeInMs migrates to the envelope.
        3: cue list fields (number, colours, waits, continue mode, hotkey, wall clock, fade-stop-others, duck), settings (template, row size). */
    constexpr int currentVersion = 5;   // 4: levels / trim / patches / audition / fade / devamp / slices / groups / control cues. 5: cue lists / carts ("lists")
    constexpr const char* fileExtension = ".enqueue";          // 0.9.0: the app was renamed from GoCue
    constexpr const char* openableExtensions = ".enqueue;.gocue";   // projects from before the rename still open

    /** @param projectDir  when valid, a project-relative path is stored next to each
                           absolute file path so projects survive being moved. */
    juce::var toVar (const Project& project, const juce::File& projectDir = {});
    juce::String toJson (const Project& project, const juce::File& projectDir = {});

    /** Fails only for unparsable input. Missing / bad fields fall back to defaults and
        (where useful) add a line to 'warnings'. */
    juce::Result fromJson (const juce::String& json, Project& out,
                           juce::StringArray* warnings = nullptr,
                           const juce::File& projectDir = {});

    juce::Result save (const Project& project, const juce::File& file);
    juce::Result load (const juce::File& file, Project& out, juce::StringArray* warnings = nullptr);
}

} // namespace gocue
