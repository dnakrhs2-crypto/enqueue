#include "AppRestart.h"
#include <limits>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::livemix
{
std::optional<std::uint32_t> restartAfterPid (const juce::String& commandLine)
{
    const juce::ArgumentList args ("LiveMix", commandLine);
    std::optional<std::uint32_t> pid;
    for (int i = 0; i < args.size(); ++i)
    {
        if (args[i].text != "--restart-after") continue;
        if (pid || ++i >= args.size()) return std::nullopt;
        const auto value = args[i].text;
        if (value.isEmpty() || value.length() > 10 || ! value.containsOnly ("0123456789"))
            return std::nullopt;
        const auto number = value.getLargeIntValue();
        if (number <= 0 || number > (std::numeric_limits<std::uint32_t>::max)()) return std::nullopt;
        pid = static_cast<std::uint32_t> (number);
    }
    return pid;
}

void waitForRestartParent (const juce::String& commandLine)
{
   #if JUCE_WINDOWS
    if (const auto pid = restartAfterPid (commandLine); pid && *pid != GetCurrentProcessId())
        if (const auto process = OpenProcess (SYNCHRONIZE, FALSE, *pid))
        {
            WaitForSingleObject (process, 20000);
            CloseHandle (process);
        }
   #else
    juce::ignoreUnused (commandLine);
   #endif
}

bool launchRestart()
{
   #if JUCE_WINDOWS
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    return exe.startAsProcess ("--restart-after " + juce::String ((juce::int64) GetCurrentProcessId()));
   #else
    return false;
   #endif
}
}
