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
    // The open device's output pairs, as a channel's direct output lists them.
    struct OutputPairs
    {
        juce::StringArray names;
        bool asio = false, hasOutput = false, running = false;
        int numOutputs = 0;
    };
    struct Actions
    {
        std::function<void (const juce::String&)> create;
        std::function<void (const juce::String&, const juce::String&)> join;
        std::function<void()> leave, rebuffer, hide;
        std::function<void (float, bool)> monitor;
        std::function<void (collab::CollabAudio::Route, int)> route;
        std::function<OutputPairs()> outputs;
        std::function<void()> createWebInvite;
        std::function<void (const juce::String&)> acceptWebRequest, rejectWebRequest;
        std::function<void (collab::contracts::ParticipantId)> kickWebGuest;
        std::function<void (collab::contracts::ParticipantId, float)> personMonitorVolume;
    };
    CollabPanel (LiveMixSettings&, Actions);
    ~CollabPanel() override;
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
    void updatePeople();
    void updateWeb();
    void layoutPerson (size_t index, juce::Rectangle<int>);
    void layoutInvite (juce::Rectangle<int>);
    struct RequestCard;
    LiveMixSettings& settings;
    Actions actions;
    collab::CollabSession::Snapshot snapshot;
    juce::Label nickCaption, codeCaption, rateCaption, roomRate, waiting, notice, peerNick, connection, volumeCaption, volumeValue,
                routeCaption;
    std::array<Chip, 3> routeChips;   // 마스터, 직접 출력, OBS에서 듣기
    juce::ComboBox routePair;
    juce::String routePairKey;        // the combo is refilled only when the pairs or the choice change
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
    juce::Label roomCode, secondNick, secondVolumeValue, inviteCaption, inviteUrl;
    juce::Slider secondVolume;
    std::array<juce::Label, 2> seatLabels, webLabels, delays;
    std::array<juce::TextButton, 2> kickButtons;
    std::array<collab::CollabSession::PersonSnapshot, 2> people;
    std::array<bool, 2> personVisible {};
    std::array<juce::Rectangle<int>, 2> personBounds;
    juce::TextButton copyInvite, renewInvite;
    juce::Rectangle<int> inviteCard, qrBounds, inviteField;
    juce::String qrUrl;
    juce::Image qrImage;
    juce::Component requestHolder;
    juce::Viewport requestViewport;
    std::vector<std::unique_ptr<RequestCard>> requests;
};

class CollabWindow : public juce::DocumentWindow, private juce::Timer
{
public:
    CollabWindow (collab::CollabSession&, LiveMixSettings&, std::function<CollabPanel::OutputPairs()> outputs = {});
    ~CollabWindow() override;
    void open();
    void closeButtonPressed() override;
    bool keyPressed (const juce::KeyPress&) override;
    void pushMeter (collab::CollabAudio::Meter m) { content->pushMeter (m, isVisible()); }

private:
    void timerCallback() override;
    collab::CollabSession& session;
    CollabPanel* content = nullptr;
    bool inviteRequested = false, expandedForRoom = false;
    juce::String roomCode;
    juce::TooltipWindow tooltips { this, 700 };   // LiveMix has no app-wide one: the full peer name and the pair note
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CollabWindow)
};
}
