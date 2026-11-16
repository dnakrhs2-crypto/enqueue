#include "MixEngine.h"
#include "CollabTransport.h"
#include "LiveMixCollabTestHelpers.h"
#include "TestGainPlugin.h"
#include "../livemix/src/ui/MainComponent.h"
#include "ui/LiveMixLookAndFeel.h"
#include "ui/SettingsDialog.h"
#include "lm_obs_protocol.h"
#include "livemix017/r5/RecorderChecks.h"

#include <windows.h>

namespace gocue::tests
{
using namespace livemix;
namespace
{
    struct TempFolder
    {
        juce::File path = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getNonexistentChildFile ("LiveMixMonitorOff-" + juce::Uuid().toString(), {}, false);
        TempFolder() { path.createDirectory(); }
        ~TempFolder() { path.deleteRecursively(); }
    };

    struct RingView
    {
        explicit RingView (const juce::String& name)
        {
            handle = OpenFileMappingW (FILE_MAP_READ, FALSE, name.toWideCharPointer());
            if (handle != nullptr)
                header = static_cast<lm_obs_ring_header*> (MapViewOfFile (handle, FILE_MAP_READ, 0, 0, 0));
        }
        ~RingView() { if (header != nullptr) UnmapViewOfFile (header); if (handle != nullptr) CloseHandle (handle); }
        float sample (int64_t frame, int channel) const
        {
            const auto* pcm = reinterpret_cast<const float*> (reinterpret_cast<const char*> (header) + header->data_offset);
            return pcm[(size_t) (frame % LM_OBS_CAPACITY_FRAMES) * 2 + (size_t) channel];
        }
        HANDLE handle = nullptr;
        lm_obs_ring_header* header = nullptr;
    };

    // Exercise the split routing buffer offline, without opening the user's audio devices.
    template <typename Tag, typename Tag::Type field>
    struct PrivateField { friend typename Tag::Type member (Tag) { return field; } };
    struct SplitFlag { using Type = std::atomic<bool> MixEngine::*; friend Type member (SplitFlag); };
    struct StereoFlag { using Type = std::atomic<bool> MixEngine::*; friend Type member (StereoFlag); };
    struct SplitStage { using Type = juce::AudioBuffer<float> MixEngine::*; friend Type member (SplitStage); };
    template struct PrivateField<SplitFlag, &MixEngine::splitMonitor>;
    template struct PrivateField<StereoFlag, &MixEngine::stereoOutputsOnly>;
    template struct PrivateField<SplitStage, &MixEngine::monitorStage>;

    juce::Component* find (juce::Component& root, const juce::String& id)
    {
        if (root.getComponentID() == id) return &root;
        for (auto* child : root.getChildren())
            if (auto* found = find (*child, id)) return found;
        return nullptr;
    }

    bool says (juce::Component& root, const juce::String& text)
    {
        if (auto* label = dynamic_cast<juce::Label*> (&root); label != nullptr && label->isVisible() && label->getText() == text)
            return true;
        for (auto* child : root.getChildren())
            if (says (*child, text)) return true;
        return false;
    }

    struct AppRig
    {
        TempFolder folder;
        LiveMixSettings settings { folder.path };
        MixEngine engine { "Local\\LiveMix.MonitorOff.UI." + juce::Uuid().toString() };
        MixDocument document { engine };
        ObsPluginActions actions;
        std::unique_ptr<MainComponent> main;

