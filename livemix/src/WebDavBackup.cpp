#include "WebDavBackup.h"

#include <juce_events/juce_events.h>

namespace gocue::livemix
{

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
}

int WebDavBackup::request (const Target& target, const juce::String& method, const juce::String& path, const juce::MemoryBlock* body, juce::String& error)
{
    auto base = target.baseUrl.trim();

    while (base.endsWithChar ('/'))
        base = base.dropLastCharacters (1);

    juce::URL url (base + encodePath (path));

    if (body != nullptr)
        url = url.withPOSTData (*body);

    const auto auth = "Basic " + juce::Base64::toBase64 (target.user + ":" + target.password);
    juce::String headers = "Authorization: " + auth + "\r\n";

    if (body != nullptr)
        headers << "Content-Type: application/octet-stream\r\n";

    int status = 0;
    auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                       .withHttpRequestCmd (method)
                       .withExtraHeaders (headers)
                       .withConnectionTimeoutMs (20000)
                       .withStatusCode (&status)
                       .withNumRedirectsToFollow (0);
    auto stream = url.createInputStream (options);

    if (stream == nullptr && status == 0)
    {
        error = juce::String::fromUTF8 ("서버에 연결하지 못했습니다: ") + base;
        return 0;
    }

    if (stream != nullptr)
        stream->readEntireStreamAsString();   // drain

    return status;
}

void WebDavBackup::upload (const Target& target, const juce::File& localFile, const juce::String& remotePath,
                           std::function<void (bool, const juce::String&)> done)
{
    juce::MemoryBlock data;

    if (! localFile.loadFileAsData (data))
    {
        done (false, juce::String::fromUTF8 ("세션 파일을 읽지 못했습니다: ") + localFile.getFullPathName());
        return;
    }

    juce::Thread::launch ([target, remotePath, data, done]
    {
        juce::String error;
        bool ok = false;
        juce::String message;

        // the folders: MKCOL each level (405 = exists already)
        juce::StringArray parts;
        parts.addTokens (remotePath, "/", "");
        parts.removeEmptyStrings();
        juce::String path;

        for (int i = 0; i < parts.size() - 1; ++i)
        {
            path << "/" << parts[i];
            const int status = request (target, "MKCOL", path, nullptr, error);

            if (status == 0)
            {
                message = error;
                break;
            }

            if (status == 401 || status == 403)
            {
                message = juce::String::fromUTF8 ("로그인이 거부되었습니다 (사용자 이름 / 비밀번호 확인).");
                break;
            }
        }

        if (message.isEmpty())
        {
            const int status = request (target, "PUT", remotePath, &data, error);

            if (status == 200 || status == 201 || status == 204)
            {
                ok = true;
                message = juce::String::fromUTF8 ("백업 완료: ") + remotePath;
            }
            else if (status == 401 || status == 403)
                message = juce::String::fromUTF8 ("로그인이 거부되었습니다 (사용자 이름 / 비밀번호 확인).");
            else if (status == 0)
                message = error;
            else
                message = juce::String::fromUTF8 ("서버가 거부했습니다 (HTTP ") + juce::String (status) + "): " + remotePath;
        }

        juce::MessageManager::callAsync ([done, ok, message] { done (ok, message); });
    });
}

} // namespace gocue::livemix
