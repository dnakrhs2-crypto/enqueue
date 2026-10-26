#include "RecordingWindow.h"
#include <cmath>

namespace gocue::livemix
{
juce::String recordingElapsedText (double seconds)
{
    const auto total = (juce::int64) std::floor (juce::jmax (0.0, seconds));
    const auto two = [] (juce::int64 n) { return juce::String (n).paddedLeft ('0', 2); };
    return (total >= 3600 ? two (total / 3600) + ":" : juce::String())
           + two ((total / 60) % 60) + ":" + two (total % 60);
}

void RecordingButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto fill = red ? Palette::brand : Palette::card2;
    if (down) fill = fill.darker (0.15f);
    else if (over) fill = fill.brighter (0.08f);
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (fill); g.fillRoundedRectangle (bounds, Palette::controlRadius);
    g.setColour (red ? Palette::brand : Palette::line);
    g.drawRoundedRectangle (bounds, Palette::controlRadius, 1.0f);
    const auto font = large ? titleFont() : getLookAndFeel().getTextButtonFont (*this, getHeight());
    const int size = large ? 12 : 10, gap = large ? 10 : 8;
    const int width = juce::GlyphArrangement::getStringWidthInt (font, getButtonText());
    const float x = (getWidth() - width - size - gap) * 0.5f;
    const juce::Rectangle<float> dot (x, (getHeight() - size) * 0.5f, (float) size, (float) size);
    g.setColour (red ? juce::Colours::white : Palette::brand);
    if (stop) g.fillRoundedRectangle (dot, 2.0f);
    else g.fillEllipse (dot);
    g.setColour (red ? juce::Colours::white : Palette::text); g.setFont (font);
    g.drawText (getButtonText(), juce::Rectangle<float> (x + size + gap, 0, (float) width + 1, (float) getHeight()),
                juce::Justification::centredLeft, false);
}

RecordingPanel::RecordingPanel (LiveMixSettings& prefs, Actions callbacks)
    : settings (prefs), actions (std::move (callbacks)), options (prefs.getRecordingOptions())
{
    auto label = [this] (juce::Label& l, const char* id, const juce::String& text, juce::Font font, juce::Colour colour)
    {
        l.setComponentID (id); l.setText (text, juce::dontSendNotification); l.setFont (font);
        l.setBorderSize (juce::BorderSize<int> (0)); l.setMinimumHorizontalScale (1.0f);
        l.setColour (juce::Label::textColourId, colour); addAndMakeVisible (l);
    };
    label (formatCaption, "format-caption", ko ("형식"), captionFont(), Palette::dimText);
    label (bitrateCaption, "bitrate-caption", ko ("비트레이트"), captionFont(), Palette::dimText);
    label (directoryCaption, "directory-caption", ko ("저장 위치"), captionFont(), Palette::dimText);
    label (directory, "directory", {}, bodyFont (15), Palette::text);
    label (status, "recording-status", ko ("녹음 중"), titleFont (14), Palette::danger);
    label (elapsed, "elapsed", {}, titleFont (26), Palette::text);
    elapsed.setJustificationType (juce::Justification::centredRight);
    label (filename, "filename", {}, bodyFont (12.5f), Palette::dimText);
    label (error, "recording-error", ko ("녹음 실패"), bodyFont (14), Palette::danger);
    for (size_t i = 0; i < formats.size(); ++i)
    {
        auto& chip = formats[i]; chip.setComponentID (i == 0 ? "format-wav" : "format-mp3");
        chip.setButtonText (i == 0 ? "WAV" : "MP3"); chip.setClickingTogglesState (false);
        chip.onClick = [this, i] { options.format = i == 0 ? recording::Format::wav : recording::Format::mp3; saveOptions(); };
        addAndMakeVisible (chip);
    }
    for (size_t i = 0; i < bitrates.size(); ++i)
    {
        const auto rate = recording::mp3BitratesKbps[i];
        auto& chip = bitrates[i]; chip.setComponentID ("bitrate-" + juce::String (rate));
        chip.setButtonText (juce::String (rate) + " kbps"); chip.setClickingTogglesState (false);
        chip.onClick = [this, rate] { options.mp3Kbps = rate; saveOptions(); };
        addAndMakeVisible (chip);
    }
    change.setComponentID ("change-directory"); change.setButtonText (ko ("변경")); change.setWantsKeyboardFocus (false);
    change.onClick = [this] { chooseDirectory(); }; addAndMakeVisible (change);
    record.setComponentID ("record"); record.setButtonText (ko ("녹음")); record.setWantsKeyboardFocus (false);
    record.red = record.large = true;
    record.onClick = [this]
    {
        if (! record.isEnabled()) return;
        if (snapshot.state == recording::State::recording)
        {
            stopPending = true;
            if (actions.stop) actions.stop();
        }
        else
        {
            settings.setRecordingOptions (options); settings.saveIfNeeded();
            startPending = actions.start && actions.start (settings.getRecordingOptions());
            startFailed = ! startPending;
        }
        refresh();
    };
    addAndMakeVisible (record);
    setSize (420, 304); refresh();
}

