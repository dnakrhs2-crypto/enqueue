// LiveMix 0.18.1 (2026-10-30, gom "알아서해"): the names the cards and the chain drawer cut to '...' read in full - two
// guests' mic names, long FX names in the send rows, long plugin names and a missing plugin's '(없음)' - the backup
// window's status line holds two lines, and its file column drops LiveMix's own extension.
#include "../livemix/src/ui/MainComponent.h"
#include "ui/BackupDialog.h"
#include "ui/ChainDrawer.h"
#include "ui/ChannelCard.h"
#include "ui/LiveMixLookAndFeel.h"
#include "ui/Widgets.h"

#include <set>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
using namespace gocue::livemix;

class LiveMixNameFitTests : public juce::UnitTest
{
public:
    LiveMixNameFitTests() : juce::UnitTest ("LiveMix name fit", "LiveMix") {}

    void runTest() override
    {
        LiveMixLookAndFeel lookAndFeel;
        struct RestoreLookAndFeel
        {
            juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
            ~RestoreLookAndFeel() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
        } restoreLookAndFeel;
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        beginTest ("a text too wide for its line takes two lines at the smaller font, never '...'");
        {
            TwoLineLabel label;
            const juce::Font one (juce::FontOptions (pt (13.0f), juce::Font::bold)), two (juce::FontOptions (pt (12.0f), juce::Font::bold));
            label.setFonts (one, two);
            label.setSize (56, 34);   // a send row's name at a 1440 window (Vercel 10/30: 56 px)

            for (const auto* text : { "리버브", "딜레이" })
            {
                label.setText (ko (text), juce::dontSendNotification);
                expect (label.getFont() == one, ko (text));
                expectEquals (drawnLines (label), 1, ko (text));
                expect (drawsWhole (label), ko (text));
            }

            // broken between words, never inside one (JUCE's own breaking drew '보컬 리 / 버브')
            const std::pair<const char*, const char*> splits[] { { "보컬 리버브", "보컬\n리버브" }, { "홀 리버브", "홀\n리버브" },
                                                                 { "딜레이 1/4", "딜레이\n1/4" } };
            for (const auto& [text, lines] : splits)
            {
                label.setText (ko (text), juce::dontSendNotification);
                expect (label.getFont() == two, ko (text));
                expectEquals (label.getShownText(), ko (lines));
                expectEquals (drawnLines (label), 2, ko (text));
                expect (drawsWhole (label), ko (text));
            }

            // a one-word name keeps JUCE's breaking; a second line still too wide ends in '…'
            expectEquals (TwoLineLabel::wordLines (two, ko ("돌비애트모스리버브"), 46), juce::String());
            const auto cut = TwoLineLabel::wordLines (two, ko ("보컬 리버브 플레이트 롱테일"), 46);
            expect (cut.startsWith (ko ("보컬\n")) && cut.endsWith (ko ("…")), cut);

            label.setSize (140, 34);   // room again: one line at the one-line font
            expect (label.getFont() == one);
            expectEquals (drawnLines (label), 1);
            expect (drawsWhole (label));
        }

        beginTest ("the cards at 1440, 1280 and 1920: two guests' names and long FX names read in full");
        {
            Fixture f;
            f.nameEverything();
            gocue::livemix::MainComponent main (f.document, f.settings, f.actions);

            for (const int width : { 1440, 1280, 1920 })
            {
                main.setSize (width, 900);
                main.resized();
                const auto where = " @" + juce::String (width);

                for (const auto* text : { "게스트 마이크 1", "게스트 마이크 2", "보컬 리버브", "딜레이 1/4" })
                {
                    const auto labels = labelsWithText (main, ko (text));
                    expect (! labels.empty(), ko (text) + where);
                    for (auto* label : labels)
                        expect (drawsWhole (*label), ko (text) + where + " " + label->getBounds().toString());
                }

                // the channel menu under the name: on the mute group chip's row, at the name's right edge
                int cards = 0;
                for (auto* card : find<ChannelCard> (main))
                {
                    ++cards;
                    auto* menu = withTooltip<juce::Button> (*card, ko ("채널 메뉴"));
                    auto* chip = withTooltip<juce::Button> (*card, ko ("켜 두면 마이크 뮤트그룹"));
                    NameLabel* name = nullptr;
                    for (auto* child : card->getChildren())
                        if (auto* l = dynamic_cast<NameLabel*> (child)) name = l;
                    expect (menu != nullptr && chip != nullptr && name != nullptr, where);
                    if (menu == nullptr || chip == nullptr || name == nullptr)
                        continue;
                    expectEquals (menu->getY(), chip->getY(), where);
                    expectEquals (menu->getRight(), name->getRight(), where);
                    expect (menu->getX() > chip->getRight(), where);
                    expectGreaterOrEqual (name->getWidth(), 150, where);
                }
                expectEquals (cards, 2, where);
            }

            // what Vercel measured still cut at 1440 (10/30): a name without a space, a long one with a note, a mic with its
            // model - the wide layout's fader gives a long name room down to 84 px, a long card name gets smaller first
            const auto& s = f.document.getSession();
            f.document.renameFx (s.fx[0].id, ko ("보컬리버브"));
            f.document.renameFx (s.fx[1].id, ko ("룸 리버브 (짧게)"));
            f.document.renameChannel (s.channels[1].id, ko ("진행자 마이크 (SM7B)"));
            for (const int width : { 1440, 1280, 1920 })
            {
                main.setSize (width, 900);
                main.resized();
                const auto where = " @" + juce::String (width);
                for (const auto* text : { "보컬리버브", "룸 리버브 (짧게)", "진행자 마이크 (SM7B)" })
                {
                    const auto labels = labelsWithText (main, ko (text));
                    expect (! labels.empty(), ko (text) + where);
                    for (auto* label : labels)
                        expect (drawsWhole (*label), ko (text) + where + " " + label->getBounds().toString());
                }
                for (auto* label : labelsWithText (main, ko ("보컬리버브")))
                    expectEquals (drawnLines (*label), 1, ko ("보컬리버브") + where);   // one word: never broken inside
            }
        }

        beginTest ("the chain drawer at 1440 and 1280: long plugin names and a missing plugin's '(없음)' read in full");
        {
            Fixture f;
            f.nameEverything();
            auto session = f.document.getSession();
            const juce::StringArray plugins { "Waves Tune Real-Time Stereo", "NVIDIA Broadcast Noise Removal", "iZotope Neutron 4 Equalizer",
                                              "Waves CLA-76 Stereo", "FabFilter Pro-Q 3" };
            for (const auto& plugin : plugins)
            {
                PluginSlotState slot;
                slot.name = plugin;
                slot.fileOrIdentifier = f.folder.file.getChildFile ("missing.vst3").getFullPathName();   // nothing there: '(없음)'
                session.channels[0].chain.push_back (slot);
            }
            const auto file = f.folder.file.getChildFile ("long-chain.livemix");
            expect (file.replaceWithText (session.toJson()));
            expect (f.document.load (file).wasOk());

            gocue::livemix::MainComponent main (f.document, f.settings, f.actions);
            main.setSize (1440, 900);
            main.resized();
            auto cards = find<ChannelCard> (main);
            expect (! cards.empty() && cards.front()->onOpenChain != nullptr);
            if (cards.empty() || cards.front()->onOpenChain == nullptr)
                return;
            cards.front()->onOpenChain (cards.front()->getChannelId());

            for (const int width : { 1440, 1280 })
            {
                main.setSize (width, 900);
                main.resized();
                const auto where = " @" + juce::String (width);
                auto drawers = find<ChainDrawer> (main);
                expect (drawers.size() == 1 && drawers.front()->isVisible(), where);
                if (drawers.empty())
                    continue;

                for (const auto& plugin : plugins)
                {
                    const auto labels = labelsWithText (*drawers.front(), plugin + ko (" (없음)"));
                    expectEquals ((int) labels.size(), 1, plugin + where);
                    for (auto* label : labels)
                    {
                        expect (drawsWhole (*label), plugin + where + " " + label->getBounds().toString());
                        // the row takes the mouse, so the whole name is the row's tip (the name's own never showed)
                        auto* row = dynamic_cast<juce::SettableTooltipClient*> (label->getParentComponent());
                        expect (row != nullptr && row->getTooltip() == label->getText(), plugin + where);
                    }
                }
            }
        }

        beginTest ("the backup window's status line holds a long result on two lines");
        {
            Fixture f;
            gocue::livemix::MainComponent main (f.document, f.settings, f.actions);
            main.setSize (1440, 900);
            main.menuItemSelected (1, 1);   // 온라인 백업 창 열기...
            pump();

            juce::Label* status = nullptr;
            auto& desktop = juce::Desktop::getInstance();
            for (int i = 0; i < desktop.getNumComponents() && status == nullptr; ++i)
                if (auto* window = desktop.getComponent (i); window != nullptr && window->isVisible())
                    for (auto* label : labelsWithText (*window, {}))
                        if (label->getText().startsWith (ko ("이 프로그램에는 백업 서버")))
                            status = label;

            expect (status != nullptr, "the status line of a build without a backup server");
            if (status != nullptr && status->getParentComponent() != nullptr)
                expectGreaterOrEqual (status->getParentComponent()->getWidth(), 900);   // 820 cut a common '세션_PC'
            if (status != nullptr)
            {
                expectGreaterOrEqual (status->getHeight(), 40);
                // one and a half lines of the line's own width, whatever width the window opened at (Vercel: the longest
                // result, '세션 폴더로 옮기기 실패', is 1,247 px in an 820 px window's 770 px line)
                const auto lineWidth = status->getBorderSize().subtractedFrom (status->getLocalBounds()).getWidth();
                const auto words = juce::StringArray::fromTokens (ko ("세션 폴더로 옮기지 못했습니다. 받은 백업 파일은 그대로 남겨 두었습니다."), " ", {});
                juce::String text;
                for (int i = 0; juce::GlyphArrangement::getStringWidthInt (status->getFont(), text) < lineWidth * 3 / 2; ++i)
                    text << (text.isEmpty() ? "" : " ") << words[i % words.size()];
                status->setText (text, juce::dontSendNotification);
                expectEquals (drawnLines (*status), 2, juce::String (lineWidth) + " px line");
                expect (drawsWhole (*status));
            }

            BackupDialog::closeIfOpen();
            pump();
        }

        beginTest ("the backup list's file column drops LiveMix's own extension only");
        {
            expectEquals (BackupDialog::fileColumnText ("리붕후_DESKTOP-K3J2L9P_2026-10-30_165928.livemix"), juce::String ("리붕후_DESKTOP-K3J2L9P_2026-10-30_165928"));
            expectEquals (BackupDialog::fileColumnText (ko ("보컬 체인.livemixpreset")), ko ("보컬 체인"));
            expectEquals (BackupDialog::fileColumnText ("Show.LIVEMIX"), juce::String ("Show"));
            expectEquals (BackupDialog::fileColumnText (ko ("보컬 v1.2")), ko ("보컬 v1.2"));
            expectEquals (BackupDialog::fileColumnText ("a.livemix.bak"), juce::String ("a.livemix.bak"));
        }
    }

private:
    struct IsolatedFolder
    {
        juce::File file = juce::File::createTempFile ("-livemix-namefit");
        IsolatedFolder() { file.createDirectory(); }
        ~IsolatedFolder() { file.deleteRecursively(); }
    };

