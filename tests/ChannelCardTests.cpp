#include "MixDocument.h"
#include "ui/ChannelCard.h"
#include "ui/FxDrawer.h"
#include "ui/LiveMixLookAndFeel.h"
#include "TestGainPlugin.h"

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
        struct RestoreLookAndFeel
        {
            juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
            ~RestoreLookAndFeel() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
        } restoreLookAndFeel;
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);   // app startup also installs the measured Malgun Gothic typeface globally
        MixEngine engine;
        MixDocument document (engine);
        document.applyToEngine();
        const auto id = document.getSession().channels[0].id;
        ChannelCard card (document, id);
        card.setLookAndFeel (&lookAndFeel);
        document.onValueChanged = [&] { card.refresh(); };

        beginTest ("input meter follows the input; output controls share one row with pan underneath in every layout");
        auto* slider = childOfType<PanSlider> (card);
        auto* meter = childOfType<MeterBar> (card);
        auto* master = withText (card, ko ("마스터"));
        auto* direct = withText (card, ko ("직접 출력"));
        auto* caption = withText (card, ko ("팬"));
        auto* meterCaption = withText (card, ko ("입력 미터"));
        auto* input = childOfType<juce::ComboBox> (card);
        auto* mute = withText (card, ko ("뮤트그룹"));
        auto* mic = childOfType<LampButton> (card);
        expect (slider != nullptr && meter != nullptr && master != nullptr && direct != nullptr && caption != nullptr && meterCaption != nullptr);
        if (slider == nullptr || meter == nullptr || master == nullptr || direct == nullptr || caption == nullptr || meterCaption == nullptr)
            return;

        struct Layout { CardLayout mode; int width, height; const char* name; };
        const Layout layouts[] {
            { CardLayout::wide, 1920, 156, "pan-wide-1920" },
            { CardLayout::wide, 1400, 156, "pan-wide" },
            { CardLayout::wide, 1385, 156, "pan-wide-min" },
            { CardLayout::medium, 1384, 272, "pan-medium-max" },
            { CardLayout::medium, 1140, 272, "pan-medium-1140" },
            { CardLayout::medium, 960, 272, "pan-medium" },
            { CardLayout::medium, 770, 272, "pan-medium-group-fit" },
            { CardLayout::medium, 769, 308, "pan-medium-group-wrap" },
            { CardLayout::medium, 760, 308, "pan-medium-min" },
            { CardLayout::narrow, 759, 580, "pan-narrow-max" },
            { CardLayout::narrow, 640, 580, "pan-narrow" },
            { CardLayout::narrow, 421, 580, "pan-narrow-421" },
            { CardLayout::narrow, 420, 580, "pan-portrait-420" },
            { CardLayout::narrow, 380, 580, "pan-portrait-380" },
            { CardLayout::narrow, 379, 616, "pan-portrait-group-wrap" },
            { CardLayout::narrow, 364, 616, "pan-portrait-364" }
        };
        document.setChannelPan (id, -0.3);
        for (const auto& layout : layouts)
        {
            card.setLayout (layout.mode);
            expectEquals (card.getPreferredHeight (layout.width), layout.height);
            card.setSize (layout.width, card.getPreferredHeight (layout.width));
            expect (slider->getY() > master->getBottom() && slider->getY() > direct->getBottom());
            expect (input != nullptr && mute != nullptr && mic != nullptr);
            if (input != nullptr) expect (meterCaption->getY() > input->getBottom());
            if (mute != nullptr && mic != nullptr) expect (mute->getY() > mic->getBottom());
            if (layout.mode == CardLayout::wide)
                expect (meter->getRight() < master->getX());
            else
                expect (meter->getBottom() < master->getY());
            expectEquals (caption->getY(), slider->getY());
            expectEquals (meter->getHeight(), 40);
            expect (card.getLocalBounds().contains (meter->getBounds()));
            expectGreaterThan (slider->getWidth(), 100);
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

        beginTest ("l06 mockup rectangles at 1400, 960 and 420 with two FX sends");
        document.addFx();
        struct NamedPlugin : TestGainPlugin
        {
            explicit NamedPlugin (const char* text) : TestGainPlugin (1.0f), name (text) {}
            const juce::String getName() const override { return name; }
            juce::String name;
        };
        for (const auto* text : { "3 Band EQ", "MaBitcrush", "Ping Pong Pan" })
            engine.getChannelChain (id)->addPlugin (std::make_unique<NamedPlugin> (text));
        card.refresh();
        const auto rect = [&] (juce::Component* component, juce::Rectangle<int> expected)
        {
            expect (component != nullptr);
            if (component != nullptr) expectEquals (component->getBounds().toString(), expected.toString());
        };
        for (const auto& example : { Layout { CardLayout::wide, 1400, 156, "l06-wide" },
                                     Layout { CardLayout::medium, 960, 272, "l06-medium" },
                                     Layout { CardLayout::narrow, 420, 618, "l06-narrow" } })
        {
            card.setLayout (example.mode);
            expectEquals (card.getPreferredHeight (example.width), example.height);
            card.setSize (example.width, example.height);
            const bool narrow = example.mode == CardLayout::narrow;
            const bool wide = example.mode == CardLayout::wide;
            rect (mic, { 14, 54, narrow ? 392 : 194, 40 });
            rect (mute, { 14, 102, 100, 24 });
            rect (input, narrow ? juce::Rectangle<int> { 48, 140, 266, 30 } : juce::Rectangle<int> { 226, 40, 160, 30 });
            rect (withText (card, ko ("스테레오")), narrow ? juce::Rectangle<int> { 318, 140, 88, 30 } : juce::Rectangle<int> { 298, 10, 88, 30 });
            rect (meterCaption, narrow ? juce::Rectangle<int> { 14, 178, 392, 18 } : juce::Rectangle<int> { 226, 86, 160, 18 });
            rect (meter, narrow ? juce::Rectangle<int> { 14, 196, 392, 40 } : juce::Rectangle<int> { 226, 104, 160, 40 });
            rect (withText (card, ko ("체인 열기")), { narrow ? 14 : 404, narrow ? 316 : 78, 92, 30 });
            rect (withText (card, ko ("+ 추가")), { narrow ? 114 : 504, narrow ? 316 : 78, 76, 30 });
            rect (withText (card, ko ("플러그인 그룹")), { narrow ? 14 : 404, narrow ? 352 : 114, 112, 30 });
            rect (withText (card, ko ("그룹 OFF")), { narrow ? 134 : 524, narrow ? 354 : 116, 66, 26 });
            int groupCount = 0;
            for (int i = 0; i < 5; ++i)
                for (auto* child : card.getChildren())
                    if (auto* button = dynamic_cast<juce::TextButton*> (child); button != nullptr && button->getButtonText() == juce::String (i + 1))
                    {
                        rect (button, { (narrow ? 200 : 590) + 34 * i, narrow ? 355 : 117, 30, 24 });
                        ++groupCount;
                    }
            expectEquals (groupCount, 5);
            rect (withText (card, "3 Band EQ"), { narrow ? 14 : 404, narrow ? 277 : 39, 110, 32 });
            rect (withText (card, "MaBitcrush"), { narrow ? 130 : 520, narrow ? 277 : 39, 110, 32 });
            rect (withText (card, "Ping Pong Pan"), { narrow ? 246 : 636, narrow ? 277 : 39, 129, 32 });
            rect (master, { wide ? 1152 : narrow ? 14 : 404, wide ? 38 : narrow ? 532 : 186, 72, 34 });
            rect (slider, wide ? juce::Rectangle<int> { 1186, 76, 136, 34 }
                              : narrow ? juce::Rectangle<int> { 48, 570, 294, 34 } : juce::Rectangle<int> { 438, 224, 444, 34 });
            for (auto* child : card.getChildren())
                if (withText (*child, "FX1") != nullptr)
                {
                    rect (child, wide ? juce::Rectangle<int> { 783, 40, 351, 30 }
                                      : narrow ? juce::Rectangle<int> { 14, 424, 392, 30 } : juce::Rectangle<int> { 14, 188, 372, 30 });
                    rect (childOfType<juce::Slider> (*child), { 102, 0, wide ? 115 : narrow ? 156 : 136, 30 });
                }
            screenshot (card, example.name);
        }

        beginTest ("up to four FX sends and wrapped long chains keep every control inside and disjoint");
        struct LongPlugin : TestGainPlugin
        {
            LongPlugin() : TestGainPlugin (1.0f) {}
            const juce::String getName() const override { return "A long plugin name that fills a complete chain column"; }
        };
        for (int fxCount = 2; fxCount <= MixSession::maxFx; ++fxCount)
        {
            if (fxCount > 2) document.addFx();
            for (bool wrapped : { false, true })
            {
                auto* chain = engine.getChannelChain (id);
                if (chain != nullptr)
                {
                    chain->clear();
                    if (wrapped)
                        for (int i = 0; i < 7; ++i) chain->addPlugin (std::make_unique<LongPlugin>());
                }
                card.refresh();
                for (const auto& layout : layouts)
                {
                    card.setLayout (layout.mode);
                    card.setSize (layout.width, card.getPreferredHeight (layout.width));
                    expectEquals (meter->getHeight(), 40);
                    for (auto* child : card.getChildren())
                    {
                        if (! child->isVisible()) continue;
                        expect (card.getLocalBounds().contains (child->getBounds()), "Outside card at " + juce::String (layout.width));
                        for (auto* other : card.getChildren())
                            if (other != child && other->isVisible()) expect (! child->getBounds().intersects (other->getBounds()));
                        if (childOfType<juce::Slider> (*child) != nullptr)
                            for (auto* control : child->getChildren())
                                expect (child->getLocalBounds().contains (control->getBounds()), "Outside send row");
                    }
                }
            }
        }
        card.setLookAndFeel (nullptr);

        beginTest ("channel and FX direct output controls show effective Windows routing and preserve ASIO requests");
        FxDrawer fx (document);
        juce::ComboBox* channelOutput = nullptr;
        for (auto* child : card.getChildren())
            if (auto* combo = dynamic_cast<juce::ComboBox*> (child)) channelOutput = combo;
        auto* fxOutput = childOfType<juce::ComboBox> (fx);
        auto* fxDirect = withText (fx, ko ("직접 출력"));
        expect (channelOutput != nullptr && fxOutput != nullptr && fxDirect != nullptr);
        if (channelOutput != nullptr && fxOutput != nullptr && fxDirect != nullptr)
        {
            document.setChannelOutput (id, { true, true, 2 });
            document.setFxOutput (document.getSession().fx[0].id, { false, true, 4 });
            for (int mode : { 0, 1, 2 })
            {
                document.getSession().device = { mode == 0 ? "ASIO" : "Windows Audio", "Input", mode == 2 ? "" : "Output", 256, 48000.0 };
                juce::StringArray names;
                for (int i = 0; i < (mode == 0 ? 8 : mode == 1 ? 2 : 0); ++i) names.add (juce::String (i + 1));
                card.setDeviceChannels ({ "1", "2" }, names);
                fx.setDeviceChannels (names);
                expectEquals (channelOutput->getNumItems(), mode == 0 ? 4 : 1);
                expectEquals (fxOutput->getNumItems(), mode == 0 ? 4 : 1);
                expect (channelOutput->isEnabled() == (mode != 2));
                expect (fxOutput->isEnabled() == (mode != 2));
                expect (direct->isEnabled() == (mode != 2));
                expect (fxDirect->isEnabled() == (mode != 2));
                expect (channelOutput->getText().startsWith (mode == 0 ? "3-4" : mode == 1 ? "1-2" : ko ("없음")));
                expect (fxOutput->getText().startsWith (mode == 0 ? "5-6" : mode == 1 ? "1-2" : ko ("없음")));
                if (mode == 1)
                {
                    expect (channelOutput->getTooltip().contains (ko ("ASIO에서는 3-4로 나갑니다")));
                    expect (fxOutput->getTooltip().contains (ko ("ASIO에서는 5-6로 나갑니다")));
                }
                expectEquals (document.getSession().channels[0].output.directFirst, 2);
                expectEquals (document.getSession().fx[0].output.directFirst, 4);
            }
        }
    }
};

static ChannelCardTests channelCardTests;

} // namespace gocue::tests
