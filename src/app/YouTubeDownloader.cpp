#include "app/YouTubeDownloader.h"

#include "audio/MediaFoundationAudioFormat.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace gocue
{

namespace
{
    juce::String k (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    constexpr int maxKeptLines = 200;         // of yt-dlp's output, for the log
    constexpr int ytDlpTimeoutMs = 30 * 60 * 1000;
}

//==============================================================================
YouTubeDownloader::YouTubeDownloader (juce::String version)
    : juce::Thread ("YouTube download"), appVersion (std::move (version))
{
}

YouTubeDownloader::~YouTubeDownloader()
{
    cancel();
    stopThread (20000);
    cancelPendingUpdate();
}

bool YouTubeDownloader::start (const juce::String& url, const juce::File& dir)
{
    if (busy.exchange (true))
        return false;

    stopThread (20000);   // the previous job's thread has finished its work: this only joins it
    videoUrl = url;
    directory = dir;
    startThread();
    return true;
}

void YouTubeDownloader::cancel()
{
    signalThreadShouldExit();
    notify();

    const juce::ScopedLock sl (lock);

    if (activeProcess != nullptr)
        activeProcess->kill();
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
             "--no-js-runtimes", "--js-runtimes", "quickjs:" + tools.qjs.getFullPathName(),   // the bundled runtime, not a system one
             "--no-playlist",
             "--encoding", "utf-8",
             "--newline", "--progress",
             "--socket-timeout", "30", "--retries", "3",
             "--windows-filenames", "--trim-filenames", "150",
             "-f", "bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]/bestaudio",
             "-o", outputDirectory.getChildFile ("%(title)s.%(ext)s").getFullPathName(),
             "--print", "after_move:filepath",   // the last line: where the file ended up
             url };
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

    struct Known { const char* needle; const char* message; bool aboutTheVideo; };
    static const Known known[] = {
        { "Sign in to confirm",          "유튜브가 로그인(봇 확인)을 요구합니다. 잠시 뒤 다시 하거나 다른 네트워크에서 시도해 주세요.", false },
        { "Private video",               "비공개 영상입니다.", true },
        { "Video unavailable",           "볼 수 없는 영상입니다 (삭제되었거나 이 지역에서 막혀 있습니다).", true },
        { "This video is not available", "볼 수 없는 영상입니다 (삭제되었거나 이 지역에서 막혀 있습니다).", true },
        { "members-only",                "멤버십 전용 영상입니다.", true },
        { "age",                         "연령 확인이 필요한 영상이라 받을 수 없습니다.", true },
        { "Requested format is not available", "받을 수 있는 오디오 형식이 없는 영상입니다.", true },
        { "is not a valid URL",          "올바른 주소가 아닙니다.", true },
        { "Unsupported URL",             "지원하지 않는 주소입니다.", true },
        { "is a live event",             "아직 시작하지 않은 실시간 방송입니다.", true },
        { "No supported JavaScript runtime", "다운로드 도구(JS 런타임)가 없거나 손상되었습니다.", false },
        { "getaddrinfo failed",          "인터넷에 연결할 수 없습니다.", false },
        { "Unable to download webpage",  "유튜브에 연결하지 못했습니다. 인터넷 연결을 확인해 주세요.", false },
    };

    for (const auto& item : known)
    {
        // "age" alone is too short to search for anywhere: only in an ERROR line, as a word
        const bool ageCase = juce::String (item.needle) == "age";
        const bool hit = ageCase ? (lastError.containsIgnoreCase ("age-restricted") || lastError.containsIgnoreCase ("age verification") || lastError.containsIgnoreCase ("confirm your age"))
                                 : all.containsIgnoreCase (item.needle);

        if (hit)
        {
            f.message = k (item.message);
            f.aboutTheVideo = item.aboutTheVideo;
            return f;
        }
    }

    f.message = lastError.isNotEmpty() ? k ("유튜브에서 받지 못했습니다: ") + lastError.fromFirstOccurrenceOf ("ERROR:", false, false).trim()
                                       : k ("유튜브에서 받지 못했습니다.");
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

    const auto host = juce::URL (url).getDomain().toLowerCase();
    const bool youtube = host == "youtube.com" || host.endsWith (".youtube.com")
                      || host == "youtu.be" || host.endsWith (".youtu.be");

    return youtube ? url : juce::String();
}

//==============================================================================
juce::File YouTubeDownloader::runYtDlp (const YouTubeTools::Paths& tools, const juce::File& workDirectory, int& exitCode, juce::StringArray& lines)
{
    exitCode = -1;
    lines.clear();
    juce::ChildProcess process;

    {
        const juce::ScopedLock sl (lock);

        if (threadShouldExit())
            return {};

        activeProcess = &process;
    }

    struct Unregister
    {
        YouTubeDownloader& owner;
        ~Unregister() { const juce::ScopedLock sl (owner.lock); owner.activeProcess = nullptr; }
    } unregister { *this };

    if (! process.start (ytDlpArguments (tools, videoUrl, workDirectory), juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        lines.add ("ERROR: could not start yt-dlp");
        return {};
    }

    juce::MemoryBlock partial;
    const auto deadline = juce::Time::currentTimeMillis() + ytDlpTimeoutMs;
    char buffer[4096];
    juce::File lastFile;

    const auto takeLine = [&] (const juce::String& raw)
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
    };

    for (;;)
    {
        const int n = process.readProcessOutput (buffer, (int) sizeof (buffer));

        if (n > 0)
        {
            partial.append (buffer, (size_t) n);

            // whole lines out of the accumulated bytes (utf-8; a line may arrive in pieces)
            for (;;)
            {
                const auto* data = (const char*) partial.getData();
                const auto size = partial.getSize();
                size_t cut = 0;

                while (cut < size && data[cut] != '\n' && data[cut] != '\r')
                    ++cut;

                if (cut >= size)
                    break;

                takeLine (juce::String::fromUTF8 (data, (int) cut));
                partial.removeSection (0, cut + 1);
            }
        }
        else if (! process.isRunning())
        {
            break;
        }
        else
        {
            juce::Thread::sleep (30);
        }

        if (threadShouldExit() || juce::Time::currentTimeMillis() > deadline)
        {
            process.kill();
            break;
        }
    }

    if (partial.getSize() > 0)
        takeLine (juce::String::fromUTF8 ((const char*) partial.getData(), (int) partial.getSize()));

    exitCode = (int) process.getExitCode();
    return lastFile;
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

    juce::LAMEEncoderAudioFormat encoder (lame);
    const int quality = encoder.getQualityOptions().indexOf ("320 Kb/s CBR");

    if (quality < 0)
        return k ("mp3 인코더 설정을 찾지 못했습니다.");

    const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
    target.deleteFile();
    auto fileStream = std::make_unique<juce::FileOutputStream> (target);

    if (! fileStream->openedOk())
        return k ("파일을 쓸 수 없습니다: ") + target.getFullPathName();

    std::unique_ptr<juce::OutputStream> out = std::move (fileStream);
    auto writer = encoder.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (reader->sampleRate)
                                                                                    .withNumChannels (channels)
                                                                                    .withBitsPerSample (16)
                                                                                    .withQualityOptionIndex (quality));

    if (writer == nullptr)
        return k ("mp3 인코더를 시작하지 못했습니다: ") + lame.getFullPathName();

    const juce::int64 total = juce::jmax ((juce::int64) 1, reader->lengthInSamples);
    juce::AudioBuffer<float> block (channels, 16384);
    juce::int64 position = 0;

    while (position < reader->lengthInSamples)
    {
        if (threadShouldExit())
        {
            writer.reset();
            target.deleteFile();
            return k ("취소");
        }

        const int n = (int) juce::jmin ((juce::int64) block.getNumSamples(), reader->lengthInSamples - position);

        if (! reader->read (&block, 0, n, position, true, true))
            break;

        if (! writer->writeFromAudioSampleBuffer (block, 0, n))
        {
            writer.reset();
            target.deleteFile();
            return k ("mp3를 쓰는 중 오류가 났습니다.");
        }

        position += n;
        report (Stage::converting, (double) position / (double) total, k ("mp3(320 kbps)로 변환 중 ") + juce::String ((int) (100 * position / total)) + "%");
    }

    writer.reset();   // runs LAME on the decoded audio and writes the mp3 into the file

    if (! target.existsAsFile() || target.getSize() < 1024)
    {
        target.deleteFile();
        return k ("mp3 변환에 실패했습니다 (LAME 실행 실패).");
    }

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

    // 2. yt-dlp, with one more try after a self-update when the tool (not the video) is the problem
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
        int exitCode = -1;
        audio = runYtDlp (tools, work, exitCode, lines);

        if (threadShouldExit())
            return cancelled();

        if (exitCode == 0 && audio.existsAsFile())
            break;

        audio = {};
        const auto failure = classifyFailure (lines);
        juce::StringArray tail;   // yt-dlp's last words, for the log

        for (int i = juce::jmax (0, lines.size() - 8); i < lines.size(); ++i)
            tail.add (lines[i]);

        if (attempt == 1 || failure.aboutTheVideo)
        {
            fail (failure.message, tail.joinIntoString ("\n"));
            return;
        }

        report (Stage::updatingTool, -1.0, k ("유튜브 쪽이 바뀌었을 수 있어 yt-dlp를 갱신하는 중..."));
        juce::String note;

        if (! YouTubeTools::selfUpdateYtDlp (tools.ytDlp, note))
        {
            fail (failure.message, tail.joinIntoString ("\n") + "\n" + note);
            return;
        }

        lines.clear();

        if (threadShouldExit())
            return cancelled();
    }

    // 3. the mp3
    const auto target = directory.getChildFile (audio.getFileNameWithoutExtension() + ".mp3").getNonexistentSibling (true);
    report (Stage::converting, 0.0, k ("mp3(320 kbps)로 변환 중..."));

    if (const auto error = convertToMp3 (audio, target, tools.lame); error.isNotEmpty())
    {
        if (threadShouldExit())
            return cancelled();

        fail (error);
        return;
    }

    if (threadShouldExit())
    {
        target.deleteFile();
        return cancelled();
    }

    report (Stage::done, 1.0, k ("완료: ") + target.getFileName(), {}, target);
}

} // namespace gocue
