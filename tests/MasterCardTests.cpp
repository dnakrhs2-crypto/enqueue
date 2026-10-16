#include "ui/MasterCard.h"
#include "../livemix/src/ui/MainComponent.h"
#include "ui/LiveMixLookAndFeel.h"
#include "TestGainPlugin.h"

namespace gocue::tests
{
using namespace gocue::livemix;

class MasterCardTests : public juce::UnitTest
{
public:
    MasterCardTests() : juce::UnitTest ("LiveMix master OBS UI", "LiveMix") {}
    void runTest() override
    {
        LiveMixLookAndFeel lookAndFeel;
        struct RestoreLookAndFeel
        {
            juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
            ~RestoreLookAndFeel() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
        } restoreLookAndFeel;
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);   // match LiveMix startup for text measurements as well as painting
        MixEngine engine ("Local\\LiveMix.MasterObsTest." + juce::Uuid().toString());
        MixDocument document (engine);
        document.applyToEngine();
        MasterCard card (document);
        card.setLookAndFeel (&lookAndFeel);
        document.onValueChanged = [&] { card.refresh(); };
        beginTest ("OBS advice explains every waiting reason in priority order");
        using State = MasterCard::ObsStatus;
        using Reader = ObsSender::ReaderState;
        using Info = ObsPluginInstaller::RunningObs;
        const Info old { {}, (juce::uint64 (31) << 48) | (juce::uint64 (4) << 16), false };
        const Info portable { {}, (juce::uint64 (32) << 48) | (juce::uint64 (1) << 32), true };
        const Info installed { {}, portable.version, false };
        auto checkAdvice = [&] (const juce::String& error, bool current, bool restart, bool running, Reader reader,
                               const std::vector<Info>& processes, State expected, const char* text, const char* reason)
        {
            const auto advice = MasterCard::obsStatusFor (error, current, restart, running, reader, processes);
            expect (advice.status == expected);
            expectEquals (advice.text, ko (text));
            if (reason != nullptr) expect (advice.reason.contains (ko (reason)), advice.reason);
            card.setObsStatus (advice.status, advice.reason);
            for (bool folded : { false, true })
                for (int width : { 364, 388, 399, 400, 428, 500, 580, 640, 700, 759, 760, 927, 928, 960, 987, 988, 1000, 1384, 1385, 1400, 1920 })
                {
                    card.setStrip (folded);
                    card.setSize (width, card.getPreferredHeight (width));
                    auto* label = dynamic_cast<juce::Label*> (card.findChildWithID ("obs-status"));
                    expect (label != nullptr);
                    if (label == nullptr) continue;
                    expectEquals (label->getText(), folded ? ko ("●") : advice.text);
                    expectGreaterOrEqual (label->getWidth(), labelWidthForText (*label, label->getText()));
                    const auto colour = expected == State::sendFailed || expected == State::audioStopped ? livemix::Palette::danger
                        : expected == State::connected ? livemix::Palette::lampOn
                        : expected == State::waiting ? livemix::Palette::dimText : juce::Colour (0xffffb454);
                    expect (label->findColour (juce::Label::textColourId) == colour);
                    expect (label->getTooltip().contains (advice.text) && label->getTooltip().contains (advice.reason));
                    expect (! label->getTooltip().contains (ko ("필터")));
                    auto* hint = dynamic_cast<juce::Label*> (card.findChildWithID ("obs-source-hint"));
                    expect (hint != nullptr, "The full card always explains how to add the OBS source");
                    if (hint != nullptr)
                    {
                        expectEquals (hint->getText(), ko ("OBS에서 반드시 소스 추가(+)로 'LiveMix'를 추가해 주세요"));
                        expect (hint->isVisible() == ! folded);
                        expect (hint->findColour (juce::Label::textColourId) == livemix::Palette::dimText);
                        if (! folded)
                        {
                            juce::AttributedString hintText;
                            hintText.append (hint->getText(), hint->getFont());
                            juce::TextLayout layout;
                            layout.createLayout (hintText, (float) hint->getWidth() - hint->getBorderSize().getLeftAndRight());
                            expectGreaterOrEqual ((float) hint->getHeight() - hint->getBorderSize().getTopAndBottom(), layout.getHeight());
                            expectEquals (hint->getMinimumHorizontalScale(), 1.0f);
                            for (auto* child : card.getChildren())
                                if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child))
                                    expect (hint->getY() >= juce::jmax (toggle->getBottom(), label->getBottom()));
                            for (auto* child : card.getChildren())
                                if (child != hint && child->isVisible()) expect (! hint->getBounds().intersects (child->getBounds()));
                        }
                    }
                    for (auto* child : card.getChildren())
                        if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child))
                            expectEquals (toggle->getTooltip(), ko ("OBS에서 반드시 소스 추가(+)로 'LiveMix'를 추가해 주세요"));
                    for (auto* child : card.getChildren())
                    {
                        if (! child->isVisible() || child->getBounds().isEmpty()) continue;
                        expect (card.getLocalBounds().contains (child->getBounds()));
                        if (child != label) expect (! child->getBounds().intersects (label->getBounds()));
                    }
                    const auto folder = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_UI_SCREENSHOT_DIR", {});
                    if (folder.isNotEmpty())
                    {
                        const auto directory = juce::File (folder).getChildFile ("obs-status");
                        expect (directory.createDirectory().wasOk());
                        juce::FileOutputStream output (directory.getChildFile ("master-" + juce::String (width) + "-"
                            + juce::String ((int) expected) + (folded ? "-strip.png" : ".png")));
                        expect (output.openedOk());
                        if (output.openedOk())
                        {
                            expect (output.setPosition (0));
                            expect (output.truncate().wasOk());
                            expect (juce::PNGImageFormat().writeImageToStream (card.createComponentSnapshot (card.getLocalBounds()), output));
                        }
                    }
                }
        };
        checkAdvice ("Win32 5", false, true, false, Reader::connected, { old, portable }, State::sendFailed, "OBS 보내기 실패", "Win32 5");
        checkAdvice ({}, false, true, false, Reader::connected, { old }, State::installNeeded, "OBS 플러그인 설치 필요", nullptr);
        checkAdvice ({}, true, true, false, Reader::connected, { old, portable }, State::connected, "OBS 연결됨", nullptr);
        checkAdvice ({}, true, true, false, Reader::idle, { old }, State::restartObs, "OBS를 다시 시작하세요", nullptr);
        checkAdvice ({}, true, false, false, Reader::idle, { old }, State::audioStopped, "오디오 멈춤", nullptr);
        checkAdvice ({}, true, false, true, Reader::idle, { old, portable }, State::addSource, "OBS에 소스 추가 필요",
                     "OBS에 LiveMix 플러그인은 올라와 있지만 소리를 받는 곳이 없습니다. OBS 소스(+)에서 'LiveMix'를 추가하세요.");
        checkAdvice ({}, true, false, true, Reader::none, { portable, old }, State::updateObs, "OBS 31.1 이상 필요", "31.0.4");
        checkAdvice ({}, true, false, true, Reader::none, { installed, portable }, State::portableObs, "휴대용 OBS: 플러그인 복사 필요", "obs-plugins\\64bit\\");
        const auto copy = MasterCard::obsStatusFor ({}, true, false, true, Reader::none, { portable }).reason;
        expect (copy.contains ("obs-plugin\\livemix-obs\\livemix-obs.dll") && copy.contains ("obs-plugin\\livemix-obs\\data\\locale\\")
                && copy.contains ("data\\obs-plugins\\livemix-obs\\locale\\") && copy.contains (ko ("다시 시작")));
        checkAdvice ({}, true, false, true, Reader::none, { installed }, State::restartObs, "OBS를 다시 시작하세요",
                     "켜져 있는 OBS가 LiveMix 플러그인을 읽지 않았습니다. OBS를 완전히 끄고 다시 켜세요.");
        checkAdvice ({}, true, false, true, Reader::none, {}, State::waiting, "OBS 대기 중", "OBS를 켜고 소스(+)에서 'LiveMix'를 추가하세요.");
        card.setStrip (false);
        beginTest ("l06 master columns, two tiers and narrow stack match the mockup rectangles");
        engine.getMasterChain().addPlugin (std::make_unique<TestGainPlugin> (1.0f));
        card.refresh();
        card.setLatency (53.0, 480, 48000.0);
        const auto withText = [&] (const juce::String& text) -> juce::Component*
        {
            for (auto* child : card.getChildren())
            {
                if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getText() == text) return child;
                if (auto* button = dynamic_cast<juce::Button*> (child); button != nullptr && button->getButtonText() == text) return child;
            }
            return nullptr;
        };
        const auto rect = [&] (juce::Component* component, juce::Rectangle<int> expected)
        {
            expect (component != nullptr);
            if (component != nullptr) expectEquals (component->getBounds().toString(), expected.toString());
        };
        for (int width : { 1400, 960, 428 })
        {
            const bool wide = width == 1400, narrow = width == 428;
            expectEquals (card.getPreferredHeight (width), wide ? 192 : narrow ? 310 : 202);
            card.setSize (width, card.getPreferredHeight (width));
            rect (withText ("M"), { 14, 14, 32, 30 });
            rect (withText (ko ("마스터")), { 56, 12, narrow ? 160 : 152, 34 });
            rect (withText (ko ("VST3 체인")), { narrow ? 14 : 404, narrow ? 55 : 12, narrow ? 70 : 361, narrow ? 30 : 26 });
            rect (withText ("1  TestGain"), { narrow ? 92 : 404, narrow ? 54 : 39, 110, 32 });
            rect (withText (ko ("체인 열기")), { narrow ? 210 : 522, narrow ? 55 : 40, 92, 30 });
            rect (withText (ko ("+ 추가")), { narrow ? 310 : 622, narrow ? 55 : 40, 76, 30 });
            rect (withText ("LUFS"), { wide ? 895 : narrow ? 156 : 890, narrow ? 94 : 40, 56, 30 });
            if (wide)
            {
                rect (withText (ko ("지연")), { 783, 12, 168, 26 });
                rect (withText ("53.0 ms"), { 783, 38, 104, 34 });
                rect (withText (ko ("480 샘플") + " · 48.0 kHz"), { 783, 72, 168, 20 });
            }
            else
                rect (withText (ko ("지연 53.0 ms · 480 샘플")), { narrow ? 14 : 748, narrow ? 94 : 40, 134, 30 });
            rect (withText (ko ("메인 출력")), { wide ? 1136 : 14, wide ? 12 : narrow ? 132 : 88, 60, wide ? 26 : 30 });
            rect (withText (ko ("출력 미터 L / R")), { wide ? 1136 : 14, wide ? 46 : narrow ? 170 : 126, wide ? 250 : narrow ? 400 : 372, 18 });
            rect (withText (ko ("OBS로 보내기")), { wide ? 1136 : narrow ? 14 : 404, wide ? 118 : narrow ? 242 : 89, 117, 28 });
            rect (card.findChildWithID ("obs-status"), { wide ? 1261 : narrow ? 139 : 529, wide ? 118 : narrow ? 242 : 89, 78, 28 });
            // Preserve obsHintHeight's native font measurement: 37 px fits with a 7 px bottom inset.
            rect (card.findChildWithID ("obs-source-hint"), { wide ? 1136 : narrow ? 14 : 404, wide ? 148 : narrow ? 270 : 121, wide ? 250 : narrow ? 400 : 542, wide ? 37 : 28 });
            for (auto* child : card.getChildren())
            {
                if (dynamic_cast<juce::ComboBox*> (child) != nullptr)
                    rect (child, wide ? juce::Rectangle<int> { 1202, 10, 184, 30 }
                                     : narrow ? juce::Rectangle<int> { 80, 132, 334, 30 } : juce::Rectangle<int> { 80, 88, 306, 30 });
                if (dynamic_cast<MeterBar*> (child) != nullptr)
                    rect (child, wide ? juce::Rectangle<int> { 1136, 64, 250, 46 }
                                     : narrow ? juce::Rectangle<int> { 14, 188, 400, 46 } : juce::Rectangle<int> { 14, 144, 372, 46 });
            }
        }
        engine.getMasterChain().clear();
        card.refresh();
        beginTest ("OBS controls and full meter fit every form and strip visibility threshold");
        struct LongPlugin : TestGainPlugin
        {
            LongPlugin() : TestGainPlugin (1.0f) {}
            const juce::String getName() const override { return "A long plugin name that fills a complete chain column"; }
        };
        for (int pluginCase : { 0, 1, 2, 3, 7, -1, -7 })
        {
            engine.getMasterChain().clear();
            const int plugins = std::abs (pluginCase);
            for (int i = 0; i < plugins; ++i)
                if (pluginCase < 0) engine.getMasterChain().addPlugin (std::make_unique<LongPlugin>());
                else engine.getMasterChain().addPlugin (std::make_unique<TestGainPlugin> (1.0f));
            card.refresh();
            for (bool folded : { false, true })
            {
                card.setStrip (folded);
                for (int width : { 364, 388, 399, 400, 428, 500, 580, 640, 700, 759, 760, 927, 928, 960, 987, 988, 1000, 1384, 1385, 1400, 1920 })
                {
                    card.setSize (width, card.getPreferredHeight (width));
                    juce::ToggleButton* toggle = nullptr;
                    MeterBar* meter = nullptr;
                    juce::Label* status = nullptr;
                    for (auto* child : card.getChildren())
                    {
                        if (auto* t = dynamic_cast<juce::ToggleButton*> (child)) toggle = t;
                        if (auto* m = dynamic_cast<MeterBar*> (child)) meter = m;
                        if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getTooltip() == ko ("OBS 연결됨")) status = label;
                        if (child->isVisible())
                        {
                            expect (card.getLocalBounds().contains (child->getBounds()), "Control outside master at " + juce::String (width));
                            for (auto* other : card.getChildren())
                                if (other != child && other->isVisible())
                                    expect (! child->getBounds().intersects (other->getBounds()), "Master overlap at " + juce::String (width));
                        }
                    }
                    expect (toggle != nullptr && meter != nullptr);
                    if (toggle != nullptr && meter != nullptr)
                    {
                        expect (toggle->isVisible());
                        expectEquals (toggle->getButtonText(), folded ? juce::String ("OBS") : ko ("OBS로 보내기"));
                        expect (! toggle->getBounds().intersects (meter->getBounds()));
                        expectGreaterThan (meter->getWidth(), 25);
                        expectEquals (meter->getHeight(), folded ? 28 : 46);
                        card.setObsStatus (MasterCard::ObsStatus::connected);
                        for (auto* child : card.getChildren())
                            if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getTooltip() == ko ("OBS 연결됨")) status = label;
                        expect (status != nullptr && status->isVisible());
                        if (status != nullptr)
                        {
                            expect (! toggle->getBounds().intersects (status->getBounds()));
                            expectEquals (status->findColour (juce::Label::textColourId).getARGB(), livemix::Palette::lampOn.getARGB());
                        }
                    }
                    const auto folder = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_UI_SCREENSHOT_DIR", {});
                    if (folder.isNotEmpty() && (width == 364 || width == 988 || width == 1400))
                    {
                        const juce::File directory (folder);
                        expect (directory.createDirectory().wasOk());
                        juce::FileOutputStream output (directory.getChildFile ("master-" + juce::String (width) + "-" + juce::String (pluginCase) + (folded ? "-strip.png" : ".png")));
                        if (output.openedOk())
                        {
                            expect (output.setPosition (0));
                            expect (output.truncate().wasOk());
                            expect (juce::PNGImageFormat().writeImageToStream (card.createComponentSnapshot (card.getLocalBounds()), output));
                        }
                    }
                }
            }
        }

        beginTest ("Windows master output is limited to the monitor pair or OBS only; toggle edits the document");
        document.getSession().device = { "Windows Audio", "Test input", "", 480, 48000.0 };
        card.setDeviceChannels ({});
        juce::ComboBox* output = nullptr;
        juce::ToggleButton* toggle = nullptr;
        for (auto* child : card.getChildren())
        {
            if (auto* c = dynamic_cast<juce::ComboBox*> (child)) output = c;
            if (auto* t = dynamic_cast<juce::ToggleButton*> (child)) toggle = t;
        }
        expect (output != nullptr && toggle != nullptr);
        if (output != nullptr && toggle != nullptr)
        {
            expectEquals (output->getNumItems(), 1);
            expectEquals (output->getText(), ko ("없음 (OBS로만)"));
            document.getSession().device.output = "Test output";
            card.setDeviceChannels ({ "1", "2", "3", "4" });
            expectEquals (output->getNumItems(), 1);
            expectEquals (output->getText(), juce::String ("1-2"));
            toggle->setToggleState (true, juce::dontSendNotification);
            toggle->onClick();
            expect (document.getSession().master.sendToObs && engine.getObsSender().isEnabled());
            card.setObsStatus (MasterCard::ObsStatus::audioStopped);
            bool stopped = false;
            for (auto* child : card.getChildren())
                if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getTooltip() == ko ("오디오 멈춤"))
                    stopped = label->findColour (juce::Label::textColourId) == livemix::Palette::danger;
            expect (stopped);
        }
        beginTest ("Install and restart states are orange; installing fits every master form and still allows sending");
        for (auto state : { MasterCard::ObsStatus::installNeeded, MasterCard::ObsStatus::restartObs })
        {
            const auto text = state == MasterCard::ObsStatus::installNeeded ? ko ("OBS 플러그인 설치 필요") : ko ("OBS를 다시 시작하세요");
            card.setObsStatus (state);
            for (bool folded : { false, true })
            {
                card.setStrip (folded);
                for (bool installing : { false, true })
                {
                    card.setObsInstalling (installing);
                    for (int width : { 364, 988, 1400 })
                    {
                        card.setSize (width, card.getPreferredHeight (width));
                        bool found = false;
                        for (auto* child : card.getChildren())
                        {
                            if (child->isVisible()) expect (card.getLocalBounds().contains (child->getBounds()));
                            if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getTooltip() == text)
                            {
                                found = true;
                                expect (label->findColour (juce::Label::textColourId) == juce::Colour (0xffffb454));
                            }
                        }
                        expect (found);
                        expect (toggle != nullptr && toggle->isEnabled());
                        if (toggle != nullptr && installing) expectEquals (toggle->getButtonText(), ko ("설치 중..."));
                    }
                }
            }
        }
        int enabledCallbacks = 0;
        card.onObsEnabled = [&]
        {
            expect (document.getSession().master.sendToObs && engine.getObsSender().isEnabled());
            ++enabledCallbacks;
        };
        if (toggle != nullptr)
        {
            toggle->setToggleState (false, juce::dontSendNotification);
            toggle->onClick();
            expectEquals (enabledCallbacks, 0);
            toggle->setToggleState (true, juce::dontSendNotification);
            toggle->onClick();
            expectEquals (enabledCallbacks, 1);
        }
        card.setLookAndFeel (nullptr);
        runLayoutIntegration();
    }

    void runLayoutIntegration()
    {
        beginTest ("l06 window geometry, scrollbar auto-hide, both drawers, strip folding and UI scales");
        struct IsolatedFolder
        {
            juce::File file = juce::File::createTempFile ("-livemix-layout");
            ~IsolatedFolder() { file.deleteRecursively(); }
        } folder;
        LiveMixSettings settings (folder.file);
        settings.setMicMuteHotkey ({});
        settings.setFxMuteHotkey ({});
        settings.setWindowHotkey ({});
        MixEngine engine ("Local\\LiveMix.LayoutTest." + juce::Uuid().toString());
        MixDocument document (engine);
        document.applyToEngine();
        document.addChannel();
        document.addChannel();
        document.addFx();
        engine.getMasterChain().addPlugin (std::make_unique<TestGainPlugin> (1.0f));
        ObsPluginActions actions;
        actions.roots = [path = folder.file] { return ObsPluginInstaller::Roots { path, path, path, [] { return false; }, {} }; };
        actions.elevate = [] (juce::String&) { return ObsPluginInstaller::Result::needsElevation; };
        gocue::livemix::MainComponent main (document, settings, actions);   // no native window or audio device is opened
        juce::Viewport* viewport = nullptr;
        MasterCard* master = nullptr;
        TopBar* top = nullptr;
        ChainDrawer* drawer = nullptr;
        for (auto* child : main.getChildren())
        {
            if (auto* v = dynamic_cast<juce::Viewport*> (child); v != nullptr && v->isVisible()) viewport = v;
            if (auto* m = dynamic_cast<MasterCard*> (child)) master = m;
            if (auto* t = dynamic_cast<TopBar*> (child)) top = t;
            if (auto* d = dynamic_cast<ChainDrawer*> (child)) drawer = d;
        }
        expect (viewport != nullptr && master != nullptr && top != nullptr && drawer != nullptr);
        if (viewport == nullptr || master == nullptr || top == nullptr || drawer == nullptr) return;
        ChannelCard* first = nullptr;
        juce::Component* add = nullptr;
        for (auto* child : viewport->getViewedComponent()->getChildren())
        {
            if (auto* c = dynamic_cast<ChannelCard*> (child); c != nullptr && first == nullptr) first = c;
            if (dynamic_cast<juce::TextButton*> (child) != nullptr) add = child;
        }
        expect (first != nullptr && add != nullptr);
        if (first == nullptr || add == nullptr) return;
        const auto rect = [&] (juce::Component& component, juce::Rectangle<int> expected)
        {
            expectEquals (component.getBounds().toString(), expected.toString());
        };
        for (float scale : { 1.0f, 1.1f, 1.25f, 1.5f })
        {
            main.setTransform (juce::AffineTransform::scale (scale));
            main.setSize (1440, 900);
            main.resized();
            rect (*master, { 16, 670, 1400, 192 });
            rect (*viewport, { 16, 106, 1408, 544 });
            rect (*first, { 0, 0, 1400, 156 });
            rect (*add, { 0, 504, 1400, 56 });
            expectEquals (viewport->getViewedComponent()->getHeight(), 560);
            expect (viewport->getVerticalScrollBar().isVisible());
            main.setSize (1440, 1000);
            expect (! viewport->getVerticalScrollBar().isVisible());
            expectEquals (first->getWidth(), 1400);
            main.setSize (1440, 900);
            for (bool fx : { false, true })
            {
                if (fx) top->onFxPanel();
                else first->onOpenChain (first->getChannelId());
                rect (*master, { 16, 660, 960, 202 });
                rect (*viewport, { 16, 106, 968, 534 });
                rect (*first, { 0, 0, 960, 272 });
                rect (*add, { 0, 852, 960, 56 });
                expectEquals (viewport->getViewedComponent()->getHeight(), 908);
                expect (viewport->getVerticalScrollBar().isVisible());
                if (fx) top->onFxPanel();
                else drawer->onClose();
                expectEquals (first->getWidth(), 1400);
                expect (viewport->getVerticalScrollBar().isVisible());
            }
            main.setSize (460, 993);
            rect (*master, { 16, 645, 428, 310 });
            rect (*viewport, { 16, 190, 428, 435 });
            rect (*first, { 0, 0, 420, 618 });
            rect (*add, { 0, 1890, 420, 56 });
            expectEquals (viewport->getViewedComponent()->getHeight(), 1946);
            expect (viewport->getVerticalScrollBar().isVisible());
            first->onOpenChain (first->getChannelId());
            expect (main.getLocalBounds().contains (master->getBounds()));
            expect (main.getLocalBounds().contains (viewport->getBounds()));
            expectEquals (drawer->getWidth(), 460);
            drawer->onClose();
            main.setSize (420, 993);
            rect (*master, { 16, 607, 388, 348 });
            rect (*first, { 0, 0, 380, 618 });
            expect (! master->isStrip());
            main.setSize (460, 650);
            expect (master->isStrip());
            expectEquals (master->getHeight(), MasterCard::stripHeight);
            expectGreaterOrEqual (viewport->getHeight(), 250);
            main.setSize (1440, 900);
            expect (! master->isStrip());
        }
        main.setTransform ({});
        beginTest ("l03 top bar uses 24 px between groups and a 330 px device at the mockup status");
        MixEngine::DeviceFormat format;
        format.kind = MixEngine::DeviceFormat::Kind::windowsShared;
        format.inputBits = 16;
        top->setDevices ({ "HDMI(StreamLine Mini+ GC311G2)" }, "HDMI(StreamLine Mini+ GC311G2)", "Windows Audio");
        top->setStatus (48000.0, 480, 53.0, 0.03, true, format);
        for (auto* child : top->getChildren())
        {
            if (dynamic_cast<juce::ComboBox*> (child) != nullptr) rect (*child, { 460, 15, 330, 34 });
            if (auto* label = dynamic_cast<juce::Label*> (child))
            {
                if (label->getText() == ko ("윈도우")) rect (*child, { 404, 15, 48, 34 });
                if (label->getText() == "CPU 3%") rect (*child, { 1037, 15, 57, 34 });
                if (label->getText() == ko ("저장 안 됨")) rect (*child, { 312, 15, 68, 34 });
            }
        }
    }
};
static MasterCardTests masterCardTests;
}
