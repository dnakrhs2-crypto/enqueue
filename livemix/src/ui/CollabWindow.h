#pragma once

#include "CollabSession.h"
#include "LiveMixSettings.h"
#include "Widgets.h"

namespace gocue::livemix
{
// TextEditor keeps native text input and clipboard handling inside the existing rounded field style.
class CollabField : public juce::TextEditor
{
public:
    CollabField();
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    bool firstEmpty = false;
};

class CollabCodeEntry : public juce::Component
{
public:
    CollabCodeEntry();
    juce::String getCode() const;
    bool complete() const;
    void resized() override;
    std::function<void()> onChange, onSubmit, onEscape;

private:
    struct Digit : CollabField
    {
        Digit (CollabCodeEntry&, int);
        void insertTextAtCaret (const juce::String&) override;
        bool keyPressed (const juce::KeyPress&) override;
        CollabCodeEntry& entry;
        int index;
    };
    void insert (int index, const juce::String&);
    void backspace (int index);
    void focus (int index);
    void changed();
    std::array<std::unique_ptr<Digit>, 4> digits;
};

// Presentation accepts snapshots and actions so the exact production controls can be tested without a server.
class CollabPanel : public juce::Component
{
public:
    struct Actions
    {
        std::function<void (const juce::String&)> create;
        std::function<void (const juce::String&, const juce::String&)> join;
        std::function<void()> leave, rebuffer, hide;
        std::function<void (float, bool)> monitor;
    };
    CollabPanel (LiveMixSettings&, Actions);
    void setSnapshot (const collab::CollabSession::Snapshot&);
    void pushMeter (collab::CollabAudio::Meter m, bool repaint) { meter.push ({ m.left, m.right }, repaint); }
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    friend struct CollabPanelTestAccess;
    struct Message : juce::Component   // wraps whole Korean words (keep-all) and measures its own height
    {
        void setText (const juce::String&);
        void resized() override;
        void paint (juce::Graphics&) override;
        int heightFor (int width) const;
        juce::String text;
        juce::TextLayout layout;
        float size = 15.0f;
        juce::Colour colour = Palette::text;
    };
    struct Statistic : juce::Component
    {
        void paint (juce::Graphics&) override;
        juce::String caption, value, unit;
    };
    void updateEnabled();
    void join();
    LiveMixSettings& settings;
    Actions actions;
    collab::CollabSession::Snapshot snapshot;
    juce::Label nickCaption, codeCaption, rateCaption, roomRate, waiting, notice, peerNick, connection, volumeCaption, volumeValue;
    CollabField nickname;
    CollabCodeEntry code;
    std::array<juce::Label, 4> roomDigits;
    juce::TextButton createButton, joinButton, retryButton, dismissButton, leaveButton, rebufferButton;
    std::array<Chip, 2> rateChips;
    LampButton headphones;
    juce::Slider volume;
    MeterBar meter { true };
    Message message, reason;
    std::array<Statistic, 4> stats;
    juce::Rectangle<int> firstCard, joinCard;
};

class CollabWindow : public juce::DocumentWindow, private juce::Timer
{
public:
    CollabWindow (collab::CollabSession&, LiveMixSettings&);
    ~CollabWindow() override;
    void open();
    void closeButtonPressed() override;
    bool keyPressed (const juce::KeyPress&) override;
    void pushMeter (collab::CollabAudio::Meter m) { content->pushMeter (m, isVisible()); }

private:
    void timerCallback() override;
    collab::CollabSession& session;
    CollabPanel* content = nullptr;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CollabWindow)
};
}
