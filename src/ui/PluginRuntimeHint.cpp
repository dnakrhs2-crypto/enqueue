#include "app/UiTranslation.h"
#include "PluginRuntimeHint.h"
#include <cmath>

namespace gocue
{
namespace { juce::String ko (const char* text) { return juce::String::fromUTF8 (text); } }

PluginRuntimeHint::Paragraph::Paragraph (juce::String t, const Style& s)
    : text (std::move (t)), font (s.body), colour (s.dimText)
{
    setName (text);
    setAccessible (true);
}
juce::TextLayout PluginRuntimeHint::Paragraph::layout (int width) const
{
    juce::AttributedString string;
    string.setWordWrap (juce::AttributedString::byWord);
    string.append (text, font, colour);
    juce::TextLayout result;
    result.createLayout (string, (float) juce::jmax (1, width));
    return result;
}
int PluginRuntimeHint::Paragraph::heightFor (int width) const
{ return (int) std::ceil (layout (width).getHeight()) + 2; }
void PluginRuntimeHint::Paragraph::paint (juce::Graphics& g)
{ layout (getWidth()).draw (g, getLocalBounds().toFloat()); }

PluginRuntimeHint::PluginRuntimeHint (Style s, Options o) : style (std::move (s)), options (std::move (o))
{
    setName ("pluginRuntimeHint");
    title.setFont (style.title);
    title.setColour (juce::Label::textColourId, style.text);
    title.setBorderSize ({});
    title.setMinimumHorizontalScale (1.0f);
    content.addAndMakeVisible (title);
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);
    setVisible (false);
}
PluginRuntimeHint::~PluginRuntimeHint() { cancelBackground(); }
void PluginRuntimeHint::cancelBackground()
{
    if (cancellation) *cancellation = true;
    cancellation.reset();
}
void PluginRuntimeHint::clear()
{
    cancelBackground();
    messages.clear();
    buttons.clear();
    footer.reset();
    titleFiles.clear();
    title.setText ({}, juce::dontSendNotification);
    setVisible (false);
    if (onChanged) onChanged();
}
void PluginRuntimeHint::showResults (const std::vector<PluginRuntimeResult>& results)
{
    clear();
    juce::StringArray files, packages;
    bool vst3 = false, vst2 = false;
    for (const auto& result : results)
    {
        if (result.packages.empty()) continue;
        auto file = juce::File (result.file).getFileName();
        if (file.startsWithIgnoreCase ("WaveShell")) file += sharedTr (" (웨이브즈)");
        files.addIfNotAlreadyThere (file);
        if (result.format == "VST" || result.format == "VST2") vst2 = true; else vst3 = true;
        for (const auto& package : result.packages) packages.addIfNotAlreadyThere (package.name);
    }
    if (packages.isEmpty()) return;
    const auto scan = ! style.vst2 || ! vst2 ? sharedTr ("[VST3 스캔]")
                    : vst3 ? sharedTr ("[VST3 스캔]·[VST2 스캔]") : sharedTr ("[VST2 스캔]");
    const auto retry = sharedTrf ("설치한 뒤 {0}을 다시 누르세요.", scan);
    const auto heading = sharedTrf ("불러오지 못한 파일: {0}", files.joinIntoString (", "));
    titleFiles = files;
    title.setText (heading, juce::dontSendNotification);
    title.setTooltip (heading);
    for (const auto& package : pluginRuntimePackages())
    {
        if (! packages.contains (package.name)) continue;
        auto message = sharedTrf ("이 PC에 {0} 런타임이 없습니다.", package.name);
        if (packages.size() == 1) message += " " + retry;
        auto* paragraph = messages.add (new Paragraph (message, style));
        content.addAndMakeVisible (paragraph);
        auto* button = buttons.add (new juce::TextButton (sharedTr ("설치 파일 받기")));
        button->setColour (juce::TextButton::buttonColourId, style.accent);
        button->setColour (juce::TextButton::textColourOffId, style.accentInk);
        button->setWantsKeyboardFocus (false);
        button->onClick = [open = options.openDownload, url = package.url] { if (open) open (url); };
        content.addAndMakeVisible (button);
    }
    if (packages.size() > 1)
    {
        footer = std::make_unique<Paragraph> (retry, style);
        content.addAndMakeVisible (*footer);
    }
    viewport.setViewPosition (0, 0);
    setVisible (true);
    resized();
    if (onChanged) onChanged();
}

void PluginRuntimeHint::checkBlacklisted (juce::StringArray files)
{
    cancelBackground();
    if (files.isEmpty()) return;
    cancellation = std::make_shared<std::atomic<bool>> (false);
    const auto stop = cancellation;
    const auto checkOptions = options.runtimeCheck;
    juce::Component::SafePointer<PluginRuntimeHint> safe (this);
    juce::Thread::launch ([safe, stop, files = std::move (files), checkOptions]
    {
        PluginRuntimeCheck check (checkOptions);
        std::vector<PluginRuntimeResult> results;
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + checkOptions.maxMilliseconds;
        const auto cancelled = [&] { return stop->load() || juce::Time::getMillisecondCounterHiRes() >= deadline; };
        for (const auto& file : files)
        {
            if (cancelled()) break;
            if (! juce::File::isAbsolutePath (file) || ! juce::File (file).exists()) continue;
            results.push_back (check.check (file, file.endsWithIgnoreCase (".dll") ? "VST" : "VST3", cancelled));
        }
        if (stop->load()) return;
        // SafePointer is only dereferenced on the message thread. Destruction never joins this worker.
        juce::MessageManager::callAsync ([safe, stop, results = std::move (results)]
        { if (! stop->load() && safe != nullptr) safe->showResults (results); });
    });
}

