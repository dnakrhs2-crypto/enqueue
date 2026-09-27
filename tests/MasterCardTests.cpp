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
