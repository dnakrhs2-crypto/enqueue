#pragma once

#include "model/AudioPatch.h"
#include "model/Cue.h"
#include "model/WorkspaceSettings.h"

namespace gocue
{

/** Everything that goes into a project file. */
struct Project
{
    juce::String name;
    std::vector<Cue> cues;
    std::vector<PluginSlotState> masterPlugins;
    std::vector<AudioPatch> patches;   // never empty after fromJson / ensureDefaultPatch: the first one is the default
    WorkspaceSettings settings;

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
    constexpr int currentVersion = 4;   // 4: levels / trim / patches / audition / fade / devamp / slices / groups / control cues
    constexpr const char* fileExtension = ".gocue";

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
