#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <map>
#include <vector>

namespace gocue
{
struct PluginRuntimeEnvironment
{
    juce::File windowsSystemDirectory, applicationDirectory, programFilesX86, programFiles, programData;
    static PluginRuntimeEnvironment current();
};

struct PluginRuntimeEvidence
{
    juce::String dll;
    juce::File importedBy;
};

struct PluginRuntimePackage
{
    juce::String name, url;
    juce::StringArray dlls;
    std::vector<PluginRuntimeEvidence> evidence;
};

struct PluginRuntimeResult
{
    juce::String file, format;
    std::vector<PluginRuntimePackage> packages;
    juce::int64 elapsedMs = 0;
};

/** Reads PE metadata only. Invalid/non-x64 images have no imports; never executes plugin code. */
juce::StringArray readX64PeImports (const juce::File&, juce::int64* bytesRead = nullptr);
const std::vector<PluginRuntimePackage>& pluginRuntimePackages();

/** One scan/batch owns one instance, so multiple WaveShells reuse the same import cache.
    Use on a background thread. The environment and limits are injectable for temporary fixtures. */
class PluginRuntimeCheck
{
public:
    struct Options
    {
        PluginRuntimeEnvironment environment = PluginRuntimeEnvironment::current();
        int maxFiles = 20000;
        int maxMilliseconds = 20000;
        std::function<void (const juce::File&)> onFileRead;
    };
    explicit PluginRuntimeCheck (Options);
    PluginRuntimeResult check (const juce::String& file, const juce::String& format,
                               const std::function<bool()>& cancelled = {});

private:
    Options options;
    std::map<juce::String, juce::StringArray> imports;
};
} // namespace gocue
