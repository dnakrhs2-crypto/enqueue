#include "app/YouTubeDownloader.h"
#include "app/YouTubeTools.h"

#include <juce_core/juce_core.h>

namespace gocue
{

class YouTubeDownloaderTests : public juce::UnitTest
{
public:
    YouTubeDownloaderTests() : juce::UnitTest ("YouTubeDownloader", "Enqueue") {}

    void runTest() override
    {
        using D = YouTubeDownloader;

        beginTest ("normaliseUrl takes a link to one YouTube video only");
        {
            expectEquals (D::normaliseUrl ("https://www.youtube.com/watch?v=dQw4w9WgXcQ"), juce::String ("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
            expectEquals (D::normaliseUrl ("  youtu.be/dQw4w9WgXcQ  "), juce::String ("https://youtu.be/dQw4w9WgXcQ"));
            expectEquals (D::normaliseUrl ("https://youtu.be/dQw4w9WgXcQ?t=12"), juce::String ("https://youtu.be/dQw4w9WgXcQ?t=12"));
            expectEquals (D::normaliseUrl ("\"https://music.youtube.com/watch?v=abcdefghijk&list=x\""), juce::String ("https://music.youtube.com/watch?v=abcdefghijk&list=x"));   // a video inside a list: the video
            expectEquals (D::normaliseUrl ("https://www.youtube.com/shorts/abcdefghijk"), juce::String ("https://www.youtube.com/shorts/abcdefghijk"));
            expectEquals (D::normaliseUrl ("https://www.youtube.com/live/abcdefghijk?feature=share"), juce::String ("https://www.youtube.com/live/abcdefghijk?feature=share"));
            expectEquals (D::normaliseUrl ("https://www.youtube.com/embed/abcdefghijk"), juce::String ("https://www.youtube.com/embed/abcdefghijk"));
            expectEquals (D::normaliseUrl ("m.youtube.com/watch?v=abcdefghijk"), juce::String ("https://m.youtube.com/watch?v=abcdefghijk"));
            expect (D::normaliseUrl ("").isEmpty());
            expect (D::normaliseUrl ("   ").isEmpty());
            expect (D::normaliseUrl ("https://vimeo.com/12345").isEmpty());
            expect (D::normaliseUrl ("https://notyoutube.com/watch?v=abcdefghijk").isEmpty());
            expect (D::normaliseUrl ("https://youtube.com.evil.com/watch?v=abcdefghijk").isEmpty());
            expect (D::normaliseUrl ("youtube").isEmpty());
            expect (D::normaliseUrl ("ftp://www.youtube.com/watch?v=abcdefghijk").isEmpty());
            expect (D::normaliseUrl ("my song https://youtu.be/abcdefghijk").isEmpty());   // not a single address
            expect (D::normaliseUrl ("https://www.youtube.com/playlist?list=PLabcdefghijklmnop").isEmpty());   // a whole playlist
            expect (D::normaliseUrl ("https://www.youtube.com/@somechannel").isEmpty());                        // a channel
            expect (D::normaliseUrl ("https://www.youtube.com/channel/UCabcdefghijklmnop").isEmpty());
            expect (D::normaliseUrl ("https://www.youtube.com/c/somechannel/videos").isEmpty());
            expect (D::normaliseUrl ("https://www.youtube.com/watch?list=PLabc").isEmpty());                    // watch without a video
            expect (D::normaliseUrl ("https://www.youtube.com/").isEmpty());
            expect (D::normaliseUrl ("https://youtu.be/").isEmpty());
        }

        beginTest ("parseProgress reads yt-dlp's progress lines");
        {
            expectWithinAbsoluteError (D::parseProgress ("[download]  45.3% of    3.45MiB at    1.23MiB/s ETA 00:02"), 0.453, 0.0001);
            expectWithinAbsoluteError (D::parseProgress ("[download] 100% of  302.04KiB in 00:00:00 at 1.43MiB/s"), 1.0, 0.0001);
            expectWithinAbsoluteError (D::parseProgress ("[download]   0.0% of ~ 10.00MiB at  Unknown B/s ETA Unknown"), 0.0, 0.0001);
            expectEquals (D::parseProgress ("[download] Destination: C:\\x\\y.m4a"), -1.0);
            expectEquals (D::parseProgress ("[youtube] jNQXAC9IVRw: Downloading webpage"), -1.0);
            expectEquals (D::parseProgress ("C:\\Users\\me\\AppData\\Local\\Temp\\Enqueue-youtube\\a\\Me at the zoo.m4a"), -1.0);
            expectEquals (D::parseProgress (""), -1.0);
        }

        beginTest ("classifyFailure tells video, network and tool problems apart (only a tool problem earns an update)");
        {
            using Kind = D::Failure::Kind;
            auto f = D::classifyFailure ({ "[youtube] x: Downloading webpage", "ERROR: [youtube] x: Private video. Sign in if you've been granted access to this video" });
            expect (f.kind == Kind::video);
            expect (f.message.isNotEmpty());

            f = D::classifyFailure ({ "ERROR: [youtube] x: Video unavailable" });
            expect (f.kind == Kind::video);

            f = D::classifyFailure ({ "ERROR: [youtube] x: Requested format is not available. Use --list-formats for a list of available formats" });
            expect (f.kind == Kind::video);

            f = D::classifyFailure ({ "ERROR: [generic] 'abc' is not a valid URL." });
            expect (f.kind == Kind::video);

            f = D::classifyFailure ({ "ERROR: [youtube] x: Sign in to confirm you're not a bot. Use --cookies-from-browser or --cookies for the authentication." });
            expect (f.kind == Kind::tool);

            f = D::classifyFailure ({ "WARNING: [youtube] x: nsig extraction failed: Some formats may be missing", "ERROR: unable to download video data: HTTP Error 403: Forbidden" });
            expect (f.kind == Kind::tool);   // an update may fix it
            expect (f.message.contains ("403"));

            f = D::classifyFailure ({ "ERROR: [youtube] x: Unable to download webpage: <urlopen error [Errno 11001] getaddrinfo failed> (caused by URLError(gaierror(11001, 'getaddrinfo failed')))" });
            expect (f.kind == Kind::network);   // offline: no update attempt, the hour's allowance kept

            f = D::classifyFailure ({ "ERROR: [youtube] x: Unable to download API page: The read operation timed out" });
            expect (f.kind == Kind::network);

            f = D::classifyFailure ({ "ERROR: [youtube] x: This video is age-restricted; some formats may be missing", "ERROR: [youtube] x: Sign in to confirm your age" });
            expect (f.kind == Kind::tool);   // "Sign in to confirm" wins as the first known needle (the message says so)

            f = D::classifyFailure ({ "ERROR: [youtube] x: age-restricted video, confirm your age" });
            expect (f.kind == Kind::video);

            f = D::classifyFailure ({});
            expect (f.kind == Kind::tool);
            expect (f.message.isNotEmpty());
        }

        beginTest ("ytDlpArguments: one video, its AAC audio, the bundled runtime, no user config, the print of the final path");
        {
            YouTubeTools::Paths p;
            p.ytDlp = juce::File ("C:\\Users\\me\\AppData\\Local\\Enqueue\\tools\\yt-dlp.exe");
            p.qjs = juce::File ("C:\\Users\\me\\AppData\\Local\\Enqueue\\tools\\qjs.exe");
            const auto args = D::ytDlpArguments (p, "https://youtu.be/abcdefghijk", juce::File ("C:\\Temp\\work"));
            expectEquals (args[0], p.ytDlp.getFullPathName());
            expectEquals (args[args.size() - 1], juce::String ("https://youtu.be/abcdefghijk"));
            expect (args.contains ("--ignore-config"));
            expect (args.contains ("--no-js-runtimes"));
            expect (args.contains ("quickjs:" + p.qjs.getFullPathName()));
            expect (args.contains ("--no-playlist"));
            expect (args.indexOf ("--playlist-items") >= 0 && args[args.indexOf ("--playlist-items") + 1] == "1");
            expect (args.contains ("bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]"));   // no webm/opus fallback: the app cannot decode it
            expect (args.contains ("after_move:filepath"));
            expect (args.contains ("C:\\Temp\\work\\%(title)s.%(ext)s"));
            expect (! args.joinIntoString (" ").contains ("--audio-format"));   // no ffmpeg: the app makes the mp3

            const auto lame = D::lameArguments (juce::File ("C:\\t\\lame.exe"), juce::File ("C:\\w\\a.decoded.wav"), juce::File ("C:\\out\\a.mp3.part"));
            expectEquals (lame.joinIntoString ("|"), juce::String ("C:\\t\\lame.exe|--quiet|--cbr|-b|320|C:\\w\\a.decoded.wav|C:\\out\\a.mp3.part"));
        }

        beginTest ("ToolProcess::quoteArgument follows the Windows command-line rules");
        {
            expectEquals (ToolProcess::quoteArgument ("plain"), juce::String ("plain"));
            expectEquals (ToolProcess::quoteArgument ("C:\\Program Files\\x.exe"), juce::String ("\"C:\\Program Files\\x.exe\""));
            expectEquals (ToolProcess::quoteArgument (""), juce::String ("\"\""));
            expectEquals (ToolProcess::quoteArgument ("a \"b\" c"), juce::String ("\"a \\\"b\\\" c\""));
            expectEquals (ToolProcess::quoteArgument ("C:\\dir with space\\"), juce::String ("\"C:\\dir with space\\\\\""));   // a trailing backslash doubles before the closing quote
            expectEquals (ToolProcess::quoteArgument ("x\\\"y"), juce::String ("\"x\\\\\\\"y\""));                             // backslashes before a quote double, the quote escapes
            expectEquals (ToolProcess::quoteArgument ("%(title)s.%(ext)s"), juce::String ("%(title)s.%(ext)s"));
        }

        beginTest ("ToolProcess runs a program, collects its lines and ends it on cancel");
        {
            juce::StringArray lines;
            const auto r = ToolProcess::run ({ "cmd.exe", "/c", "echo one& echo two words" }, 10000, nullptr, [&lines] (const juce::String& l) { lines.add (l.trim()); });
            expect (r.started && r.finished && ! r.cancelled && ! r.timedOut);
            expectEquals (r.exitCode, 0);
            expectEquals (lines.joinIntoString ("|"), juce::String ("one|two words"));

            const auto missing = ToolProcess::run ({ "C:\\definitely\\not\\here\\tool.exe" }, 1000, nullptr, nullptr);
            expect (! missing.started);

            std::atomic<bool> cancel { false };
            const auto t0 = juce::Time::currentTimeMillis();
            juce::Thread::launch ([&cancel] { juce::Thread::sleep (300); cancel.store (true); });
            const auto slow = ToolProcess::run ({ "cmd.exe", "/c", "ping -n 30 127.0.0.1 > nul" }, 60000, &cancel, nullptr);
            expect (slow.started && slow.cancelled && ! slow.finished);
            expectLessThan ((int) (juce::Time::currentTimeMillis() - t0), 10000);

            const auto late = ToolProcess::run ({ "cmd.exe", "/c", "ping -n 30 127.0.0.1 > nul" }, 300, nullptr, nullptr);
            expect (late.started && late.timedOut && ! late.finished);
        }

        beginTest ("YouTubeTools: version parsing, bundled-vs-user decision, rate limit");
        {
            using T = YouTubeTools;
            expectEquals (T::versionFromOutput ("2026.08.19\r\n"), juce::String ("2026.08.19"));
            expectEquals (T::versionFromOutput ("yt-dlp 2025.12.08"), juce::String ("2025.12.08"));
            expectEquals (T::versionFromOutput ("2026.08.19.232500"), juce::String());   // a nightly stamp is not what we expect
            expectEquals (T::versionFromOutput ("error: not found"), juce::String());
            expectEquals (T::versionFromOutput (""), juce::String());

            expect (T::shouldReplaceWithBundled ("", "2026.08.19"));           // no user copy
            expect (T::shouldReplaceWithBundled ("", ""));                     // nothing readable at all: copy anyway
            expect (T::shouldReplaceWithBundled ("2026.08.19", "2026.08.19"));  // same
            expect (T::shouldReplaceWithBundled ("2026.07.01", "2026.08.19"));  // bundled newer
            expect (! T::shouldReplaceWithBundled ("2026.09.03", "2026.08.19"));   // the copy updated itself past the bundle
            expect (! T::shouldReplaceWithBundled ("2026.09.03", ""));           // a bundle that cannot run does not win

            expect (T::isRateLimited (1000, 1000 + 59 * 60 * 1000, T::selfUpdateMinIntervalMs));
            expect (! T::isRateLimited (1000, 1000 + 61 * 60 * 1000, T::selfUpdateMinIntervalMs));
            expect (! T::isRateLimited (0, 5000, T::selfUpdateMinIntervalMs));            // never attempted
            expect (! T::isRateLimited (9000, 5000, T::selfUpdateMinIntervalMs));         // the clock went backwards: allow
        }
    }
};

static YouTubeDownloaderTests youTubeDownloaderTests;

} // namespace gocue
