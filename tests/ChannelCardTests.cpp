#include "MixDocument.h"
#include "ui/ChannelCard.h"
#include "ui/LiveMixLookAndFeel.h"

namespace gocue::tests
{

using namespace gocue::livemix;

class ChannelCardTests : public juce::UnitTest
{
public:
    ChannelCardTests() : juce::UnitTest ("LiveMix channel pan UI", "LiveMix") {}

    template <typename T>
    static T* childOfType (juce::Component& parent)
    {
        for (auto* child : parent.getChildren())
            if (auto* found = dynamic_cast<T*> (child))
                return found;
        return nullptr;
    }

    static juce::Component* withText (juce::Component& parent, const juce::String& text)
    {
        for (auto* child : parent.getChildren())
        {
            if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getText() == text)
                return child;
            if (auto* button = dynamic_cast<juce::Button*> (child); button != nullptr && button->getButtonText() == text)
                return child;
        }
        return nullptr;
    }

    void screenshot (ChannelCard& card, const juce::String& imageName)
    {
        // Opt-in review artifacts: render the real JUCE card without opening an audio device or application window.
        const auto path = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_UI_SCREENSHOT_DIR", {});
        if (path.isEmpty())
            return;
        const juce::File directory (path);
        expect (directory.createDirectory().wasOk());
        juce::FileOutputStream stream (directory.getChildFile (imageName + ".png"));
        expect (stream.openedOk());
        if (stream.openedOk())
        {
            expect (stream.setPosition (0));
            expect (stream.truncate().wasOk());
            expect (juce::PNGImageFormat().writeImageToStream (card.createComponentSnapshot (card.getLocalBounds()), stream));
        }
    }

    void runTest() override
    {
        LiveMixLookAndFeel lookAndFeel;
        MixEngine engine;
        MixDocument document (engine);
        document.applyToEngine();
        const auto id = document.getSession().channels[0].id;
        ChannelCard card (document, id);
        card.setLookAndFeel (&lookAndFeel);
        document.onValueChanged = [&] { card.refresh(); };

        beginTest ("pan is below every output control and above the input meter in every layout, including portrait stacking");
        auto* slider = childOfType<PanSlider> (card);
        auto* meter = childOfType<MeterBar> (card);
        auto* master = withText (card, ko ("마스터"));
        auto* direct = withText (card, ko ("직접 출력"));
        auto* caption = withText (card, ko ("팬"));
        auto* meterCaption = withText (card, ko ("입력 미터"));
        expect (slider != nullptr && meter != nullptr && master != nullptr && direct != nullptr && caption != nullptr && meterCaption != nullptr);
        if (slider == nullptr || meter == nullptr || master == nullptr || direct == nullptr || caption == nullptr || meterCaption == nullptr)
            return;

        struct Layout { CardLayout mode; int width, height; const char* name; };
        const Layout layouts[] {
            { CardLayout::wide, 1400, 198, "pan-wide" },
            { CardLayout::wide, 1140, 198, "pan-wide-min" },
            { CardLayout::medium, 960, 338, "pan-medium" },
            { CardLayout::medium, 760, 338, "pan-medium-min" },
            { CardLayout::narrow, 640, 544, "pan-narrow" },
            { CardLayout::narrow, 421, 544, "pan-narrow-421" },
            { CardLayout::narrow, 420, 584, "pan-portrait-420" },
            { CardLayout::narrow, 364, 584, "pan-portrait-364" }
        };
        document.setChannelPan (id, -0.3);
        for (const auto& layout : layouts)
        {
            card.setLayout (layout.mode);
            expectEquals (card.getPreferredHeight (layout.width), layout.height);
            card.setSize (layout.width, card.getPreferredHeight (layout.width));
            expect (slider->getY() > master->getBottom() && slider->getY() > direct->getBottom());
            expect (slider->getBottom() < meterCaption->getY());
            expectEquals (caption->getY(), slider->getY());
            expectEquals (meter->getHeight(), 40);
            expect (card.getLocalBounds().contains (meter->getBounds()));
            expectGreaterThan (slider->getWidth(), 100);
            if (layout.width <= 420)
                expect (direct->getY() > master->getBottom());
            else
                expectEquals (direct->getY(), master->getY());
            auto* value = withText (card, "L30");
            expect (value != nullptr && value->getX() > slider->getRight());
            for (auto* child : card.getChildren())
                if (child->isVisible())
                    expect (card.getLocalBounds().contains (child->getBounds()), "Control escaped card: " + child->getName());
            screenshot (card, layout.name);
        }

        beginTest ("pan text, centre snap, double-click reset, wheel scrolling and stereo tooltip use the document value");
        expect (slider->getTooltip() == ko ("팬: 왼쪽/오른쪽 배치. 더블클릭하면 가운데"));
        for (const double near : { -0.05, -0.01, 0.0, 0.01, 0.05 })
            expectWithinAbsoluteError (slider->snapValue (near, juce::Slider::absoluteDrag), 0.0, 1e-12);
        expectWithinAbsoluteError (slider->snapValue (-0.06, juce::Slider::absoluteDrag), -0.06, 1e-12);
        expectWithinAbsoluteError (slider->snapValue (0.06, juce::Slider::absoluteDrag), 0.06, 1e-12);

        slider->setValue (1.0, juce::sendNotificationSync);
        expectWithinAbsoluteError (document.getSession().channels[0].pan, 1.0, 1e-12);
        expect (withText (card, "R100") != nullptr);
        document.setChannelInput (id, 0, true);
        expect (slider->getTooltip().contains (ko ("스테레오 채널은 좌우 균형")));
        screenshot (card, "pan-stereo-right");

        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent event (juce::Desktop::getInstance().getMainMouseSource(), { 30.0f, 15.0f }, {},
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, slider, slider, now, { 30.0f, 15.0f }, now, 2, false);
        juce::MouseWheelDetails wheel;
        wheel.deltaY = -1.0f;
        slider->mouseWheelMove (event, wheel);
        expectWithinAbsoluteError (slider->getValue(), 1.0, 1e-12);
        expectWithinAbsoluteError (document.getSession().channels[0].pan, 1.0, 1e-12);
        slider->mouseDoubleClick (event);
        expectWithinAbsoluteError (slider->getValue(), 0.0, 1e-12);
        expectWithinAbsoluteError (document.getSession().channels[0].pan, 0.0, 1e-12);
        expect (withText (card, "C") != nullptr);
        screenshot (card, "pan-stereo-centre");

        beginTest ("four FX send rows still leave room for pan and the complete meter");
        for (int i = 1; i < MixSession::maxFx; ++i)
            document.addFx();
        card.refresh();
        for (const auto& layout : layouts)
        {
            card.setLayout (layout.mode);
            card.setSize (layout.width, card.getPreferredHeight (layout.width));
            expectEquals (meter->getHeight(), 40);
            expect (card.getLocalBounds().contains (meter->getBounds()));
            expect (slider->getBottom() < meterCaption->getY());
        }
        card.setLookAndFeel (nullptr);
    }
};

static ChannelCardTests channelCardTests;

} // namespace gocue::tests
