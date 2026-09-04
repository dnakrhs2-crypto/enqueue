#include "LiveMixSettings.h"

#include "WebDavBackup.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <wincred.h>
 #pragma comment(lib, "advapi32.lib")
#endif

namespace gocue::livemix
{

namespace Keys
{
    constexpr const char* audioDeviceState = "audioDeviceState";
    constexpr const char* pluginList = "pluginList";
    constexpr const char* lastSession = "lastSession";
    constexpr const char* recent = "recentSessions";
    constexpr const char* windowState = "windowState";
    constexpr const char* minimiseToTray = "minimiseToTray";
    constexpr const char* closeToTray = "closeToTray";
    constexpr const char* startWithWindows = "startWithWindows";
    constexpr const char* autosaveSeconds = "autosaveSeconds";
    constexpr const char* backupUrl = "backupUrl";
    constexpr const char* backupFolder = "backupFolder";
    constexpr const char* backupUser = "backupUser";
    constexpr const char* backupCreator = "backupCreator";
    constexpr const char* backupRemember = "backupRememberPassword";
    constexpr const char* lastRunVersion = "lastRunVersion";
}

LiveMixSettings::LiveMixSettings()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "LiveMix";
    options.filenameSuffix = "settings";
    options.folderName = "LiveMix";
    options.osxLibrarySubFolder = "Application Support";
    options.commonToAllUsers = false;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.millisecondsBeforeSaving = 1000;
    properties.setStorageParameters (options);
    settings = properties.getUserSettings();
}

std::unique_ptr<juce::XmlElement> LiveMixSettings::getAudioDeviceState() const { return settings->getXmlValue (Keys::audioDeviceState); }
void LiveMixSettings::setAudioDeviceState (const juce::XmlElement* xml)
{
    if (xml != nullptr) settings->setValue (Keys::audioDeviceState, xml); else settings->removeValue (Keys::audioDeviceState);
}

std::unique_ptr<juce::XmlElement> LiveMixSettings::getPluginList() const { return settings->getXmlValue (Keys::pluginList); }
void LiveMixSettings::setPluginList (const juce::XmlElement* xml)
{
    if (xml != nullptr) settings->setValue (Keys::pluginList, xml); else settings->removeValue (Keys::pluginList);
}

juce::File LiveMixSettings::getLastSessionFile() const
{
    const auto path = settings->getValue (Keys::lastSession);
    return path.isNotEmpty() ? juce::File (path) : juce::File();
}

void LiveMixSettings::setLastSessionFile (const juce::File& file)
{
    settings->setValue (Keys::lastSession, file == juce::File() ? juce::String() : file.getFullPathName());
}

juce::StringArray LiveMixSettings::getRecentSessions() const
{
    juce::StringArray list;
    list.addLines (settings->getValue (Keys::recent));
    list.removeEmptyStrings();
    return list;
}

void LiveMixSettings::addRecentSession (const juce::File& file)
{
    auto list = getRecentSessions();
    list.removeString (file.getFullPathName());
    list.insert (0, file.getFullPathName());

    while (list.size() > 10)
        list.remove (list.size() - 1);

    settings->setValue (Keys::recent, list.joinIntoString ("\n"));
}

juce::String LiveMixSettings::getWindowState() const { return settings->getValue (Keys::windowState); }
void LiveMixSettings::setWindowState (const juce::String& state) { settings->setValue (Keys::windowState, state); }

bool LiveMixSettings::getMinimiseToTray() const { return settings->getBoolValue (Keys::minimiseToTray, true); }
void LiveMixSettings::setMinimiseToTray (bool on) { settings->setValue (Keys::minimiseToTray, on); }
bool LiveMixSettings::getCloseToTray() const { return settings->getBoolValue (Keys::closeToTray, true); }
void LiveMixSettings::setCloseToTray (bool on) { settings->setValue (Keys::closeToTray, on); }
bool LiveMixSettings::getStartWithWindows() const { return settings->getBoolValue (Keys::startWithWindows, false); }
void LiveMixSettings::setStartWithWindows (bool on) { settings->setValue (Keys::startWithWindows, on); }
int LiveMixSettings::getAutosaveSeconds() const { return juce::jlimit (3, 600, settings->getIntValue (Keys::autosaveSeconds, 10)); }
void LiveMixSettings::setAutosaveSeconds (int seconds) { settings->setValue (Keys::autosaveSeconds, juce::jlimit (3, 600, seconds)); }

juce::String LiveMixSettings::getBackupUrl() const { return settings->getValue (Keys::backupUrl, "https://parkdoomin.synology.me:5006"); }   // the credential key's server part (the server itself is built in)
void LiveMixSettings::setBackupUrl (const juce::String& url) { settings->setValue (Keys::backupUrl, url.trim()); }
juce::String LiveMixSettings::getBackupFolder() const { return settings->getValue (Keys::backupFolder, juce::String::fromUTF8 ("/LiveMix 백업")); }
void LiveMixSettings::setBackupFolder (const juce::String& folder) { settings->setValue (Keys::backupFolder, folder.trim()); }
juce::String LiveMixSettings::getBackupUser() const { return settings->getValue (Keys::backupUser); }
void LiveMixSettings::setBackupUser (const juce::String& user) { settings->setValue (Keys::backupUser, user.trim()); }
juce::String LiveMixSettings::getBackupCreator() const { return settings->getValue (Keys::backupCreator); }
void LiveMixSettings::setBackupCreator (const juce::String& name) { settings->setValue (Keys::backupCreator, name.trim()); }
bool LiveMixSettings::getBackupRememberPassword() const { return settings->getBoolValue (Keys::backupRemember, false); }
void LiveMixSettings::setBackupRememberPassword (bool on) { settings->setValue (Keys::backupRemember, on); }

juce::String LiveMixSettings::getBackupPassword() const
{
   #if JUCE_WINDOWS
    PCREDENTIALW cred = nullptr;
    const auto target = WebDavBackup::credentialKeyFor (getBackupUrl(), getBackupUser());   // one entry per server + user

    if (CredReadW (target.toWideCharPointer(), CRED_TYPE_GENERIC, 0, &cred) && cred != nullptr)
    {
        const juce::String password = juce::String::fromUTF8 ((const char*) cred->CredentialBlob, (int) cred->CredentialBlobSize);
        CredFree (cred);
        return password;
    }
   #endif

    return {};
}

void LiveMixSettings::setBackupPassword (const juce::String& password)
{
   #if JUCE_WINDOWS
    const auto target = WebDavBackup::credentialKeyFor (getBackupUrl(), getBackupUser());

    if (password.isEmpty())
    {
        CredDeleteW (target.toWideCharPointer(), CRED_TYPE_GENERIC, 0);
        return;
    }

    const auto utf8 = password.toUTF8();
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR> (target.toWideCharPointer());
    cred.CredentialBlobSize = (DWORD) (utf8.sizeInBytes() - 1);
    cred.CredentialBlob = (LPBYTE) utf8.getAddress();
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    const juce::String user = getBackupUser();
    cred.UserName = const_cast<LPWSTR> (user.toWideCharPointer());
    CredWriteW (&cred, 0);
   #else
    juce::ignoreUnused (password);
   #endif
}

juce::String LiveMixSettings::getLastRunVersion() const { return settings->getValue (Keys::lastRunVersion); }
void LiveMixSettings::setLastRunVersion (const juce::String& version) { settings->setValue (Keys::lastRunVersion, version); }

void LiveMixSettings::saveIfNeeded() { properties.saveIfNeeded(); }

} // namespace gocue::livemix