        explicit AppRig (bool shown = false, bool off = false)
        {
            settings.setMicMuteHotkey ({});
            settings.setFxMuteHotkey ({});
            settings.setWindowHotkey ({});
            settings.setMicAllMuteHotkey ({});
            settings.setFxAllMuteHotkey ({});
            settings.setMonitorOffButton (shown);
            settings.setMonitorOff (off);
            settings.saveIfNeeded();
            // Main.cpp applies this before opening the device and creating MainComponent.
            engine.setMonitorMuted (settings.getMonitorOffButton() && settings.getMonitorOff());
            engine.prepare (48000, 120);
            auto& manager = engine.getDeviceManager();
            while (! manager.getAvailableDeviceTypes().isEmpty())
                manager.removeAudioDeviceType (manager.getAvailableDeviceTypes().getLast());
            document.applyToEngine();
            actions.roots = [path = folder.path] { return ObsPluginInstaller::Roots { path, path, path, [] { return false; }, {} }; };
            actions.elevate = [] (juce::String&) { return ObsPluginInstaller::Result::needsElevation; };
            main = std::make_unique<MainComponent> (document, settings, actions);
            main->setSize (1300, 900);
        }
        ~AppRig() { SettingsDialog::closeIfOpen(); }
        juce::TextButton* button (const juce::String& id) { return dynamic_cast<juce::TextButton*> (find (*main, id)); }
        TopBar* top()
        {
            for (auto* child : main->getChildren())
                if (auto* bar = dynamic_cast<TopBar*> (child)) return bar;
            return nullptr;
        }
        juce::ToggleButton* openSettings()
        {
            main->menuItemSelected (1, 2);
            auto& desktop = juce::Desktop::getInstance();
            for (int i = 0; i < desktop.getNumComponents(); ++i)
                if (auto* tick = dynamic_cast<juce::ToggleButton*> (find (*desktop.getComponent (i), "monitor-off-button")))
                    return tick;
            return nullptr;
        }
    };
}

class LiveMixMonitorOffTests : public juce::UnitTest
{
public:
    LiveMixMonitorOffTests() : UnitTest ("LiveMix monitor off", "LiveMix") {}

    void runTest() override
    {
        ramp();
        consumers();
        directAndSplit();
        settingsAndApp();
        layout();
    }

private:
    void ramp()
    {
        beginTest ("main output ramps both ways within 5 ms across chunks and sample rates; prepare starts at the saved target");
        for (double rate : { 44100.0, 48000.0, 96000.0 })
            for (int chunk : { 64, 256, 1024 })
            {
                MixEngine engine ("Local\\LiveMix.MonitorOff.Ramp." + juce::Uuid().toString());
                MixSession session; session.addChannel(); session.master.outputFirst = 2;
                engine.applySession (session); engine.prepare (rate, chunk);
                expect (! engine.isMonitorMuted());
                const int frames = chunk * 3 + 17;   // also check offsets when the driver delivers more than prepared
                std::vector<float> input ((size_t) frames, 0.25f);
                const float* inputs[] { input.data() };
                juce::AudioBuffer<float> output (4, frames);
                const auto render = [&] { engine.renderBlock (inputs, 1, output.getArrayOfWritePointers(), 4, frames); };
                render();
                expectEquals (output.getSample (2, frames - 1), 0.25f);
                for (bool off : { true, false })
                {
                    engine.setMonitorMuted (off);
                    expect (engine.isMonitorMuted() == off);
                    float previous = off ? 0.25f : 0.0f, maxStep = 0.0f;
                    bool monotonic = true, settled = true, stereo = true;
                    int elapsed = 0;
                    const int rampFrames = (int) std::ceil (MixEngine::onOffRampSeconds * rate);
                    do
                    {
                        render();
                        for (int i = 0; i < frames; ++i, ++elapsed)
                        {
                            const float sample = output.getSample (2, i);
                            maxStep = std::max (maxStep, std::abs (sample - previous));
                            monotonic &= off ? sample <= previous + 1.0e-7f : sample >= previous - 1.0e-7f;
                            if (elapsed >= rampFrames) settled &= std::abs (sample - (off ? 0.0f : 0.25f)) < 1.0e-6f;
                            stereo &= sample == output.getSample (3, i);
                            previous = sample;
                        }
                    } while (elapsed <= rampFrames);
                    const auto context = juce::String (rate) + " Hz / " + juce::String (chunk) + (off ? " off" : " on");
                    expect (maxStep <= 0.25f / (float) (rate * MixEngine::onOffRampSeconds) + 1.0e-6f, context);
                    expect (monotonic && settled && stereo, context);
                    expectEquals (previous, off ? 0.0f : 0.25f, context);
                    expectEquals (output.getMagnitude (0, 0, frames), 0.0f);
                    expectEquals (output.getMagnitude (1, 0, frames), 0.0f);
                }
                engine.setMonitorMuted (true);
                engine.prepare (rate, chunk);
                render();
                expectEquals (output.getMagnitude (0, frames), 0.0f, "muted prepare has no first-block leakage");
                engine.setMonitorMuted (false);
                engine.prepare (rate, chunk);
                render();
                expectEquals (output.getSample (2, 0), 0.25f, "unmuted prepare starts at unity");
            }
    }

