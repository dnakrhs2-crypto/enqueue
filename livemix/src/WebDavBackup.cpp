#include "WebDavBackup.h"

#include <juce_events/juce_events.h>

#include <algorithm>

namespace gocue::livemix
{

namespace
{
    juce::String encodePath (const juce::String& path)
    {
        // each segment percent-encoded (UTF-8), the slashes kept
        juce::StringArray parts;
        parts.addTokens (path, "/", "");
        juce::StringArray encoded;

        for (const auto& p : parts)
            encoded.add (juce::URL::addEscapeChars (p, false, false));

        return encoded.joinIntoString ("/");
    }

    juce::String trimmedBase (const juce::String& baseUrl)
    {
        auto base = baseUrl.trim();

        while (base.endsWithChar ('/'))
            base = base.dropLastCharacters (1);

        return base;
    }

    juce::String normalisedFolder (const juce::String& folder)
    {
        auto base = folder.trim();

        if (! base.startsWithChar ('/'))
            base = "/" + base;

        while (base.endsWithChar ('/') && base.length() > 1)
            base = base.dropLastCharacters (1);

        return base;
    }

    juce::String loginRefused()
    {
        return juce::String::fromUTF8 ("아이디 또는 비밀번호가 맞지 않습니다.");
    }

    juce::String httpRefused (int status, const juce::String& path)
    {
        return juce::String::fromUTF8 ("서버가 거부했습니다 (HTTP ") + juce::String (status) + "): " + path;
    }

    bool isHexGroup (const juce::String& group)
    {
        return group.isNotEmpty() && group.length() <= 4 && group.containsOnly ("0123456789abcdefABCDEF");
    }

    /** RFC 4291 text form: up to 8 hex groups, one "::" compression, an optional dotted IPv4 tail. */
    bool looksLikeIPv6 (const juce::String& text)
    {
        auto s = text;
        int groups = 0;

        if (s.containsChar ('.'))
        {
            // an embedded IPv4 (::ffff:192.168.0.1) stands for the last two groups
            const auto tail = s.fromLastOccurrenceOf (":", false, false);
            juce::StringArray quad;
            quad.addTokens (tail, ".", "");

            if (quad.size() != 4)
                return false;

            for (const auto& q : quad)
                if (q.isEmpty() || q.length() > 3 || ! q.containsOnly ("0123456789") || q.getIntValue() > 255 || (q.length() > 1 && q.startsWithChar ('0')))
                    return false;

            const auto head = s.upToLastOccurrenceOf (":", true, false);   // up to and including the ':' before the tail
            s = head.endsWith ("::") ? head : head.dropLastCharacters (1);   // "::1.2.3.4" keeps its compression
            groups = 2;
        }

        const int compress = s.indexOf ("::");

        if (compress >= 0 && s.indexOf (compress + 2, "::") >= 0)
            return false;   // at most one "::"

        auto countSide = [&groups] (const juce::String& side) -> bool
        {
            if (side.isEmpty())
                return true;

            juce::StringArray parts;
            parts.addTokens (side, ":", "");

            for (const auto& g : parts)
                if (! isHexGroup (g))
                    return false;

            groups += parts.size();
            return true;
        };

        if (compress >= 0)
        {
            if (! countSide (s.substring (0, compress)) || ! countSide (s.substring (compress + 2)))
                return false;

            return groups <= 7;
        }

        if (! countSide (s))
            return false;

        return groups == 8;
    }

    /** The first child with that local name (any namespace prefix), or null. */
    const juce::XmlElement* childNamed (const juce::XmlElement& parent, const char* localName)
    {
        for (auto* child : parent.getChildIterator())
            if (child->getTagNameWithoutNamespace() == localName)
                return child;

        return nullptr;
    }

