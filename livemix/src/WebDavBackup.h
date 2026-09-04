#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

namespace gocue::livemix
{

/** The online backup: one WebDAV share on the server (Synology), reached with one server account that is built into
    the app; the people are LiveMix accounts made in the app and kept on that share:
        <share>/accounts/<id>.json      the account (a salted, iterated SHA-256 of its password)
        <share>/accounts/admins.txt     the ids that see every account's backups, one per line
        <share>/<id>/<PC>_<date time>.livemix   the backups
    Four jobs, one at a time on its own thread, every one of them checking the account's password first:
      - create account: the account file is written create-only (If-None-Match), so an id is taken once
      - sign in: the account, whether it is on the admin list, then the list of backups (its own, or everyone's)
      - upload: MKCOL for the folder, PUT under a temporary name, MOVE onto the final name - a cut-off upload leaves
        only the temporary file behind, never a half file under a backup's name
      - download: one backup into a local file (written whole, then renamed into place); another account's backup
        only for an admin
    Results come back on the message thread. The owner cancels (or destroys) the object. */
class WebDavBackup : private juce::Thread
{
public:
    struct Target
    {
        juce::String baseUrl;               // https://host:port
        juce::String share;                 // e.g. /backups
        juce::String user, password;        // the server account (built into the app)
        juce::String accountId, accountPassword;   // the LiveMix account (made in the app)
    };

    /** One backup on the server. */
    struct Entry
    {
        juce::String owner;      // the account
        juce::String pc;         // the PC it was made on
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
    using SignInDone = std::function<void (bool ok, const juce::String& message, std::vector<Entry> entries, bool everyone)>;

    WebDavBackup();
    ~WebDavBackup() override;

    //==============================================================================
    /** "" when the id will do: 2 to 20 letters, digits, Korean syllables, '_' or '-'. Otherwise why not. */
    static juce::String validateAccountId (const juce::String& id);
    /** A fresh random salt (hex). */
    static juce::String newSalt();
    /** The password's iterated SHA-256 with the salt (64 hex characters). */
    static juce::String hashPassword (const juce::String& password, const juce::String& salt);
    /** The account file's text. */
    static juce::String accountJson (const juce::String& id, const juce::String& salt, const juce::String& hash, const juce::String& pcName, juce::Time when);
    /** Reads salt and hash out of an account file; false when it is not one. */
    static bool parseAccount (const juce::String& json, juce::String& salt, juce::String& hash);
    /** The ids on the admin list (one per line, blanks and '#' comments ignored). */
    static juce::StringArray parseAdminList (const juce::String& text);

    static juce::String accountsFolder (const juce::String& share);                            // <share>/accounts
    static juce::String accountPath (const juce::String& share, const juce::String& id);         // <share>/accounts/<id>.json
    static juce::String adminListPath (const juce::String& share);                             // <share>/accounts/admins.txt
    static juce::String accountFolder (const juce::String& share, const juce::String& id);       // <share>/<id>
    /** <share>/<id>/<pc>_<yyyy-MM-dd_HHmmss>.livemix, with the characters a file name cannot carry replaced. */
    static juce::String backupPathFor (const juce::String& share, const juce::String& id, const juce::String& pcName, juce::Time when);
    /** True when 'path' is exactly <share>/<owner>/<file>.livemix with clean segments (no empty, '.' or '..' ones,
        the owner a valid id); 'owner' receives the account. */
    static bool parseBackupPath (const juce::String& share, const juce::String& path, juce::String& owner);
    static juce::String sanitiseName (const juce::String& name);
    /** "" when the address may carry a password: https, a server, nothing after it. Otherwise why not. */
    static juce::String validateBaseUrl (const juce::String& baseUrl);
    /** The credential store key for one server + LiveMix account. */
    static juce::String credentialKeyFor (const juce::String& baseUrl, const juce::String& accountId);
    /** The items of a multistatus answer, the requested folder itself left out. Any namespace prefixes. 'parsedOk'
        (when given) tells an unreadable answer from an empty folder. */
    static std::vector<DavItem> parseMultistatus (const juce::String& xml, const juce::String& requestedPath, bool* parsedOk = nullptr);
    /** "Fri, 17 Jul 2026 02:32:05 GMT" (RFC 1123, the WebDAV getlastmodified form) -> UTC time; Time() when not that. */
    static juce::Time parseHttpDate (const juce::String& text);

    //==============================================================================
    /** Makes the account (its id and password from the target); 'done (ok, message)' on the message thread. */
    juce::Result createAccount (const Target& target, Done done);
    /** Checks the account's password and lists its backups (everyone's for an admin). */
    juce::Result signIn (const Target& target, SignInDone done);
    /** Uploads the local file as <share>/<id>/<pc>_<date>.livemix (the path from backupPathFor). */
    juce::Result start (const Target& target, const juce::File& localFile, const juce::String& remotePath, Done done);
    /** Downloads one backup into 'localFile'. */
    juce::Result startDownload (const Target& target, const juce::String& remotePath, const juce::File& localFile, Done done);
    bool isBusy() const noexcept { return isThreadRunning(); }
    /** Aborts a running job and waits for the thread. */
    void cancel();

private:
    enum class Job { createAccount, signIn, upload, download };

    void run() override;
    void runCreateAccount (bool& ok, juce::String& message);
    void runSignIn (bool& ok, juce::String& message, std::vector<Entry>& entries, bool& everyone);
    void runUpload (bool& ok, juce::String& message);
    void runDownload (bool& ok, juce::String& message);
    juce::Result begin (const Target& target, Job job, bool needsAccount);
    /** One request: the status code, or 0 with 'error' when no connection was made. The answer's body lands in
        'response' when asked for. Worker thread. */
    int request (const juce::String& method, const juce::String& path, const juce::MemoryBlock* body,
                 const juce::String& extraHeaders, juce::String& error, juce::MemoryBlock* response = nullptr);
    /** The account file checked against the password. Worker thread. */
    bool verifyAccount (juce::String& message);
    /** Whether the account is on the admin list. Worker thread. */
    bool isAdmin (juce::String& message, bool& failed);
    /** The .livemix files in one account's folder. Worker thread. */
    bool collectBackups (const juce::String& owner, std::vector<Entry>& entries, juce::String& message);
    /** MKCOL, 405 (exists) is fine. Worker thread. */
    bool makeFolder (const juce::String& path, juce::String& message);

    Job job = Job::signIn;
    Target target;
    juce::String remotePath;
    juce::File localFile;
    juce::MemoryBlock data;
    Done done;
    SignInDone signInDone;
    juce::CriticalSection activeLock;
    juce::WebInputStream* active = nullptr;   // the request in flight, for cancel()

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebDavBackup)
};

} // namespace gocue::livemix
