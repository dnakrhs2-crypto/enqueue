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

        beginTest ("normaliseUrl takes YouTube links only, adds the scheme, trims and unquotes");
        {
            expectEquals (D::normaliseUrl ("https://www.youtube.com/watch?v=dQw4w9WgXcQ"), juce::String ("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
            expectEquals (D::normaliseUrl ("  youtu.be/dQw4w9WgXcQ  "), juce::String ("https://youtu.be/dQw4w9WgXcQ"));
            expectEquals (D::normaliseUrl ("\"https://music.youtube.com/watch?v=abc&list=x\""), juce::String ("https://music.youtube.com/watch?v=abc&list=x"));
            expectEquals (D::normaliseUrl ("https://www.youtube.com/shorts/abc"), juce::String ("https://www.youtube.com/shorts/abc"));
            expectEquals (D::normaliseUrl ("m.youtube.com/watch?v=abc"), juce::String ("https://m.youtube.com/watch?v=abc"));
            expect (D::normaliseUrl ("").isEmpty());
            expect (D::normaliseUrl ("   ").isEmpty());
            expect (D::normaliseUrl ("https://vimeo.com/12345").isEmpty());
            expect (D::normaliseUrl ("https://notyoutube.com/watch?v=abc").isEmpty());
            expect (D::normaliseUrl ("https://youtube.com.evil.com/watch?v=abc").isEmpty());
            expect (D::normaliseUrl ("youtube").isEmpty());
            expect (D::normaliseUrl ("ftp://www.youtube.com/watch?v=abc").isEmpty());
            expect (D::normaliseUrl ("my song https://youtu.be/abc").isEmpty());   // not a single address
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

        beginTest ("classifyFailure tells video problems (no update helps) from tool problems");
        {
            auto f = D::classifyFailure ({ "[youtube] x: Downloading webpage", "ERROR: [youtube] x: Private video. Sign in if you've been granted access to this video" });
            expect (f.aboutTheVideo);
            expect (f.message.isNotEmpty());

            f = D::classifyFailure ({ "ERROR: [youtube] x: Video unavailable" });
            expect (f.aboutTheVideo);

            f = D::classifyFailure ({ "ERROR: [youtube] x: Requested format is not available. Use --list-formats for a list of available formats" });
            expect (f.aboutTheVideo);

            f = D::classifyFailure ({ "ERROR: [generic] 'abc' is not a valid URL." });
            expect (f.aboutTheVideo);

            f = D::classifyFailure ({ "ERROR: [youtube] x: Sign in to confirm you're not a bot. Use --cookies-from-browser or --cookies for the authentication." });
            expect (! f.aboutTheVideo);

            f = D::classifyFailure ({ "WARNING: [youtube] x: nsig extraction failed: Some formats may be missing", "ERROR: unable to download video data: HTTP Error 403: Forbidden" });
            expect (! f.aboutTheVideo);   // the tool's problem: an update may fix it
            expect (f.message.contains ("403"));

            f = D::classifyFailure ({});
            expect (! f.aboutTheVideo);
            expect (f.message.isNotEmpty());
        }

        beginTest ("ytDlpArguments: audio only, the bundled runtime, the print of the final path");
        {
            YouTubeTools::Paths p;
            p.ytDlp = juce::File ("C:\\Users\\me\\AppData\\Local\\Enqueue\\tools\\yt-dlp.exe");
            p.qjs = juce::File ("C:\\Users\\me\\AppData\\Local\\Enqueue\\tools\\qjs.exe");
            const auto args = D::ytDlpArguments (p, "https://youtu.be/abc", juce::File ("C:\\Temp\\work"));
            expectEquals (args[0], p.ytDlp.getFullPathName());
            expectEquals (args[args.size() - 1], juce::String ("https://youtu.be/abc"));
            expect (args.contains ("--no-js-runtimes"));
            expect (args.contains ("quickjs:" + p.qjs.getFullPathName()));
            expect (args.contains ("--no-playlist"));
            expect (args.contains ("bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]/bestaudio"));
            expect (args.contains ("after_move:filepath"));
            expect (args.contains ("C:\\Temp\\work\\%(title)s.%(ext)s"));
            expect (! args.joinIntoString (" ").contains ("--audio-format"));   // no ffmpeg: the app makes the mp3
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