    /** The first descendant with that local name, or null (a property may sit under any propstat). */
    const juce::XmlElement* descendantNamed (const juce::XmlElement& parent, const char* localName)
    {
        for (auto* child : parent.getChildIterator())
        {
            if (child->getTagNameWithoutNamespace() == localName)
                return child;

            if (auto* deeper = descendantNamed (*child, localName))
                return deeper;
        }

        return nullptr;
    }

    juce::String stripTrailingSlash (juce::String path)
    {
        while (path.endsWithChar ('/') && path.length() > 1)
            path = path.dropLastCharacters (1);

        return path;
    }

    /** A folder the server keeps for itself, never a PC or an account. */
    bool isServiceName (const juce::String& name)
    {
        return name.isEmpty() || name.startsWithChar ('@') || name.startsWithChar ('#') || name.startsWithChar ('.');
    }
}

WebDavBackup::WebDavBackup() : juce::Thread ("LiveMix backup") {}

WebDavBackup::~WebDavBackup()
{
    cancel();
}

juce::String WebDavBackup::sanitiseName (const juce::String& name)
{
    juce::String out;

    for (auto c : name.trim())
        out << (juce::String ("\\/:*?\"<>|").containsChar (c) ? juce::juce_wchar ('_') : c);

    return out.isEmpty() ? juce::String ("session") : out;
}

juce::String WebDavBackup::remotePathFor (const juce::String& folder, const juce::String& pcName, const juce::String& creator, juce::Time when)
{
    return normalisedFolder (folder) + "/" + sanitiseName (pcName) + "/" + sanitiseName (creator) + "_" + when.formatted ("%Y-%m-%d_%H%M%S") + ".livemix";
}

juce::String WebDavBackup::homeFolder (const juce::String& folder)
{
    return "/home" + normalisedFolder (folder);
}

juce::String WebDavBackup::homeFolderOf (const juce::String& owner, const juce::String& folder)
{
    return "/homes/" + owner + normalisedFolder (folder);
}

juce::String WebDavBackup::validateBaseUrl (const juce::String& baseUrl)
{
    const auto base = trimmedBase (baseUrl);

    if (! base.startsWithIgnoreCase ("https://"))
        return juce::String::fromUTF8 ("백업 주소는 https:// 로 시작해야 합니다 (비밀번호가 암호화 없이 나가지 않도록 http는 쓰지 않습니다)");

    const auto rest = base.substring (8);

    if (rest.isEmpty() || rest.startsWithChar ('/') || rest.startsWithChar (':'))
        return juce::String::fromUTF8 ("백업 주소에 서버 이름이 없습니다 (예: https://서버:5006)");

    if (rest.containsChar ('/') || rest.containsChar ('\\') || rest.containsAnyOf ("?#@ \t\r\n"))
        return juce::String::fromUTF8 ("백업 주소는 https://서버:포트 까지만 적습니다 (폴더는 '폴더' 칸에)");

    // host[:port]: a name / IPv4, or a bracketed IPv6; the port numeric, 1..65535
    juce::String host = rest, port;

    if (rest.startsWithChar ('['))
    {
        const int close = rest.indexOfChar (']');

        if (close < 0)
            return juce::String::fromUTF8 ("백업 주소의 IPv6 주소는 [ ] 로 감쌉니다");

        host = rest.substring (1, close);
        port = rest.substring (close + 1);

        if (! looksLikeIPv6 (host))
            return juce::String::fromUTF8 ("백업 주소의 IPv6 주소가 올바르지 않습니다");
    }
    else
    {
        if (rest.containsChar (':'))
        {
            host = rest.upToFirstOccurrenceOf (":", false, false);
            port = rest.substring (host.length());
        }

        for (auto c : host)
            if (! (juce::CharacterFunctions::isLetterOrDigit (c) || c == '-' || c == '.'))
                return juce::String::fromUTF8 ("백업 주소의 서버 이름에 쓸 수 없는 글자가 있습니다: ") + juce::String::charToString (c);

        if (host.isEmpty() || host.startsWithChar ('.') || host.endsWithChar ('.') || host.contains (".."))
            return juce::String::fromUTF8 ("백업 주소의 서버 이름이 올바르지 않습니다");
    }

    if (port.isNotEmpty())
    {
        if (! port.startsWithChar (':'))
            return juce::String::fromUTF8 ("백업 주소는 https://서버:포트 형식입니다");

        const auto digits = port.substring (1);

        if (digits.isEmpty() || ! digits.containsOnly ("0123456789") || digits.length() > 5 || digits.getIntValue() < 1 || digits.getIntValue() > 65535)
            return juce::String::fromUTF8 ("백업 주소의 포트는 1~65535 사이의 숫자여야 합니다 (예: 5006)");
    }

    return {};
}

juce::String WebDavBackup::credentialKeyFor (const juce::String& baseUrl, const juce::String& user)
{
    const auto base = trimmedBase (baseUrl);
    auto origin = base.contains ("://") ? base.fromFirstOccurrenceOf ("://", false, false) : base;
    origin = origin.upToFirstOccurrenceOf ("/", false, false).toLowerCase();
    return "LiveMix/WebDAV/" + origin + "/" + user.trim();
}

juce::Time WebDavBackup::parseHttpDate (const juce::String& text)
{
    // "Fri, 17 Jul 2026 02:32:05 GMT" - the weekday and the zone are decoration
    juce::StringArray tokens;
    tokens.addTokens (text.replaceCharacter (',', ' '), " ", "");
    tokens.removeEmptyStrings();

    if (tokens.size() >= 5 && ! tokens[0].containsOnly ("0123456789"))
        tokens.remove (0);   // the weekday

    if (tokens.size() < 4)
        return {};

    static const char* months[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    int month = -1;

    for (int i = 0; i < 12; ++i)
        if (tokens[1].toLowerCase().startsWith (months[i]))
            month = i;

    juce::StringArray clock;
    clock.addTokens (tokens[3], ":", "");

    if (month < 0 || clock.size() != 3 || ! tokens[0].containsOnly ("0123456789") || ! tokens[2].containsOnly ("0123456789"))
        return {};

    const int day = tokens[0].getIntValue(), year = tokens[2].getIntValue();

    if (day < 1 || day > 31 || year < 1970)
        return {};

    return juce::Time (year, month, day, clock[0].getIntValue(), clock[1].getIntValue(), clock[2].getIntValue(), 0, false);
}

std::vector<WebDavBackup::DavItem> WebDavBackup::parseMultistatus (const juce::String& xml, const juce::String& requestedPath)
{
    std::vector<DavItem> items;
    const auto root = juce::XmlDocument::parse (xml);

    if (root == nullptr || root->getTagNameWithoutNamespace() != "multistatus")
        return items;

    const auto requested = stripTrailingSlash (juce::URL::removeEscapeChars (requestedPath));

    for (auto* response : root->getChildIterator())
    {
        if (response->getTagNameWithoutNamespace() != "response")
            continue;

        auto* href = childNamed (*response, "href");

        if (href == nullptr)
            continue;

        // the href may be absolute (https://server/path): only the path counts
        auto raw = href->getAllSubText().trim();

        if (raw.contains ("://"))
            raw = raw.fromFirstOccurrenceOf ("://", false, false).fromFirstOccurrenceOf ("/", true, false);

        DavItem item;
        item.path = stripTrailingSlash (juce::URL::removeEscapeChars (raw));

        if (item.path.isEmpty() || item.path == requested)
            continue;

        if (auto* type = descendantNamed (*response, "resourcetype"))
            item.collection = childNamed (*type, "collection") != nullptr;

        if (auto* modified = descendantNamed (*response, "getlastmodified"))
            item.modified = parseHttpDate (modified->getAllSubText());

        if (auto* length = descendantNamed (*response, "getcontentlength"))
            item.size = length->getAllSubText().trim().getLargeIntValue();

        items.push_back (std::move (item));
    }

    return items;
}

juce::Result WebDavBackup::begin (const Target& t, Job which)
{
    if (isThreadRunning())
        return juce::Result::fail (juce::String::fromUTF8 ("백업 작업이 이미 진행 중입니다"));

    if (const auto bad = validateBaseUrl (t.baseUrl); bad.isNotEmpty())
        return juce::Result::fail (bad);

    if (t.user.trim().isEmpty() || t.password.isEmpty())
        return juce::Result::fail (juce::String::fromUTF8 ("아이디와 비밀번호를 넣으세요"));

    target = t;
    target.user = target.user.trim();
    job = which;
    return juce::Result::ok();
}

juce::Result WebDavBackup::start (const Target& t, const juce::File& file, const juce::String& path, Done onDone)
{
    if (const auto result = begin (t, Job::upload); result.failed())
        return result;

    juce::MemoryBlock bytes;

    if (! file.loadFileAsData (bytes))
        return juce::Result::fail (juce::String::fromUTF8 ("세션 파일을 읽지 못했습니다: ") + file.getFullPathName());

    remotePath = path;
    data = std::move (bytes);
    done = std::move (onDone);
    listDone = nullptr;

    if (! startThread())
    {
        done = nullptr;
        data.reset();
        target = {};
        return juce::Result::fail (juce::String::fromUTF8 ("백업 스레드를 시작하지 못했습니다"));
    }

    return juce::Result::ok();
}

juce::Result WebDavBackup::startList (const Target& t, ListDone onDone)
{
    if (const auto result = begin (t, Job::list); result.failed())
        return result;

    remotePath.clear();
    data.reset();
    done = nullptr;
    listDone = std::move (onDone);

    if (! startThread())
    {
        listDone = nullptr;
        target = {};
        return juce::Result::fail (juce::String::fromUTF8 ("백업 스레드를 시작하지 못했습니다"));
    }

    return juce::Result::ok();
}

juce::Result WebDavBackup::startDownload (const Target& t, const juce::String& path, const juce::File& file, Done onDone)
{
    if (const auto result = begin (t, Job::download); result.failed())
        return result;

    remotePath = path;
    localFile = file;
    data.reset();
    done = std::move (onDone);
    listDone = nullptr;

    if (! startThread())
    {
        done = nullptr;
        target = {};
        return juce::Result::fail (juce::String::fromUTF8 ("백업 스레드를 시작하지 못했습니다"));
    }

    return juce::Result::ok();
}

void WebDavBackup::cancel()
{
    signalThreadShouldExit();

    {
        const juce::ScopedLock sl (activeLock);

        if (active != nullptr)
            active->cancel();
    }

    stopThread (30000);   // a cancelled request returns at once; the wait is only for a response already on its way
}

int WebDavBackup::request (const juce::String& method, const juce::String& path, const juce::MemoryBlock* body,
                           const juce::String& extraHeaders, juce::String& error, juce::MemoryBlock* response)
{
    const auto base = trimmedBase (target.baseUrl);
    juce::URL url (base + encodePath (path));

    if (body != nullptr)
        url = url.withPOSTData (*body);

    juce::String headers = "Authorization: Basic " + juce::Base64::toBase64 (target.user + ":" + target.password) + "\r\n" + extraHeaders;

    if (body != nullptr)
        headers << "Content-Type: application/octet-stream\r\n";

    auto stream = std::make_unique<juce::WebInputStream> (url, body != nullptr);
    stream->withCustomRequestCommand (method).withExtraHeaders (headers).withConnectionTimeout (20000).withNumRedirectsToFollow (0);

    {
        // published under the lock cancel() takes: either cancel() sees this request, or this request sees the exit
        // flag before it connects - never a 20 s connect that nobody can abort
        const juce::ScopedLock sl (activeLock);

        if (threadShouldExit())
        {
            error = juce::String::fromUTF8 ("백업 작업이 취소되었습니다");
            return 0;
        }

        active = stream.get();
    }

    const bool connected = stream->connect (nullptr);
    const int status = stream->getStatusCode();

    if (connected)
    {
        if (response != nullptr)
        {
            response->reset();
            stream->readIntoMemoryBlock (*response);
        }
        else
        {
            stream->readEntireStreamAsString();   // drain the (small) response
        }
    }

    {
        const juce::ScopedLock sl (activeLock);
        active = nullptr;
    }

    if (status == 0)
    {
        error = threadShouldExit() ? juce::String::fromUTF8 ("백업 작업이 취소되었습니다")
                                   : juce::String::fromUTF8 ("서버에 연결하지 못했습니다: ") + base;
        return 0;
    }

    return status;
}

void WebDavBackup::run()
{
    bool ok = false, everyone = false;
    juce::String message;
    std::vector<Entry> entries;

    switch (job)
    {
        case Job::upload:   runUpload (ok, message); break;
        case Job::list:     runList (ok, message, entries, everyone); break;
        case Job::download: runDownload (ok, message); break;
    }

    if (threadShouldExit())
        return;   // cancelled (a quit): nobody waits for the result

    if (job == Job::list)
    {
        auto callback = std::move (listDone);
        juce::MessageManager::callAsync ([callback, ok, message, entries, everyone]
        {
            if (callback)
                callback (ok, message, entries, everyone);
        });
        return;
    }

    auto callback = std::move (done);
    juce::MessageManager::callAsync ([callback, ok, message]
    {
        if (callback)
            callback (ok, message);
    });
}

void WebDavBackup::runUpload (bool& ok, juce::String& message)
{
    juce::String error;

    // the folders: MKCOL each level (405 = exists already)
    juce::StringArray parts;
    parts.addTokens (remotePath, "/", "");
    parts.removeEmptyStrings();
    juce::String path;

    for (int i = 0; i < parts.size() - 1 && ! threadShouldExit(); ++i)
    {
        path << "/" << parts[i];
        const int status = request ("MKCOL", path, nullptr, {}, error);

        if (status == 0)
        {
            message = error;
            return;
        }

        if (status == 401)
        {
            message = loginRefused();
            return;
        }

        if (status == 403 && i > 0)   // the account's home itself answers 403 to MKCOL: that one is fine, a folder inside is not
        {
            message = juce::String::fromUTF8 ("서버가 폴더 만들기를 거부했습니다 (HTTP 403): ") + path;
            return;
        }
    }

    if (threadShouldExit())
        return;

    // the file lands under a temporary name and is renamed onto the final one: a cut-off upload never sits under a
    // backup's name
    const auto partPath = remotePath + ".part";
    const int put = request ("PUT", partPath, &data, {}, error);

    if (put == 200 || put == 201 || put == 204)
    {
        const auto destination = trimmedBase (target.baseUrl) + encodePath (remotePath);
        const int moved = request ("MOVE", partPath, nullptr, "Destination: " + destination + "\r\nOverwrite: T\r\n", error);

        if (moved == 200 || moved == 201 || moved == 204)
        {
            ok = true;
            message = juce::String::fromUTF8 ("백업 완료: ") + remotePath;
        }
        else if (moved == 0)
            message = error;
        else
            message = juce::String::fromUTF8 ("서버가 이름 바꾸기를 거부했습니다 (HTTP ") + juce::String (moved) + "): " + partPath
                      + juce::String::fromUTF8 (" 로 남아 있습니다");
    }
    else if (put == 401 || put == 403)
        message = loginRefused();
    else if (put == 0)
        message = error;
    else
        message = httpRefused (put, partPath);
}

bool WebDavBackup::collectBackups (const juce::String& owner, const juce::String& root, std::vector<Entry>& entries, juce::String& message)
{
    juce::String error;
    juce::MemoryBlock xml;
    const int status = request ("PROPFIND", root + "/", nullptr, "Depth: 1\r\n", error, &xml);

    if (status == 0)
    {
        message = error;
        return false;
    }

    if (status == 401)
    {
        message = loginRefused();
        return false;
    }

    if (status != 207)
        return true;   // no backups there yet (404), or a home this account may not read (403): nothing to show

    for (const auto& pcFolder : parseMultistatus (xml.toString(), root))
    {
        if (threadShouldExit())
            return false;

        if (! pcFolder.collection || isServiceName (pcFolder.name()))
            continue;

        juce::MemoryBlock files;
        const int inner = request ("PROPFIND", pcFolder.path + "/", nullptr, "Depth: 1\r\n", error, &files);

        if (inner == 0)
        {
            message = error;
            return false;
        }

        if (inner != 207)
            continue;

        for (auto& file : parseMultistatus (files.toString(), pcFolder.path))
        {
            if (file.collection || ! file.name().endsWithIgnoreCase (".livemix"))
                continue;

            Entry entry;
            entry.owner = owner;
            entry.pc = pcFolder.name();
            entry.name = file.name();
            entry.path = file.path;
            entry.modified = file.modified;
            entry.size = file.size;
            entries.push_back (std::move (entry));
        }
    }

    return true;
}

void WebDavBackup::runList (bool& ok, juce::String& message, std::vector<Entry>& entries, bool& everyone)
{
    juce::String error;
    juce::MemoryBlock xml;

    // an account that may read the server's homes folder sees every account's backups; any other its own home
    const int homes = request ("PROPFIND", "/homes/", nullptr, "Depth: 1\r\n", error, &xml);

    if (homes == 0)
    {
        message = error;
        return;
    }

    if (homes == 401)
    {
        message = loginRefused();
        return;
    }

    if (homes == 207)
    {
        everyone = true;

        for (const auto& home : parseMultistatus (xml.toString(), "/homes"))
        {
            if (threadShouldExit())
                return;

            if (! home.collection || isServiceName (home.name()))
                continue;

            if (! collectBackups (home.name(), homeFolderOf (home.name(), target.folder), entries, message))
                return;
        }
    }
    else
    {
        // the account's own home: 403 / 404 on homes is the normal answer for a non-administrator
        if (! collectBackups ({}, homeFolder (target.folder), entries, message))
            return;
    }

    std::stable_sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b) { return a.modified > b.modified; });
    ok = true;
    message = entries.empty() ? juce::String::fromUTF8 ("백업이 아직 없습니다")
                              : juce::String::fromUTF8 ("백업 ") + juce::String ((int) entries.size()) + juce::String::fromUTF8 ("개");
}

