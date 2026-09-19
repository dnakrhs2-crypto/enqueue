#include "ui/CueMenuIcons.h"
#include "ui/GoCueLookAndFeel.h"

#include <vector>

namespace gocue::tests
{

namespace
{
    struct CueCase { juce::CommandID command; CueIcons::Key key; const char* label; };
    const std::array<CueCase, 9> cueCases {{
        { CommandIDs::addCue,        { CueType::audio }, "큐 추가..." },
        { CommandIDs::addFadeCue,    { CueType::fade, FadeMode::fadeIn }, "페이드 인 큐 추가" },
        { CommandIDs::addFadeOutCue, { CueType::fade, FadeMode::fadeOut }, "페이드 아웃 큐 추가" },
        { CommandIDs::addDevampCue,  { CueType::devamp }, "디밴프 큐 추가" },
        { CommandIDs::addGroupCue,   { CueType::group }, "그룹 큐 추가" },
        { CommandIDs::addControlCue, { CueType::control }, "제어 큐 추가" },
        { CommandIDs::addWaitCue,   { CueType::control, FadeMode::custom, ControlKind::wait }, "대기 큐 추가" },
        { CommandIDs::addMemoCue,   { CueType::control, FadeMode::custom, ControlKind::memo }, "메모 큐 추가" },
        { CommandIDs::addMicCue,    { CueType::mic }, "마이크 큐 추가" }
    }};

    const std::array<juce::CommandID, 11> menuCommands {
        CommandIDs::addCue, CommandIDs::addFadeCue, CommandIDs::addFadeOutCue, CommandIDs::addDevampCue,
        CommandIDs::addGroupCue, CommandIDs::addControlCue, CommandIDs::addWaitCue, CommandIDs::addMemoCue,
        CommandIDs::addMicCue, CommandIDs::addCueList, CommandIDs::addCart
    };

    juce::Image renderRow (GoCueLookAndFeel& look, const juce::Drawable* icon, const juce::String& shortcut,
                           float scale = 1.0f, bool active = true, bool highlighted = false,
                           bool ticked = false, bool subMenu = false, const juce::Colour* colour = nullptr)
    {
        juce::Image result (juce::Image::ARGB, juce::roundToInt (480.0f * scale), juce::roundToInt (28.0f * scale), true);
        juce::Graphics g (result);
        g.addTransform (juce::AffineTransform::scale (scale));
        look.drawPopupMenuItem (g, { 0, 0, 480, 28 }, false, active, highlighted, ticked, subMenu,
                                "Menu item", shortcut, icon, colour);
        return result;
    }

    bool samePixels (const juce::Image& a, const juce::Image& b, juce::Rectangle<int> area)
    {
        const juce::Image::BitmapData first (a, juce::Image::BitmapData::readOnly);
        const juce::Image::BitmapData second (b, juce::Image::BitmapData::readOnly);
        for (int y = area.getY(); y < area.getBottom(); ++y)
            for (int x = area.getX(); x < area.getRight(); ++x)
                if (first.getPixelColour (x, y) != second.getPixelColour (x, y))
                    return false;
        return true;
    }

    bool hasInkColour (const juce::Image& image, juce::Rectangle<int> area, juce::Colour expected, bool requireFullAlpha = true)
    {
        const juce::Image::BitmapData pixels (image, juce::Image::BitmapData::readOnly);
        int maxAlpha = 0;
        for (int y = area.getY(); y < area.getBottom(); ++y)
            for (int x = area.getX(); x < area.getRight(); ++x)
            {
                const auto pixel = pixels.getPixelColour (x, y);
                maxAlpha = juce::jmax (maxAlpha, (int) pixel.getAlpha());
                if (pixel.getAlpha() > 20)
                {
                    // Allow one premultiplied byte of quantisation when checking antialiased edges.
                    const int tolerance = 1 + 255 / (int) pixel.getAlpha();
                    if (std::abs ((int) pixel.getRed() - (int) expected.getRed()) > tolerance
                        || std::abs ((int) pixel.getGreen() - (int) expected.getGreen()) > tolerance
                        || std::abs ((int) pixel.getBlue() - (int) expected.getBlue()) > tolerance)
                        return false;
                }
            }
        return maxAlpha > 0 && (! requireFullAlpha || std::abs (maxAlpha - (int) expected.getAlpha()) <= 1);
    }
}

class CueIconsTests : public juce::UnitTest
{
public:
    CueIconsTests() : juce::UnitTest ("Cue icons and popup menu rendering", "Enqueue") {}

