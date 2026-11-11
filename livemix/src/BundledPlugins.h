#pragma once

#include "LiveMixSettings.h"
#include "audio/PluginHost.h"
#include "audio/PluginScan.h"

namespace gocue::livemix
{
inline juce::File bundledPluginsDirectory()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("VST3");
}

inline bool isRegisterBundledPluginsCommand (const juce::String& commandLine)
{
    return juce::ArgumentList ("LiveMix", commandLine).containsOption ("--register-bundled-plugins");
}

struct BundledPluginsResult
{
    int scanned = 0;
    bool allRegistered = true;
};

/** Called on the message thread before restoring a session, or by the headless installer command.
    The coordinator's pipe callbacks and heartbeat run on their own threads (InterprocessConnection(false));
    waiting here does not need a nested message loop. Plugin code runs on the worker's message thread. */
inline BundledPluginsResult syncBundledPlugins (PluginHost& host, LiveMixSettings& settings,
                                               const juce::File& directory = bundledPluginsDirectory(),
                                               const juce::File& executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile))
{
    BundledPluginsResult result;
    host.setBundledPluginsDirectory (directory);
    auto& list = host.getKnownPlugins();
    if (! directory.isDirectory())
    {
        bool removed = false;
        for (const auto& type : list.getTypes())
            if (host.isBundledPluginFile (type.fileOrIdentifier))
            {
                list.removeType (type);
                removed = true;
            }
        const auto blacklisted = list.getBlacklistedFiles();
        for (const auto& file : blacklisted)
            if (host.isBundledPluginFile (file))
            {
                list.removeFromBlacklist (file);
                removed = true;
            }
        if (removed) settings.setPluginList (host.createKnownPluginsXml().get());
        return result;
    }

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto stamp = juce::JSON::parse (directory.getChildFile ("bundled_plugins.json"))
                          .getProperty ("id", "").toString();
    const bool changed = stamp != settings.getBundledPluginsStamp();
    for (const auto& type : list.getTypes())
        if (host.isBundledPluginFile (type.fileOrIdentifier)
            && (changed || ! juce::File (type.fileOrIdentifier).exists()))
            list.removeType (type);
    if (changed)
    {
        const auto blacklisted = list.getBlacklistedFiles();
        for (const auto& file : blacklisted)
            if (host.isBundledPluginFile (file)) list.removeFromBlacklist (file);
    }

    const auto belongsToBundle = [] (const juce::String& path, const juce::File& bundle)
    {
        return juce::File::isAbsolutePath (path)
            && (juce::File (path) == bundle || juce::File (path).isAChildOf (bundle));
    };
    const auto hasBundle = [&] (const juce::File& bundle)
    {
        for (const auto& type : list.getTypes())
            if (belongsToBundle (type.fileOrIdentifier, bundle))
                return true;
        return false;
    };
    const auto isBlacklisted = [&] (const juce::File& bundle)
    {
        for (const auto& file : list.getBlacklistedFiles())
            if (belongsToBundle (file, bundle)) return true;
        return false;
    };
    auto bundles = directory.findChildFiles (juce::File::findFilesAndDirectories, false, "*.vst3");
    std::sort (bundles.begin(), bundles.end());
    PluginScanCoordinator::Options options;
    options.executable = executable;
    options.logFile = settings.getSettingsDirectory().getChildFile ("logs/plugin-scan.log");
    options.timeoutMs = 20000;
    // A separate list leaves the manager's CustomScanner and its 60 minute timeout untouched.
    juce::KnownPluginList scanned;
    auto coordinator = std::make_unique<PluginScanCoordinator> (options);
    if (changed) coordinator->clearRuntimeMissingFiles (directory);
    scanned.setCustomScanner (std::move (coordinator));
    for (const auto& bundle : bundles)
    {
        if (! hasBundle (bundle) && ! isBlacklisted (bundle))
        {
            juce::OwnedArray<juce::PluginDescription> types;
            scanned.scanAndAddFile (bundle.getFullPathName(), false, types, *host.getVST3Format());
            for (const auto* type : types) list.addType (*type);
            if (types.isEmpty()) list.addToBlacklist (bundle.getFullPathName());
            ++result.scanned;
        }
        result.allRegistered = hasBundle (bundle) && result.allRegistered;
    }
    scanned.scanFinished();
    settings.setBundledPluginsStamp (stamp);
    settings.setPluginList (host.createKnownPluginsXml().get());
    if (result.scanned > 0)
    {
        options.logFile.getParentDirectory().createDirectory();
        options.logFile.appendText ("bundled-plugins\ttotal\t" + juce::String (result.scanned) + " scanned\t"
            + juce::String (juce::Time::getMillisecondCounterHiRes() - started, 1) + " ms\n", false, false, "\n");
    }
    return result;
}

inline void resetPluginListKeepingBundled (PluginHost& host)
{
    auto& list = host.getKnownPlugins();
    for (const auto& type : list.getTypes())
        if (! host.isBundledPluginFile (type.fileOrIdentifier)) list.removeType (type);
    list.clearBlacklistedFiles();
}
} // namespace gocue::livemix
