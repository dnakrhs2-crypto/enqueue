#include "MarkerExport.h"
#include <algorithm>

namespace gocue::recorder
{
juce::String markerExportText(const RecorderProject& project)
{
    if (project.Fs == 0) return {};
    std::vector<const Marker*> sorted;
    for (const auto& marker : project.markers) sorted.push_back(&marker);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Marker* a, const Marker* b)
    { return a->sample == b->sample ? a->name < b->name : a->sample < b->sample; });
    juce::String text;
    for (const auto* marker : sorted)
    {
        const auto seconds = (std::max)(Sample{0}, marker->sample) / project.Fs;
        const auto time = seconds < 3600
            ? juce::String::formatted("%02lld:%02lld", seconds / 60, seconds % 60)
            : juce::String::formatted("%lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
        const auto name = marker->name.replace("\r\n", " ")
            .replaceCharacters(juce::String::fromUTF8("\r\n\v\f\u0085\u2028\u2029"), "       ");
        text += time + " " + name + "\r\n";
    }
    return text;
}
}
