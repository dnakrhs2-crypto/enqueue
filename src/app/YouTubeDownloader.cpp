#include "app/YouTubeDownloader.h"

#include "audio/MediaFoundationAudioFormat.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace gocue
{

namespace
{
    juce::String k (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    constexpr int maxKeptLines = 200;              // of yt-dlp's output, for the log
    constexpr int ytDlpTimeoutMs = 30 * 60 * 1000;
    constexpr int lameTimeoutMs = 20 * 60 * 1000;

    bool isVideoId (const juce::String& s)
    {
        return s.length() >= 8 && s.length() <= 16 && s.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-");
    }
}

//==============================================================================
YouTubeDownloader::YouTubeDownloader (juce::String version)
    : juce::Thread ("YouTube download"), appVersion (std::move (version))
{
}

YouTubeDownloader::~YouTubeDownloader()
{
    cancel();
    stopThread (30000);   // the tool running at that moment is ended by the cancel flag, so the thread returns soon
    cancelPendingUpdate();
}

bool YouTubeDownloader::start (const juce::String& url, const juce::File& dir)
{
    if (busy.exchange (true))
        return false;

    stopThread (30000);   // the previous job's thread has finished its work: this only joins it
    cancelRequested.store (false);
    videoUrl = url;
    directory = dir;
    startThread();
    return true;
}

void YouTubeDownloader::cancel()
{
    cancelRequested.store (true);
    signalThreadShouldExit();
    notify();
}

//==============================================================================
void YouTubeDownloader::report (Stage stage, double fraction, const juce::String& message, const juce::String& detail, const juce::File& file)
{
    {
        const juce::ScopedLock sl (lock);
        pending.stage = stage;
        pending.fraction = fraction;
        pending.message = message;
        pending.detail = detail;
        pending.file = file;
    }

    if (stage == Stage::done || stage == Stage::failed || stage == Stage::cancelled)
        busy.store (false);   // before the UI hears about it: it may start the next job from the callback

    triggerAsyncUpdate();
}

void YouTubeDownloader::handleAsyncUpdate()
{
    Progress p;

    {
        const juce::ScopedLock sl (lock);
        p = pending;
    }

    if (onProgress)
        onProgress (p);
}

//==============================================================================
juce::StringArray YouTubeDownloader::ytDlpArguments (const YouTubeTools::Paths& tools, const juce::String& url, const juce::File& outputDirectory)
{
    return { tools.ytDlp.getFullPathName(),
             "--ignore-config",                                                            // no yt-dlp.conf of the user's changing what we expect
             "--no-js-runtimes", "--js-runtimes", "quickjs:" + tools.qjs.getFullPathName(),   // the bundled runtime, not a system one
             "--no-playlist", "--playlist-items", "1",                                       // one video, whatever the link also names
             "--encoding", "utf-8",
             "--newline", "--progress",
             "--socket-timeout", "30", "--retries", "3",
             "--windows-filenames", "--trim-filenames", "150",
             "-f", "bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]",                             // AAC only: what the app can decode
             "-o", outputDirectory.getChildFile ("%(title)s.%(ext)s").getFullPathName(),
             "--print", "after_move:filepath",                                                // the last line: where the file ended up
             url };
}

juce::StringArray YouTubeDownloader::lameArguments (const juce::File& lame, const juce::File& wav, const juce::File& mp3)
{
    return { lame.getFullPathName(), "--quiet", "--cbr", "-b", "320", wav.getFullPathName(), mp3.getFullPathName() };
}

double YouTubeDownloader::parseProgress (const juce::String& line)
{
    const auto t = line.trim();

    if (! t.startsWith ("[download]"))
        return -1.0;

    const auto rest = t.fromFirstOccurrenceOf ("[download]", false, false).trim();
    const auto number = rest.upToFirstOccurrenceOf ("%", false, false).trim();

    if (number.isEmpty() || ! rest.contains ("%") || ! number.containsOnly ("0123456789."))
        return -1.0;

    return juce::jlimit (0.0, 1.0, number.getDoubleValue() / 100.0);
}

YouTubeDownloader::Failure YouTubeDownloader::classifyFailure (const juce::StringArray& lines)
{
    Failure f;
    juce::String lastError;

    for (const auto& line : lines)
        if (line.trim().startsWith ("ERROR:"))
            lastError = line.trim();

    const auto all = lines.joinIntoString ("\n");

    struct Known { const char* needle; const char* message; Failure::Kind kind; };
    static const Known known[] = {
        { "Sign in to confirm",                     "유튜브가 로그인(봇 확인)을 요구합니다. 잠시 뒤 다시 하거나 다른 네트워크에서 시도해 주세요.", Failure::Kind::tool },
        { "Private video",                          "비공개 영상입니다.", Failure::Kind::video },
        { "Video unavailable",                      "볼 수 없는 영상입니다 (삭제되었거나 이 지역에서 막혀 있습니다).", Failure::Kind::video },
        { "This video is not available",            "볼 수 없는 영상입니다 (삭제되었거나 이 지역에서 막혀 있습니다).", Failure::Kind::video },
        { "members-only",                           "멤버십 전용 영상입니다.", Failure::Kind::video },
        { "Requested format is not available",      "받을 수 있는 오디오(AAC) 형식이 없는 영상입니다.", Failure::Kind::video },
        { "is not a valid URL",                     "올바른 주소가 아닙니다.", Failure::Kind::video },
        { "Unsupported URL",                        "지원하지 않는 주소입니다.", Failure::Kind::video },
        { "is a live event",                        "아직 시작하지 않은 실시간 방송입니다.", Failure::Kind::video },
        { "No supported JavaScript runtime",        "다운로드 도구(JS 런타임)가 없거나 손상되었습니다.", Failure::Kind::tool },
        { "getaddrinfo failed",                     "인터넷에 연결할 수 없습니다.", Failure::Kind::network },
        { "Temporary failure in name resolution",   "인터넷에 연결할 수 없습니다.", Failure::Kind::network },
        { "Network is unreachable",                 "인터넷에 연결할 수 없습니다.", Failure::Kind::network },
        { "Errno 11001",                            "인터넷에 연결할 수 없습니다.", Failure::Kind::network },
        { "Unable to download webpage",             "유튜브에 연결하지 못했습니다. 인터넷 연결을 확인해 주세요.", Failure::Kind::network },
        { "Unable to download API page",            "유튜브에 연결하지 못했습니다. 인터넷 연결을 확인해 주세요.", Failure::Kind::network },
        { "timed out",                              "유튜브 응답이 없습니다 (시간 초과). 인터넷 연결을 확인해 주세요.", Failure::Kind::network },
        { "Connection reset",                       "유튜브와 연결이 끊겼습니다. 다시 시도해 주세요.", Failure::Kind::network },
        { "Remote end closed connection",           "유튜브와 연결이 끊겼습니다. 다시 시도해 주세요.", Failure::Kind::network },
    };

    for (const auto& item : known)
    {
        if (all.containsIgnoreCase (item.needle))
        {
            f.message = k (item.message);
            f.kind = item.kind;
            return f;
        }
    }

    // an age check reads differently across versions: only an ERROR line about it counts
    if (lastError.containsIgnoreCase ("age-restricted") || lastError.containsIgnoreCase ("age verification") || lastError.containsIgnoreCase ("confirm your age"))
    {
        f.message = k ("연령 확인이 필요한 영상이라 받을 수 없습니다.");
        f.kind = Failure::Kind::video;
        return f;
    }

    f.message = lastError.isNotEmpty() ? k ("유튜브에서 받지 못했습니다: ") + lastError.fromFirstOccurrenceOf ("ERROR:", false, false).trim()
                                       : k ("유튜브에서 받지 못했습니다.");
    f.kind = Failure::Kind::tool;
    return f;
}

juce::String YouTubeDownloader::normaliseUrl (const juce::String& text)
{
    auto url = text.trim().unquoted().trim();

    if (url.isEmpty() || url.containsAnyOf (" \t\r\n"))
        return {};

    if (! url.contains ("://"))
        url = "https://" + url;

    if (! (url.startsWithIgnoreCase ("https://") || url.startsWithIgnoreCase ("http://")))
        return {};

    const juce::URL parsed (url);
    const auto host = parsed.getDomain().toLowerCase();
    const bool youtube = host == "youtube.com" || host.endsWith (".youtube.com");
    const bool shortHost = host == "youtu.be" || host.endsWith (".youtu.be");

    if (! youtube && ! shortHost)
        return {};

    // one video: a watch link with v=, youtu.be/<id>, or /shorts|live|embed|v/<id> - not a playlist or a channel
    const auto path = parsed.getSubPath (false);
    const auto segments = juce::StringArray::fromTokens (path, "/", {});
    juce::StringArray parts;

    for (const auto& s : segments)
        if (s.isNotEmpty())
            parts.add (s);

    if (shortHost)
        return parts.size() >= 1 && isVideoId (parts[0].upToFirstOccurrenceOf ("?", false, false)) ? url : juce::String();

    if (parts.size() >= 1 && parts[0].startsWithIgnoreCase ("watch"))
    {
        const auto names = parsed.getParameterNames();
        const auto values = parsed.getParameterValues();
        const int v = names.indexOf ("v");
        return v >= 0 && isVideoId (values[v]) ? url : juce::String();
    }

    if (parts.size() >= 2)
    {
        const auto kind = parts[0].toLowerCase();

        if (kind == "shorts" || kind == "live" || kind == "embed" || kind == "v")
            return isVideoId (parts[1].upToFirstOccurrenceOf ("?", false, false)) ? url : juce::String();
    }

    return {};
}

//==============================================================================
juce::File YouTubeDownloader::runYtDlp (const YouTubeTools::Paths& tools, const juce::File& workDirectory, ToolProcess::Result& outcome, juce::StringArray& lines)
{
    lines.clear();
    juce::File lastFile;

    outcome = ToolProcess::run (ytDlpArguments (tools, videoUrl, workDirectory), ytDlpTimeoutMs, &cancelRequested, [&] (const juce::String& raw)
    {
        const auto line = raw.trim();

        if (line.isEmpty())
            return;

        if (lines.size() >= maxKeptLines)
            lines.remove (0);

        lines.add (line);

        if (const double p = parseProgress (line); p >= 0.0)
            report (Stage::fetching, p, k ("유튜브에서 받는 중 ") + juce::String ((int) (p * 100.0)) + "%");

        if (juce::File::isAbsolutePath (line) && juce::File (line).existsAsFile())
            lastFile = juce::File (line);   // --print after_move:filepath
    });

    if (! outcome.started)
        lines.add ("ERROR: could not start yt-dlp");

    return outcome.finished && outcome.exitCode == 0 ? lastFile : juce::File();
}

juce::String YouTubeDownloader::convertToMp3 (const juce::File& source, const juce::File& target, const juce::File& lame)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    if (MediaFoundationAudioFormat::isAvailable())
        formats.registerFormat (new MediaFoundationAudioFormat(), false);   // AAC in m4a: what YouTube serves

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));

    if (reader == nullptr)
        return k ("받은 오디오를 읽을 수 없습니다 (") + source.getFileExtension() + ")";

    if (reader->numChannels < 1 || reader->numChannels > 2)
        return k ("채널이 ") + juce::String ((int) reader->numChannels) + k ("개인 오디오는 지원하지 않습니다 (모노·스테레오만).");

    if (reader->lengthInSamples <= 0)
        return k ("받은 오디오가 비어 있습니다.");

    // 1. the decoded audio as a 16-bit WAV next to the source (the work folder, deleted afterwards)
    const auto wav = source.getSiblingFile (source.getFileNameWithoutExtension() + ".decoded.wav");
    wav.deleteFile();
    const int channels = (int) reader->numChannels;

    {
        auto stream = std::make_unique<juce::FileOutputStream> (wav);

        if (! stream->openedOk())
            return k ("임시 파일을 쓸 수 없습니다: ") + wav.getFullPathName();

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::OutputStream> out = std::move (stream);
        auto writer = wavFormat.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (reader->sampleRate)
                                                                                     .withNumChannels (channels)
                                                                                     .withBitsPerSample (16));

        if (writer == nullptr)
            return k ("임시 WAV를 만들지 못했습니다.");

        const juce::int64 total = reader->lengthInSamples;
        juce::AudioBuffer<float> block (channels, 16384);
        juce::int64 position = 0;

        while (position < total)
        {
            if (cancelRequested.load())
                return k ("취소");

            const int n = (int) juce::jmin ((juce::int64) block.getNumSamples(), total - position);

            if (! reader->read (&block, 0, n, position, true, true))
                return k ("받은 오디오를 끝까지 읽지 못했습니다 (파일이 손상되었을 수 있습니다).");

            if (! writer->writeFromAudioSampleBuffer (block, 0, n))
                return k ("임시 WAV를 쓰는 중 오류가 났습니다.");

            position += n;
            report (Stage::converting, 0.5 * (double) position / (double) total, k ("디코딩 중 ") + juce::String ((int) (50 * position / total)) + "%");
        }

        writer->flush();
    }

    // 2. LAME: cancellable like yt-dlp, the mp3 into a .part file first
    const auto part = target.getSiblingFile (target.getFileName() + ".part");
    part.deleteFile();
    report (Stage::converting, 0.5, k ("mp3(320 kbps)로 변환 중..."));
    const auto outcome = ToolProcess::run (lameArguments (lame, wav, part), lameTimeoutMs, &cancelRequested, nullptr);
    wav.deleteFile();

    if (outcome.cancelled)
    {
        part.deleteFile();
        return k ("취소");
    }

    if (! outcome.started || ! outcome.finished || outcome.exitCode != 0 || ! part.existsAsFile() || part.getSize() < 1024)
    {
        part.deleteFile();
        return ! outcome.started ? k ("mp3 인코더(lame.exe)를 실행할 수 없습니다: ") + lame.getFullPathName()
             : outcome.timedOut ? k ("mp3 변환이 너무 오래 걸려 중단했습니다.")
                                : k ("mp3 변환에 실패했습니다 (LAME 오류 ") + juce::String (outcome.exitCode) + ").";
    }

    target.deleteFile();

    if (! part.moveFileTo (target))
    {
        part.deleteFile();
        return k ("파일을 저장하지 못했습니다: ") + target.getFullPathName();
    }

    report (Stage::converting, 1.0, k ("변환 완료"));
    return {};
}

