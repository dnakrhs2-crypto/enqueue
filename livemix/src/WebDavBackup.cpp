#include "WebDavBackup.h"

#include <juce_cryptography/juce_cryptography.h>
#include <juce_events/juce_events.h>

#include <algorithm>

namespace gocue::livemix
{

namespace
{
    constexpr int passwordRounds = 20000;

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

    /** "/share" without a trailing slash. */
    juce::String normalisedShare (const juce::String& share)
    {
        auto base = share.trim();

        while (base.endsWithChar ('/'))
            base = base.dropLastCharacters (1);

        if (base.isEmpty())
            return {};

        return base.startsWithChar ('/') ? base : "/" + base;
    }

    juce::String withTrailingSlash (const juce::String& path)
    {
        return path.endsWithChar ('/') ? path : path + "/";
    }

    /** Percent-decoding of a URL path: a literal '+' stays a '+' (JUCE's decoder would make it a space). */
    juce::String decodePath (const juce::String& encoded)
    {
        return juce::URL::removeEscapeChars (encoded.replace ("+", "%2B"));
    }

    juce::String serverRefused()
    {
        return juce::String::fromUTF8 ("백업 서버가 프로그램의 접속을 거부했습니다. 프로그램을 업데이트하거나 관리자에게 알리세요.");
    }

    juce::String httpRefused (int status, const juce::String& path)
    {
        return juce::String::fromUTF8 ("서버가 거부했습니다 (HTTP ") + juce::String (status) + "): " + path;
    }

    juce::String unreadableAnswer (const juce::String& path)
    {
        return juce::String::fromUTF8 ("서버의 목록 응답을 읽을 수 없습니다: ") + path;
    }

