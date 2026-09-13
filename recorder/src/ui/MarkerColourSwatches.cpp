#include "MarkerColourSwatches.h"

namespace gocue::recorder
{
namespace
{
constexpr int size = 22, gap = 6;
struct ColourChoice { juce::String name; const char* hex; };
const ColourChoice palette[] = {
    {ko("파랑"), "#4c8dff"}, {ko("빨강"), "#e0443a"}, {ko("주황"), "#f28c28"}, {ko("노랑"), "#e2a93b"},
    {ko("초록"), "#4ec27a"}, {ko("청록"), "#2bb5b5"}, {ko("보라"), "#8b7cf6"}, {ko("분홍"), "#e76fb1"},
    {ko("흰색"), "#ffffff"}, {ko("회색"), "#9a9a9a"}
};
class Swatch final : public juce::Button
{
public:
    explicit Swatch(const ColourChoice& choice) : Button(choice.name), fill(juce::Colour::fromString(juce::String("ff") + juce::String(choice.hex).substring(1)))
    {
        setTitle(choice.name); setTooltip(choice.name); setComponentID(choice.hex);
        setWantsKeyboardFocus(true); setToggleable(true);
    }
    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(fill.withMultipliedAlpha(isEnabled() ? 1.0f : 0.35f)); g.fillRoundedRectangle(bounds, 4.0f);
        const auto ink = fill == juce::Colours::white ? Palette::background : juce::Colours::white;
        g.setColour(ink.withMultipliedAlpha(isEnabled() ? 1.0f : 0.35f));
        if (getToggleState())
        {
            g.drawRoundedRectangle(bounds, 4.0f, 2.0f);
            g.fillEllipse(bounds.getCentreX() - 2.0f, bounds.getCentreY() - 2.0f, 4.0f, 4.0f);
        }
        else if (over || down || hasKeyboardFocus(false)) g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
        if (hasKeyboardFocus(false)) g.drawRoundedRectangle(bounds.reduced(3.0f), 2.0f, 1.0f);
    }
private:
    juce::Colour fill;
};
}
MarkerColourSwatches::MarkerColourSwatches()
{
    setName(ko("색"));
    for (const auto& choice : palette)
    {
        auto* button = buttons.add(new Swatch(choice)); addAndMakeVisible(button);
        button->onClick = [this, button]
        {
            if (!isEnabled() || !button->isEnabled()) return;
            const auto hex = button->getComponentID(); setSelected(hex);
            if (onPick) onPick(hex);
        };
    }
    setSelected("#4c8dff"); setSize(10 * (size + gap) - gap, size);
}
void MarkerColourSwatches::setSelected(const juce::String& hex)
{
    selectedColour = hex;
    for (auto* button : buttons) button->setToggleState(hex.equalsIgnoreCase(button->getComponentID()), juce::dontSendNotification);
}
int MarkerColourSwatches::heightForWidth(int width) const
{
    const auto columns = juce::jmax(1, (width + gap) / (size + gap));
    return ((buttons.size() + columns - 1) / columns) * (size + gap) - gap;
}
void MarkerColourSwatches::resized()
{
    const auto columns = juce::jmax(1, (getWidth() + gap) / (size + gap));
    for (int i = 0; i < buttons.size(); ++i) buttons[i]->setBounds((i % columns) * (size + gap), (i / columns) * (size + gap), size, size);
}
}
