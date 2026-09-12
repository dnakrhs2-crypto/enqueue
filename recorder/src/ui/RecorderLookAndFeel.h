#pragma once
#include "RecorderPalette.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::recorder
{
inline juce::String ko(const char* text) { return juce::String::fromUTF8(text); }
// JUCE's point height is the em-square in logical pixels, matching CSS font-size.
inline juce::Font recorderFont(float pixels, int style = juce::Font::plain)
{ return juce::Font(juce::FontOptions(pixels, style).withPointHeight(pixels)); }
juce::Font recorderMonoFont(float height, int style = juce::Font::plain);
void styleRecorderWindow(juce::DocumentWindow&);
class RecorderLookAndFeel : public juce::LookAndFeel_V4
{
public:
    enum { buttonOutlineColourId = 0x2a00100 };
    RecorderLookAndFeel();
    juce::Font getPopupMenuFont() override;
    juce::Font getMenuBarFont(juce::MenuBarComponent&, int, const juce::String&) override;
    int getDefaultMenuBarHeight() override { return 30; }
    juce::Font getTextButtonFont(juce::TextButton&, int) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getLabelFont(juce::Label& label) override { return label.getFont(); }
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool, bool) override;
    void drawTickBox(juce::Graphics&, juce::Component&, float, float, float, float, bool, bool, bool, bool) override;
    void drawComboBox(juce::Graphics&, int, int, bool, int, int, int, int, juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    void fillTextEditorBackground(juce::Graphics&, int, int, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int, int, juce::TextEditor&) override;
    int getTabButtonOverlap(int) override { return 0; }
    int getTabButtonSpaceAroundImage() override { return 0; }
    int getTabButtonBestWidth(juce::TabBarButton&, int) override;
    void drawTabButton(juce::TabBarButton&, juce::Graphics&, bool, bool) override;
    void drawTabbedButtonBarBackground(juce::TabbedButtonBar&, juce::Graphics&) override;
    void drawTabAreaBehindFrontButton(juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}
};
}
