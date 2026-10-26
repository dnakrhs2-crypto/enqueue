#include "ui/RecordingWindow.h"
#include "ui/CollabWindow.h"
#include "ui/MasterCard.h"
#include "ui/LiveMixLookAndFeel.h"
#include "../livemix/src/ui/MainComponent.h"
#include <thread>

namespace gocue::tests
{
namespace
{
using namespace livemix;
namespace c = livemix::collab::contracts;
using State = collab::CollabSession::State;
struct R10Folder
{
    juce::File file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveMixR10-" + juce::Uuid().toString());
    R10Folder() { file.createDirectory(); }
    ~R10Folder() { file.deleteRecursively(); }
};
struct R10Theme
{
    juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
    LiveMixLookAndFeel look;
    R10Theme() { juce::LookAndFeel::setDefaultLookAndFeel (&look); }
    ~R10Theme() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
};
template<class T> T& control (juce::Component& parent, const char* id)
{
    return *dynamic_cast<T*> (parent.findChildWithID (id));
}
collab::CollabSession::Snapshot room (bool connected = true)
{
    collab::CollabSession::Snapshot s;
    s.state = connected ? State::connected : State::hostWaiting;
    s.isHost = true; s.code = "4827"; s.selfId = 11; s.myNick = ko ("하람");
    s.roomMode = c::RoomMode::v2; s.nativeAvailable = s.signalingConnected = true;
    s.routeFirst = 2;
    s.personCount = connected ? 3 : 1;
    auto& self = s.people[1]; self.participantId = 11; self.isSelf = self.isHost = true; self.nickname = ko ("하람");
    self.kind = c::ParticipantKind::native; self.state = c::ConnectionState::connected;
    auto& native = s.people[2]; native.participantId = 22; native.slot = 1; native.nickname = ko ("윤슬");
    native.kind = c::ParticipantKind::native; native.state = c::ConnectionState::connected; native.estimatedOneWayMs = 18;
    auto& web = s.people[0]; web.participantId = 33; web.slot = 2; web.nickname = ko ("하늘");
    web.kind = c::ParticipantKind::web; web.state = c::ConnectionState::connected; web.estimatedOneWayMs = 46;
    s.kickableWebId = connected ? 33 : 0;
    if (! connected)
    {
        const auto me = self; s.people = {}; s.people[0] = me;
        s.canCreateWebInvite = true;
        s.webInviteUrl = "https://dnakrhs2-crypto.github.io/livemix-join/#v=2&code=4827&invite="
                         + juce::String::repeatedString ("a", 64) + "&generation=1";
        s.webInviteQrUrl = s.webInviteUrl;
    }
    return s;
}
}

class LiveMixRecordingWindowTests : public juce::UnitTest
{
public:
    LiveMixRecordingWindowTests() : UnitTest ("LiveMix Recorder window and R10 controls", "LiveMix") {}
    void runTest() override
    {
        R10Theme theme;
        R10Folder folder;
        LiveMixSettings settings (folder.file.getChildFile ("settings"));
        settings.setCollabNick (ko ("하람"));
        recording::Options options;
        options.directoryUtf8 = ko ("C:\\Users\\하람\\Documents\\LiveMix 녹음").toStdString(); // mock path; the injected start writes nothing
        settings.setRecordingOptions (options);

        beginTest ("WAV and MP3 settings, saved bitrates, and a successful start closes only after recording");
        int starts = 0, stops = 0, hides = 0;
        bool accepted = true;
        recording::Options started;
        RecordingPanel::Actions actions;
        actions.start = [&] (const auto& value) { ++starts; started = value; return accepted; };
        actions.stop = [&] { ++stops; }; actions.hide = [&] { ++hides; };
        RecordingPanel panel (settings, actions);
        expectEquals (panel.getWidth(), 420); expectEquals (panel.getHeight(), 304);
        expect (control<Chip> (panel, "format-wav").getToggleState());
        expect (! control<Chip> (panel, "bitrate-320").isEnabled());
        capture (panel, "rec-wav");
        control<Chip> (panel, "format-mp3").onClick();
        for (int rate : { 128, 256, 320 })
        {
            const auto id = "bitrate-" + juce::String (rate);
            auto& chip = control<Chip> (panel, id.toRawUTF8());
            expect (chip.isEnabled()); chip.onClick();
            LiveMixSettings reloaded (folder.file.getChildFile ("settings"));
            expect (reloaded.getRecordingOptions().format == recording::Format::mp3);
            expectEquals ((int) reloaded.getRecordingOptions().mp3Kbps, rate);
            expectEquals (juce::String (reloaded.getRecordingOptions().directoryUtf8), juce::String (options.directoryUtf8));
        }
        capture (panel, "rec-mp3");
        auto& record = control<RecordingButton> (panel, "record"); record.onClick();
        expectEquals (starts, 1); expectEquals (hides, 0); expect (! record.isEnabled());
        expect (started.format == recording::Format::mp3 && started.mp3Kbps == 320);
        recording::Snapshot snap; snap.state = recording::State::starting; snap.options = started;
        panel.setSnapshot (snap); expectEquals (hides, 0); expect (! control<Chip> (panel, "format-wav").isEnabled());
        snap.state = recording::State::recording; snap.elapsedSeconds = 5025;
        snap.fileCount = 1;
        snap.files[0].pathUtf8 = folder.file.getChildFile ("LiveMix_2026-10-26_14-05-33.mp3").getFullPathName().toStdString();
        panel.setSnapshot (snap); expectEquals (hides, 1);
        panel.setSnapshot (snap); expectEquals (hides, 1);
        expectEquals (record.getButtonText(), ko ("녹음 중지"));
        expectEquals (control<juce::Label> (panel, "elapsed").getText(), juce::String ("01:23:45"));
        for (const auto* id : { "format-wav", "format-mp3", "bitrate-128", "bitrate-256", "bitrate-320", "change-directory" })
            expect (! panel.findChildWithID (id)->isEnabled(), id);
        capture (panel, "rec-recording");
        RecordingPanel reopened (settings, actions); reopened.setSnapshot (snap);
        expectEquals (hides, 1); expectEquals (control<RecordingButton> (reopened, "record").getButtonText(), ko ("녹음 중지"));
        reopened.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)); expectEquals (stops, 0);
        record.onClick(); expectEquals (stops, 1); expect (! record.isEnabled());
        snap.state = recording::State::stopping; panel.setSnapshot (snap); expect (! record.isEnabled());
        snap.state = recording::State::idle; panel.setSnapshot (snap);
        expect (control<Chip> (panel, "format-wav").isEnabled()); expectEquals (record.getButtonText(), ko ("녹음"));

