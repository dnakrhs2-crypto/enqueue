#pragma once

#include "model/Cue.h"

namespace gocue
{

/** Everything that goes into a project file. */
struct Project
{
    juce::String name;
    std::vector<Cue> cues;
    std::vector<PluginSlotState> masterPlugins;
};

/** JSON <-> Project.
    Tolerant by design: unknown fields are ignored, missing fields take their
    defaults, and a file written by a newer version only produces a warning. */
namespace ProjectSerializer
{
    constexpr int currentVersion = 1;
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
