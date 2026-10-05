#include "VolumeCueUiTestSupport.h"
#include "app/CueController.h"
#include "app/Scheduler.h"
#include "audio/AudioEngine.h"
#include "ui/UiUtils.h"

#include <optional>

namespace gocue::tests
{
namespace
{
using namespace volume_ui;
using K = juce::KeyPress;
using GoResult = CueController::GoResult;

/** 설정 > 스페이스바 재생/일시정지 (this PC, off by default): with it on, Space resumes paused cues, pauses the
    playing ones, and plays the playhead cue when nothing plays. The GO button, MIDI GO and the setting off stay GO. */
class SpaceBarPlayPauseTests : public juce::UnitTest
{
public:
    SpaceBarPlayPauseTests() : UnitTest ("SpaceBar PlayPause", "Enqueue") {}

    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 512;

    juce::File writeTone (const juce::File& dir, double seconds)
    {
        const auto file = dir.getChildFile ("tone.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        expect (stream != nullptr);
        if (stream == nullptr)
            return {};
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (16));
        expect (writer != nullptr);
        if (writer == nullptr)
            return {};
        const int numSamples = (int) (seconds * sampleRate);
        juce::AudioBuffer<float> buffer (2, numSamples);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));
        expect (writer->writeFromAudioSampleBuffer (buffer, 0, numSamples));
        return file;
    }

    static void render (AudioEngine& engine, Scheduler& scheduler, double& now, juce::AudioBuffer<float>& out, int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            engine.renderBlock (out, blockSize);
            now += blockSize / sampleRate;
            engine.reapFinishedPlayers();
            scheduler.tick();
        }
    }

    void runTest() override
    {
        controllerRules();
        mainWindow();
    }

