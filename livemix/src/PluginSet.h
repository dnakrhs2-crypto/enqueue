#pragma once

#include "MixModel.h"

#include <array>
#include <optional>

namespace gocue::livemix
{

struct PluginSetSend
{
    juce::Uuid fx = juce::Uuid::null();
    juce::String name;   // the FX name at capture time, for matching across sessions
    double amount = 0.0;
    bool pre = false;
};

/** Five shared files, independent of sessions and the existing named plugin presets.
    An absent optional leaves that part of the destination channel alone; an empty groups array clears it. */
struct PluginSet
{
    int number = 1;
    juce::String source, savedAt;
    std::vector<PluginSlotState> plugins;
    std::optional<std::vector<PluginSetSend>> sends;
    std::optional<std::vector<MixPluginGroup>> groups;

    static constexpr int numSets = 5;
    static constexpr int currentVersion = 1;
    static constexpr int maxPlugins = MixSession::maxChainSlots;
    static constexpr juce::int64 maxFileBytes = 8 * 1024 * 1024;
    static constexpr const char* fileExtension = ".livemixset";

    juce::String toJson() const;
    static juce::Result fromJson (const juce::String& json, PluginSet& out);
    juce::Result save (const juce::File& target) const;
    static juce::Result load (const juce::File& file, PluginSet& out);
    static juce::File defaultFolder();
    /** Also used with a temporary documents root in tests; neither helper creates directories. */
    static juce::File folderIn (const juce::File& documents);
    static juce::File fileFor (int number, const juce::File& folder);
    juce::String summary() const;
};

struct PluginSetEntry
{
    enum class State { empty, ready, unreadable };
    State state = State::empty;
    PluginSet set;
    juce::String error;

    juce::String description() const;
    juce::String menuText (bool saving) const;
};

/** Always five entries, in slot order. A broken file remains available for overwriting. */
std::array<PluginSetEntry, PluginSet::numSets> listPluginSets (const juce::File& folder);

/** Pure capture from an already complete live state read. No engine, filesystem, clock or UI access.
    Without groups, only the bypasses attributable to OFF groups are released. */
PluginSet capturePluginSet (int number, const juce::String& source, const juce::String& savedAt,
                            std::vector<PluginSlotState> states, const MixChannel* channel,
                            const std::vector<MixFx>& fx, bool includeSends, bool includeGroups);

struct PluginSetSendMatches
{
    std::vector<MixSend> sends;
    juce::StringArray warnings;
};

/** Reserve exact IDs first, then consume unused exact-name matches in session order. */
PluginSetSendMatches matchPluginSetSends (const std::vector<PluginSetSend>& sends, const std::vector<MixFx>& fx);

struct PluginSetApplication
{
    std::vector<PluginSlotState> plugins;   // OFF groups already applied before restore publishes these slots
    std::optional<std::vector<MixPluginGroup>> groups;   // replace only when present
    PluginSetSendMatches sends;
};

/** Pure application plan. Null channel means FX/master: plugins only. New groups admit only members
    in the incoming chain, once per group, and at most five groups. Absent groups retain the old groups. */
PluginSetApplication planPluginSet (const PluginSet& set, const MixChannel* channel, const std::vector<MixFx>& fx);

} // namespace gocue::livemix
