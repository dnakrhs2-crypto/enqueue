#pragma once

#include "WebDavBackup.h"

// The backup server account is built into the app at build time from a file outside the repository
// (CMake: LIVEMIX_BACKUP_ACCOUNT_FILE). A build without it has no online backup.
#ifndef LIVEMIX_BACKUP_URL
 #define LIVEMIX_BACKUP_URL ""
#endif
#ifndef LIVEMIX_BACKUP_SHARE
 #define LIVEMIX_BACKUP_SHARE ""
#endif
#ifndef LIVEMIX_BACKUP_USER
 #define LIVEMIX_BACKUP_USER ""
#endif
#ifndef LIVEMIX_BACKUP_PASSWORD_HEX
 #define LIVEMIX_BACKUP_PASSWORD_HEX ""
#endif

namespace gocue::livemix::BackupServer
{

inline bool isConfigured()
{
    return juce::String (LIVEMIX_BACKUP_URL).isNotEmpty() && juce::String (LIVEMIX_BACKUP_SHARE).isNotEmpty()
           && juce::String (LIVEMIX_BACKUP_USER).isNotEmpty() && juce::String (LIVEMIX_BACKUP_PASSWORD_HEX).isNotEmpty();
}

inline juce::String baseUrl() { return LIVEMIX_BACKUP_URL; }
inline juce::String share() { return LIVEMIX_BACKUP_SHARE; }

/** The target for one LiveMix account: the built-in server account plus the account's own id and password. */
inline WebDavBackup::Target target (const juce::String& accountId, const juce::String& accountPassword)
{
    juce::MemoryBlock bytes;
    bytes.loadFromHexString (LIVEMIX_BACKUP_PASSWORD_HEX);

    WebDavBackup::Target t;
    t.baseUrl = LIVEMIX_BACKUP_URL;
    t.share = LIVEMIX_BACKUP_SHARE;
    t.user = LIVEMIX_BACKUP_USER;
    t.password = juce::String::fromUTF8 ((const char*) bytes.getData(), (int) bytes.getSize());
    t.accountId = accountId.trim();
    t.accountPassword = accountPassword;
    return t;
}

} // namespace gocue::livemix::BackupServer
