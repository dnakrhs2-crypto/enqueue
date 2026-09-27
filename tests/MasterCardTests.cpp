#include "ui/MasterCard.h"
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
                for (int width : { 364, 400, 500, 580, 640, 700, 987, 988, 1000, 1400 })
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
                            const int needed = labelWidthForText (*hint, hint->getText()) + label->getWidth() + 16;
                            for (auto* child : card.getChildren())
                                if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child))
                                {
                                    if (toggle->getWidth() + needed <= width - 28)
                                        expectEquals (hint->getY(), toggle->getY());
                                    else
                                        expect (hint->getY() >= juce::jmax (toggle->getBottom(), label->getBottom()));
                                }
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
        beginTest ("OBS controls and full meter fit every form and strip visibility threshold");
        for (int plugins : { 0, 7 })
        {
            for (int i = 0; i < plugins; ++i)
                engine.getMasterChain().addPlugin (std::make_unique<TestGainPlugin> (1.0f));
            card.refresh();
            for (bool folded : { false, true })
            {
                card.setStrip (folded);
                for (int width : { 364, 400, 500, 580, 640, 700, 987, 988, 1000, 1400 })
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
                            expect (card.getLocalBounds().contains (child->getBounds()), "Control outside master at " + juce::String (width));
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
                        juce::FileOutputStream output (directory.getChildFile ("master-" + juce::String (width) + "-" + juce::String (plugins) + (folded ? "-strip.png" : ".png")));
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
    }
};
static MasterCardTests masterCardTests;
}