    /** A session as a broadcast names it, its own settings folder, no OBS install, no native window or device. */
    struct Fixture
    {
        IsolatedFolder folder;
        LiveMixSettings settings { folder.file };
        MixEngine engine { "Local\\LiveMix.NameFit." + juce::Uuid().toString() };
        MixDocument document { engine };
        ObsPluginActions actions;

        Fixture()
        {
            settings.setMicMuteHotkey ({});
            settings.setFxMuteHotkey ({});
            settings.setWindowHotkey ({});
            settings.setMicAllMuteHotkey ({});
            settings.setFxAllMuteHotkey ({});
            document.applyToEngine();
            actions.roots = [path = folder.file] { return ObsPluginInstaller::Roots { path, path, path, [] { return false; }, {} }; };
            actions.elevate = [] (juce::String&) { return ObsPluginInstaller::Result::needsElevation; };
        }

        void nameEverything()
        {
            while (document.getSession().channels.size() < 2)
                document.addChannel();
            while (document.getSession().fx.size() < 2)
                document.addFx();
            const auto& s = document.getSession();
            const auto mic1 = s.channels[0].id, mic2 = s.channels[1].id, reverb = s.fx[0].id, delay = s.fx[1].id;
            document.renameChannel (mic1, ko ("게스트 마이크 1"));
            document.renameChannel (mic2, ko ("게스트 마이크 2"));
            document.renameFx (reverb, ko ("보컬 리버브"));
            document.renameFx (delay, ko ("딜레이 1/4"));
            for (const auto& mic : { mic1, mic2 })
            {
                document.setSend (mic, reverb, 0.35, false);
                document.setSend (mic, delay, 0.2, false);
            }
        }
    };

