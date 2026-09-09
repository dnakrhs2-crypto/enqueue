#pragma once
#include "export/ExportController.h"
#include "support/Platform.h"

namespace gocue::recorder::probe
{
// Probe output is scratch owned by this run. Never recursively delete a supplied
// directory (it may contain source media or a previous completed delivery).
inline juce::File prepareOutputRoot(const juce::File& requested)
{
    const auto root = ExportController::resolveDestination(requested);
    exportCheck(root.getParentDirectory().createDirectory());
    exportRequire(CreateDirectoryW(root.getFullPathName().toWideCharPointer(), nullptr) != FALSE,
                  "Cannot exclusively create probe output directory");
    return root;
}
inline std::array<int, 8> physicalInputs(const juce::String& text)
{
    std::array<int, 8> result{-1,-1,-1,-1,-1,-1,-1,-1};
    if (text == "none") return result;
    const auto tokens = juce::StringArray::fromTokens(text, ",", "");
    exportRequire(!tokens.isEmpty() && tokens.size() <= 8 && !text.startsWithChar(',') && !text.endsWithChar(',')
        && !text.contains(",,"), "--inputs requires 1..8 distinct physical channels or none");
    for (int i = 0; i < tokens.size(); ++i)
    {
        exportRequire(tokens[i].isNotEmpty() && tokens[i].containsOnly("0123456789") && tokens[i].length() <= 3,
                      "Invalid physical input number");
        const auto channel = tokens[i].getIntValue();
        exportRequire(channel >= 1 && channel <= 256, "Physical input must be 1..256");
        for (int j = 0; j < i; ++j) exportRequire(result[std::size_t(j)] != channel - 1, "Duplicate physical input");
        result[std::size_t(i)] = channel - 1;
    }
    return result;
}
}