//==============================================================================
void YouTubeDownloader::run()
{
    struct ClearBusy
    {
        std::atomic<bool>& flag;
        ~ClearBusy() { flag.store (false); }
    } clearBusy { busy };

    const auto cancelled = [this] { report (Stage::cancelled, 0.0, k ("취소했습니다.")); };

    directory.createDirectory();

    if (! directory.isDirectory())
    {
        fail (k ("저장 폴더를 만들 수 없습니다: ") + directory.getFullPathName());
        return;
    }

    // 1. the tools
    report (Stage::preparing, -1.0, k ("도구 준비 중..."));
    YouTubeTools::Paths tools;

    if (const auto error = YouTubeTools::prepare (tools, appVersion); error.isNotEmpty())
    {
        fail (error);
        return;
    }

    if (cancelRequested.load())
        return cancelled();

    // 2. yt-dlp, with one more try after a self-update when the tool (not the video, not the network) is the problem
    const auto work = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("Enqueue-youtube").getChildFile (juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    work.createDirectory();

    struct CleanUp
    {
        juce::File dir;
        ~CleanUp() { dir.deleteRecursively(); }
    } cleanUp { work };

    juce::File audio;
    juce::StringArray lines;

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        report (Stage::fetching, -1.0, k ("유튜브에서 받는 중..."));
        ToolProcess::Result outcome;
        audio = runYtDlp (tools, work, outcome, lines);

        if (outcome.cancelled || cancelRequested.load())
            return cancelled();

        if (audio.existsAsFile())
            break;

        audio = {};

        if (outcome.timedOut)
        {
            fail (k ("유튜브에서 받는 데 너무 오래 걸려 중단했습니다 (30분)."));
            return;
        }

        const auto failure = classifyFailure (lines);
        juce::StringArray tail;   // yt-dlp's last words, for the log

        for (int i = juce::jmax (0, lines.size() - 8); i < lines.size(); ++i)
            tail.add (lines[i]);

        if (attempt == 1 || failure.kind != Failure::Kind::tool)
        {
            fail (failure.message, tail.joinIntoString ("\n"));
            return;
        }

        report (Stage::updatingTool, -1.0, k ("유튜브 쪽이 바뀌었을 수 있어 yt-dlp를 갱신하는 중..."));
        juce::String note;
        const bool updated = YouTubeTools::selfUpdateYtDlp (tools.ytDlp, note, &cancelRequested);

        if (cancelRequested.load())
            return cancelled();

        if (! updated)
        {
            fail (failure.message, tail.joinIntoString ("\n") + "\n" + note);
            return;
        }

        lines.clear();
    }

    // 3. the mp3
    const auto target = directory.getChildFile (audio.getFileNameWithoutExtension() + ".mp3").getNonexistentSibling (true);
    report (Stage::converting, 0.0, k ("변환 준비 중..."));

    if (const auto error = convertToMp3 (audio, target, tools.lame); error.isNotEmpty())
    {
        if (cancelRequested.load())
            return cancelled();

        fail (error);
        return;
    }

    report (Stage::done, 1.0, k ("완료: ") + target.getFileName(), {}, target);
}

} // namespace gocue
