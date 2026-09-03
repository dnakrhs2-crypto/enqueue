#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace gocue::livemix
{

/** Uploads a session file to a WebDAV folder (Synology / QNAP / Nextcloud): MKCOL for the folders, PUT for the
    file, Basic authentication over HTTPS. Runs on its own thread; the result comes back on the message thread. */
class WebDavBackup
{
public:
    struct Target
    {
        juce::String baseUrl;    // https://host:port
        juce::String folder;     // /LiveMix 백업
        juce::String user, password;
    };

    /** <folder>/<pc>/<creator>_<yyyy-MM-dd_HHmmss>.livemix, with the characters a file name cannot carry replaced. */
    static juce::String remotePathFor (const juce::String& folder, const juce::String& pcName, const juce::String& creator, juce::Time when);
    static juce::String sanitiseName (const juce::String& name);

    /** Starts the upload; 'done (ok, message)' is called on the message thread. */
    static void upload (const Target& target, const juce::File& localFile, const juce::String& remotePath,
                        std::function<void (bool ok, const juce::String& message)> done);

    /** Synchronous (worker thread): one HTTP request with a method, returns the status code (0 = no connection). */
    static int request (const Target& target, const juce::String& method, const juce::String& path, const juce::MemoryBlock* body, juce::String& error);
};

} // namespace gocue::livemix
