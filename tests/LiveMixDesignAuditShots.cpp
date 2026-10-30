// DESIGN AUDIT ONLY (2026-10-30, gom "라이브믹스 디자인 정밀검사"): every LiveMix screen as the app draws it, for the
// review. Opt-in (LIVEMIX_DESIGN_AUDIT_DIR); removed once the audit's fixes are in.
#include "ui/MasterCard.h"
#include "../livemix/src/ui/MainComponent.h"
#include "ui/LiveMixLookAndFeel.h"
#include "ui/TopBar.h"
#include "ui/ChannelCard.h"
#include "ui/ChainDrawer.h"
#include "ui/SettingsDialog.h"
#include "ui/BackupDialog.h"

#include <set>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
using namespace gocue::livemix;

class LiveMixDesignAuditShots : public juce::UnitTest
{
public:
    LiveMixDesignAuditShots() : juce::UnitTest ("LiveMix design audit shots", "LiveMix") {}

    void runTest() override
    {
        beginTest ("every screen (only with LIVEMIX_DESIGN_AUDIT_DIR)");
        const auto dir = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_DESIGN_AUDIT_DIR", {});

        if (dir.isEmpty())
        {
            expect (true);
            return;
        }

        LiveMixLookAndFeel lookAndFeel;
        struct RestoreLookAndFeel
        {
            juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
            ~RestoreLookAndFeel() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
        } restoreLookAndFeel;
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        struct IsolatedFolder
        {
            juce::File file = juce::File::createTempFile ("-livemix-audit");
            ~IsolatedFolder() { file.deleteRecursively(); }
        } folder;
        LiveMixSettings settings (folder.file);
        settings.setMicMuteHotkey ({});
        settings.setFxMuteHotkey ({});
        settings.setWindowHotkey ({});
        settings.setMicAllMuteHotkey ({});
        settings.setFxAllMuteHotkey ({});
        MixEngine engine ("Local\\LiveMix.DesignAudit." + juce::Uuid().toString());
        MixDocument document (engine);
        document.applyToEngine();

        // a broadcast-like session: a main mic and a guest mic with a long name, reverb + delay, sends set
        while (document.getSession().channels.size() < 2)
            document.addChannel();
        while (document.getSession().fx.size() < 2)
            document.addFx();
        const auto mic = document.getSession().channels[0].id;
        const auto guest = document.getSession().channels[1].id;
        const auto reverb = document.getSession().fx[0].id;
        const auto delay = document.getSession().fx[1].id;
        document.renameChannel (mic, ko ("메인 마이크"));
        document.renameChannel (guest, ko ("게스트 마이크 - 무선 핸드헬드 (왼쪽 스탠드)"));
        document.renameFx (reverb, ko ("리버브"));
        document.renameFx (delay, ko ("딜레이"));
        document.setSend (mic, reverb, 0.35, false);
        document.setSend (mic, delay, 0.2, false);
        document.setSend (guest, reverb, 0.25, false);
        document.setChannelMuteGroup (guest, true);
        document.setFxMuteGroup (reverb, true);

        ObsPluginActions actions;
        actions.roots = [path = folder.file] { return ObsPluginInstaller::Roots { path, path, path, [] { return false; }, {} }; };
        actions.elevate = [] (juce::String&) { return ObsPluginInstaller::Result::needsElevation; };
        gocue::livemix::MainComponent main (document, settings, actions);   // no native window or audio device

        TopBar* top = nullptr;
        juce::Viewport* viewport = nullptr;
        for (auto* child : main.getChildren())
        {
            if (auto* t = dynamic_cast<TopBar*> (child)) top = t;
            if (auto* v = dynamic_cast<juce::Viewport*> (child); v != nullptr && v->isVisible()) viewport = v;
        }
        expect (top != nullptr && viewport != nullptr);
        if (top == nullptr || viewport == nullptr) return;

        const auto status = [top]
        {
            MixEngine::DeviceFormat format;
            format.kind = MixEngine::DeviceFormat::Kind::asio;
            format.inputBits = 24;
            top->setDevices ({ "Yamaha Steinberg USB ASIO" }, "Yamaha Steinberg USB ASIO", "ASIO");
            top->setStatus (48000.0, 128, 5.3, 0.04, true, format);
        };

        const auto shootMain = [&] (int width, int height, const juce::String& name)
        {
            main.setSize (width, height);
            status();
            main.resized();
            save (main.createComponentSnapshot (main.getLocalBounds()), dir, name);
        };

        shootMain (1440, 900, "01_main_1440");
        shootMain (1920, 1040, "02_main_1920");
        shootMain (1280, 800, "03_main_1280");
        shootMain (1000, 800, "04_main_1000_two_rows");
        shootMain (700, 900, "05_main_700");
        shootMain (460, 900, "06_main_460_three_rows");

        main.getMuteGroups().setAll (MuteGroups::Group::mic, true);
        main.getMuteGroups().setAll (MuteGroups::Group::fx, true);
        shootMain (1440, 900, "07_main_all_muted");
        document.setChannelOn (guest, false);   // a switch flipped while muted shows OFF on its lamp
        shootMain (1440, 900, "07b_main_all_muted_guest_off");
        document.setChannelOn (guest, true);
        main.getMuteGroups().reset();
        main.getMuteGroups().set (MuteGroups::Group::mic, true);
        shootMain (1440, 900, "08_main_mic_group_muted");
        main.getMuteGroups().reset();

        if (auto* fxButton = findButton (*top, ko ("FX 채널")))
        {
            fxButton->onClick();
            shootMain (1440, 900, "09_fx_drawer_1440");
            shootMain (1000, 800, "10_fx_drawer_1000");
            fxButton->onClick();
        }

        ChannelCard* card = nullptr;
        for (auto* child : viewport->getViewedComponent()->getChildren())
            if (auto* c = dynamic_cast<ChannelCard*> (child); c != nullptr && card == nullptr) card = c;
        if (card != nullptr && card->onOpenChain)
        {
            card->onOpenChain (card->getChannelId());
            shootMain (1440, 900, "11_chain_drawer_1440");
        }

        main.setSize (1440, 900);

        // the windows the menus open, as the app opens them
        const auto shootNewWindows = [&] (const juce::String& prefix)
        {
            auto& desktop = juce::Desktop::getInstance();
            int n = 0;
            for (int i = 0; i < desktop.getNumComponents(); ++i)
                if (auto* w = desktop.getComponent (i); w != nullptr && w->isVisible() && seen.count (w) == 0)
                {
                    seen.insert (w);
                    save (w->createComponentSnapshot (w->getLocalBounds()), dir, prefix + "_" + juce::String (++n));
                    if (auto* dialog = dynamic_cast<juce::DialogWindow*> (w))
                        if (auto* view = dynamic_cast<juce::Viewport*> (dialog->getContentComponent()))
                            if (auto* content = view->getViewedComponent())
                                save (content->createComponentSnapshot (content->getLocalBounds()), dir, prefix + "_full_content");
                }
        };
        {
            auto& desktop = juce::Desktop::getInstance();
            for (int i = 0; i < desktop.getNumComponents(); ++i)
                seen.insert (desktop.getComponent (i));
        }

        main.menuItemSelected (1, 2);   // 설정...
        pump();
        shootNewWindows ("20_settings");
        main.menuItemSelected (2, 2);   // 플러그인 관리...
        pump();
        shootNewWindows ("21_plugin_manager");
        main.menuItemSelected (3, 2);   // LUFS 미터...
        pump();
        shootNewWindows ("22_lufs");
        main.menuItemSelected (1, 1);   // 온라인 백업 창 열기...
        pump();
        shootNewWindows ("23_backup");

        for (auto* child : main.getChildren())
            if (auto* master = dynamic_cast<MasterCard*> (child))
                if (auto* record = findButton (*master, ko ("녹음")))
                {
                    record->onClick();
                    pump();
                    shootNewWindows ("24_recording");
                }

        // 10/30 (gom "알아서해"): the names a broadcast uses - two guests, a vocal reverb, a quarter delay - and a chain of
        // long plugin names not installed here (each opens as '(없음)')
        for (auto* child : main.getChildren())   // the plain cards first: the chain drawer of shot 11 is still open
            if (auto* drawer = dynamic_cast<ChainDrawer*> (child); drawer != nullptr && drawer->isVisible() && drawer->onClose)
                drawer->onClose();
        document.renameChannel (mic, ko ("게스트 마이크 1"));
        document.renameChannel (guest, ko ("게스트 마이크 2"));
        document.renameFx (reverb, ko ("보컬 리버브"));
        document.renameFx (delay, ko ("딜레이 1/4"));
        shootMain (1440, 900, "30_names_1440");
        shootMain (1280, 800, "31_names_1280");
        shootMain (1920, 1040, "32_names_1920");
        {
            auto session = document.getSession();
            for (const auto* pluginName : { "Waves Tune Real-Time Stereo", "NVIDIA Broadcast Noise Removal", "iZotope Neutron 4 Equalizer",
                                            "Waves CLA-76 Stereo", "FabFilter Pro-Q 3" })
            {
                PluginSlotState slot;
                slot.name = pluginName;
                slot.fileOrIdentifier = folder.file.getChildFile ("missing.vst3").getFullPathName();   // nothing there
                session.channels[0].chain.push_back (slot);
            }
            const auto file = folder.file.getChildFile ("long-chain.livemix");
            expect (file.replaceWithText (session.toJson()));
            expect (document.load (file).wasOk());
            pump();
            ChannelCard* first = nullptr;
            for (auto* child : viewport->getViewedComponent()->getChildren())
                if (auto* c = dynamic_cast<ChannelCard*> (child); c != nullptr && first == nullptr) first = c;
            if (first != nullptr && first->onOpenChain)
            {
                first->onOpenChain (first->getChannelId());
                shootMain (1440, 900, "33_chain_long_1440");
                shootMain (1280, 800, "34_chain_long_1280");
            }
        }

        // close what was opened (a dialog left on the desktop would outlive the main component)
        SettingsDialog::closeIfOpen();
        BackupDialog::closeIfOpen();
        pump();
    }

private:
    std::set<juce::Component*> seen;

    static void pump()   // what a window opening posts (layout, async refreshes), as AuditFix0923 tests pump
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

    static juce::Button* findButton (juce::Component& root, const juce::String& textStart)
    {
        if (auto* b = dynamic_cast<juce::Button*> (&root); b != nullptr && b->getButtonText().startsWith (textStart))
            return b;
        for (auto* child : root.getChildren())
            if (auto* found = findButton (*child, textStart))
                return found;
        return nullptr;
    }

    void save (const juce::Image& image, const juce::String& dir, const juce::String& name)
    {
        const auto file = juce::File (dir).getChildFile (name + ".png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        expect (out.openedOk(), file.getFullPathName());
        expect (juce::PNGImageFormat().writeImageToStream (image, out), file.getFullPathName());
    }
};

static LiveMixDesignAuditShots liveMixDesignAuditShots;
}
