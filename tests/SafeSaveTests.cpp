#include "model/ProjectSerializer.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

namespace gocue::tests
{

/** Saving never leaves a half-written project behind, broken files are refused, and a copied show folder plays
    the media that travelled with it. */
class SafeSaveTests : public juce::UnitTest
{
public:
    SafeSaveTests() : juce::UnitTest ("Safe project save and load", "Enqueue") {}

    static juce::File writeTone (const juce::File& file)
    {
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));
        juce::AudioBuffer<float> buffer (1, 4410);
        buffer.clear();
        writer->writeFromAudioSampleBuffer (buffer, 0, 4410);
        return file;
    }

    void runTest() override
    {
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("enqueue_safesave_" + juce::Uuid().toString());
        expect (root.createDirectory().wasOk());

        beginTest ("a save writes next to the target, verifies and swaps: no temp file stays, the content reads back");
        {
            Project p;
            p.name = "First";
            Cue c;
            c.name = "one";
            p.cues().push_back (c);

            const auto file = root.getChildFile ("show.enqueue");
            expect (ProjectSerializer::save (p, file).wasOk());
            expect (file.existsAsFile());
            expectEquals (root.getNumberOfChildFiles (juce::File::findFiles), 1);   // no temp file stays behind

            Project q;
            expect (ProjectSerializer::load (file, q).wasOk());
            expectEquals (q.name, juce::String ("First"));

            p.name = "Second";
            expect (ProjectSerializer::save (p, file).wasOk());   // replaces the existing file
            expectEquals (root.getNumberOfChildFiles (juce::File::findFiles), 1);

            Project r;
            expect (ProjectSerializer::load (file, r).wasOk());
            expectEquals (r.name, juce::String ("Second"));
        }

        beginTest ("a save that cannot write leaves the previous file untouched");
        {
            Project p;
            p.name = "Kept";
            const auto blocker = root.getChildFile ("blocked");
            expect (blocker.replaceWithText ("not a directory"));   // a file where the project directory should be

            const auto target = blocker.getChildFile ("show.enqueue");
            expect (ProjectSerializer::save (p, target).failed());
            expect (! target.existsAsFile());
            expect (blocker.existsAsFile());
        }

        beginTest ("empty, non-object and foreign JSON files are refused");
        {
            Project q;
            const auto empty = root.getChildFile ("empty.enqueue");
            expect (empty.replaceWithText (""));
            expect (ProjectSerializer::load (empty, q).failed());

            const auto blank = root.getChildFile ("blank.enqueue");
            expect (blank.replaceWithText ("   \n"));
            expect (ProjectSerializer::load (blank, q).failed());

            const auto array = root.getChildFile ("array.enqueue");
            expect (array.replaceWithText ("[1, 2, 3]"));
            expect (ProjectSerializer::load (array, q).failed());

            const auto foreign = root.getChildFile ("foreign.enqueue");
            expect (foreign.replaceWithText ("{\"title\": \"someone else's file\"}"));
            expect (ProjectSerializer::load (foreign, q).failed());

            const auto truncated = root.getChildFile ("truncated.enqueue");
            expect (truncated.replaceWithText ("{\"app\": \"Enqueue\", \"version\": 3, \"cues\": ["));
            expect (ProjectSerializer::load (truncated, q).failed());

            const auto versionOnly = root.getChildFile ("versionOnly.enqueue");   // an object with a version but no cue list is not a show
            expect (versionOnly.replaceWithText ("{\"version\": 999}"));
            expect (ProjectSerializer::load (versionOnly, q).failed());

            const auto trailing = root.getChildFile ("trailing.enqueue");
            expect (trailing.replaceWithText ("{\"app\": \"Enqueue\", \"version\": 3, \"cues\": []} garbage"));
            expect (ProjectSerializer::load (trailing, q).failed());

            const auto future = root.getChildFile ("future.enqueue");   // saved by a newer Enqueue: refused, not silently downgraded
            expect (future.replaceWithText ("{\"app\": \"Enqueue\", \"version\": 9999, \"cues\": []}"));
            const auto futureResult = ProjectSerializer::load (future, q);
            expect (futureResult.failed());
            expect (futureResult.getErrorMessage().contains ("newer"));

            const auto minimal = root.getChildFile ("minimal.enqueue");   // an empty cue list is a (blank) show
            expect (minimal.replaceWithText ("{\"app\": \"Enqueue\", \"version\": 3, \"cues\": []}"));
            expect (ProjectSerializer::load (minimal, q).wasOk());
        }

        beginTest ("a show folder copied elsewhere plays the media that travelled with it, even if the original still exists");
        {
            const auto original = root.getChildFile ("original");
            const auto tone = writeTone (original.getChildFile ("audio").getChildFile ("tone.wav"));

            Project p;
            Cue c;
            c.name = "tone";
            c.file = tone;
            p.cues().push_back (c);
            expect (ProjectSerializer::save (p, original.getChildFile ("show.enqueue")).wasOk());

            const auto copy = root.getChildFile ("copy");
            expect (original.copyDirectoryTo (copy));
            expect (copy.getChildFile ("audio").getChildFile ("tone.wav").existsAsFile());

            Project q;
            expect (ProjectSerializer::load (copy.getChildFile ("show.enqueue"), q).wasOk());
            expectEquals (q.cues()[0].file.getFullPathName(), copy.getChildFile ("audio").getChildFile ("tone.wav").getFullPathName());
            expect (! q.cues()[0].fileMissing);

            // the project moved alone: the absolute path still resolves
            const auto alone = root.getChildFile ("alone");
            expect (alone.createDirectory().wasOk());
            expect (original.getChildFile ("show.enqueue").copyFileTo (alone.getChildFile ("show.enqueue")));

            Project r;
            expect (ProjectSerializer::load (alone.getChildFile ("show.enqueue"), r).wasOk());
            expectEquals (r.cues()[0].file.getFullPathName(), tone.getFullPathName());
            expect (! r.cues()[0].fileMissing);
        }

        root.deleteRecursively();
    }
};

static SafeSaveTests safeSaveTests;

} // namespace gocue::tests