    /** The glyphs the look-and-feel draws for the label (LookAndFeel_V2::drawLabel: as many lines as the height holds). */
    static juce::GlyphArrangement arrange (const juce::Label& label)
    {
        const auto area = label.getBorderSize().subtractedFrom (label.getLocalBounds()).toFloat();
        const auto font = label.getFont();
        juce::GlyphArrangement glyphs;
        glyphs.addFittedText (font, label.getText(), area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                              label.getJustificationType(), juce::jmax (1, (int) (area.getHeight() / font.getHeight())),
                              label.getMinimumHorizontalScale());
        return glyphs;
    }

    /** A two-line label drawing its own word lines (TwoLineLabel::getShownText differs from the text), or nullptr. */
    static const TwoLineLabel* wordLined (const juce::Label& label)
    {
        auto* two = dynamic_cast<const TwoLineLabel*> (&label);
        return two != nullptr && two->getShownText() != label.getText() ? two : nullptr;
    }

    /** Every letter of the text is drawn - nothing cut, no '...' - and a word line breaks only where a space was. */
    static bool drawsWhole (const juce::Label& label)
    {
        if (auto* two = wordLined (label))
        {
            const auto shown = two->getShownText();
            const auto area = label.getBorderSize().subtractedFrom (label.getLocalBounds());
            for (const auto& line : juce::StringArray::fromLines (shown))
                if (juce::GlyphArrangement::getStringWidthInt (label.getFont(), line) > area.getWidth())
                    return false;
            return shown.replace ("\n", " ") == label.getText();
        }

        auto glyphs = arrange (label);   // getGlyph() is not const
        juce::String drawn;
        for (int i = 0; i < glyphs.getNumGlyphs(); ++i)
            drawn += juce::String::charToString (glyphs.getGlyph (i).getCharacter());
        return drawn.removeCharacters (" ") == label.getText().removeCharacters (" ");
    }

