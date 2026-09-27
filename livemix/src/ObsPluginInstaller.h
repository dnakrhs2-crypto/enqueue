#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <map>
#include <vector>

namespace gocue::livemix
{
/** Blocking file/process operations. Call from a worker, or from the headless install command only. */
struct ObsPluginInstaller
{
    struct Roots
    {
        juce::File programData, obsInstallDir, bundledPlugin; // bundledPlugin is the livemix-obs directory
        std::function<bool()> isObsRunning;
        // Optional fault-injection seam: CopyFileW(..., TRUE) semantics, returning a Win32 error (0 = success).
        std::function<juce::uint32 (const juce::File&, const juce::File&)> copyFile;
    };
    enum class Result { alreadyCurrent, installed, installedRestartObs, needsElevation, obsBusyCloseIt, noBundledFiles, failed };

    static Roots systemRoots(); // never used by installer tests
    static int obsMajorVersion (const Roots&); // 0 = not found / unreadable VERSIONINFO
    static bool isInstalledAndCurrent (const Roots&);
    static Result install (const Roots&, juce::String& message);

    struct RunningObs
    {
        juce::File image;
        juce::uint64 version = 0; // native VERSIONINFO; 0 = unknown
        bool portable = false;
        bool needsUpdate() const noexcept;
        juce::String versionString() const;
    };
    static RunningObs runningObsInfo (const juce::File& image, juce::uint64 version);

    // Shared by Main's single-instance decision, headless mode and the UI's elevation worker.
    static bool isInstallCommandLine (const juce::String&);
    static const char* resultName (Result);
    static juce::File resultFile (const juce::String& commandLine = {});
    static bool writeResult (const juce::File&, Result, const juce::String& message);
    static Result readResult (const juce::File&, juce::String& message);
    static Result installElevated (juce::String& message); // UAC cancellation returns needsElevation
    static juce::String elevatedCommandLine (const juce::File& report);
    static Result runInstallCommandLine (const juce::String&, const Roots&, juce::String& message, bool& reported,
                                         const std::function<Result (juce::String&)>& elevate = installElevated);
};

/** Message-thread discovery, throttled to two seconds. Metadata lives only as long as its image is running. */
class RunningObsDetector
{
public:
    using ImageQuery = std::function<std::vector<juce::File>()>;
    using VersionQuery = std::function<juce::uint64 (const juce::File&)>;
    explicit RunningObsDetector (ImageQuery images = {}, VersionQuery version = {});
    const std::vector<ObsPluginInstaller::RunningObs>& scan (double nowMs = juce::Time::getMillisecondCounterHiRes());

private:
    ImageQuery images;
    VersionQuery version;
    double nextScan = 0.0;
    std::map<juce::String, ObsPluginInstaller::RunningObs> byImage;
    std::vector<ObsPluginInstaller::RunningObs> running;
};
}