    void runTest() override
    {
        const juce::Rectangle<float> frame (0.0f, 0.0f, 24.0f, 24.0f);
        std::vector<const juce::Path*> paths;

        beginTest ("all cue and container keys have distinct nonempty filled paths inside the shared frame");
        for (const auto& item : cueCases)
            paths.push_back (CueIcons::pathFor (item.key));
        paths.push_back (CueIcons::pathFor (CueIcons::Key { CueType::fade, FadeMode::custom }));
        paths.push_back (CueIcons::pathFor (CueIcons::Container::cueList));
        paths.push_back (CueIcons::pathFor (CueIcons::Container::cart));

        for (size_t i = 0; i < paths.size(); ++i)
        {
            const auto* path = paths[i];
            expect (path != nullptr);
            if (path == nullptr)
                continue;
            expect (! path->isEmpty());
            expect (! path->getBounds().isEmpty());
            expect (frame.contains (path->getBounds()), "Artwork escaped its 24-unit frame");
            for (size_t j = 0; j < i; ++j)
                if (paths[j] != nullptr)
                {
                    expect (path != paths[j], "Different kinds shared a registry entry");
                    expect (*path != *paths[j], "Different kinds shared identical geometry");
                }
        }

        beginTest ("command adapters and model cues select the same artwork");
        for (const auto& item : cueCases)
        {
            auto icon = CueMenuIcons::create (item.command);
            auto* drawable = dynamic_cast<juce::DrawablePath*> (icon.get());
            const auto* registered = CueIcons::pathFor (item.key);
            expect (drawable != nullptr && registered != nullptr);
            if (drawable != nullptr && registered != nullptr)
            {
                expect (drawable->getPath() == *registered);
                expect (drawable->getDrawableBounds() == frame);
                expectEquals (drawable->getStrokeType().getStrokeThickness(), 0.0f);
            }

            Cue cue;
            cue.type = item.key.type;
            cue.fade.mode = item.key.fadeMode;
            cue.control.kind = item.key.controlKind;
            auto modelIcon = CueIcons::create (cue);
            auto* modelPath = dynamic_cast<juce::DrawablePath*> (modelIcon.get());
            expect (modelPath != nullptr && registered != nullptr && modelPath->getPath() == *registered);
        }
        for (const auto command : { CommandIDs::addCueList, CommandIDs::addCart })
        {
            auto icon = CueMenuIcons::create (command);
            const auto* drawable = dynamic_cast<const juce::DrawablePath*> (icon.get());
            const auto* expected = CueIcons::pathFor (command == CommandIDs::addCueList
                                                         ? CueIcons::Container::cueList : CueIcons::Container::cart);
            expect (drawable != nullptr && expected != nullptr && drawable->getPath() == *expected);
        }
        for (const auto command : std::array<juce::CommandID, 6> { CommandIDs::go, CommandIDs::removeCue, CommandIDs::groupSelectedCues,
                                                                 CommandIDs::toggleAlwaysAudition, 0, -1 })
            expect (CueMenuIcons::create (command) == nullptr);

        beginTest ("all control kinds resolve, and unrelated subtype fields do not change a cue's icon");
        for (const auto kind : { ControlKind::start, ControlKind::stop, ControlKind::pause, ControlKind::load,
                                 ControlKind::reset, ControlKind::gotoCue, ControlKind::wait, ControlKind::memo,
                                 ControlKind::arm, ControlKind::disarm, ControlKind::target })
        {
            const auto* path = CueIcons::pathFor ({ CueType::control, FadeMode::fadeOut, kind });
            expect (path != nullptr);
            if (kind != ControlKind::wait && kind != ControlKind::memo)
                expect (path == CueIcons::pathFor ({ CueType::control }));
        }
        for (const auto type : { CueType::audio, CueType::devamp, CueType::group, CueType::mic })
            expect (CueIcons::pathFor ({ type }) == CueIcons::pathFor ({ type, FadeMode::fadeOut, ControlKind::memo }));
        expect (CueIcons::pathFor ({ static_cast<CueType> (-1) }) == nullptr);
        expect (CueIcons::pathFor ({ CueType::fade, static_cast<FadeMode> (-1) }) == nullptr);

        beginTest ("fresh menu drawables and copies retain the frame and cannot recolour one another");
        for (const auto command : menuCommands)
        {
            auto first = CueMenuIcons::create (command);
            auto second = CueMenuIcons::create (command);
            expect (first != nullptr && second != nullptr && first.get() != second.get());
            if (first == nullptr || second == nullptr)
                continue;
            auto copy = first->createCopy();
            expect (copy != nullptr && copy->getDrawableBounds() == frame);
            expect (first->replaceColour (juce::Colours::white, juce::Colours::red));
            expect (second->replaceColour (juce::Colours::white, juce::Colours::blue));
            expect (copy != nullptr && copy->replaceColour (juce::Colours::white, juce::Colours::green));
        }

        beginTest ("directional fades mirror one another, and all menu silhouettes differ at 14 and 18 pixels");
        auto mirrored = *CueIcons::pathFor ({ CueType::fade, FadeMode::fadeIn });
        mirrored.applyTransform (juce::AffineTransform (-1.0f, 0.0f, 24.0f, 0.0f, 1.0f, 0.0f));
        expect (mirrored == *CueIcons::pathFor ({ CueType::fade, FadeMode::fadeOut }));
        for (const int size : { 14, 18 })
        {
            std::vector<juce::Image> silhouettes;
            for (const auto command : menuCommands)
            {
                juce::Image silhouette (juce::Image::ARGB, size, size, true);
                auto icon = CueMenuIcons::create (command);
                if (icon == nullptr)
                    continue;
                {
                    juce::Graphics g (silhouette);
                    icon->drawWithin (g, silhouette.getBounds().toFloat(), juce::RectanglePlacement::centred, 1.0f);
                } // Finish Direct2D's drawing batch before reading pixels.
                expect (hasInkColour (silhouette, silhouette.getBounds(), juce::Colours::white, false));
                for (const auto& other : silhouettes)
                    expect (! samePixels (silhouette, other, silhouette.getBounds()));
                silhouettes.push_back (silhouette);
            }
        }

        testRendering();
        writePreview();
    }

private:
    void testRendering()
    {
        GoCueLookAndFeel look;
        look.setColour (juce::PopupMenu::textColourId, juce::Colours::lime);
        look.setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::red);
        look.setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colours::transparentBlack);
        const auto customColour = juce::Colours::blue.withAlpha (0.8f);
        const std::array<juce::String, 2> shortcuts { juce::String(), "Ctrl+7" };
        const std::array<float, 4> scales { 1.0f, 1.1f, 1.25f, 1.5f };

