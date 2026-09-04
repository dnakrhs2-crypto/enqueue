#include "ui/YouTubeWindow.h"

#include "app/AppSettings.h"
#include "ui/UiUtils.h"

namespace gocue
{

//==============================================================================
class YouTubeWindow::Content : public juce::Component
{
public:
    Content (YouTubeWindow& w, AppSettings& s, juce::String appVersion)
        : owner (w), settings (s), downloader (std::move (appVersion))
    {
        intro.setText (ko ("유튜브 링크를 넣고 다운로드를 누르면 이 PC에서 소리만 받아 mp3(320 kbps)로 만들어 저장합니다. 한 번에 하나씩 받습니다. 처음 한 번은 도구를 준비하느라 조금 더 걸릴 수 있습니다."),
                       juce::dontSendNotification);
        intro.setColour (juce::Label::textColourId, Palette::dimText);
        intro.setJustificationType (juce::Justification::topLeft);
        intro.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (intro);

        linkLabel.setText (ko ("링크"), juce::dontSendNotification);
        addAndMakeVisible (linkLabel);

        urlEditor.setTextToShowWhenEmpty ("https://www.youtube.com/watch?v=...", Palette::dimText);
        urlEditor.setSelectAllWhenFocused (true);
        urlEditor.onReturnKey = [this] { startDownload(); };
        addAndMakeVisible (urlEditor);

        downloadButton.onClick = [this] { startDownload(); };
        addAndMakeVisible (downloadButton);
        cancelButton.onClick = [this] { downloader.cancel(); };
        addChildComponent (cancelButton);

        addToQueue.setToggleState (settings.getYouTubeAddToQueue(), juce::dontSendNotification);
        addToQueue.setTooltip (ko ("내려받자마자 큐 리스트(또는 카트)의 끝에 큐로 넣습니다"));
        addToQueue.onClick = [this] { settings.setYouTubeAddToQueue (addToQueue.getToggleState()); };
        addAndMakeVisible (addToQueue);

        folderButton.setTooltip (YouTubeWindow::downloadDirectory().getFullPathName());
        folderButton.onClick = []
        {
            const auto dir = YouTubeWindow::downloadDirectory();
            dir.createDirectory();
            dir.revealToUser();
        };
        addAndMakeVisible (folderButton);

        progressBar.setPercentageDisplay (false);
        addAndMakeVisible (progressBar);

        status.setColour (juce::Label::textColourId, Palette::dimText);
        status.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (status);

        log.setMultiLine (true, true);
        log.setReadOnly (true);
        log.setCaretVisible (false);
        log.setScrollbarsShown (true);
        log.setColour (juce::TextEditor::backgroundColourId, Palette::background);
        log.setColour (juce::TextEditor::outlineColourId, Palette::outline);
        log.setColour (juce::TextEditor::focusedOutlineColourId, Palette::outline);
        log.setTextToShowWhenEmpty (ko ("받은 파일이 여기에 표시됩니다."), Palette::dimText);
        addAndMakeVisible (log);

        downloader.onProgress = [this] (const YouTubeDownloader::Progress& p) { handleProgress (p); };
        setSize (660, 440);
    }

    void focusLink()
    {
        if (! downloader.isBusy())
            urlEditor.grabKeyboardFocus();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 14);
        intro.setBounds (area.removeFromTop (40));
        area.removeFromTop (6);

        auto row = area.removeFromTop (30);
        linkLabel.setBounds (row.removeFromLeft (44));
        downloadButton.setBounds (row.removeFromRight (110));
        cancelButton.setBounds (downloadButton.getBounds());
        row.removeFromRight (8);
        urlEditor.setBounds (row);
        area.removeFromTop (10);

        auto options = area.removeFromTop (28);
        folderButton.setBounds (options.removeFromRight (140));
        options.removeFromRight (8);
        addToQueue.setBounds (options);
        area.removeFromTop (10);

        progressBar.setBounds (area.removeFromTop (20));
        area.removeFromTop (6);
        status.setBounds (area.removeFromTop (22));
        area.removeFromTop (8);
        log.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::background);
    }

