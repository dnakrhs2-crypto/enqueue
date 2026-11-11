#pragma once

#include "Localisation.h"
#include "MixDocument.h"
#include "LiveMixSettings.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::tests::i18nshots
{
void dispatch();
juce::Component* findComponent (juce::Component&, const std::function<bool (juce::Component&)>&);
juce::DialogWindow* dialogNamed (const juce::String&);

// Output is an inspection artifact, not a golden-image assertion. Overflow is recorded, never repaired here.
struct Capture
{
    Capture (juce::UnitTest&, const juce::File&);
    void take (juce::Component&, const juce::String& name, juce::Component* overlay = nullptr);
    void window (juce::DocumentWindow&, const juce::String& name);
    void alert (const juce::String& name);
    void finish();
    void inspect (juce::Component& root, juce::Component& node, const juce::String& image);
    void measure (const juce::String& image, const juce::String& type, const juce::String& text,
                  const juce::Font&, juce::Rectangle<int>, bool multiline = false,
                  float minimumScale = 1.0f, int maxLines = 1);

    juce::UnitTest& test;
    juce::File folder;
    juce::StringArray overflow, manifest, menus;
    int count = 0;
};

// Reads keys/options from the real application source for application-only dialogs and menus.
juce::String sourceBlock (const char* file, const char* begin, const char* end);
juce::StringArray sourceKeys (const juce::String&);
void captureBackup (Capture&, livemix::MixDocument&, livemix::LiveMixSettings&);
}