        beginTest ("icons and labels share normal, highlighted, custom and disabled colours at every UI scale");
        for (const auto scale : scales)
            for (const auto& shortcut : shortcuts)
                for (const auto command : menuCommands)
                {
                    auto icon = CueMenuIcons::create (command);
                    if (icon == nullptr)
                        continue;
                    struct State { bool active, highlighted; const juce::Colour* custom; juce::Colour expected; };
                    const std::array<State, 6> states {{
                        { true, false, nullptr, juce::Colours::lime },
                        { true, true, nullptr, juce::Colours::red },
                        { false, false, nullptr, juce::Colours::lime.withMultipliedAlpha (0.5f) },
                        { true, false, &customColour, customColour },
                        { true, true, &customColour, juce::Colours::red },
                        { false, true, &customColour, customColour.withMultipliedAlpha (0.5f) }
                    }};
                    for (const auto& state : states)
                    {
                        const auto row = renderRow (look, icon.get(), shortcut, scale, state.active, state.highlighted,
                                                    false, false, state.custom);
                        const auto iconRegion = juce::Rectangle<float> (0.0f, 0.0f, 32.0f, 28.0f) * scale;
                        const auto textRegion = juce::Rectangle<float> (36.0f, 0.0f, 120.0f, 28.0f) * scale;
                        expect (hasInkColour (row, iconRegion.getSmallestIntegerContainer(), state.expected), "Icon tint / alpha differs");
                        expect (hasInkColour (row, textRegion.getSmallestIntegerContainer(), state.expected), "Label tint / alpha differs");
                    }
                    expect (icon->replaceColour (juce::Colours::white, juce::Colours::black), "Drawing modified the original icon");
                }