private:
    void startDownload()
    {
        if (downloader.isBusy())
            return;

        const auto url = YouTubeDownloader::normaliseUrl (urlEditor.getText());

        if (url.isEmpty())
        {
            setStatus (ko ("유튜브 링크가 아닙니다. youtube.com 또는 youtu.be 주소를 넣어 주세요."), true);
            urlEditor.grabKeyboardFocus();
            return;
        }

        if (! downloader.start (url, YouTubeWindow::downloadDirectory()))
            return;

        progressValue = -1.0;
        setBusy (true);
        setStatus (ko ("시작하는 중..."), false);
    }

    void handleProgress (const YouTubeDownloader::Progress& p)
    {
        using Stage = YouTubeDownloader::Stage;
        progressValue = p.fraction < 0.0 ? -1.0 : juce::jlimit (0.0, 1.0, p.fraction);   // < 0: the bar's busy animation

        switch (p.stage)
        {
            case Stage::done:
            {
                progressValue = 1.0;
                setBusy (false);
                auto line = ko ("✓ ") + p.file.getFileName();

                if (addToQueue.getToggleState())
                    line += owner.onAddToQueue && owner.onAddToQueue (p.file) ? ko ("  →  큐에 넣었습니다")
                                                                                : ko ("  (쇼 모드라 큐에 넣지 않았습니다)");

                appendLog (line);
                setStatus (p.message, false);
                urlEditor.clear();
                break;
            }

            case Stage::failed:
                progressValue = 0.0;
                setBusy (false);
                appendLog (ko ("✗ ") + p.message);

                for (const auto& detail : juce::StringArray::fromLines (p.detail))
                    if (detail.trim().isNotEmpty())
                        appendLog ("      " + detail.trim());

                setStatus (p.message, true);
                break;

            case Stage::cancelled:
                progressValue = 0.0;
                setBusy (false);
                setStatus (p.message, false);
                break;

            case Stage::idle:
            case Stage::preparing:
            case Stage::fetching:
            case Stage::updatingTool:
            case Stage::converting:
                setStatus (p.message, false);
                break;
        }
    }

    void setBusy (bool busy)
    {
        downloadButton.setVisible (! busy);
        cancelButton.setVisible (busy);
        urlEditor.setEnabled (! busy);
    }

    void setStatus (const juce::String& text, bool isError)
    {
        status.setText (text, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, isError ? Palette::missing : Palette::dimText);
    }

    void appendLog (const juce::String& line)
    {
        lines.add (line);
        log.setText (lines.joinIntoString ("\n"), false);
        log.moveCaretToEnd();
    }

    YouTubeWindow& owner;
    AppSettings& settings;
    YouTubeDownloader downloader;

    juce::Label intro, linkLabel, status;
    juce::TextEditor urlEditor;
    juce::TextButton downloadButton { ko ("다운로드") }, cancelButton { ko ("취소") }, folderButton { ko ("저장 폴더 열기") };
    juce::ToggleButton addToQueue { ko ("다운받고 큐에 넣기") };
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::TextEditor log;
    juce::StringArray lines;
};

//==============================================================================
YouTubeWindow::YouTubeWindow (AppSettings& settings, const juce::String& appVersion)
    : DocumentWindow (ko ("유튜브 다운로드"), Palette::background, DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);
    auto* c = new Content (*this, settings, appVersion);
    content = c;
    setContentOwned (c, true);
    setResizable (true, false);
    setResizeLimits (520, 360, 10000, 10000);
    centreWithSize (getWidth(), getHeight());
}

YouTubeWindow::~YouTubeWindow()
{
    clearContentComponent();
}

void YouTubeWindow::open()
{
    setVisible (true);
    toFront (true);

    if (content != nullptr)
        content->focusLink();
}

void YouTubeWindow::closeButtonPressed()
{
    setVisible (false);   // kept: a download in progress carries on, and reopening shows where it is
}

juce::File YouTubeWindow::downloadDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Enqueue").getChildFile (ko ("유튜브다운"));
}

} // namespace gocue
