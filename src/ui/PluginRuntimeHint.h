#pragma once

#include "audio/PluginRuntimeCheck.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue
{
/** Shared content/behaviour; each app supplies its existing palette and fonts. */
class PluginRuntimeHint final : public juce::Component
{
public:
    struct Options
    {
        PluginRuntimeCheck::Options runtimeCheck;
        std::function<void (const juce::String&)> openDownload = [] (const juce::String& url)
        { juce::URL (url).launchInDefaultBrowser(); };
        // Optional isolated scan-state root for window fixtures (never touch the user's settings).
        juce::File scanStateDirectory;
        // Window tests use the real app worker instead of starting the test runner as a worker.
        juce::File scanExecutable;
    };
    struct Style
    {
        juce::Colour error, text, dimText, accent, accentInk;
        juce::Font body, title;
        float radius;
        bool vst2;
    };
    PluginRuntimeHint (Style, Options);
    ~PluginRuntimeHint() override;
    void showResults (const std::vector<PluginRuntimeResult>&);
    void checkBlacklisted (juce::StringArray);
    void clear();
    int preferredHeight (int width) const;
    void resized() override;
    void paint (juce::Graphics&) override;
    std::function<void()> onChanged;

private:
    class Paragraph final : public juce::Component
    {
    public:
        Paragraph (juce::String, const Style&);
        int heightFor (int width) const;
        void paint (juce::Graphics&) override;
        const juce::String text;
    private:
        juce::TextLayout layout (int width) const;
        juce::Font font;
        juce::Colour colour;
    };
    void cancelBackground();
    juce::StringArray titleLines (int width) const;
    int layoutContent (int width, bool apply) const;
    Style style;
    Options options;
    juce::Viewport viewport;
    juce::Component content;
    mutable juce::Label title;
    juce::StringArray titleFiles;
    juce::OwnedArray<Paragraph> messages;
    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<Paragraph> footer;
    std::shared_ptr<std::atomic<bool>> cancellation;
};
} // namespace gocue