    void consumers()
    {
        beginTest ("muted master still reaches OBS, meters, collab packets and the recorded WAV");
        TempFolder folder;
        const auto mapping = "Local\\LiveMix.MonitorOff.Consumers." + juce::Uuid().toString();
        MixEngine engine (mapping);
        MixSession session; session.addChannel(); session.channels[0].stereo = true; session.master.outputFirst = 2;
        session.channels[0].output = { true, true, 0 };
        engine.applySession (session); engine.prepare (48000, 120);
        engine.getMasterChain().addPlugin (std::make_unique<TestGainPlugin> (0.5f));
        engine.setObsEnabled (true);
        RingView ring (mapping);
        expect (ring.header != nullptr);
        if (ring.header == nullptr) return;
        collab::CollabTransport transport;
        engine.getCollab().attach (transport);
        engine.getCollab().setConnected (true);
        std::array<float, 120> left, right; left.fill (0.25f); right.fill (0.125f);
        const float* inputs[] { left.data(), right.data() };
        juce::AudioBuffer<float> output (4, 120);
        const auto render = [&] { engine.renderBlock (inputs, 2, output.getArrayOfWritePointers(), 4, 120); };
        render();
        engine.setMonitorMuted (true);
        for (int block = 0; block < 4; ++block) render();
        engine.getCollab().drain ([] (const collab::Header&, const uint8_t*) {});
        engine.readMasterMeter();
        expect (engine.startRecording ({ recording::Format::wav, 320, folder.path.getFullPathName().toStdString() }));
        auto& recorder = engine.getRecorder();
        expect (recording::checks::waitFor ([&] { return recorder.snapshot().state == recording::State::recording; }));
        const auto firstFrame = lm_obs_load_acquire (&ring.header->write_frames);
        int packets = 0;
        bool silent = true, direct = true, sent = true;
        for (int block = 0; block < 40; ++block)
        {
            render();
            silent &= output.getMagnitude (2, 0, 120) == 0.0f && output.getMagnitude (3, 0, 120) == 0.0f;
            for (int i = 0; i < 120; ++i)
                direct &= output.getSample (0, i) == 0.25f && output.getSample (1, i) == 0.125f;
            engine.getCollab().drain ([&] (const collab::Header& header, const uint8_t* payload)
            {
                ++packets;
                std::array<float, collab::framesPerPacket * 2> decoded;
                collab::unpackPcm24 (payload, decoded.data(), header.frames);
                for (int i = 0; i < header.frames; ++i)
                    sent &= std::abs (decoded[(size_t) i * 2] - 0.125f) < 1.0e-6f
                         && std::abs (decoded[(size_t) i * 2 + 1] - 0.0625f) < 1.0e-6f;
            });
        }
        recorder.finish();
        expect (silent && direct);
        expect (sent && packets > 0, "collab keeps the post-master stereo signal");
        expectEquals (lm_obs_load_acquire (&ring.header->write_frames), firstFrame + int64_t (4800));
        bool obs = true;
        for (int64_t frame = firstFrame; frame < firstFrame + 4800; ++frame)
            obs &= ring.sample (frame, 0) == 0.125f && ring.sample (frame, 1) == 0.0625f;
        expect (obs);
        const auto meter = engine.readMasterMeter();
        expectEquals (meter.left, 0.125f); expectEquals (meter.right, 0.0625f);
        engine.getLoudnessMeter().poll();
        expect (engine.getLoudnessMeter().truePeakMax().valid);
        expect (recorder.snapshot().state == recording::State::idle);
        expectEquals ((int) recorder.snapshot().capturedFrames, 4800);
        const auto files = folder.path.findChildFiles (juce::File::findFiles, true, "*.wav");
        expectEquals (files.size(), 1);
        if (files.size() != 1) return;
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (files[0].createInputStream().release(), true));
        expect (reader != nullptr);
        if (reader == nullptr) return;
        expectEquals (reader->lengthInSamples, juce::int64 (4800));
        juce::AudioBuffer<float> recorded (2, 4800);
        expect (reader->read (&recorded, 0, 4800, 0, true, true));
        bool intact = true;
        for (int i = 0; i < 4800; ++i)
            intact &= std::abs (recorded.getSample (0, i) - 0.125f) < 1.0e-6f
                   && std::abs (recorded.getSample (1, i) - 0.0625f) < 1.0e-6f;
        expect (intact, "recording receives the unscaled master bus");
    }

