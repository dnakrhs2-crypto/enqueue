#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

namespace gocue::livemix
{

/** The online backup on a WebDAV server (Synology): every account keeps its backups in its own home folder,
    <home>/<folder>/<PC>/<creator>_<date time>.livemix. Three jobs, one at a time on its own thread, each with the
    account's name and password on every request (Basic authentication over HTTPS only):
      - upload: MKCOL for the folders, PUT under a temporary name, MOVE onto the final name - a cut-off upload
        leaves only the temporary file behind, never a half file under a backup's name
      - list: the account's own backups; an account that may read the server's "homes" folder (an administrator)
        gets everyone's
      - download: one backup into a local file (written whole, then renamed into place)
    Results come back on the message thread. The owner cancels (or destroys) the object. */
class WebDavBackup : private juce::Thread
{
public:
    struct Target
    {
        juce::String baseUrl;    // https://host:port
        juce::String folder;     // inside the home folder, e.g. /LiveMix 백업
        juce::String user, password;
    };

    /** One backup on the server. */
    struct Entry
    {
        juce::String owner;      // the home's account ("" for the signed-in account's own home)
        juce::String pc;         // the PC folder
        juce::String name;       // the file name
        juce::String path;       // the WebDAV path, for the download
        juce::Time modified;
        juce::int64 size = 0;
    };

    /** One line of a PROPFIND answer. */
    struct DavItem
    {
        juce::String path;       // decoded, no trailing slash
        bool collection = false;
        juce::Time modified;
        juce::int64 size = 0;

        juce::String name() const { return path.fromLastOccurrenceOf ("/", false, false); }
    };

    using Done = std::function<void (bool ok, const juce::String& message)>;
    using ListDone = std::function<void (bool ok, const juce::String& message, std::vector<Entry> entries, bool everyone)>;

    WebDavBackup();
    ~WebDavBackup() override;

    /** <folder>/<pc>/<creator>_<yyyy-MM-dd_HHmmss>.livemix, with the characters a file name cannot carry replaced. */
    static juce::String remotePathFor (const juce::String& folder, const juce::String& pcName, const juce::String& creator, juce::Time when);
    static juce::String sanitiseName (const juce::String& name);
    /** The folder inside the account's own home: "/home" + folder ("/home/LiveMix 백업"). */
    static juce::String homeFolder (const juce::String& folder);
    /** The same folder inside another account's home: "/homes/<owner>" + folder. */
    static juce::String homeFolderOf (const juce::String& owner, const juce::String& folder);
    /** "" when the address may carry a password: https, a server, nothing after it. Otherwise why not. */
    static juce::String validateBaseUrl (const juce::String& baseUrl);
    /** The credential store key for one server + user: a changed address or user asks for the password again
        instead of sending the remembered one somewhere else. */
    static juce::String credentialKeyFor (const juce::String& baseUrl, const juce::String& user);
    /** The items of a multistatus answer, the requested folder itself left out. Any namespace prefixes. */
    static std::vector<DavItem> parseMultistatus (const juce::String& xml, const juce::String& requestedPath);
    /** "Fri, 17 Jul 2026 02:32:05 GMT" (RFC 1123, the WebDAV getlastmodified form) -> UTC time; Time() when not that. */
    static juce::Time parseHttpDate (const juce::String& text);

    /** Starts the upload; 'done (ok, message)' is called on the message thread when it is over (not when cancelled). */
    juce::Result start (const Target& target, const juce::File& localFile, const juce::String& remotePath, Done done);
    /** Starts the listing. */
    juce::Result startList (const Target& target, ListDone done);
    /** Starts the download of one backup into 'localFile'. */
    juce::Result startDownload (const Target& target, const juce::String& remotePath, const juce::File& localFile, Done done);
    bool isBusy() const noexcept { return isThreadRunning(); }
    /** Aborts a running job and waits for the thread. */
    void cancel();

private:
    enum class Job { upload, list, download };

    void run() override;
    void runUpload (bool& ok, juce::String& message);
    void runList (bool& ok, juce::String& message, std::vector<Entry>& entries, bool& everyone);
    void runDownload (bool& ok, juce::String& message);
    juce::Result begin (const Target& target, Job job);
    /** One request: the status code, or 0 with 'error' when no connection was made. The answer's body lands in
        'response' when asked for. Worker thread. */
    int request (const juce::String& method, const juce::String& path, const juce::MemoryBlock* body,
                 const juce::String& extraHeaders, juce::String& error, juce::MemoryBlock* response = nullptr);
    /** The .livemix files in every PC folder under 'root' (a folder in one account's home). Worker thread. */
    bool collectBackups (const juce::String& owner, const juce::String& root, std::vector<Entry>& entries, juce::String& message);

    Job job = Job::upload;
    Target target;
    juce::String remotePath;
    juce::File localFile;
    juce::MemoryBlock data;
    Done done;
    ListDone listDone;
    juce::CriticalSection activeLock;
    juce::WebInputStream* active = nullptr;   // the request in flight, for cancel()

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebDavBackup)
};

} // namespace gocue::livemix
