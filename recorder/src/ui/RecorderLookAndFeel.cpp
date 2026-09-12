#include "RecorderLookAndFeel.h"
#if JUCE_WINDOWS
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace gocue::recorder
{
juce::Font recorderMonoFont(float height, int style)
{
    static const auto name = juce::Font::findAllTypefaceNames().contains("Cascadia Mono") ? "Cascadia Mono" : "Consolas";
    return juce::Font(juce::FontOptions(name, height, style).withPointHeight(height));
}
void styleRecorderWindow(juce::DocumentWindow& window)
{
#if JUCE_WINDOWS
    if (const auto* peer = window.getPeer())
    {
        const auto handle = static_cast<HWND>(peer->getNativeHandle()); const BOOL dark = TRUE;
        const auto background = RGB(Palette::card.getRed(), Palette::card.getGreen(), Palette::card.getBlue());
        const auto text = RGB(Palette::text.getRed(), Palette::text.getGreen(), Palette::text.getBlue());
        // Unsupported DWM attributes simply retain the operating system's title bar.
        DwmSetWindowAttribute(handle, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(handle, 35, &background, sizeof(background));
        DwmSetWindowAttribute(handle, 36, &text, sizeof(text));
    }
#else
    juce::ignoreUnused(window);
#endif
}
RecorderLookAndFeel::RecorderLookAndFeel()
    : juce::LookAndFeel_V4(ColourScheme(Palette::background, Palette::card, Palette::card2, Palette::line,
          Palette::text, Palette::field, juce::Colours::white, Palette::accent, Palette::text))
{
    setDefaultSansSerifTypefaceName("Malgun Gothic");
    setColour(juce::TextButton::buttonColourId, Palette::card2);
    setColour(juce::TextButton::buttonOnColourId, Palette::accent);
    setColour(juce::TextButton::textColourOffId, Palette::text);
    setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    setColour(buttonOutlineColourId, Palette::line);
    setColour(juce::ComboBox::backgroundColourId, Palette::field);
    setColour(juce::ComboBox::outlineColourId, Palette::line);
    setColour(juce::ComboBox::arrowColourId, Palette::dimText);
    setColour(juce::ComboBox::textColourId, Palette::text);
    setColour(juce::ComboBox::focusedOutlineColourId, Palette::accent);
    setColour(juce::TextEditor::backgroundColourId, Palette::field);
    setColour(juce::TextEditor::outlineColourId, Palette::line);
    setColour(juce::TextEditor::focusedOutlineColourId, Palette::accent);
    setColour(juce::TextEditor::textColourId, Palette::text);
    setColour(juce::TextEditor::highlightColourId, Palette::selection);
    setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::white);
    setColour(juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    setColour(juce::CaretComponent::caretColourId, Palette::text);
    setColour(juce::Label::textColourId, Palette::text);
    setColour(juce::Label::backgroundWhenEditingColourId, Palette::field);
    setColour(juce::Label::textWhenEditingColourId, Palette::text);
    setColour(juce::Label::outlineWhenEditingColourId, Palette::accent);
    setColour(juce::ToggleButton::textColourId, Palette::text);
    setColour(juce::ToggleButton::tickColourId, Palette::accent);
    setColour(juce::ToggleButton::tickDisabledColourId, Palette::dimText);
    setColour(juce::Slider::backgroundColourId, Palette::field);
    setColour(juce::Slider::trackColourId, Palette::accent);
    setColour(juce::Slider::thumbColourId, juce::Colours::white);
    setColour(juce::Slider::textBoxBackgroundColourId, Palette::field);
    setColour(juce::Slider::textBoxTextColourId, Palette::text);
    setColour(juce::Slider::textBoxOutlineColourId, Palette::line);
    setColour(juce::Slider::textBoxHighlightColourId, Palette::selection);
    setColour(juce::PopupMenu::backgroundColourId, Palette::bar);
    setColour(juce::PopupMenu::textColourId, Palette::text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, Palette::accent);
    setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour(juce::PopupMenu::headerTextColourId, Palette::dimText);
    setColour(juce::ScrollBar::backgroundColourId, Palette::card);
    setColour(juce::ScrollBar::thumbColourId, Palette::line);
    setColour(juce::TooltipWindow::backgroundColourId, Palette::card2);
    setColour(juce::TooltipWindow::textColourId, Palette::text);
    setColour(juce::TooltipWindow::outlineColourId, Palette::line);
    setColour(juce::AlertWindow::backgroundColourId, Palette::card);
    setColour(juce::AlertWindow::textColourId, Palette::text);
    setColour(juce::AlertWindow::outlineColourId, Palette::line);
    setColour(juce::ListBox::backgroundColourId, Palette::card);
    setColour(juce::ListBox::outlineColourId, Palette::line);
    setColour(juce::ListBox::textColourId, Palette::text);
    setColour(juce::TabbedComponent::backgroundColourId, Palette::card);
    setColour(juce::TabbedComponent::outlineColourId, Palette::line);
    setColour(juce::ResizableWindow::backgroundColourId, Palette::background);
    setColour(juce::DocumentWindow::textColourId, Palette::text);
}

// Preserve the existing fonts; compact design surfaces specify their own size.
juce::Font RecorderLookAndFeel::getPopupMenuFont() { return juce::Font(juce::FontOptions(19.2f)); }
juce::Font RecorderLookAndFeel::getMenuBarFont(juce::MenuBarComponent&, int, const juce::String&) { return juce::Font(juce::FontOptions(18.0f)); }
juce::Font RecorderLookAndFeel::getTextButtonFont(juce::TextButton& button, int height)
{
    if (button.getProperties().contains("recorderFontSize"))
        return recorderFont(juce::jmin(float(button.getProperties()["recorderFontSize"]), float(height) * .6f), juce::Font::bold);
    return juce::Font(juce::FontOptions(juce::jmin(18.0f, float(height) * .6f), juce::Font::bold));
}
juce::Font RecorderLookAndFeel::getComboBoxFont(juce::ComboBox&) { return juce::Font(juce::FontOptions(17.4f)); }
void RecorderLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
    const juce::Colour& background, bool over, bool down)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced(.5f);
    auto colour = background;
    if (button.isEnabled()) colour = down ? colour.darker(.12f) : over ? colour.brighter(.06f) : colour;
    const float alpha = button.isEnabled() ? 1.0f : .45f;
    juce::Path shape;
    shape.addRoundedRectangle(bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(),
        Palette::controlRadius, Palette::controlRadius,
        !button.isConnectedOnLeft(), !button.isConnectedOnRight(), !button.isConnectedOnLeft(), !button.isConnectedOnRight());
    g.setColour(colour.withMultipliedAlpha(alpha)); g.fillPath(shape);
    if (!button.isConnectedOnLeft() && !button.isConnectedOnRight())
    {
        g.setColour(button.findColour(buttonOutlineColourId).withMultipliedAlpha(alpha));
        g.strokePath(shape, juce::PathStrokeType(1));
    }
}
void RecorderLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.setColour(button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
        .withMultipliedAlpha(button.isEnabled() ? 1.0f : .45f));
    auto text = button.getLocalBounds().reduced(6, 2);
    if (bool(button.getProperties().getWithDefault("recorderMenuArrow", false)))
    {
        const auto c = text.removeFromRight(14).toFloat().getCentre();
        juce::Path path; path.startNewSubPath(c.x - 3, c.y - 1.5f); path.lineTo(c.x, c.y + 1.5f); path.lineTo(c.x + 3, c.y - 1.5f);
        g.strokePath(path, juce::PathStrokeType(1.5f));
    }
    g.drawFittedText(button.getButtonText(), text, juce::Justification::centred, 1);
}
void RecorderLookAndFeel::drawTickBox(juce::Graphics& g, juce::Component&, float x, float y, float w, float h,
    bool ticked, bool enabled, bool, bool)
{
    const auto box = juce::Rectangle<float>(x, y, w, h).withSizeKeepingCentre(14, 14).reduced(.5f);
    const float alpha = enabled ? 1.0f : .45f;
    g.setColour((ticked ? Palette::accent : Palette::field).withMultipliedAlpha(alpha)); g.fillRoundedRectangle(box, 4);
    g.setColour((ticked ? Palette::accent : Palette::dimText).withMultipliedAlpha(alpha)); g.drawRoundedRectangle(box, 4, 1);
    if (ticked)
    {
        juce::Path tick; tick.startNewSubPath(box.getX() + 3, box.getCentreY()); tick.lineTo(box.getX() + 5.5f, box.getBottom() - 3); tick.lineTo(box.getRight() - 2.5f, box.getY() + 3);
        g.setColour(juce::Colours::white.withAlpha(alpha)); g.strokePath(tick, juce::PathStrokeType(1.5f));
    }
}
void RecorderLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool over, bool down)
{
    const float size = juce::jmin(18.0f, float(button.getHeight()) * .75f), tick = size * 1.1f;
    drawTickBox(g, button, 4, (float(button.getHeight()) - tick) * .5f, tick, tick, button.getToggleState(), button.isEnabled(), over, down);
    g.setColour(button.findColour(juce::ToggleButton::textColourId).withMultipliedAlpha(button.isEnabled() ? 1.0f : .45f));
    g.setFont(juce::Font(juce::FontOptions(size)));
    g.drawFittedText(button.getButtonText(), button.getLocalBounds().withTrimmedLeft(juce::roundToInt(tick) + 10).withTrimmedRight(2), juce::Justification::centredLeft, 10);
}
void RecorderLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int>(0, 0, w, h).toFloat().reduced(.5f);
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId)); g.fillRoundedRectangle(bounds, Palette::controlRadius);
    g.setColour(box.findColour(box.hasKeyboardFocus(true) ? juce::ComboBox::focusedOutlineColourId : juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle(bounds, Palette::controlRadius, 1);
    juce::Path arrow; const float x = float(w) - 16, y = float(h) * .5f;
    arrow.addTriangle(x - 4, y - 2, x + 4, y - 2, x, y + 3);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId).withMultipliedAlpha(box.isEnabled() ? 1.0f : .45f)); g.fillPath(arrow);
}
void RecorderLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{ label.setBounds(10, 1, box.getWidth() - 30, box.getHeight() - 2); label.setFont(getComboBoxFont(box)); label.setMinimumHorizontalScale(1); }
void RecorderLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int w, int h, juce::TextEditor& editor)
{ g.setColour(editor.findColour(juce::TextEditor::backgroundColourId)); g.fillRoundedRectangle(juce::Rectangle<int>(0, 0, w, h).toFloat(), Palette::controlRadius); }
void RecorderLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int w, int h, juce::TextEditor& editor)
{
    const bool focused = editor.isEnabled() && !editor.isReadOnly() && editor.hasKeyboardFocus(true);
    g.setColour(editor.findColour(focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle(juce::Rectangle<int>(0, 0, w, h).toFloat().reduced(.5f), Palette::controlRadius, 1);
}
int RecorderLookAndFeel::getTabButtonBestWidth(juce::TabBarButton& button, int)
{ const auto& bar = button.getTabbedButtonBar(); return juce::jmax(80, bar.getWidth() / juce::jmax(1, bar.getNumTabs())); }
void RecorderLookAndFeel::drawTabbedButtonBarBackground(juce::TabbedButtonBar& bar, juce::Graphics& g)
{ g.fillAll(Palette::bar); g.setColour(Palette::line); g.fillRect(0, bar.getHeight() - 1, bar.getWidth(), 1); }
void RecorderLookAndFeel::drawTabButton(juce::TabBarButton& button, juce::Graphics& g, bool over, bool down)
{
    const bool selected = button.getToggleState(), enabled = button.isEnabled(); // isEnabled() also reflects a disabled parent
    const float alpha = enabled ? 1.0f : .45f;
    g.fillAll(Palette::bar);
    if (enabled && (over || down)) { g.setColour(Palette::text.withAlpha(down ? .10f : .06f)); g.fillRect(button.getLocalBounds()); }
    g.setColour((selected ? Palette::text : Palette::dimText).withMultipliedAlpha(alpha)); g.setFont(recorderFont(12.5f, juce::Font::bold));
    g.drawFittedText(button.getButtonText(), button.getLocalBounds().reduced(6, 2), juce::Justification::centred, 1);
    g.setColour((selected ? Palette::accent : Palette::line).withMultipliedAlpha(alpha)); g.fillRect(0, button.getHeight() - (selected ? 2 : 1), button.getWidth(), selected ? 2 : 1);
}
}