        beginTest ("empty, icon and tick slots align labels, shortcuts and submenu arrows with and without shortcuts");
        for (const auto scale : scales)
            for (const auto& shortcut : shortcuts)
            {
                auto icon = CueMenuIcons::create (CommandIDs::addCue);
                const auto plain = renderRow (look, nullptr, shortcut, scale, true, false, false, true);
                const auto illustrated = renderRow (look, icon.get(), shortcut, scale, true, false, false, true);
                const auto ticked = renderRow (look, nullptr, shortcut, scale, true, false, true, true);
                const auto textAndRightEdge = (juce::Rectangle<float> (32.0f, 0.0f, 448.0f, 28.0f) * scale).getSmallestIntegerContainer();
                expect (samePixels (plain, illustrated, textAndRightEdge));
                expect (samePixels (plain, ticked, textAndRightEdge));
                const auto tickRegion = (juce::Rectangle<float> (0.0f, 0.0f, 32.0f, 28.0f) * scale).getSmallestIntegerContainer();
                expect (hasInkColour (ticked, tickRegion, juce::Colours::lime));
                const auto disabledTick = renderRow (look, nullptr, shortcut, scale, false, false, true);
                expect (hasInkColour (disabledTick, tickRegion, juce::Colours::lime.withMultipliedAlpha (0.5f)));
            }
    }

    void writePreview()
    {
        // Optional review screenshot of real JUCE rendering; no bitmap is loaded by the application.
        const auto directoryName = juce::SystemStats::getEnvironmentVariable ("ENQUEUE_MENU_ICON_PREVIEW_DIR", {});
        if (directoryName.isEmpty())
            return;
        const juce::File directory (directoryName);
        expect (directory.createDirectory().wasOk());
        juce::Image preview (juce::Image::ARGB, 1200, 780, true);
        GoCueLookAndFeel look;
        int idealWidth = 0, rowHeight = 0;
        look.getIdealPopupMenuItemSize ("Menu item", false, 0, idealWidth, rowHeight);
        const std::array<float, 4> scales { 1.0f, 1.1f, 1.25f, 1.5f };
        {
            juce::Graphics g (preview);
            g.fillAll (Palette::panel);
            for (size_t column = 0; column < scales.size(); ++column)
            {
                const auto scale = scales[column];
                const float left = 8.0f + (float) column * 300.0f;
                g.setFont (Palette::font());
                g.setColour (Palette::text);
                g.drawText (juce::String (juce::roundToInt (scale * 100.0f)) + "%", (int) left, 10, 280, 24, juce::Justification::centredLeft);
                for (int row = 0; row < 16; ++row)
                {
                    auto icon = row < 11 ? CueMenuIcons::create (menuCommands[(size_t) row])
                                        : row >= 14 ? CueMenuIcons::create (CommandIDs::addCue) : nullptr;
                    const char* label = row < 9 ? cueCases[(size_t) row].label
                                      : row == 9 ? "새 큐 리스트" : row == 10 ? "새 카트 (버튼 격자)"
                                      : row == 11 ? "그룹으로 묶기" : row == 12 ? "항상 오디션"
                                      : row == 13 ? "큐 리스트 / 카트" : row == 14 ? "강조된 큐 추가..." : "비활성 큐 추가...";
                    juce::Graphics::ScopedSaveState save (g);
                    g.addTransform (juce::AffineTransform::scale (scale).translated (left, 48.0f + (float) row * 45.0f));
                    look.drawPopupMenuItem (g, { 0, 0, (int) (284.0f / scale), rowHeight }, false, row != 15, row == 14,
                                            row == 12, row == 13, juce::String::fromUTF8 (label), row == 1 ? "Ctrl+7" : "",
                                            icon.get(), nullptr);
                }
            }
        }
        juce::FileOutputStream stream (directory.getChildFile ("cue-menu-icons.png"));
        expect (stream.openedOk());
        if (stream.openedOk())
        {
            expect (stream.setPosition (0));
            expect (stream.truncate().wasOk());
            expect (juce::PNGImageFormat().writeImageToStream (preview, stream));
        }
    }
};

static CueIconsTests cueIconsTests;

} // namespace gocue::tests