    void directAndSplit()
    {
        beginTest ("direct mic, FX and aux plus remote voices survive on the main pair, including split monitorStage");
        for (bool split : { false, true })
            for (int directKind = 0; directKind < 3; ++directKind)
            {
                MixEngine engine ("Local\\LiveMix.MonitorOff.Routes." + juce::Uuid().toString());
                MixSession session; session.addChannel(); session.master.outputFirst = 2;
                if (directKind == 0) session.channels[0].output = { true, true, 2 };
                if (directKind == 1)
                {
                    session.addFx(); session.channels[0].sends[0].amount = 1.0;
                    session.fx[0].output = { false, true, 2 };
                }
                if (directKind == 2)
                {
                    session.addAux(); session.channels[0].auxSends.push_back (session.auxes[0].id);
                    session.auxes[0].output = { false, true, 2 };
                }
                engine.applySession (session); engine.prepare (48000, 120);
                if (split)
                {
                    (engine.*member (SplitFlag {})).store (true);
                    (engine.*member (StereoFlag {})).store (true);
                }
                std::array<float, 120> input; input.fill (0.125f);
                const float* inputs[] { input.data() };
                juce::AudioBuffer<float> output (4, 120);
                const auto render = [&] { engine.renderBlock (inputs, 1, output.getArrayOfWritePointers(), 4, 120); };
                const auto& routed = split ? engine.*member (SplitStage {}) : output;
                const int first = split ? 0 : 2;
                for (int i = 0; i < 4; ++i) render();
                expectEquals (routed.getSample (first, 119), 0.25f);
                engine.setMonitorMuted (true);
                for (int i = 0; i < 4; ++i) render();
                for (int side = 0; side < 2; ++side)
                    expectWithinAbsoluteError (routed.getSample (first + side, 119), 0.125f, 1.0e-6f);
                engine.getCollab().applyRoster (collabtest::rosterFor ({ 1, 2 }), 1);
                for (uint64_t block = 0; block < 100; ++block)
                {
                    collabtest::receiveTone (engine.getCollab(), 2, block, 0.25f);
                    render();
                }
                for (int side = 0; side < 2; ++side)
                    expectWithinAbsoluteError (routed.getSample (first + side, 119), 0.375f, 1.0e-5f,
                                               "direct sound plus remote voice, with only master removed");
            }
    }

    void settingsAndApp()
    {
        beginTest ("both settings default false and persist independently in an isolated folder");
        TempFolder folder;
        {
            LiveMixSettings defaults (folder.path);
            expect (! defaults.getMonitorOffButton() && ! defaults.getMonitorOff());
        }
        for (const auto values : { std::pair { true, true }, { true, false }, { false, true }, { false, false } })
        {
            {
                LiveMixSettings settings (folder.path);
                settings.setMonitorOffButton (values.first); settings.setMonitorOff (values.second); settings.saveIfNeeded();
            }
            LiveMixSettings saved (folder.path);
            expect (saved.getMonitorOffButton() == values.first && saved.getMonitorOff() == values.second);
        }

        LiveMixLookAndFeel look;
        struct RestoreLook
        {
            juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
            ~RestoreLook() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
        } restore;
        juce::LookAndFeel::setDefaultLookAndFeel (&look);
        beginTest ("the real settings callback shows, saves, colours and hides the switch, restoring monitoring and layout");
        {
            AppRig rig;
            auto* button = rig.button ("monitor-off"); auto* top = rig.top(); auto* tick = rig.openSettings();
            expect (button != nullptr && top != nullptr && tick != nullptr);
            if (button == nullptr || top == nullptr || tick == nullptr) return;
            expect (! button->isVisible() && ! rig.engine.isMonitorMuted() && ! tick->getToggleState());
            const int hiddenHeight = top->getHeight();
            tick->setToggleState (true, juce::sendNotificationSync);
            expect (button->isVisible() && ! button->getToggleState() && rig.settings.getMonitorOffButton());
            expectEquals (top->getHeight(), top->preferredHeight (top->getWidth()));
            expectGreaterThan (top->getHeight(), hiddenHeight);
            for (bool off : { true, false, true })
            {
                button->onClick();
                expect (rig.engine.isMonitorMuted() == off && rig.settings.getMonitorOff() == off && button->getToggleState() == off);
                LiveMixSettings saved (rig.folder.path);
                expect (saved.getMonitorOffButton() && saved.getMonitorOff() == off);
                expect (says (*rig.main, off ? tr ("모니터링 꺼짐 (OBS로는 소리 계속)") : tr ("모니터링 켜짐")));
            }
            expect (button->findColour (juce::TextButton::buttonOnColourId) == livemix::Palette::dangerFill);
            tick->setToggleState (false, juce::sendNotificationSync);
            expect (! button->isVisible() && ! button->getToggleState() && ! rig.engine.isMonitorMuted());
            expect (! rig.settings.getMonitorOffButton() && ! rig.settings.getMonitorOff());
            expect (says (*rig.main, tr ("모니터링 켜짐")));
            expectEquals (top->getHeight(), hiddenHeight);
            LiveMixSettings saved (rig.folder.path);
            expect (! saved.getMonitorOffButton() && ! saved.getMonitorOff());
            tick->setToggleState (true, juce::sendNotificationSync);
            expect (button->isVisible() && ! button->getToggleState() && ! rig.engine.isMonitorMuted());
        }
        beginTest ("startup shows saved muted state; a hidden switch cannot mute startup");
        for (bool shown : { true, false })
        {
            AppRig rig (shown, true);
            auto* button = rig.button ("monitor-off");
            expect (button != nullptr);
            if (button == nullptr) continue;
            expect (button->isVisible() == shown && button->getToggleState() == shown);
            expect (rig.engine.isMonitorMuted() == shown);
        }
    }

