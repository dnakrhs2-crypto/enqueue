#include "WebDavBackup.h"

#include <juce_events/juce_events.h>

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

    juce::String loginRefused()
    {
        return juce::String::fromUTF8 ("로그인이 거부되었습니다 (사용자 이름 / 비밀번호 확인).");
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
    auto base = folder.trim();

    if (! base.startsWithChar ('/'))
        base = "/" + base;

    while (base.endsWithChar ('/') && base.length() > 1)
        base = base.dropLastCharacters (1);

    return base + "/" + sanitiseName (pcName) + "/" + sanitiseName (creator) + "_" + when.formatted ("%Y-%m-%d_%H%M%S") + ".livemix";
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

        for (auto c : host)
            if (! juce::String ("0123456789abcdefABCDEF:.").containsChar (c))
                return juce::String::fromUTF8 ("백업 주소의 IPv6 주소가 올바르지 않습니다");

        if (host.isEmpty())
            return juce::String::fromUTF8 ("백업 주소에 서버 이름이 없습니다 (예: https://서버:5006)");
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

juce::Result WebDavBackup::start (const Target& t, const juce::File& localFile, const juce::String& path,
                                  std::function<void (bool, const juce::String&)> onDone)
{
    if (isThreadRunning())
        return juce::Result::fail (juce::String::fromUTF8 ("백업 업로드가 이미 진행 중입니다"));

    if (const auto bad = validateBaseUrl (t.baseUrl); bad.isNotEmpty())
        return juce::Result::fail (bad);

    juce::MemoryBlock bytes;

    if (! localFile.loadFileAsData (bytes))
        return juce::Result::fail (juce::String::fromUTF8 ("세션 파일을 읽지 못했습니다: ") + localFile.getFullPathName());

    target = t;
    remotePath = path;
    data = std::move (bytes);
    done = std::move (onDone);

    if (! startThread())
    {
        done = nullptr;
        data.reset();
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
                           const juce::String& extraHeaders, juce::String& error)
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
            error = juce::String::fromUTF8 ("백업이 취소되었습니다");
            return 0;
        }

        active = stream.get();
    }

    const bool connected = stream->connect (nullptr);
    const int status = stream->getStatusCode();

    if (connected)
        stream->readEntireStreamAsString();   // drain the (small) response

    {
        const juce::ScopedLock sl (activeLock);
        active = nullptr;
    }

    if (status == 0)
    {
        error = threadShouldExit() ? juce::String::fromUTF8 ("백업이 취소되었습니다")
                                   : juce::String::fromUTF8 ("서버에 연결하지 못했습니다: ") + base;
        return 0;
    }

    return status;
}

void WebDavBackup::run()
{
    juce::String error, message;
    bool ok = false;

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
            break;
        }

        if (status == 401 || status == 403)
        {
            message = loginRefused();
            break;
        }
    }

    if (message.isEmpty() && ! threadShouldExit())
    {
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
            message = juce::String::fromUTF8 ("서버가 거부했습니다 (HTTP ") + juce::String (put) + "): " + partPath;
    }

    if (threadShouldExit())
        return;   // cancelled (a quit): nobody waits for the result

    auto callback = std::move (done);
    juce::MessageManager::callAsync ([callback, ok, message]
    {
        if (callback)
            callback (ok, message);
    });
}

} // namespace gocue::livemix
