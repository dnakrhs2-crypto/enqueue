#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace gocue::livemix
{

/** Uploads a session file to a WebDAV folder (Synology / QNAP / Nextcloud): MKCOL for the folders, PUT under a
    temporary name, MOVE onto the final name, Basic authentication over HTTPS only. One upload at a time on its own
    thread; the result comes back on the message thread. The owner cancels (or destroys) it: a cut-off upload leaves
    only the temporary file behind, never a half file under a backup's name. */
class WebDavBackup : private juce::Thread
{
public:
    struct Target
    {
        juce::String baseUrl;    // https://host:port
        juce::String folder;     // /LiveMix 백업
        juce::String user, password;
    };

    WebDavBackup();
    ~WebDavBackup() override;

    /** <folder>/<pc>/<creator>_<yyyy-MM-dd_HHmmss>.livemix, with the characters a file name cannot carry replaced. */
    static juce::String remotePathFor (const juce::String& folder, const juce::String& pcName, const juce::String& creator, juce::Time when);
    static juce::String sanitiseName (const juce::String& name);
    /** "" when the address may carry a password: https, a server, nothing after it. Otherwise why not. */
    static juce::String validateBaseUrl (const juce::String& baseUrl);
    /** The credential store key for one server + user: a changed address or user asks for the password again
        instead of sending the remembered one somewhere else. */
    static juce::String credentialKeyFor (const juce::String& baseUrl, const juce::String& user);

    /** Starts the upload; 'done (ok, message)' is called on the message thread when it is over (not when cancelled). */
    juce::Result start (const Target& target, const juce::File& localFile, const juce::String& remotePath,
                        std::function<void (bool ok, const juce::String& message)> done);
    bool isBusy() const noexcept { return isThreadRunning(); }
    /** Aborts a running upload and waits for the thread. */
    void cancel();

private:
    void run() override;
    /** One request: the status code, or 0 with 'error' when no connection was made. Worker thread. */
    int request (const juce::String& method, const juce::String& path, const juce::MemoryBlock* body,
                 const juce::String& extraHeaders, juce::String& error);

    Target target;
    juce::String remotePath;
    juce::MemoryBlock data;
    std::function<void (bool, const juce::String&)> done;
    juce::CriticalSection activeLock;
    juce::WebInputStream* active = nullptr;   // the request in flight, for cancel()

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebDavBackup)
};

} // namespace gocue::livemix