    bool nothingThere (int status)
    {
        return status == 404 || status == 405;   // no such folder (405: a share that is not there)
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

    /** A folder the server keeps for itself, never an account. */
    bool isServiceName (const juce::String& name)
    {
        return name.isEmpty() || name.startsWithChar ('@') || name.startsWithChar ('#') || name.startsWithChar ('.');
    }

    bool isKoreanSyllable (juce::juce_wchar c)
    {
        return c >= 0xAC00 && c <= 0xD7A3;
    }
}

WebDavBackup::WebDavBackup() : juce::Thread ("LiveMix backup") {}

WebDavBackup::~WebDavBackup()
{
    cancel();
}

//==============================================================================
juce::String WebDavBackup::validateAccountId (const juce::String& idIn)
{
    const auto id = idIn.trim();

    if (id.length() < 2 || id.length() > 20)
        return juce::String::fromUTF8 ("아이디는 2~20자입니다");

    for (auto c : id)
        if (! (juce::CharacterFunctions::isLetterOrDigit (c) && c < 128) && ! isKoreanSyllable (c) && c != '_' && c != '-')
            return juce::String::fromUTF8 ("아이디에는 영문, 숫자, 한글, '_', '-'만 쓸 수 있습니다");

    if (id.equalsIgnoreCase ("accounts") || id.startsWithChar ('-') || id.startsWithChar ('_'))
        return juce::String::fromUTF8 ("그 아이디는 쓸 수 없습니다");

    return {};
}

juce::String WebDavBackup::newSalt()
{
    return juce::Uuid().toString();   // 32 hex characters from the system's random source
}

juce::String WebDavBackup::hashPassword (const juce::String& password, const juce::String& salt)
{
    auto round = [] (const juce::String& text)
    {
        const auto utf8 = text.toUTF8();
        return juce::SHA256 (utf8.getAddress(), utf8.sizeInBytes() - 1).toHexString();
    };

    auto hash = round (salt + ":" + password);

    for (int i = 1; i < passwordRounds; ++i)
        hash = round (hash + salt);

    return hash;
}

juce::String WebDavBackup::accountJson (const juce::String& id, const juce::String& salt, const juce::String& hash, const juce::String& pcName, juce::Time when)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("app", "LiveMix");
    object->setProperty ("id", id.trim());
    object->setProperty ("salt", salt);
    object->setProperty ("hash", hash);
    object->setProperty ("rounds", passwordRounds);
    object->setProperty ("created", when.toISO8601 (true));
    object->setProperty ("pc", pcName);
    return juce::JSON::toString (juce::var (object), true);
}

bool WebDavBackup::parseAccount (const juce::String& json, juce::String& salt, juce::String& hash)
{
    juce::var root;

    if (juce::JSON::parse (json, root).failed() || root.getDynamicObject() == nullptr)
        return false;

    salt = root.getProperty ("salt", "").toString();
    hash = root.getProperty ("hash", "").toString();
    return salt.isNotEmpty() && hash.length() == 64 && hash.containsOnly ("0123456789abcdefABCDEF");
}

juce::StringArray WebDavBackup::parseAdminList (const juce::String& text)
{
    juce::StringArray ids;

    for (const auto& line : juce::StringArray::fromLines (text))
    {
        const auto id = line.upToFirstOccurrenceOf ("#", false, false).trim();

        if (id.isNotEmpty())
            ids.add (id);
    }

    return ids;
}

juce::String WebDavBackup::accountsFolder (const juce::String& share)
{
    return normalisedShare (share) + "/accounts";
}

juce::String WebDavBackup::accountPath (const juce::String& share, const juce::String& id)
{
    return accountsFolder (share) + "/" + id.trim() + ".json";
}

juce::String WebDavBackup::adminListPath (const juce::String& share)
{
    return accountsFolder (share) + "/admins.txt";
}

juce::String WebDavBackup::accountFolder (const juce::String& share, const juce::String& id)
{
    return normalisedShare (share) + "/" + id.trim();
}

juce::String WebDavBackup::sanitiseName (const juce::String& name)
{
    juce::String out;

    for (auto c : name.trim())
        out << (juce::String ("\\/:*?\"<>|").containsChar (c) ? juce::juce_wchar ('_') : c);

    return out.isEmpty() ? juce::String ("session") : out;
}

juce::String WebDavBackup::backupPathFor (const juce::String& share, const juce::String& id, const juce::String& pcName, juce::Time when)
{
    return accountFolder (share, id) + "/" + sanitiseName (pcName) + "_" + when.formatted ("%Y-%m-%d_%H%M%S") + ".livemix";
}

bool WebDavBackup::parseBackupPath (const juce::String& share, const juce::String& path, juce::String& owner)
{
    owner.clear();
    const auto base = normalisedShare (share);

    if (base.isEmpty() || ! path.startsWith (base + "/"))
        return false;

    juce::StringArray segments;
    segments.addTokens (path.substring (base.length() + 1), "/", "");

    if (segments.size() != 2)
        return false;

    for (const auto& segment : segments)
        if (segment.isEmpty() || segment == "." || segment == "..")
            return false;

    if (validateAccountId (segments[0]).isNotEmpty() || ! segments[1].endsWithIgnoreCase (".livemix"))
        return false;

    owner = segments[0];
    return true;
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
        return juce::String::fromUTF8 ("백업 주소는 https://서버:포트 까지만 적습니다");

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

juce::String WebDavBackup::credentialKeyFor (const juce::String& baseUrl, const juce::String& accountId)
{
    const auto base = trimmedBase (baseUrl);
    auto origin = base.contains ("://") ? base.fromFirstOccurrenceOf ("://", false, false) : base;
    origin = origin.upToFirstOccurrenceOf ("/", false, false).toLowerCase();
    return "LiveMix/WebDAV/" + origin + "/" + accountId.trim();
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

std::vector<WebDavBackup::DavItem> WebDavBackup::parseMultistatus (const juce::String& xml, const juce::String& requestedPath, bool* parsedOk)
{
    std::vector<DavItem> items;
    const auto root = juce::XmlDocument::parse (xml);

    if (root == nullptr || root->getTagNameWithoutNamespace() != "multistatus")
    {
        if (parsedOk != nullptr)
            *parsedOk = false;

        return items;
    }

    if (parsedOk != nullptr)
        *parsedOk = true;

    const auto requested = stripTrailingSlash (decodePath (requestedPath));

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
        item.path = stripTrailingSlash (decodePath (raw));

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

//==============================================================================
juce::Result WebDavBackup::begin (const Target& t, Job which, bool needsAccount)
{
    if (isThreadRunning())
        return juce::Result::fail (juce::String::fromUTF8 ("백업 작업이 이미 진행 중입니다"));

    if (const auto bad = validateBaseUrl (t.baseUrl); bad.isNotEmpty())
        return juce::Result::fail (bad);

    if (normalisedShare (t.share).isEmpty() || t.user.trim().isEmpty() || t.password.isEmpty())
        return juce::Result::fail (juce::String::fromUTF8 ("이 프로그램에는 백업 서버가 설정되어 있지 않습니다"));

    if (needsAccount)
    {
        if (const auto bad = validateAccountId (t.accountId); bad.isNotEmpty())
            return juce::Result::fail (bad);

        if (t.accountPassword.isEmpty())
            return juce::Result::fail (juce::String::fromUTF8 ("비밀번호를 넣으세요"));
    }

    target = t;
    target.share = normalisedShare (t.share);
    target.user = target.user.trim();
    target.accountId = target.accountId.trim();
    job = which;
    remotePath.clear();
    localFile = juce::File();
    data.reset();
    done = nullptr;
    signInDone = nullptr;
    return juce::Result::ok();
}

juce::Result WebDavBackup::createAccount (const Target& t, Done onDone)
{
    if (const auto result = begin (t, Job::createAccount, true); result.failed())
        return result;

    if (t.accountPassword.length() < 4)
        return juce::Result::fail (juce::String::fromUTF8 ("비밀번호는 4자 이상입니다"));

    done = std::move (onDone);

    if (! startThread())
    {
        done = nullptr;
        return juce::Result::fail (juce::String::fromUTF8 ("백업 스레드를 시작하지 못했습니다"));
    }

    return juce::Result::ok();
}

juce::Result WebDavBackup::signIn (const Target& t, SignInDone onDone)
{
    if (const auto result = begin (t, Job::signIn, true); result.failed())
        return result;

    signInDone = std::move (onDone);

    if (! startThread())
    {
        signInDone = nullptr;
        return juce::Result::fail (juce::String::fromUTF8 ("백업 스레드를 시작하지 못했습니다"));
    }

    return juce::Result::ok();
}

juce::Result WebDavBackup::start (const Target& t, const juce::File& file, const juce::String& path, Done onDone)
{
    if (const auto result = begin (t, Job::upload, true); result.failed())
        return result;

    juce::MemoryBlock bytes;

    if (! file.loadFileAsData (bytes))
        return juce::Result::fail (juce::String::fromUTF8 ("세션 파일을 읽지 못했습니다: ") + file.getFullPathName());

    remotePath = path;
    data = std::move (bytes);
    done = std::move (onDone);

    if (! startThread())
    {
        done = nullptr;
        data.reset();
        return juce::Result::fail (juce::String::fromUTF8 ("백업 스레드를 시작하지 못했습니다"));
    }

    return juce::Result::ok();
}

juce::Result WebDavBackup::startDownload (const Target& t, const juce::String& path, const juce::File& file, Done onDone)
{
    if (const auto result = begin (t, Job::download, true); result.failed())
        return result;

    remotePath = path;
    localFile = file;
    done = std::move (onDone);

    if (! startThread())
    {
        done = nullptr;
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

//==============================================================================
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
    bool truncated = false;

    if (connected)
    {
        if (response != nullptr)
        {
            response->reset();
            const auto expected = stream->getTotalLength();   // -1 when the server did not say
            const auto got = (juce::int64) stream->readIntoMemoryBlock (*response);
            truncated = expected >= 0 && got != expected;
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

    if (truncated)
    {
        error = threadShouldExit() ? juce::String::fromUTF8 ("백업 작업이 취소되었습니다")
                                   : juce::String::fromUTF8 ("서버의 응답이 중간에 끊겼습니다: ") + path;
        return 0;
    }

    return status;
}

bool WebDavBackup::verifyAccount (juce::String& message)
{
    juce::String error;
    juce::MemoryBlock body;
    const int status = request ("GET", accountPath (target.share, target.accountId), nullptr, {}, error, &body);

    if (status == 0)
    {
        message = error;
        return false;
    }

    if (status == 401 || status == 403)
    {
        message = serverRefused();
        return false;
    }

    if (status == 404)
    {
        message = juce::String::fromUTF8 ("없는 아이디입니다. '계정 만들기'로 먼저 만드세요.");
        return false;
    }

    if (status != 200)
    {
        message = httpRefused (status, accountPath (target.share, target.accountId));
        return false;
    }

    juce::String salt, hash;

    if (! parseAccount (body.toString(), salt, hash))
    {
        message = juce::String::fromUTF8 ("서버의 계정 정보를 읽을 수 없습니다: ") + target.accountId;
        return false;
    }

    if (hashPassword (target.accountPassword, salt) != hash)
    {
        message = juce::String::fromUTF8 ("비밀번호가 맞지 않습니다.");
        return false;
    }

    return true;
}

bool WebDavBackup::isAdmin (juce::String& message, bool& failed)
{
    failed = false;
    juce::String error;
    juce::MemoryBlock body;
    const int status = request ("GET", adminListPath (target.share), nullptr, {}, error, &body);

    if (status == 0)
    {
        message = error;
        failed = true;
        return false;
    }

    if (status == 200)
        return parseAdminList (body.toString()).contains (target.accountId);

    if (status == 404)
        return false;   // no list: nobody is an admin

    // anything else is a failure, not "not an admin"
    message = (status == 401 || status == 403) ? serverRefused() : httpRefused (status, adminListPath (target.share));
    failed = true;
    return false;
}

bool WebDavBackup::makeFolder (const juce::String& path, juce::String& message)
{
    juce::String error;
    const int status = request ("MKCOL", path, nullptr, {}, error);

    if (status == 0)
    {
        message = error;
        return false;
    }

    if (status == 201 || status == 405)   // made, or there already
        return true;

    if (status == 401 || status == 403)
    {
        message = serverRefused();
        return false;
    }

    message = juce::String::fromUTF8 ("서버가 폴더 만들기를 거부했습니다 (HTTP ") + juce::String (status) + "): " + path;
    return false;
}

bool WebDavBackup::collectBackups (const juce::String& owner, std::vector<Entry>& entries, juce::String& message)
{
    const auto folder = accountFolder (target.share, owner);
    juce::String error;
    juce::MemoryBlock xml;
    const int status = request ("PROPFIND", withTrailingSlash (folder), nullptr, "Depth: 1\r\n", error, &xml);

    if (status == 0)
    {
        message = error;
        return false;
    }

    if (status == 401 || status == 403)
    {
        message = serverRefused();
        return false;
    }

    if (status == 404)
        return true;   // no backups there yet

    if (status != 207)
    {
        message = httpRefused (status, folder);   // a redirect, a server error: not "no backups"
        return false;
    }

    bool parsed = false;
    const auto found = parseMultistatus (xml.toString(), folder, &parsed);

    if (! parsed)
    {
        message = unreadableAnswer (folder);
        return false;
    }

    for (const auto& file : found)
    {
        juce::String pathOwner;

        if (file.collection || ! parseBackupPath (target.share, file.path, pathOwner) || pathOwner != owner)
            continue;   // not a backup of this account (or a path the server should not have listed)

        Entry entry;
        entry.owner = owner;
        entry.name = file.name();
        entry.pc = entry.name.upToLastOccurrenceOf ("_", false, false).upToLastOccurrenceOf ("_", false, false);   // <pc>_<date>_<time>.livemix
        entry.path = file.path;
        entry.modified = file.modified;
        entry.size = file.size;
        entries.push_back (std::move (entry));
    }

    return true;
}

//==============================================================================
void WebDavBackup::run()
{
    bool ok = false, everyone = false;
    juce::String message;
    std::vector<Entry> entries;

    switch (job)
    {
        case Job::createAccount: runCreateAccount (ok, message); break;
        case Job::signIn:        runSignIn (ok, message, entries, everyone); break;
        case Job::upload:        runUpload (ok, message); break;
        case Job::download:      runDownload (ok, message); break;
    }

    if (threadShouldExit())
        return;   // cancelled (a quit): nobody waits for the result

    if (job == Job::signIn)
    {
        auto callback = std::move (signInDone);
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

void WebDavBackup::runCreateAccount (bool& ok, juce::String& message)
{
    if (! makeFolder (accountsFolder (target.share), message))
        return;

    const auto salt = newSalt();
    const auto json = accountJson (target.accountId, salt, hashPassword (target.accountPassword, salt), juce::SystemStats::getComputerName(), juce::Time::getCurrentTime());
    const auto utf8 = json.toUTF8();
    juce::MemoryBlock body (utf8.getAddress(), utf8.sizeInBytes() - 1);
    juce::String error;

    // create-only: two people asking for the same id at once cannot both get it
    const int status = request ("PUT", accountPath (target.share, target.accountId), &body, "If-None-Match: *\r\n", error);

    if (status == 0)
    {
        message = error;
        return;
    }

    if (status == 412)
    {
        message = juce::String::fromUTF8 ("이미 있는 아이디입니다: ") + target.accountId;
        return;
    }

    if (status == 401 || status == 403)
    {
        message = serverRefused();
        return;
    }

    if (status != 200 && status != 201 && status != 204)
    {
        message = httpRefused (status, accountPath (target.share, target.accountId));
        return;
    }

    // the account exists from here on; its folder is best effort (the first upload makes it again)
    juce::String folderMessage;
    makeFolder (accountFolder (target.share, target.accountId), folderMessage);

    ok = true;
    message = juce::String::fromUTF8 ("계정을 만들었습니다: ") + target.accountId;
}

void WebDavBackup::runSignIn (bool& ok, juce::String& message, std::vector<Entry>& entries, bool& everyone)
{
    if (! verifyAccount (message))
        return;

    bool failed = false;
    everyone = isAdmin (message, failed);

    if (failed || threadShouldExit())
        return;

    if (everyone)
    {
        juce::String error;
        juce::MemoryBlock xml;
        const int status = request ("PROPFIND", withTrailingSlash (target.share), nullptr, "Depth: 1\r\n", error, &xml);

        if (status == 0)
        {
            message = error;
            return;
        }

        if (status != 207)
        {
            message = status == 401 ? serverRefused() : httpRefused (status, target.share);
            return;
        }

        bool parsed = false;
        const auto folders = parseMultistatus (xml.toString(), target.share, &parsed);

        if (! parsed)
        {
            message = unreadableAnswer (target.share);
            return;
        }

        for (const auto& folder : folders)
        {
            if (threadShouldExit())
                return;

            if (! folder.collection || isServiceName (folder.name()) || folder.name() == "accounts")
                continue;

            if (! collectBackups (folder.name(), entries, message))
                return;
        }
    }
    else if (! collectBackups (target.accountId, entries, message))
    {
        return;
    }

    std::stable_sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b) { return a.modified > b.modified; });
    ok = true;
    message = entries.empty() ? juce::String::fromUTF8 ("백업이 아직 없습니다")
                              : juce::String::fromUTF8 ("백업 ") + juce::String ((int) entries.size()) + juce::String::fromUTF8 ("개");
}

void WebDavBackup::runUpload (bool& ok, juce::String& message)
{
    if (! verifyAccount (message))
        return;

    if (! makeFolder (accountFolder (target.share, target.accountId), message))
        return;

    // the file lands under a temporary name and is renamed onto the final one: a cut-off upload never sits under a
    // backup's name
    const auto partPath = remotePath + ".part";
    juce::String error;
    const int put = request ("PUT", partPath, &data, {}, error);

    if (put == 200 || put == 201 || put == 204)
    {
        const auto destination = trimmedBase (target.baseUrl) + encodePath (remotePath);
        const int moved = request ("MOVE", partPath, nullptr, "Destination: " + destination + "\r\nOverwrite: T\r\n", error);

        if (moved == 200 || moved == 201 || moved == 204)
        {
            ok = true;
            message = juce::String::fromUTF8 ("백업 완료: ") + remotePath.fromLastOccurrenceOf ("/", false, false);
        }
        else if (moved == 0)
            message = error;
        else
            message = juce::String::fromUTF8 ("서버가 이름 바꾸기를 거부했습니다 (HTTP ") + juce::String (moved) + "): " + partPath
                      + juce::String::fromUTF8 (" 로 남아 있습니다");
    }
    else if (put == 401 || put == 403)
        message = serverRefused();
    else if (put == 0)
        message = error;
    else
        message = httpRefused (put, partPath);
}

void WebDavBackup::runDownload (bool& ok, juce::String& message)
{
    if (! verifyAccount (message))
        return;

    // exactly <share>/<owner>/<file>.livemix - nothing the server could resolve elsewhere; another account's backup
    // only for an admin
    juce::String owner;

    if (! parseBackupPath (target.share, remotePath, owner))
    {
        message = juce::String::fromUTF8 ("백업 경로가 올바르지 않습니다: ") + remotePath;
        return;
    }

    if (owner != target.accountId)
    {
        bool failed = false;
        const bool admin = isAdmin (message, failed);

        if (failed)
            return;

        if (! admin)
        {
            message = juce::String::fromUTF8 ("다른 계정의 백업입니다.");
            return;
        }
    }

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
        message = serverRefused();
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
