#pragma once

#include "app/YouTubeDownloader.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{

class AppSettings;

/** 유튜브다운 > 유튜브 다운로드...: a window of its own (hidden on close, like the manual). A YouTube link goes in,
    this PC fetches the audio and makes the mp3 (320 kbps), the file lands in Documents/Enqueue/유튜브다운 and,
    when "다운받고 큐에 넣기" is on, becomes a cue right away. */
class YouTubeWindow : public juce::DocumentWindow
{
public:
    YouTubeWindow (AppSettings& settings, const juce::String& appVersion);
    ~YouTubeWindow() override;

    /** The downloaded file, when "다운받고 큐에 넣기" is on. Returns false when it was not added (show mode). */
    std::function<bool (const juce::File&)> onAddToQueue;

    /** Brings the window up (a running download keeps going while it is hidden). */
    void open();
    void closeButtonPressed() override;

    /** Where the files go: Documents/Enqueue/유튜브다운. */
    static juce::File downloadDirectory();

private:
    class Content;
    Content* content = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (YouTubeWindow)
};

} // namespace gocue