    void layout()
    {
        beginTest ("all three switches fit in order at every bar width and language, with stable bounds on presses");
        struct RestoreLanguage
        {
            Language previous = currentLanguage();
            ~RestoreLanguage() { initialiseLanguage (previous); }
        } restoreLanguage;
        LiveMixLookAndFeel look;
        struct RestoreLook
        {
            juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
            ~RestoreLook() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
        } restoreLook;
        juce::LookAndFeel::setDefaultLookAndFeel (&look);
        for (auto language : { Language::ko, Language::en, Language::ja })
        {
            initialiseLanguage (language);
            AppRig rig (true);
            auto* top = rig.top();
            expect (top != nullptr);
            if (top == nullptr) return;
            const auto bounds = [top]
            {
                std::vector<juce::Rectangle<int>> result;
                for (auto* child : top->getChildren())
                    result.push_back (child->isVisible() ? child->getBounds() : juce::Rectangle<int>());
                return result;
            };
            for (int width : { 1920, 1440, 1300, 1220, 1219, 1000, 760, 700, 699, 560, 460, 420 })
            {
                rig.main->setSize (width, 900);
                const auto context = juce::String (languageCode (language)) + " @" + juce::String (width);
                const auto before = bounds();
                const auto mode = top->modeFor (top->getWidth());
                expectEquals (top->preferredHeight (top->getWidth()), mode == TopBar::Mode::wide ? 64 : mode == TopBar::Mode::compact ? 106 : 148, context);
                expectEquals (top->getHeight(), top->preferredHeight (top->getWidth()), context);
                juce::TextButton* previous = nullptr;
                for (const char* id : { "mic-mute-group", "fx-mute-group", "monitor-off" })
                {
                    auto* button = rig.button (id);
                    expect (button != nullptr && button->isVisible(), context + id);
                    if (button == nullptr) continue;
                    expect (top->getLocalBounds().contains (button->getBounds()), context + id);
                    const auto font = button->getLookAndFeel().getTextButtonFont (*button, button->getHeight());
                    expectGreaterOrEqual (button->getWidth(), juce::GlyphArrangement::getStringWidthInt (font, button->getButtonText()) + 8, context + id);
                    if (previous != nullptr)
                    {
                        expectEquals (button->getY(), previous->getY(), context);
                        expectGreaterOrEqual (button->getX(), previous->getRight(), context);
                    }
                    previous = button;
                }
                std::vector<juce::Component*> shown;
                for (auto* child : top->getChildren())
                    if (child->isVisible() && ! child->getBounds().isEmpty()) shown.push_back (child);
                for (size_t i = 0; i < shown.size(); ++i)
                    for (size_t j = i + 1; j < shown.size(); ++j)
                        expect (! shown[i]->getBounds().intersects (shown[j]->getBounds()), context);
                for (int press = 0; press < 2; ++press)
                {
                    rig.button ("monitor-off")->onClick();
                    expect (bounds() == before, "monitor press moved the bar: " + context);
                    rig.button ("mic-mute-group")->onClick();
                    rig.button ("fx-mute-group")->onClick();
                    expect (bounds() == before, "mute press moved the bar: " + context);
                }
            }
        }
    }
};

static LiveMixMonitorOffTests liveMixMonitorOffTests;
}
