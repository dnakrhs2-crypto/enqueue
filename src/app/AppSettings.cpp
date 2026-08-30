#include "app/AppSettings.h"

namespace gocue
{

namespace Keys
{
    constexpr const char* audioDeviceState  = "audioDeviceState";
    constexpr const char* pluginList        = "pluginList";
    constexpr const char* lastProjectFile   = "lastProjectFile";
    constexpr const char* lastAudioDir      = "lastAudioDirectory";
    constexpr const char* windowState       = "windowState";
}

AppSettings::AppSettings()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "GoCue";
    options.filenameSuffix = "settings";
    options.folderName = "GoCue";
    options.osxLibrarySubFolder = "Application Support";
    options.commonToAllUsers = false;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.millisecondsBeforeSaving = 1000;

    properties.setStorageParameters (options);
    settings = properties.getUserSettings();
}

std::unique_ptr<juce::XmlElement> AppSettings::getAudioDeviceState() const
{
    return settings->getXmlValue (Keys::audioDeviceState);
}

void AppSettings::setAudioDeviceState (const juce::XmlElement* xml)
{
    if (xml != nullptr)
        settings->setValue (Keys::audioDeviceState, xml);
    else
        settings->removeValue (Keys::audioDeviceState);
}

std::unique_ptr<juce::XmlElement> AppSettings::getPluginList() const
{
    return settings->getXmlValue (Keys::pluginList);
}

void AppSettings::setPluginList (const juce::XmlElement* xml)
{
    if (xml != nullptr)
        settings->setValue (Keys::pluginList, xml);
    else
        settings->removeValue (Keys::pluginList);
}

juce::File AppSettings::getLastProjectFile() const
{
    return getFileValue (Keys::lastProjectFile);
}

void AppSettings::setLastProjectFile (const juce::File& file)
{
    setFileValue (Keys::lastProjectFile, file);
}

juce::File AppSettings::getLastAudioDirectory() const
{
    return getFileValue (Keys::lastAudioDir);
}

void AppSettings::setLastAudioDirectory (const juce::File& directory)
{
    setFileValue (Keys::lastAudioDir, directory);
}

juce::String AppSettings::getWindowState() const
{
    return settings->getValue (Keys::windowState);
}

void AppSettings::setWindowState (const juce::String& state)
{
    settings->setValue (Keys::windowState, state);
}

void AppSettings::flush()
{
    settings->saveIfNeeded();
}

juce::File AppSettings::getDeadMansPedalFile() const
{
    return settings->getFile().getSiblingFile ("RecentlyCrashedPluginsList");
}

juce::File AppSettings::getFileValue (const char* key) const
{
    const auto path = settings->getValue (key);

    if (path.isNotEmpty() && juce::File::isAbsolutePath (path))
        return juce::File (path);

    return {};
}

void AppSettings::setFileValue (const char* key, const juce::File& file)
{
    if (file == juce::File())
        settings->removeValue (key);
    else
        settings->setValue (key, file.getFullPathName());
}

} // namespace gocue