void WebDavBackup::runDownload (bool& ok, juce::String& message)
{
    juce::String error;
    juce::MemoryBlock bytes;
    const int status = request ("GET", remotePath, nullptr, {}, error, &bytes);

    if (status == 0)
    {
        message = error;
        return;
    }

    if (status == 401 || status == 403)
    {
        message = loginRefused();
        return;
    }

    if (status == 404)
    {
        message = juce::String::fromUTF8 ("서버에 그 백업이 없습니다: ") + remotePath;
        return;
    }

    if (status != 200)
    {
        message = httpRefused (status, remotePath);
        return;
    }

    if (bytes.getSize() == 0)
    {
        message = juce::String::fromUTF8 ("서버가 빈 파일을 보냈습니다: ") + remotePath;
        return;
    }

    // written whole under a temporary name, then renamed into place: never a half file under the session's name
    juce::TemporaryFile temp (localFile);

    if (! temp.getFile().replaceWithData (bytes.getData(), bytes.getSize()) || ! temp.overwriteTargetFileWithTemporary())
    {
        message = juce::String::fromUTF8 ("파일을 쓰지 못했습니다: ") + localFile.getFullPathName();
        return;
    }

    ok = true;
    message = juce::String::fromUTF8 ("불러옴: ") + localFile.getFileName();
}

} // namespace gocue::livemix
