#include "model/Hotkeys.h"
#include "model/ProjectSerializer.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

/** A project file from elsewhere may carry a hotkey the app itself uses, or two cues on one key: the loader clears
    them (with a warning) so that Space / Esc never fire a cue next to GO / panic and one key fires one cue. */
class HotkeyLoadTests : public juce::UnitTest
{
public:
    HotkeyLoadTests() : juce::UnitTest ("Hotkey load validation", "Enqueue") {}

    void runTest() override
    {
        beginTest ("reserved keys: the app's own keys and Ctrl / Alt combinations");
        {
            expect (Hotkeys::isReservedKey (juce::KeyPress (juce::KeyPress::spaceKey)));
            expect (Hotkeys::isReservedKey (juce::KeyPress (juce::KeyPress::escapeKey)));
            expect (Hotkeys::isReservedKey (juce::KeyPress ('P')));
            expect (Hotkeys::isReservedKey (juce::KeyPress ('S', juce::ModifierKeys::commandModifier, 0)));
            expect (! Hotkeys::isReservedKey (juce::KeyPress::createFromDescription ("F5")));
            expect (! Hotkeys::isReservedKey (juce::KeyPress ('1')));
            expect (Hotkeys::isReservedDescription ("spacebar"));
            expect (Hotkeys::isReservedDescription ("escape"));
            expect (Hotkeys::isReservedDescription ("not a key at all"));
            expect (! Hotkeys::isReservedDescription ("F5"));
        }

        beginTest ("loading clears reserved and duplicate hotkeys and keeps the rest");
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("enqueue_hotkeys_" + juce::Uuid().toString());
            expect (dir.createDirectory().wasOk());

            Project p;
            p.name = "Keys";

            auto add = [&p] (const juce::String& cueName, const juce::String& hotkey)
            {
                Cue c;
                c.name = cueName;
                c.hotkey = hotkey;
                p.cues().push_back (c);
            };

            add ("space", juce::KeyPress (juce::KeyPress::spaceKey).getTextDescription());
            add ("escape", juce::KeyPress (juce::KeyPress::escapeKey).getTextDescription());
            add ("first F5", "F5");
            add ("second F5", "F5");
            add ("fine", "F6");
            add ("none", "");

            const auto file = dir.getChildFile ("keys.enqueue");
            expect (ProjectSerializer::save (p, file).wasOk());

            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::load (file, q, &warnings).wasOk());
            expectEquals ((int) q.cues().size(), 6);

            expect (q.cues()[0].hotkey.isEmpty());   // Space would fire next to GO
            expect (q.cues()[1].hotkey.isEmpty());   // Esc next to the panic
            expectEquals (q.cues()[2].hotkey, juce::String ("F5"));
            expect (q.cues()[3].hotkey.isEmpty());   // the duplicate: one key fires one cue
            expectEquals (q.cues()[4].hotkey, juce::String ("F6"));
            expect (q.cues()[5].hotkey.isEmpty());

            int cleared = 0;

            for (const auto& w : warnings)
                if (w.contains ("Hotkey") && w.contains ("cleared"))
                    ++cleared;

            expectEquals (cleared, 3);

            dir.deleteRecursively();
        }
    }
};

static HotkeyLoadTests hotkeyLoadTests;

} // namespace gocue::tests