juce::StringArray PluginRuntimeHint::titleLines (int width) const
{
    // Keep the original single-file label, including its one-line height and elision.
    if (titleFiles.size() <= 1) return { title.getTooltip() };

    // Prefer file boundaries without replacing spaces in the displayed names or tooltip.
    juce::StringArray paragraphs;
    auto line = sharedTr ("불러오지 못한 파일: ");
    for (int i = 0; i < titleFiles.size(); ++i)
    {
        const auto piece = titleFiles[i] + (i + 1 < titleFiles.size() ? "," : "");
        const auto candidate = line + (i > 0 ? " " : "") + piece;
        if (i > 0 && juce::GlyphArrangement::getStringWidth (style.title, candidate) > (float) width)
        {
            paragraphs.add (line);
            line = piece;
        }
        else line = candidate;
    }
    paragraphs.add (line);

    const auto text = paragraphs.joinIntoString ("\n");
    juce::AttributedString attributed;
    attributed.append (text, style.title, style.text);
    attributed.setWordWrap (juce::AttributedString::byWord);
    juce::TextLayout layout;
    layout.createLayout (attributed, (float) width);
    if (layout.getWidth() > (float) width)
    {
        attributed.setWordWrap (juce::AttributedString::byChar);
        layout.createLayout (attributed, (float) width);
    }

    juce::StringArray lines;
    for (int i = 0; i < juce::jmin (3, layout.getNumLines()); ++i)
    {
        const auto range = layout.getLine (i).stringRange;
        lines.add (text.substring (range.getStart(), range.getEnd()).trim());
    }
    if (layout.getNumLines() > 3)
    {
        auto last = lines[2];
        const auto ellipsis = ko ("…");
        while (last.isNotEmpty() && juce::GlyphArrangement::getStringWidth (style.title, last + ellipsis) > (float) width)
            last = last.dropLastCharacters (1).trimEnd();
        lines.set (2, last + ellipsis);
    }
    return lines;
}

int PluginRuntimeHint::layoutContent (int width, bool apply) const
{
    if (buttons.isEmpty()) return 0;
    const auto buttonFont = buttons[0]->getLookAndFeel().getTextButtonFont (*buttons[0], 32);
    const int buttonWidth = juce::GlyphArrangement::getStringWidthInt (buttonFont, buttons[0]->getButtonText()) + 28;
    const int textWidth = juce::jmax (1, width - buttonWidth - 16);
    const bool single = buttons.size() == 1;
    const int titleWidth = single ? textWidth : width;
    const auto lines = titleLines (titleWidth);
    const int titleHeight = (int) std::ceil (style.title.getHeight() * 1.25f) * lines.size();
    if (apply)
    {
        title.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
        title.setBounds (0, 0, titleWidth, titleHeight);
    }
    int y = titleHeight + 4;
    for (int i = 0; i < messages.size(); ++i)
    {
        const int textHeight = messages[i]->heightFor (textWidth);
        const int rowHeight = single ? textHeight : juce::jmax (32, textHeight);
        if (apply)
        {
            messages[i]->setBounds (0, y + (rowHeight - textHeight) / 2, textWidth, textHeight);
            const int buttonY = single ? juce::jmax (0, (y + rowHeight - 32) / 2) : y + (rowHeight - 32) / 2;
            buttons[i]->setBounds (width - buttonWidth, buttonY, buttonWidth, 32);
        }
        y += rowHeight + 4;
    }
    if (footer)
    {
        const int height = footer->heightFor (width);
        if (apply) footer->setBounds (0, y, width, height);
        y += height + 4;
    }
    return y - 4;
}
int PluginRuntimeHint::preferredHeight (int width) const
{ return isVisible() ? layoutContent (juce::jmax (1, width - 28), false) + 20 : 0; }
void PluginRuntimeHint::resized()
{
    viewport.setBounds (getLocalBounds().reduced (14, 10));
    int width = viewport.getWidth();
    if (layoutContent (width, false) > viewport.getHeight()) width -= viewport.getScrollBarThickness();
    content.setSize (juce::jmax (1, width), layoutContent (width, true));
}
void PluginRuntimeHint::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (style.error.withAlpha (0.07f));
    g.fillRoundedRectangle (area, style.radius);
    g.setColour (style.error.withAlpha (0.45f));
    g.drawRoundedRectangle (area, style.radius, 1.0f);
    juce::Path clip;
    clip.addRoundedRectangle (area, style.radius);
    g.saveState();
    g.reduceClipRegion (clip);
    g.setColour (style.error);
    g.fillRect (0, 0, 4, getHeight());
    g.restoreState();
}
} // namespace gocue