        beginTest ("failed opens stay in the window with one error line, then allow another start");
        const int before = hides;
        accepted = false; record.onClick();
        expectEquals (hides, before); expect (panel.findChildWithID ("recording-error")->isVisible());
        capture (panel, "rec-error");
        accepted = true; record.onClick();
        snap.state = recording::State::error; snap.error = recording::Error::openFailed;
        snap.errorDetail = "private diagnostic"; panel.setSnapshot (snap);
        expectEquals (hides, before); expect (panel.findChildWithID ("recording-error")->isVisible());
        expect (! control<juce::Label> (panel, "recording-error").getText().contains ("diagnostic"));
        expect (record.isEnabled());
        checkMaster (settings);
        checkCollab (settings);
        checkEngine (settings, folder.file);
    }

private:
    void layout (juce::Component& parent)
    {
        for (auto* child : parent.getChildren())
        {
            if (! child->isVisible()) continue;
            const auto id = child->getComponentID();
            expect (! child->getBounds().isEmpty(), "empty " + id);
            expect (parent.getLocalBounds().contains (child->getBounds()), "outside " + id + ": " + child->getBounds().toString());
            for (auto* other : parent.getChildren())
                if (other != child && other->isVisible()) expect (! child->getBounds().intersects (other->getBounds()), "overlap " + id + "/" + other->getComponentID());
        }
    }
    void capture (juce::Component& panel, const juce::String& name)
    {
        layout (panel);
        const auto path = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_R10_SCREENSHOT_DIR", {});
        for (float scale : { 1.0f, 1.25f, 1.5f })
        {
            const auto image = panel.createComponentSnapshot (panel.getLocalBounds(), true, scale);
            expect (image.isValid()); expectEquals (image.getWidth(), juce::roundToInt (panel.getWidth() * scale));
            expectEquals (image.getHeight(), juce::roundToInt (panel.getHeight() * scale));
            if (path.isEmpty()) continue;
            const juce::File directory (path); expect (directory.createDirectory().wasOk());
            juce::FileOutputStream output (directory.getChildFile (name + "-" + juce::String ((int) (scale * 100)) + ".png"));
            expect (output.openedOk());
            if (output.openedOk())
            {
                output.setPosition (0); output.truncate();
                expect (juce::PNGImageFormat().writeImageToStream (image, output));
            }
        }
    }
    void checkMaster (LiveMixSettings&)
    {
        beginTest ("master recording button shows minutes or hours and opens the recording action in all forms");
        MixEngine engine ("Local\\LiveMixR10Master-" + juce::Uuid().toString());
        MixDocument document (engine);
        document.getSession().device = { "ASIO", "Fixture", "Fixture", 128, 48000.0 };
        document.getSession().master.sendToObs = true;
        MasterCard master (document); master.setDeviceChannels ({ "1", "2", "3", "4" });
        master.pushMeter ({ 0.3f, 0.2f });
        master.setLatency (12.4, 128, 48000); master.setObsStatus (MasterCard::ObsStatus::connected);
        int opens = 0; master.onOpenRecording = [&] { ++opens; };
        auto& button = control<RecordingButton> (master, "recording");
        button.onClick(); expectEquals (opens, 1); expect (! button.red);
        expectEquals (recordingElapsedText (65), juce::String ("01:05"));
        expectEquals (recordingElapsedText (3599), juce::String ("59:59"));
        expectEquals (recordingElapsedText (3600), juce::String ("01:00:00"));
        for (bool live : { false, true })
        {
            recording::Snapshot s; s.state = live ? recording::State::recording : recording::State::idle; s.elapsedSeconds = 5025;
            master.setRecordingSnapshot (s); expect (button.red == live);
            expectEquals (button.getButtonText(), live ? juce::String ("01:23:45") : ko ("녹음"));
            for (int width : { 364, 428, 960, 1400 })
                for (bool folded : { false, true })
                {
                    master.setStrip (folded); master.setSize (width, master.getPreferredHeight (width));
                    expect (button.isVisible());
                    expect (master.getLocalBounds().contains (button.getBounds()));
                    // Approved A: right beside LUFS in every form that shows LUFS.
                    for (auto* child : master.getChildren())
                        if (auto* lufs = dynamic_cast<juce::Button*> (child); lufs != nullptr && lufs->getButtonText() == "LUFS" && lufs->isVisible())
                            expect (button.getX() == lufs->getRight() + 8 && button.getY() == lufs->getY(),
                                    "record beside LUFS at " + juce::String (width) + (folded ? " strip" : ""));
                    capture (master, "master-" + juce::String (width) + (folded ? "-strip" : "") + (live ? "-recording" : "-idle"));
                }
        }
        button.onClick(); expectEquals (opens, 2);
    }
    void checkCollab (LiveMixSettings& settings)
    {
        beginTest ("three people map by slot and participant ID; each volume, invite, request and kick uses its command");
        int invites = 0; c::ParticipantId volumeId = 0, kicked = 0; float gain = -1, monitorGain = -1; bool monitorOn = true;
        juce::String accepted, rejected;
        CollabPanel::Actions actions;
        actions.personMonitorVolume = [&] (auto id, float value) { volumeId = id; gain = value; };
        actions.createWebInvite = [&] { ++invites; };
        actions.acceptWebRequest = [&] (const auto& id) { accepted = id; };
        actions.rejectWebRequest = [&] (const auto& id) { rejected = id; };
        actions.kickWebGuest = [&] (auto id) { kicked = id; };
        actions.monitor = [&] (float v, bool on) { monitorGain = v; monitorOn = on; };
        actions.outputs = [] { return CollabPanel::OutputPairs { { "1", "2", "3", "4" }, true, true, true, 4 }; };
        CollabPanel panel (settings, actions); panel.setSize (440, 800);
        auto s = room (false); panel.setSnapshot (s);
        capture (panel, "collab-host-waiting");
        const auto clipboard = juce::SystemClipboard::getTextFromClipboard();
        control<juce::TextButton> (panel, "copy-invite").onClick();
        expectEquals (juce::SystemClipboard::getTextFromClipboard(), s.webInviteUrl);
        juce::SystemClipboard::copyTextToClipboard (clipboard);
        expect (control<juce::Label> (panel, "invite-url").getText().endsWith (ko ("…")));
        // Like the approved mock: the field shows the address without its scheme; the copy above is the full URL.
        expect (control<juce::Label> (panel, "invite-url").getText().startsWith ("dnakrhs2-crypto.github.io/"));
        control<juce::TextButton> (panel, "renew-invite").onClick(); expectEquals (invites, 1);
        s.nativeAvailable = false; panel.setSnapshot (s); expect (panel.findChildWithID ("copy-invite")->isVisible());
        s.webRequestCount = 1; s.webRequests[0] = { "request-a", ko ("하늘"), 0, true, true };
        panel.setSnapshot (s); capture (panel, "collab-request");
        expect (! panel.findChildWithID ("copy-invite")->isVisible());
        auto& viewport = control<juce::Viewport> (panel, "web-requests");
        auto* request = viewport.getViewedComponent()->getChildComponent (0);
        control<juce::TextButton> (*request, "accept").onClick(); expectEquals (accepted, juce::String ("request-a"));
        control<juce::TextButton> (*request, "reject").onClick(); expectEquals (rejected, juce::String ("request-a"));
        s.webRequests[0].canAccept = false; panel.setSnapshot (s);
        expect (! control<juce::TextButton> (*request, "accept").isEnabled());
        s.webRequestCount = 8;
        for (size_t i = 0; i < 8; ++i) s.webRequests[i] = { "request-" + juce::String ((int) i), juce::String::repeatedString (ko ("긴이름"), 12), 0, true, true };
        panel.setSize (400, 640); panel.setSnapshot (s); capture (panel, "collab-requests-long");
        expect (viewport.getVerticalScrollBar().isVisible());
        viewport.setViewPosition (0, viewport.getViewedComponent()->getHeight());
        auto* last = viewport.getViewedComponent()->getChildComponent (7);
        control<juce::TextButton> (*last, "accept").onClick(); expectEquals (accepted, juce::String ("request-7"));
        panel.setSize (440, 800); s = room(); panel.setSnapshot (s); panel.pushMeter ({ 0.3f, 0.2f }, false);
        capture (panel, "collab-host-three");
        expectEquals (control<juce::Label> (panel, "peer-nick").getText(), ko ("윤슬"));
        expectEquals (control<juce::Label> (panel, "peer-nick-2").getText(), ko ("하늘"));
        expect (! panel.findChildWithID ("volume-caption")->isVisible());
        expect (! panel.findChildWithID ("copy-invite")->isVisible());
        control<juce::Slider> (panel, "volume").setValue (37, juce::sendNotificationSync);
        expectEquals (volumeId, (c::ParticipantId) 22); expectWithinAbsoluteError (gain, 0.37f, 0.0001f);
        control<juce::Slider> (panel, "volume-2").setValue (62, juce::sendNotificationSync);
        expectEquals (volumeId, (c::ParticipantId) 33); expectWithinAbsoluteError (gain, 0.62f, 0.0001f);
        control<LampButton> (panel, "headphones").onClick(); expect (! monitorOn); expectEquals (monitorGain, 1.0f);
        control<juce::TextButton> (panel, "kick-2").onClick(); expectEquals (kicked, (c::ParticipantId) 33);
        s.route = collab::CollabAudio::Route::obs; panel.setSnapshot (s);
        for (const auto* id : { "volume", "volume-2", "headphones" }) expect (! panel.findChildWithID (id)->isEnabled());
        capture (panel, "collab-obs");
        s.route = collab::CollabAudio::Route::master; s.isHost = false; s.kickableWebId = 0; s.people[2].nickname = ko ("하람");
        s.people[0].estimatedOneWayMs = 61; panel.setSnapshot (s); capture (panel, "collab-participant");
        expect (! panel.findChildWithID ("kick-2")->isVisible() && ! panel.findChildWithID ("invite-caption")->isVisible());
        s.people[2].nickname = s.people[0].nickname = juce::String::repeatedString (ko ("긴닉네임"), 10);
        panel.setSize (400, 640); panel.setSnapshot (s); capture (panel, "collab-long-names");
        expect (control<juce::Label> (panel, "peer-nick-2").getText().endsWith (ko ("…")));
        s.people[2] = {}; s.personCount = 2; s.people[0].monitorVolume = 0.62f; panel.setSnapshot (s);
        expect (! panel.findChildWithID ("volume")->isVisible() && panel.findChildWithID ("volume-2")->isVisible());
        expectEquals ((int) control<juce::Slider> (panel, "volume-2").getValue(), 62);
        expectEquals (control<juce::Label> (panel, "seat-2").getText(), ko ("자리 2"));
        capture (panel, "collab-seat-two-remains");
        s = room(); s.roomMode = c::RoomMode::legacy2; s.people[0] = s.people[2]; s.people[2] = {}; s.personCount = 2;
        s.kickableWebId = 0; panel.setSize (440, 430); panel.setSnapshot (s); capture (panel, "collab-legacy-two");
        expect (panel.findChildWithID ("volume")->isVisible() && ! panel.findChildWithID ("volume-2")->isVisible());
        control<juce::Slider> (panel, "volume").setValue (44, juce::sendNotificationSync); expectEquals (volumeId, (c::ParticipantId) 22);
    }
    void checkEngine (LiveMixSettings& settings, const juce::File& folder)
    {
        beginTest ("production recording window uses R8 context, writes a take and stops without closing the engine");
        MixEngine engine ("Local\\LiveMixR10Window-" + juce::Uuid().toString()); engine.prepare (48000, 120);
        recording::Options options; options.directoryUtf8 = folder.getChildFile ("engine-take").getFullPathName().toStdString();
        settings.setRecordingOptions (options);
        std::array<recording::TrackIdentity, 3> people {{ { 11, c::ParticipantKind::native, "Host" },
            { 22, c::ParticipantKind::native, "Native" }, { 33, c::ParticipantKind::web, "Web" } }};
        engine.setRecordingContext (true, people);
        settings.setMicMuteHotkey ({}); settings.setFxMuteHotkey ({}); settings.setWindowHotkey ({});
        MixDocument document (engine);
        ObsPluginActions obs;
        obs.roots = [folder] { return ObsPluginInstaller::Roots { folder, folder, folder, [] { return false; }, {} }; };
        obs.elevate = [] (juce::String&) { return ObsPluginInstaller::Result::needsElevation; };
        livemix::MainComponent main (document, settings, obs);
        MasterCard* master = nullptr;
        for (auto* child : main.getChildren()) if (auto* m = dynamic_cast<MasterCard*> (child)) master = m;
        expect (master != nullptr); if (master == nullptr) return;
        auto& open = control<RecordingButton> (*master, "recording"); open.onClick();
        RecordingWindow* opened = nullptr;
        for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
            if (auto* w = dynamic_cast<RecordingWindow*> (juce::TopLevelWindow::getTopLevelWindow (i))) opened = w;
        expect (opened != nullptr); if (opened == nullptr) return;
        auto& window = *opened;
        expect (window.isUsingNativeTitleBar()); expectEquals (window.getName(), ko ("녹음"));
        auto* panel = dynamic_cast<RecordingPanel*> (window.getContentComponent()); expect (panel != nullptr);
        if (panel == nullptr) return;
        open.onClick(); expect (window.getContentComponent() == panel);
        // This suite runs on the harness's invisible Win32 desktop. Captures themselves remain offscreen.
        window.open(); expect (window.isVisible());
        control<RecordingButton> (*panel, "record").onClick();
        const auto wait = [&] (recording::State target)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + 5000;
            while (engine.getRecorder().snapshot().state != target && juce::Time::getMillisecondCounterHiRes() < end)
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
            return engine.getRecorder().snapshot().state == target;
        };
        expect (wait (recording::State::recording));
        auto live = engine.getRecorder().snapshot(); expect (live.collabAtStart); expectEquals ((int) live.fileCount, 4);
        expect (juce::String (live.files[0].pathUtf8).contains ("A_Host"));
        panel->setSnapshot (live); expect (! window.isVisible());
        window.open(); expect (window.isVisible());
        expect (! panel->findChildWithID ("format-wav")->isEnabled());
        window.closeButtonPressed(); expect (! window.isVisible());
        expect (engine.getRecorder().snapshot().state == recording::State::recording);
        juce::AudioBuffer<float> audio (2, 120); audio.clear();
        for (int i = 0; i < 10; ++i) engine.renderBlock (nullptr, 0, audio.getArrayOfWritePointers(), 2, 120);
        panel->setSnapshot (engine.getRecorder().snapshot());
        control<RecordingButton> (*panel, "record").onClick(); expect (wait (recording::State::idle));
        expect (juce::File (juce::String (live.files[0].pathUtf8)).getSize() > 44);
        panel->setSnapshot (engine.getRecorder().snapshot()); expect (panel->findChildWithID ("format-wav")->isEnabled());
        engine.shutdown();
    }
};
static LiveMixRecordingWindowTests liveMixRecordingWindowTests;
}