void RecordingPanel::saveOptions()
{
    settings.setRecordingOptions (options); settings.saveIfNeeded(); refresh();
}

void RecordingPanel::chooseDirectory()
{
    const auto path = options.directoryUtf8.empty()
        ? juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (ko ("LiveMix 녹음"))
        : juce::File (juce::String::fromUTF8 (options.directoryUtf8.c_str()));
    chooser = std::make_unique<juce::FileChooser> (ko ("저장 위치"), path, juce::String(), true, false, this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safe = juce::Component::SafePointer<RecordingPanel> (this)] (const juce::FileChooser& selected)
        {
            if (safe == nullptr || selected.getResult() == juce::File()) return;
            safe->options.directoryUtf8 = selected.getResult().getFullPathName().toStdString();
            safe->saveOptions();
        });
}

void RecordingPanel::setSnapshot (const recording::Snapshot& value)
{
    snapshot = value;
    const bool started = startPending && snapshot.state == recording::State::recording;
    if (started || snapshot.state == recording::State::error) startPending = false;
    if (snapshot.state == recording::State::idle || snapshot.state == recording::State::error) stopPending = false;
    const bool active = snapshot.state == recording::State::starting || snapshot.state == recording::State::recording
                        || snapshot.state == recording::State::stopping;
    options = active ? snapshot.options : settings.getRecordingOptions();
    refresh();
    // start() only queues the request. Close only after the writer reports that the files are open.
    if (started && actions.hide) actions.hide();
}

void RecordingPanel::refresh()
{
    const bool live = snapshot.state == recording::State::recording || snapshot.state == recording::State::stopping;
    const bool locked = live || startPending || snapshot.state == recording::State::starting;
    for (size_t i = 0; i < formats.size(); ++i)
    {
        formats[i].setEnabled (! locked); formats[i].setAlpha (locked ? 0.38f : 1.0f);
        formats[i].setToggleState ((int) options.format == (int) i, juce::dontSendNotification);
    }
    const bool bitrateEnabled = ! locked && options.format == recording::Format::mp3;
    bitrateCaption.setAlpha (bitrateEnabled ? 1.0f : 0.38f);
    for (size_t i = 0; i < bitrates.size(); ++i)
    {
        bitrates[i].setEnabled (bitrateEnabled); bitrates[i].setAlpha (bitrateEnabled ? 1.0f : 0.38f);
        bitrates[i].setToggleState (options.mp3Kbps == recording::mp3BitratesKbps[i], juce::dontSendNotification);
    }
    change.setEnabled (! locked); change.setAlpha (locked ? 0.38f : 1.0f);
    directory.setAlpha (locked ? 0.38f : 1.0f);
    formatCaption.setVisible (! live); bitrateCaption.setVisible (! live); directoryCaption.setVisible (! live);
    status.setVisible (live); elapsed.setVisible (live); filename.setVisible (live);
    elapsed.setText (recordingElapsedText (snapshot.elapsedSeconds), juce::dontSendNotification);
    error.setVisible (startFailed || snapshot.state == recording::State::error);
    record.stop = live; record.setButtonText (live ? ko ("녹음 중지") : ko ("녹음"));
    record.setEnabled (! startPending && ! stopPending && snapshot.state != recording::State::starting
                       && snapshot.state != recording::State::stopping);
    record.setAlpha (record.isEnabled() ? 1.0f : 0.38f);
    resized(); repaint();
}

