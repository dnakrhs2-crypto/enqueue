#include "GlobalHotkeys.h"
#include "MixDocument.h"
#include "MixEngine.h"
#include "MuteGroups.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace gocue::livemix
{

/** The mute groups: the hotkey mapping, the session marks, and the engine's behaviour when a group is muted. */
class MuteGroupsTests : public juce::UnitTest
{
public:
    MuteGroupsTests() : juce::UnitTest ("LiveMix mute groups", "LiveMix") {}

    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 256;
    static constexpr int numIns = 4, numOuts = 4;

    struct Io
    {
        juce::AudioBuffer<float> in { numIns, blockSize }, out { numOuts, blockSize };

        void setInput (int channel, float value) { juce::FloatVectorOperations::fill (in.getWritePointer (channel), value, blockSize); }
        float last (int channel) const { return out.getSample (channel, blockSize - 1); }
    };

    static void render (MixEngine& engine, Io& io, int blocks = 1)
    {
        for (int i = 0; i < blocks; ++i)
            engine.renderBlock (io.in.getArrayOfReadPointers(), numIns, io.out.getArrayOfWritePointers(), numOuts, blockSize);
    }

    void runTest() override
    {
        beginTest ("a global hotkey: F keys and the number pad stand alone, letters and digits need a modifier, the rest is refused");
        {
            unsigned int mods = 0, vk = 0;
            expect (GlobalHotkeys::toWindowsHotkey (juce::KeyPress::createFromDescription ("F9"), mods, vk));
            expectEquals ((int) vk, 0x78);   // VK_F9
            expectEquals ((int) mods, 0);

            expect (GlobalHotkeys::toWindowsHotkey (juce::KeyPress::createFromDescription ("ctrl + alt + M"), mods, vk));
            expectEquals ((int) vk, (int) 'M');
            expectEquals ((int) mods, 0x0002 | 0x0001);   // MOD_CONTROL | MOD_ALT

            expect (GlobalHotkeys::toWindowsHotkey (juce::KeyPress::createFromDescription ("shift + 5"), mods, vk));
            expectEquals ((int) vk, (int) '5');
            expectEquals ((int) mods, 0x0004);   // MOD_SHIFT

            expect (GlobalHotkeys::toWindowsHotkey (juce::KeyPress (juce::KeyPress::numberPad3), mods, vk));
            expectEquals ((int) vk, 0x63);   // VK_NUMPAD3

            expect (GlobalHotkeys::toWindowsHotkey (juce::KeyPress ('m', juce::ModifierKeys::ctrlModifier, 0), mods, vk));   // a lower-case code
            expectEquals ((int) vk, (int) 'M');

            expect (! GlobalHotkeys::toWindowsHotkey (juce::KeyPress (juce::KeyPress::escapeKey), mods, vk));
            expect (! GlobalHotkeys::toWindowsHotkey (juce::KeyPress (juce::KeyPress::leftKey), mods, vk));
            expect (! GlobalHotkeys::toWindowsHotkey (juce::KeyPress(), mods, vk));

            expect (GlobalHotkeys::reasonToRefuse (juce::KeyPress::createFromDescription ("F9")).isEmpty());
            expect (GlobalHotkeys::reasonToRefuse (juce::KeyPress::createFromDescription ("ctrl + M")).isEmpty());
            expect (GlobalHotkeys::reasonToRefuse (juce::KeyPress (juce::KeyPress::numberPad0)).isEmpty());
            expect (GlobalHotkeys::reasonToRefuse (juce::KeyPress::createFromDescription ("M")).isNotEmpty());        // would hijack typing
            expect (GlobalHotkeys::reasonToRefuse (juce::KeyPress::createFromDescription ("7")).isNotEmpty());
            expect (GlobalHotkeys::reasonToRefuse (juce::KeyPress (juce::KeyPress::spaceKey)).isNotEmpty());
            expect (GlobalHotkeys::reasonToRefuse (juce::KeyPress (juce::KeyPress::escapeKey)).isNotEmpty());
        }

        beginTest ("the mute-group marks are saved with the session and default to off");
        {
            MixChannel c;
            MixFx f;
            expect (! c.muteGroup && ! f.muteGroup);

            MixSession s;
            s.addChannel ("A");
            s.addChannel ("B");
            s.addFx ("R");
            s.channels[0].muteGroup = true;
            s.fx[0].muteGroup = true;

            MixSession q;
            expect (MixSession::fromJson (s.toJson(), q).wasOk());
            expect (q.channels.size() == 2 && q.fx.size() == 1);
            expect (q.channels[0].muteGroup);
            expect (! q.channels[1].muteGroup);
            expect (q.fx[0].muteGroup);
        }

        beginTest ("a group has members only while a mic / FX carries its mark (the top bar switch asks before muting)");
        {
            MixEngine engine;
            MixDocument doc (engine);
            doc.newSession();   // one mic, one FX, no marks
            MuteGroups groups (doc);
            const auto mic = doc.getSession().channels[0].id;
            const auto fx = doc.getSession().fx[0].id;
            expect (! groups.hasMembers (MuteGroups::Group::mic));
            expect (! groups.hasMembers (MuteGroups::Group::fx));

            doc.setFxMuteGroup (fx, true);
            expect (groups.hasMembers (MuteGroups::Group::fx));
            expect (! groups.hasMembers (MuteGroups::Group::mic));

            doc.setChannelMuteGroup (mic, true);
            expect (groups.hasMembers (MuteGroups::Group::mic));

            doc.setFxMuteGroup (fx, false);
            expect (! groups.hasMembers (MuteGroups::Group::fx));
            expect (groups.hasMembers (MuteGroups::Group::mic));
        }

        beginTest ("the mic group mutes its members only and hands their switches back; the FX group silences the return");
        {
            MixEngine engine;
            engine.prepare (sampleRate, blockSize);
            MixDocument doc (engine);
            doc.newSession();   // one mic, one FX
            doc.applyToEngine();
            const auto a = doc.getSession().channels[0].id;
            const auto b = doc.addChannel();
            const auto fx = doc.getSession().fx[0].id;
            doc.setChannelInput (a, 0, false);
            doc.setChannelInput (b, 1, false);

            Io io;
            io.setInput (0, 0.5f);
            io.setInput (1, 0.25f);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.75f, 1e-6f);   // both mics on the master

            MuteGroups groups (doc);
            int changes = 0;
            groups.onChanged = [&changes] { ++changes; };

            doc.setChannelMuteGroup (a, true);
            groups.toggle (MuteGroups::Group::mic);
            expect (groups.isMuted (MuteGroups::Group::mic));
            expectEquals (changes, 1);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.25f, 1e-6f);   // A is gone (ramped), B is not in the group
            expect (doc.getSession().channels[0].on);                 // A's own switch is untouched

            doc.setChannelMuteGroup (b, true);   // joins while the group is muted
            groups.apply();
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.0f, 1e-6f);

            doc.setChannelMuteGroup (b, false);
            groups.apply();
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.25f, 1e-6f);

            groups.toggle (MuteGroups::Group::mic);
            expect (! groups.isMuted (MuteGroups::Group::mic));
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.75f, 1e-6f);   // back as it was

            // the FX group
            doc.setSend (a, fx, 1.0, false);   // A's 0.5 into the FX, returned in full
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 1.25f, 1e-6f);

            doc.setFxMuteGroup (fx, true);
            groups.toggle (MuteGroups::Group::fx);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.75f, 1e-6f);   // the return is silent, the mics stay
            expectWithinAbsoluteError (doc.getSession().fx[0].returnAmount, 1.0, 1e-9);

            groups.reset();
            expect (! groups.isMuted (MuteGroups::Group::fx));
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 1.25f, 1e-6f);

            // a session applied again to the engine keeps the group state consistent
            groups.set (MuteGroups::Group::mic, true);
            doc.applyToEngine();
            groups.apply();
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.25f + 0.0f, 1e-6f);   // A muted: its send is cut with it, B remains
        }

        beginTest ("every mic / every FX (the top bar's switches) mutes with no mark set and hands each switch back");
        {
            MixEngine engine;
            engine.prepare (sampleRate, blockSize);
            MixDocument doc (engine);
            doc.newSession();   // one mic, one FX, no marks
            doc.applyToEngine();
            const auto a = doc.getSession().channels[0].id;
            const auto b = doc.addChannel();
            const auto fx = doc.getSession().fx[0].id;
            doc.setChannelInput (a, 0, false);
            doc.setChannelInput (b, 1, false);
            doc.setSend (a, fx, 1.0, false);   // A's 0.5 into the FX, returned in full

            Io io;
            io.setInput (0, 0.5f);
            io.setInput (1, 0.25f);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 1.25f, 1e-6f);   // both mics and the FX

            MuteGroups groups (doc);
            int changes = 0;
            groups.onChanged = [&changes] { ++changes; };

            groups.setAll (MuteGroups::Group::fx, true);
            expectEquals (changes, 1);
            expect (groups.isAllMuted (MuteGroups::Group::fx) && groups.isAnyMuted (MuteGroups::Group::fx));
            expect (! groups.isMuted (MuteGroups::Group::fx));   // the group itself is not
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.75f, 1e-6f);   // every return silent, the mics stay
            expect (doc.getSession().fx[0].on);
            expectWithinAbsoluteError (doc.getSession().fx[0].returnAmount, 1.0, 1e-9);

            groups.setAll (MuteGroups::Group::mic, true);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.0f, 1e-6f);   // nothing left
            expect (doc.getSession().channels[0].on && doc.getSession().channels[1].on);
            expect (engine.areAllChannelsMuted() && engine.areAllFxMuted());

            // a mic added while every mic is muted is silent from its very first block - before the app applies the
            // groups again after the structure change
            io.setInput (2, 0.5f);
            const auto c = doc.addChannel();
            doc.setChannelInput (c, 2, false);
            render (engine, io, 1);
            expectWithinAbsoluteError (io.out.getMagnitude (0, 0, blockSize), 0.0f, 1e-6f);
            expectWithinAbsoluteError (io.out.getMagnitude (1, 0, blockSize), 0.0f, 1e-6f);
            io.setInput (2, 0.0f);   // out of the sums below

            groups.setAll (MuteGroups::Group::mic, false);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.75f, 1e-6f);

            // a mic switched OFF before stays OFF after
            doc.setChannelOn (b, false);
            groups.setAll (MuteGroups::Group::mic, true);
            groups.setAll (MuteGroups::Group::mic, false);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.5f, 1e-6f);   // A only, the FX still muted
            doc.setChannelOn (b, true);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.75f, 1e-6f);

            // release: everything of that kind and its group together, one change
            doc.setFxMuteGroup (fx, true);
            groups.set (MuteGroups::Group::fx, true);
            changes = 0;
            groups.release (MuteGroups::Group::fx);
            expectEquals (changes, 1);
            expect (! groups.isAnyMuted (MuteGroups::Group::fx));
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 1.25f, 1e-6f);
            groups.release (MuteGroups::Group::fx);
            expectEquals (changes, 1);   // nothing to release: no change

            // the engine rebuilt (a session applied again) keeps everything muted
            groups.setAll (MuteGroups::Group::fx, true);
            doc.applyToEngine();
            groups.apply();
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.75f, 1e-6f);

            groups.reset();   // a session opened
            expect (! groups.isAnyMuted (MuteGroups::Group::fx) && ! groups.isAnyMuted (MuteGroups::Group::mic));
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 1.25f, 1e-6f);
        }
    }
};

static MuteGroupsTests muteGroupsTests;

} // namespace gocue::livemix
