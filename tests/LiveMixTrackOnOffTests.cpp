#include "LiveMixSettings.h"
#include "MixDocument.h"
#include "MixEngine.h"
#include "MuteGroups.h"
#include "TrackHotkeys.h"
#include "ui/ChannelCard.h"
#include "ui/FxDrawer.h"
#include "ui/LiveMixLookAndFeel.h"
#include "ui/SettingsDialog.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace gocue::livemix
{

/** A user asked (2026-10-15) to switch each mic and each FX on and off from the keyboard, not only a whole mute group:
    an FX channel gets an ON/OFF of its own like a mic, and every mic / FX number can have a global hotkey. */
class TrackOnOffTests : public juce::UnitTest
{
public:
    TrackOnOffTests() : juce::UnitTest ("LiveMix track ON/OFF", "LiveMix") {}

    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 256;
    static constexpr int numIns = 2, numOuts = 2;

    struct Io
    {
        juce::AudioBuffer<float> in { numIns, blockSize }, out { numOuts, blockSize };

        void setInput (int channel, float value) { juce::FloatVectorOperations::fill (in.getWritePointer (channel), value, blockSize); }
        float last (int channel) const { return out.getSample (channel, blockSize - 1); }
    };

    static void render (MixEngine& engine, Io& io, int blocks)
    {
        for (int i = 0; i < blocks; ++i)
            engine.renderBlock (io.in.getArrayOfReadPointers(), numIns, io.out.getArrayOfWritePointers(), numOuts, blockSize);
    }

    /** The new session's one mic (input 1 at 0.5) sends all of itself to its one FX (no plugins, return 100 %): the
        master carries 0.5 dry + 0.5 back from the FX. */
    struct Rig
    {
        MixEngine engine;
        MixDocument doc { engine };
        juce::Uuid mic, fx;
        Io io;

        Rig()
        {
            engine.prepare (sampleRate, blockSize);
            doc.newSession();
            doc.applyToEngine();
            mic = doc.getSession().channels[0].id;
            fx = doc.getSession().fx[0].id;
            doc.setChannelInput (mic, 0, false);
            doc.setSend (mic, fx, 1.0, false);
            io.setInput (0, 0.5f);
        }

        float settle()
        {
            render (engine, io, 3);
            return io.last (0);
        }
    };

    template <typename T>
    static T* childOfType (juce::Component& parent)
    {
        for (auto* child : parent.getChildren())
            if (auto* found = dynamic_cast<T*> (child))
                return found;
        return nullptr;
    }

    static juce::Label* labelWithText (juce::Component& parent, const juce::String& text)
    {
        for (auto* child : parent.getChildren())
        {
            if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getText() == text)
                return label;
            if (auto* inside = labelWithText (*child, text))
                return inside;
        }
        return nullptr;
    }

    void runTest() override
    {
        using Kind = TrackHotkeys::Kind;

        beginTest ("an FX channel is ON unless its session says OFF; the switch is saved, and a file from before it opens ON");
        {
            expect (MixFx().on);

            MixSession s;
            s.addChannel ("A");
            s.addFx ("room");
            s.addFx ("hall");
            s.fx[1].on = false;

            MixSession q;
            expect (MixSession::fromJson (s.toJson(), q).wasOk());
            expect (q.fx.size() == 2 && q.fx[0].on && ! q.fx[1].on);

            auto old = juce::JSON::parse (s.toJson());   // the same session as a file written before the switch existed
            if (auto* fxArray = old.getProperty ("fx", {}).getArray())
                for (auto& f : *fxArray)
                    if (auto* o = f.getDynamicObject())
                        o->removeProperty ("on");

            MixSession r;
            expect (MixSession::fromJson (juce::JSON::toString (old), r).wasOk());
            expect (r.fx.size() == 2 && r.fx[0].on && r.fx[1].on);
        }

        beginTest ("an FX switched OFF is silent and keeps its return amount; ON brings it back");
        {
            Rig rig;
            expectWithinAbsoluteError (rig.settle(), 1.0f, 1e-6f);

            rig.doc.setFxOn (rig.fx, false);
            expect (! rig.doc.getSession().fx[0].on);
            expectWithinAbsoluteError (rig.settle(), 0.5f, 1e-6f);   // the dry mic only
            expectWithinAbsoluteError (rig.doc.getSession().fx[0].returnAmount, 1.0, 1e-9);

            rig.doc.setFxOn (rig.fx, true);
            expectWithinAbsoluteError (rig.settle(), 1.0f, 1e-6f);
        }

        beginTest ("the FX mute group and the FX's own switch: either silences it, and releasing the group leaves an OFF FX off");
        {
            Rig rig;
            MuteGroups groups (rig.doc);
            rig.doc.setFxMuteGroup (rig.fx, true);

            groups.set (MuteGroups::Group::fx, true);
            expectWithinAbsoluteError (rig.settle(), 0.5f, 1e-6f);
            rig.doc.setFxOn (rig.fx, false);
            groups.set (MuteGroups::Group::fx, false);
            expectWithinAbsoluteError (rig.settle(), 0.5f, 1e-6f);   // still OFF by its own switch
            rig.doc.setFxOn (rig.fx, true);
            expectWithinAbsoluteError (rig.settle(), 1.0f, 1e-6f);
        }

        beginTest ("a session opened with an FX OFF keeps it silent from the very first block");
        {
            MixEngine engine;
            engine.prepare (sampleRate, blockSize);
            MixDocument doc (engine);
            doc.newSession();
            auto s = doc.getSession();
            s.channels[0].inputFirst = 0;
            s.sendFor (s.channels[0], s.fx[0].id).amount = 1.0;
            s.fx[0].on = false;
            const auto file = juce::File::createTempFile (MixSession::fileExtension);
            expect (s.save (file).wasOk());
            expect (doc.load (file).wasOk());
            file.deleteFile();

            Io io;
            io.setInput (0, 0.5f);
            render (engine, io, 1);
            float worst = 0.0f;

            for (int i = 0; i < blockSize; ++i)
                worst = juce::jmax (worst, std::abs (io.out.getSample (0, i) - 0.5f));

            expectWithinAbsoluteError (worst, 0.0f, 1e-6f);   // not a sample of the return while it opens
        }

        beginTest ("the ON/OFF hotkey ids: one per mic and FX number, after the mute groups, the window and the plugin groups");
        {
            expectEquals (TrackHotkeys::idFor (Kind::mic, 1), 9);
            expectEquals (TrackHotkeys::idFor (Kind::mic, MixSession::maxChannels), 16);
            expectEquals (TrackHotkeys::idFor (Kind::fx, 1), 17);
            expectEquals (TrackHotkeys::idFor (Kind::fx, MixSession::maxFx), 20);
            expectEquals (TrackHotkeys::lastId, 20);

            Kind kind = Kind::mic;
            int number = 0;
            expect (TrackHotkeys::fromId (17, kind, number) && kind == Kind::fx && number == 1);
            expect (TrackHotkeys::fromId (12, kind, number) && kind == Kind::mic && number == 4);
            expect (! TrackHotkeys::fromId (8, kind, number));    // plugin group 5
            expect (! TrackHotkeys::fromId (21, kind, number));
        }

        beginTest ("a track hotkey switches the mic / FX at its number and names it; a number with no track does nothing");
        {
            Rig rig;
            rig.doc.renameChannel (rig.mic, ko ("메인 마이크"));
            rig.doc.renameFx (rig.fx, "room");

            expectEquals (TrackHotkeys::toggle (rig.doc, Kind::mic, 1), ko ("메인 마이크 OFF"));
            expect (! rig.doc.getSession().channels[0].on);
            expectEquals (TrackHotkeys::toggle (rig.doc, Kind::mic, 1), ko ("메인 마이크 ON"));
            expect (rig.doc.getSession().channels[0].on);

            expectEquals (TrackHotkeys::toggle (rig.doc, Kind::fx, 1), juce::String ("room OFF"));
            expect (! rig.doc.getSession().fx[0].on);
            expectWithinAbsoluteError (rig.settle(), 0.5f, 1e-6f);

            expect (TrackHotkeys::toggle (rig.doc, Kind::mic, 2).isEmpty());   // one mic only
            expect (TrackHotkeys::toggle (rig.doc, Kind::fx, 2).isEmpty());
            expect (rig.doc.getSession().channels[0].on);

            // the key goes with the number: the second mic is number 1 once the first is removed
            const auto second = rig.doc.addChannel();
            rig.doc.removeChannel (rig.mic);
            expect (rig.doc.getSession().channels[0].id == second);
            TrackHotkeys::toggle (rig.doc, Kind::mic, 1);
            expect (! rig.doc.getSession().channels[0].on);
        }

        beginTest ("the ON/OFF hotkeys are kept per number in the settings, and only the numbers the session has are registered");
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("lm-trackkeys", {});
            expect (dir.createDirectory().wasOk());
            {
                LiveMixSettings settings (dir);
                TrackHotkeys::setKey (settings, Kind::mic, 1, "F10");
                TrackHotkeys::setKey (settings, Kind::mic, 3, "F12");
                TrackHotkeys::setKey (settings, Kind::fx, 1, " ctrl + alt + R ");
                TrackHotkeys::setKey (settings, Kind::fx, 2, "F11");
                settings.saveIfNeeded();
            }

            LiveMixSettings settings (dir);   // a new launch
            expectEquals (TrackHotkeys::getKey (settings, Kind::mic, 1), juce::String ("F10"));
            expectEquals (TrackHotkeys::getKey (settings, Kind::fx, 1), juce::String ("ctrl + alt + R"));
            expect (TrackHotkeys::getKey (settings, Kind::mic, 2).isEmpty());

            MixSession s;
            s.addChannel (ko ("메인 마이크"));
            s.addChannel();   // "마이크 2"
            s.addFx ("room");

            const auto tracks = TrackHotkeys::tracks (s);
            expectEquals ((int) tracks.size(), 3);
            if (tracks.size() == 3)
            {
                expect (tracks[0].kind == Kind::mic && tracks[0].number == 1);
                expectEquals (tracks[0].label, ko ("메인 마이크 ON/OFF"));
                expectEquals (tracks[1].label, ko ("마이크 2 ON/OFF"));
                expect (tracks[2].kind == Kind::fx && tracks[2].number == 1);
                expectEquals (tracks[2].label, juce::String ("room ON/OFF"));
            }

            // every id once: a key for each number with a track, nothing for the rest (mic 3 and FX 2 have keys but no track)
            const auto regs = TrackHotkeys::registrations (s, settings);
            expectEquals ((int) regs.size(), TrackHotkeys::lastId - TrackHotkeys::firstId + 1);
            auto keyOf = [&regs] (int id)
            {
                for (const auto& r : regs)
                    if (r.id == id)
                        return r.key;
                return juce::String ("?");
            };
            expectEquals (keyOf (TrackHotkeys::idFor (Kind::mic, 1)), juce::String ("F10"));
            expectEquals (keyOf (TrackHotkeys::idFor (Kind::mic, 2)), juce::String());
            expectEquals (keyOf (TrackHotkeys::idFor (Kind::mic, 3)), juce::String());
            expectEquals (keyOf (TrackHotkeys::idFor (Kind::fx, 1)), juce::String ("ctrl + alt + R"));
            expectEquals (keyOf (TrackHotkeys::idFor (Kind::fx, 2)), juce::String());

            for (const auto& r : regs)
                if (r.id == TrackHotkeys::idFor (Kind::mic, 1))
                    expectEquals (r.what, ko ("메인 마이크 ON/OFF"));

            // the settings also list a number the session lacks while it keeps a key (mic 3, FX 2): it can be seen and
            // cleared there, and its key is not handed to another row
            const auto rows = TrackHotkeys::settingsRows (s, settings);
            expectEquals ((int) rows.size(), 5);
            if (rows.size() == 5)
            {
                expect (rows[2].kind == Kind::mic && rows[2].number == 3 && ! rows[2].inSession);
                expectEquals (rows[2].label, ko ("마이크 3 ON/OFF"));
                expect (rows[3].kind == Kind::fx && rows[3].number == 1 && rows[3].inSession);
                expect (rows[4].kind == Kind::fx && rows[4].number == 2 && ! rows[4].inSession);
                expectEquals (rows[4].label, juce::String ("FX 2 ON/OFF"));
                expect (rows[0].inSession && rows[1].inSession);
            }

            expect (dir.deleteRecursively());
        }

        LiveMixLookAndFeel lookAndFeel;
        struct RestoreLookAndFeel
        {
            juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
            ~RestoreLookAndFeel() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
        } restoreLookAndFeel;
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        beginTest ("the FX drawer's ON/OFF lamp shows the selected FX's switch and switches it; an OFF FX's tab is dimmed");
        {
            Rig rig;
            rig.doc.addFx();   // a second FX: its tab stays lit while the first is OFF
            FxDrawer drawer (rig.doc);
            drawer.setLookAndFeel (&lookAndFeel);
            drawer.setSize (430, drawer.getPreferredHeight (430));
            rig.doc.onValueChanged = [&drawer] { drawer.refresh(); };

            auto* lamp = childOfType<LampButton> (drawer);
            expect (lamp != nullptr);
            if (lamp != nullptr)
            {
                expect (lamp->isOn());
                expect (drawer.getLocalBounds().contains (lamp->getBounds()));
                auto* remove = dynamic_cast<juce::Button*> ([&drawer]() -> juce::Component*
                {
                    for (auto* child : drawer.getChildren())
                        if (auto* b = dynamic_cast<juce::Button*> (child); b != nullptr && b->getButtonText() == ko ("이 FX 삭제"))
                            return b;
                    return nullptr;
                }());
                auto* chain = labelWithText (drawer, ko ("VST3 체인"));
                expect (remove != nullptr && chain != nullptr);
                if (remove != nullptr && chain != nullptr)
                    expect (lamp->getY() > remove->getBottom() && lamp->getBottom() < chain->getY() && lamp->getWidth() > 300);   // under the name row, as wide as the drawer

                lamp->onClick();   // what a click on it does
                expect (! rig.doc.getSession().fx[0].on);
                lamp = childOfType<LampButton> (drawer);
                expect (lamp != nullptr && ! lamp->isOn());

                juce::Component* firstTab = nullptr;
                juce::Component* secondTab = nullptr;
                for (auto* child : drawer.getChildren())   // "FX1  리버브", "FX2"
                    if (auto* b = dynamic_cast<juce::Button*> (child))
                    {
                        if (b->getButtonText().startsWith ("FX1")) firstTab = b;
                        if (b->getButtonText().startsWith ("FX2")) secondTab = b;
                    }
                expect (firstTab != nullptr && secondTab != nullptr);
                if (firstTab != nullptr && secondTab != nullptr)
                {
                    expect (firstTab->getAlpha() < 0.9f);
                    expectEquals (secondTab->getAlpha(), 1.0f);
                }

                rig.doc.setFxOn (rig.fx, true);
                lamp = childOfType<LampButton> (drawer);
                expect (lamp != nullptr && lamp->isOn());
            }
        }

        beginTest ("a mic card's send to an OFF FX is dimmed, the send itself untouched");
        {
            Rig rig;
            ChannelCard card (rig.doc, rig.mic);
            card.setLookAndFeel (&lookAndFeel);
            card.setSize (1400, card.getPreferredHeight (1400));
            rig.doc.onValueChanged = [&card] { card.refresh(); };

            auto sendRow = [&card]() -> juce::Component*
            {
                for (auto* child : card.getChildren())
                    for (auto* inner : child->getChildren())
                        if (auto* label = dynamic_cast<juce::Label*> (inner); label != nullptr && label->getText() == "FX1")
                            return child;
                return nullptr;
            };

            expect (sendRow() != nullptr && sendRow()->getAlpha() == 1.0f);
            rig.doc.setFxOn (rig.fx, false);
            expect (sendRow() != nullptr && sendRow()->getAlpha() < 0.9f);
            expectWithinAbsoluteError (rig.doc.getSession().channels[0].sends[0].amount, 1.0, 1e-9);
            rig.doc.setFxOn (rig.fx, true);
            expect (sendRow() != nullptr && sendRow()->getAlpha() == 1.0f);
        }

        beginTest ("the settings list an ON/OFF hotkey for every mic and FX of the session, and follow it when a mic is added");
        {
            Rig rig;
            rig.doc.renameChannel (rig.mic, ko ("메인 마이크"));
            rig.doc.renameFx (rig.fx, "room");
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("lm-trackkeys-ui", {});
            expect (dir.createDirectory().wasOk());
            {
                LiveMixSettings settings (dir);
                TrackHotkeys::setKey (settings, Kind::mic, 1, "F10");
                SettingsDialog::show (rig.engine, settings, nullptr, {}, {}, {}, {}, {}, {}, {},
                                      [&rig, &settings] { return TrackHotkeys::settingsRows (rig.doc.getSession(), settings); });

                juce::Component* content = nullptr;
                auto& desktop = juce::Desktop::getInstance();
                for (int i = 0; i < desktop.getNumComponents(); ++i)
                    if (auto* dialog = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)); dialog != nullptr && dialog->getName() == ko ("설정"))
                        if (auto* viewport = dynamic_cast<juce::Viewport*> (dialog->getContentComponent()))
                            content = viewport->getViewedComponent();

                expect (content != nullptr);
                if (content != nullptr)
                {
                    expect (labelWithText (*content, ko ("메인 마이크 ON/OFF")) != nullptr);
                    expect (labelWithText (*content, "room ON/OFF") != nullptr);
                    expect (labelWithText (*content, ko ("마이크 2 ON/OFF")) == nullptr);
                    auto* key = dynamic_cast<juce::Button*> (content->findChildWithID ("hotkey-mic-1"));
                    expect (key != nullptr && key->getButtonText() == "F10");

                    rig.doc.addChannel();
                    SettingsDialog::tracksChanged();
                    expect (labelWithText (*content, ko ("마이크 2 ON/OFF")) != nullptr);
                    expect (content->findChildWithID ("hotkey-mic-2") != nullptr);

                    // each kind under its mute group: mic group, mics, FX group, FX
                    auto y = [&content] (const juce::String& text) { auto* l = labelWithText (*content, text); return l != nullptr ? l->getY() : -1; };
                    expect (y (ko ("마이크 뮤트그룹")) < y (ko ("메인 마이크 ON/OFF")));
                    expect (y (ko ("메인 마이크 ON/OFF")) < y (ko ("마이크 2 ON/OFF")));
                    expect (y (ko ("마이크 2 ON/OFF")) < y (ko ("FX 뮤트그룹")));
                    expect (y (ko ("FX 뮤트그룹")) < y ("room ON/OFF"));
                    expect (y ("room ON/OFF") < y (ko ("창 숨기기/불러오기")));

                    // the row follows the channel's name
                    rig.doc.renameChannel (rig.doc.getSession().channels[1].id, ko ("게스트 마이크"));
                    SettingsDialog::tracksChanged();
                    expect (labelWithText (*content, ko ("게스트 마이크 ON/OFF")) != nullptr);
                    expect (labelWithText (*content, ko ("마이크 2 ON/OFF")) == nullptr);

                    // review 1 (P2): mic 2 keeps F11, mic 2 is removed: its key stays in sight (dimmed, clearable) and
                    // FX 1 cannot take it, or both would claim F11 once a second mic is back
                    TrackHotkeys::setKey (settings, Kind::mic, 2, "F11");
                    rig.doc.removeChannel (rig.doc.getSession().channels[1].id);
                    SettingsDialog::tracksChanged();
                    auto* gone = labelWithText (*content, ko ("마이크 2 ON/OFF"));
                    expect (gone != nullptr && gone->getAlpha() < 0.9f);
                    auto* goneKey = dynamic_cast<juce::Button*> (content->findChildWithID ("hotkey-mic-2"));
                    expect (goneKey != nullptr && goneKey->getButtonText() == "F11");
                    auto* fxKey = dynamic_cast<HotkeyButton*> (content->findChildWithID ("hotkey-fx-1"));
                    expect (fxKey != nullptr && fxKey->validate != nullptr);
                    if (fxKey != nullptr && fxKey->validate != nullptr)
                    {
                        expect (fxKey->validate (juce::KeyPress::createFromDescription ("F11")).isNotEmpty());
                        expect (fxKey->validate (juce::KeyPress::createFromDescription ("F12")).isEmpty());
                    }
                }

                SettingsDialog::closeIfOpen();
            }
            expect (dir.deleteRecursively());
        }
    }
};

static TrackOnOffTests trackOnOffTests;


} // namespace gocue::livemix