    static int drawnLines (const juce::Label& label)
    {
        if (auto* two = wordLined (label))
            return juce::StringArray::fromLines (two->getShownText()).size();

        auto glyphs = arrange (label);   // getGlyph() is not const
        std::set<int> baselines;
        for (int i = 0; i < glyphs.getNumGlyphs(); ++i)
            baselines.insert (juce::roundToInt (glyphs.getGlyph (i).getBaselineY()));
        return (int) baselines.size();
    }

    template <typename T>
    static std::vector<T*> find (juce::Component& root)
    {
        // visible below the root (a test's main component has no window, so nothing in it 'isShowing')
        std::vector<T*> found;
        std::function<void (juce::Component&, bool)> walk = [&] (juce::Component& c, bool isRoot)
        {
            if (! isRoot && ! c.isVisible())
                return;
            if (auto* t = dynamic_cast<T*> (&c))
                found.push_back (t);
            for (auto* child : c.getChildren())
                walk (*child, false);
        };
        walk (root, true);
        return found;
    }

    /** Showing labels with this text (every label for an empty text). */
    static std::vector<juce::Label*> labelsWithText (juce::Component& root, const juce::String& text)
    {
        std::vector<juce::Label*> labels;
        for (auto* label : find<juce::Label> (root))
            if (text.isEmpty() || label->getText() == text)
                labels.push_back (label);
        return labels;
    }

    template <typename T>
    static T* withTooltip (juce::Component& root, const juce::String& start)
    {
        for (auto* c : find<T> (root))
            if (c->getTooltip().startsWith (start))
                return c;
        return nullptr;
    }

    static void pump()   // what a window opening posts (layout, async refreshes)
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + 300.0;
        do
        {
           #if JUCE_WINDOWS
            MSG message {};
            for (int n = 0; n < 2000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++n)
            {
                TranslateMessage (&message);
                DispatchMessageW (&message);
            }
           #endif
            juce::Timer::callPendingTimersSynchronously();
            juce::Thread::sleep (2);
        } while (juce::Time::getMillisecondCounterHiRes() < end);
    }
};

static LiveMixNameFitTests liveMixNameFitTests;
}