void RecordingPanel::resized()
{
    const bool live = status.isVisible();
    const int gap = error.isVisible() ? 6 : 10;
    auto area = getLocalBounds().reduced (16);
    cardBounds = area.removeFromTop (live ? 218 : 227 - (error.isVisible() ? 20 : 0));
    auto inner = cardBounds.reduced (14);
    if (live)
    {
        auto row = inner.removeFromTop (32);
        elapsed.setBounds (row.removeFromRight (160)); status.setBounds (row.withTrimmedLeft (18));
        inner.removeFromTop (gap); filename.setBounds (inner.removeFromTop (15));
        juce::String name;
        if (snapshot.fileCount > 0)
        {
            const juce::File file (juce::String::fromUTF8 (snapshot.files[0].pathUtf8.c_str()));
            name = snapshot.collabAtStart ? file.getParentDirectory().getFileName() : file.getFileName();
        }
        filename.setText (ellipsisedText (filename.getFont(), name, filename.getWidth()), juce::dontSendNotification);
        filename.setTooltip (name);
    }
    else formatCaption.setBounds (inner.removeFromTop (15));
    inner.removeFromTop (gap);
    auto row = inner.removeFromTop (34);
    formats[0].setBounds (row.removeFromLeft ((row.getWidth() - 8) / 2)); row.removeFromLeft (8); formats[1].setBounds (row);
    inner.removeFromTop (gap);
    if (! live) { bitrateCaption.setBounds (inner.removeFromTop (15)); inner.removeFromTop (gap); }
    row = inner.removeFromTop (34);
    const int width = (row.getWidth() - 16) / 3;
    for (auto& bitrate : bitrates) { bitrate.setBounds (row.removeFromLeft (width)); row.removeFromLeft (8); }
    inner.removeFromTop (gap);
    if (! live) { directoryCaption.setBounds (inner.removeFromTop (15)); inner.removeFromTop (gap); }
    row = inner.removeFromTop (36); change.setBounds (row.removeFromRight (58)); row.removeFromRight (8);
    directory.setBounds (row.reduced (12, 0));
    const auto path = options.directoryUtf8.empty()
        ? juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (ko ("LiveMix 녹음")).getFullPathName()
        : juce::String::fromUTF8 (options.directoryUtf8.c_str());
    directory.setText (ellipsisedText (directory.getFont(), path, directory.getWidth()), juce::dontSendNotification);
    directory.setTooltip (path);
    if (error.isVisible()) { area.removeFromTop (3); error.setBounds (area.removeFromTop (16)); area.removeFromTop (1); }
    area.removeFromTop (12); record.setBounds (area);
}

void RecordingPanel::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
    g.setColour (Palette::card); g.fillRoundedRectangle (cardBounds.toFloat(), Palette::cardRadius);
    g.setColour (Palette::line); g.drawRoundedRectangle (cardBounds.toFloat().reduced (0.5f), Palette::cardRadius, 1.0f);
    const auto field = directory.getBounds().expanded (12, 0).toFloat().reduced (0.5f);
    g.setColour (Palette::card2); g.fillRoundedRectangle (field, Palette::controlRadius);
    g.setColour (Palette::line); g.drawRoundedRectangle (field, Palette::controlRadius, 1.0f);
    if (status.isVisible())
    {
        g.setColour (Palette::brand);
        g.fillEllipse ((float) status.getX() - 18, (float) status.getBounds().getCentreY() - 5, 10, 10);
    }
}
bool RecordingPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { if (actions.hide) actions.hide(); return true; }
    return false;
}

RecordingWindow::RecordingWindow (MixEngine& mixer, LiveMixSettings& settings)
    : DocumentWindow (ko ("녹음"), Palette::background, DocumentWindow::closeButton | DocumentWindow::minimiseButton), engine (mixer)
{
    setUsingNativeTitleBar (true);
    RecordingPanel::Actions actions;
    actions.start = [this] (const auto& options) { return engine.startRecording (options); };
    actions.stop = [this] { engine.getRecorder().requestStop(); };
    actions.hide = [this] { closeButtonPressed(); };
    content = new RecordingPanel (settings, std::move (actions));
    setContentOwned (content, true); setResizable (false, false);
    timerCallback(); startTimer (200);
}
RecordingWindow::~RecordingWindow() { stopTimer(); clearContentComponent(); }
void RecordingWindow::open() { timerCallback(); setVisible (true); toFront (true); }
void RecordingWindow::closeButtonPressed() { setVisible (false); }
bool RecordingWindow::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { closeButtonPressed(); return true; }
    return false;
}
void RecordingWindow::timerCallback() { content->setSnapshot (engine.getRecorder().snapshot()); }
}