private:
    void controllerRules()
    {
        // expect() needs a current test: run on its own, this suite has no earlier result to fall back on
        beginTest ("controller fixture");
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("enq_spacebar_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());
        const auto tone = writeTone (dir, 6.0);

        AudioEngine engine (0);
        engine.prepare (sampleRate, blockSize);
        juce::AudioBuffer<float> out (2, blockSize);

        ProjectDocument document;
        document.clock = [] { return 0.0; };
        Cue a, b, c;
        a.name = "a"; a.file = tone;
        b.name = "b"; b.file = tone;
        c.name = "c"; c.file = tone;
        document.cues.add (a);
        document.cues.add (b);
        document.cues.add (c);
        document.cues.setSelectedIndex (0);

        double now = 0.0;
        Scheduler scheduler ([&now] { return now; });
        CueController controller (engine, document, scheduler);
        juce::StringArray statuses;
        controller.onStatus = [&statuses] (const juce::String& message, bool) { statuses.add (message); };

        const auto space = [&]
        {
            const auto result = controller.go (false, -1.0, true);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 2);
            return result;
        };
        const auto stopEverything = [&]
        {
            controller.hardStopAll();
            render (engine, scheduler, now, out, 2);
            expectEquals (engine.getNumPlaying(), 0);
        };

        beginTest ("stopped: Space plays the playhead cue the way GO does");
        {
            now = 10.0;
            expect (space() == GoResult::started);
            expect (engine.isPlaying (a.id) && ! engine.isPaused (a.id));
            expectEquals (document.cues.getPlayheadIndex(), 1);
        }

        beginTest ("playing: Space pauses every playing cue and leaves the playhead alone");
        {
            expect (controller.fire (c.id) == GoResult::started);   // a second cue, the way a hotkey / cart starts one
            render (engine, scheduler, now, out, 2);
            expect (engine.isPlaying (c.id));
            now = 11.0;
            expect (space() == GoResult::paused);
            expect (engine.isPaused (a.id) && engine.isPaused (c.id));
            expect (! engine.isPlaying (b.id), "the next cue is not fired");
            expectEquals (document.cues.getPlayheadIndex(), 1);
            expectEquals (statuses[statuses.size() - 1], ko ("일시정지"));
        }

        beginTest ("paused: Space resumes every paused cue");
        {
            now = 12.0;
            expect (space() == GoResult::resumed);
            expect (! engine.isPaused (a.id) && ! engine.isPaused (c.id));
            expect (engine.isPlaying (a.id) && engine.isPlaying (c.id) && ! engine.isPlaying (b.id));
            expectEquals (document.cues.getPlayheadIndex(), 1);
        }

        beginTest ("a plain GO still fires the next cue while one plays");
        {
            now = 13.0;
            expect (controller.go() == GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 2);
            expect (engine.isPlaying (b.id) && ! engine.isPaused (a.id));
            stopEverything();
        }

        beginTest ("a loaded cue is not playing: Space starts it instead of pausing it");
        {
            document.cues.setSelectedIndex (0);
            expect (controller.loadSelected (0.0));
            expect (engine.isLoaded (a.id));
            now = 20.0;
            expect (space() == GoResult::started);
            expect (engine.isPlaying (a.id) && ! engine.isPaused (a.id) && ! engine.isLoaded (a.id));
            stopEverything();
        }

        beginTest ("the double-GO window and the panic latch hold Space too");
        {
            auto settings = document.settings;
            settings.doubleGoSeconds = 0.5;
            document.setSettings (settings);
            document.cues.setSelectedIndex (0);

            now = 30.0;
            expect (space() == GoResult::started);
            now = 30.2;
            expect (space() == GoResult::rejectedDoubleGo);
            expect (! engine.isPaused (a.id));
            now = 30.8;
            expect (space() == GoResult::paused);
            expect (engine.isPaused (a.id));

            settings.doubleGoSeconds = 0.0;
            document.setSettings (settings);
            now = 31.0;
            expect (space() == GoResult::resumed);
            expect (! engine.isPaused (a.id));

            now = 32.0;
            controller.panicAll();
            render (engine, scheduler, now, out, 2);
            expect (controller.isPanicLatched());
            expect (space() == GoResult::failed, "neither a pause nor a start during the panic fade");
            expect (! engine.isPaused (a.id));
            expect (! engine.isPlaying (b.id));
            stopEverything();
        }

        dir.deleteRecursively();
    }

    void mainWindow()
    {
        Fixture f;   // no native window, visibility or activation calls
        auto& controller = ReopenLastProjectTestAccess::controller (*f.main);
        auto& service = f.main->getShortcutService();
        const auto a = f.addSound ("a");
        const auto b = f.addSound ("b");
        const K space (K::spaceKey);

        const auto press = [&] (const K& key, juce::Component& origin)
        {
            f.now += 1.0;
            const bool consumed = f.key (key, origin);
            f.release (key);
            f.settle();
            dispatch();
            return consumed;
        };
        const auto stopEverything = [&]
        {
            controller.hardStopAll();
            f.settle();
            expectEquals (f.engine.getNumPlaying(), 0);
            f.document().cues.setSelectedIndex (0);
        };
        const auto menuItem = [&]() -> std::optional<juce::PopupMenu::Item>
        {
            auto menu = f.main->getMenuForIndex (4, ko ("설정"));
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                if (it.getItem().itemID == CommandIDs::toggleSpaceBarPlayPause)
                    return it.getItem();
            return std::nullopt;
        };

        beginTest ("설정 menu: 스페이스바 재생/일시정지, off by default, ticked when on and kept on this PC");
        {
            auto item = menuItem();
            expect (item.has_value());
            if (! item.has_value())
                return;
            expectEquals (item->text, ko ("스페이스바 재생/일시정지"));
            expect (item->isEnabled && ! item->isTicked);
            expect (! f.settings.getSpaceBarPlayPause());

            f.document().markClean();
            expect (f.command (CommandIDs::toggleSpaceBarPlayPause));
            expect (f.settings.getSpaceBarPlayPause());
            expect (f.storage.getBoolValue ("spaceBarPlayPause", false), "stored in this PC's settings");
            expect (! f.document().isDirty(), "not a project change");
            item = menuItem();
            expect (item.has_value() && item->isTicked);

            f.command (CommandIDs::toggleShowMode);   // show mode locks it like the other 설정 choices
            item = menuItem();
            expect (item.has_value() && ! item->isEnabled);
            f.command (CommandIDs::toggleShowMode);
        }

        beginTest ("setting on: Space plays, pauses and resumes - the next cue never starts");
        {
            f.document().cues.setSelectedIndex (0);
            expect (press (space, f.table()));
            expect (f.engine.isPlaying (a.id) && ! f.engine.isPaused (a.id));
            expect (press (space, f.table()));
            expect (f.engine.isPaused (a.id));
            expect (! f.engine.isPlaying (b.id));
            expect (press (space, f.table()));
            expect (f.engine.isPlaying (a.id) && ! f.engine.isPaused (a.id));
            expect (! f.engine.isPlaying (b.id));
        }

        beginTest ("setting on: the GO button still fires the next cue");
        {
            f.now += 1.0;
            expect (f.command (CommandIDs::go));   // a direct invocation = the GO button's path
            f.settle();
            expect (f.engine.isPlaying (b.id) && ! f.engine.isPaused (a.id));
            stopEverything();
        }

        beginTest ("setting on: Space in the big view window plays and pauses too");
        {
            auto& window = f.makeWindow();
            expect (press (space, window));
            expect (f.engine.isPlaying (a.id));
            expect (press (space, window));
            expect (f.engine.isPaused (a.id) && ! f.engine.isPlaying (b.id));
            stopEverything();
        }

        beginTest ("setting on: Space typed into a text field stays text");
        {
            auto* editor = child<juce::TextEditor> (f.inspector(), [] (const juce::TextEditor& e) { return ! e.isReadOnly(); });
            expect (editor != nullptr);
            if (editor != nullptr)
            {
                expect (! press (space, *editor));
                expectEquals (f.engine.getNumPlaying(), 0);
            }
        }

        beginTest ("setting off: Space is GO again - the second Space starts the next cue");
        {
            expect (f.command (CommandIDs::toggleSpaceBarPlayPause));
            expect (! f.settings.getSpaceBarPlayPause());
            expect (! f.storage.getBoolValue ("spaceBarPlayPause", true));
            expect (press (space, f.table()));
            expect (f.engine.isPlaying (a.id));
            expect (press (space, f.table()));
            expect (f.engine.isPlaying (b.id) && ! f.engine.isPaused (a.id));
            stopEverything();
        }

        beginTest ("Space moved to 일시정지 / 재개 in the shortcut settings follows the setting too");
        {
            expect (service.setKeys ("transport.pauseToggle", { K ('P', 0, 0), space }, ShortcutService::ConflictPolicy::move).wasOk());
            expect (service.getKeys ("transport.go").isEmpty());

            expect (press (space, f.table()));   // setting off: pause / resume only, nothing starts from silence (unchanged)
            expectEquals (f.engine.getNumPlaying(), 0);

            expect (f.command (CommandIDs::toggleSpaceBarPlayPause));
            expect (press (space, f.table()));
            expect (f.engine.isPlaying (a.id) && ! f.engine.isPaused (a.id));
            expect (press (space, f.table()));
            expect (f.engine.isPaused (a.id) && ! f.engine.isPlaying (b.id));
            expect (press (space, f.table()));
            expect (f.engine.isPlaying (a.id) && ! f.engine.isPaused (a.id) && ! f.engine.isPlaying (b.id));
            stopEverything();

            expect (service.restoreAllDefaults().wasOk());
            expect (f.command (CommandIDs::toggleSpaceBarPlayPause));
            expect (! f.settings.getSpaceBarPlayPause());
        }
    }
};

static SpaceBarPlayPauseTests spaceBarPlayPauseTests;
}
}
