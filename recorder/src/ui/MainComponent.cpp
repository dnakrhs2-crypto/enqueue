#include "MainComponent.h"
#include <chrono>

namespace gocue::recorder
{
MainComponent::MainComponent(RecorderDocument& d, RecorderSettings& s) : document(d), settings(s), trackRows(d)
{
    for (auto* button : {&projectButton, &recordTab, &timelineTab, &normalButton, &dubButton, &settingsButton, &exportButton, &startButton, &markerButton, &undoButton, &redoButton}) addAndMakeVisible(button);
    for (auto* label : {&projectName, &statusLabel, &errorLabel}) { addAndMakeVisible(label); label->setFont(juce::Font(juce::FontOptions(17.0f))); }
    projectName.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    errorLabel.setColour(juce::Label::textColourId, Palette::danger);
    addAndMakeVisible(tracksViewport); tracksViewport.setViewedComponent(&trackRows, false); tracksViewport.setScrollBarsShown(true, false);
    projectButton.onClick = [this] { projectMenu(); };
    recordTab.onClick = [this] { timeline = false; refresh(); };
    timelineTab.onClick = [this] { timeline = true; refresh(); };
    normalButton.onClick = [this] { dub = false; refresh(); };
    dubButton.onClick = [this] { dub = true; refresh(); };
    settingsButton.onClick = [this] { showSettings = !showSettings; repaint(); };
    exportButton.setEnabled(false); exportButton.setTooltip(ko("내보내기 준비 전"));
    startButton.setEnabled(false); startButton.setTooltip(ko("녹화 장치 연결 준비 전"));
    markerButton.onClick = [this] { document.performEdit(ko("마커 추가"), [](EditState& e) { Marker m; m.name = ko("마커"); e.markers.push_back(m); }); };
    undoButton.onClick = [this] { document.undo(); }; redoButton.onClick = [this] { document.redo(); };
    document.onChanged = [this] { refresh(); };
    setSize(1180, 780); refresh(); startTimer(200);
}
MainComponent::~MainComponent() { stopTimer(); document.onChanged = nullptr; }
void MainComponent::showError(const juce::String& message) { banner = message; refresh(); }
void MainComponent::refresh()
{
    projectName.setText(document.getProject().name, juce::dontSendNotification);
    const auto error = document.getError().isNotEmpty() ? document.getError() : banner;
    errorLabel.setText(error.isNotEmpty() ? error : document.getRecoveryMessage().isNotEmpty() ? document.getRecoveryMessage()
        : ko("녹화 준비 전 · 설정에서 카메라와 오디오 장치를 확인하세요."), juce::dontSendNotification);
    const bool working = fileWork.valid();
    projectButton.setEnabled(!working); undoButton.setEnabled(!working && document.getHistory().undoDepth() > 0);
    redoButton.setEnabled(!working && document.getHistory().redoDepth() > 0); markerButton.setEnabled(!working && timeline);
    recordTab.setToggleState(!timeline, juce::dontSendNotification); timelineTab.setToggleState(timeline, juce::dontSendNotification);
    normalButton.setToggleState(!dub, juce::dontSendNotification); dubButton.setToggleState(dub, juce::dontSendNotification);
    startButton.setButtonText(dub ? ko("영상 녹화 + 재생") : ko("녹화 시작"));
    undoButton.setVisible(timeline); redoButton.setVisible(timeline); tracksViewport.setVisible(timeline);
    resized(); repaint(); trackRows.repaint();
}
void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto top = area.removeFromTop(42);
    projectButton.setBounds(top.removeFromLeft(90).reduced(2));
    exportButton.setBounds(top.removeFromRight(108).reduced(2)); settingsButton.setBounds(top.removeFromRight(70).reduced(2));
    dubButton.setBounds(top.removeFromRight(62).reduced(2)); normalButton.setBounds(top.removeFromRight(62).reduced(2));
    timelineTab.setBounds(top.removeFromRight(96).reduced(2)); recordTab.setBounds(top.removeFromRight(66).reduced(2)); projectName.setBounds(top.reduced(8, 0));
    statusLabel.setBounds(area.removeFromTop(30)); errorLabel.setBounds(area.removeFromTop(44)); area.removeFromTop(8);
    auto cameras = area.removeFromTop(juce::jmin(area.getHeight() / 2 + 12, (area.getWidth() - 16) * 9 / 32 + 42));
    leftCamera = cameras.removeFromLeft((cameras.getWidth() - 16) / 2); cameras.removeFromLeft(16); rightCamera = cameras;
    area.removeFromTop(14); auto controls = area.removeFromTop(42);
    startButton.setBounds(controls.removeFromLeft(dub ? 195 : 140).reduced(2)); markerButton.setBounds(controls.removeFromLeft(120).reduced(2));
    undoButton.setBounds(controls.removeFromLeft(115).reduced(2)); redoButton.setBounds(controls.removeFromLeft(115).reduced(2));
    area.removeFromTop(10); lowerArea = area; tracksViewport.setBounds(area);
    trackRows.setSize(juce::jmax(1, area.getWidth() - 16), juce::jmax(area.getHeight(), 42 + static_cast<int>(document.getProject().tracks.size() + 2) * 54));
}
void MainComponent::cameraCard(juce::Graphics& g, juce::Rectangle<int> bounds, bool second)
{
    g.setColour(Palette::card); g.fillRoundedRectangle(bounds.toFloat(), 14.0f);
    auto inside = bounds.reduced(14); auto heading = inside.removeFromTop(30);
    g.setColour(Palette::text); g.setFont(juce::Font(juce::FontOptions(19.0f, juce::Font::bold)));
    g.drawText((second ? ko("캠2") : ko("캠1")) + ko(" · 프로젝트 ") + juce::String(document.getProject().fps.numerator), heading, juce::Justification::centredLeft);
    auto view = juce::Rectangle<int>(0, 0, juce::jmin(inside.getWidth(), inside.getHeight() * 16 / 9), juce::jmin(inside.getHeight(), inside.getWidth() * 9 / 16));
    view.setCentre(inside.getCentre()); g.setColour(Palette::meterBg); g.fillRoundedRectangle(view.toFloat(), 7.0f);
    g.setColour(Palette::dimText); g.setFont(juce::Font(juce::FontOptions(17.0f)));
    const auto text = second && !settings.get().cameraEnabled[1] ? ko("캠2 사용 안 함 · 설정에서 연결") : timeline ? ko("재생 준비 전") : ko("카메라 연결 준비 전");
    g.drawFittedText(text, view.reduced(12), juce::Justification::centred, 2);
}
void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(Palette::background); cameraCard(g, leftCamera, false); cameraCard(g, rightCamera, true);
    if (!timeline)
    {
        g.setColour(Palette::card); g.fillRoundedRectangle(lowerArea.toFloat(), 14.0f); g.setColour(Palette::dimText); g.setFont(juce::Font(juce::FontOptions(18.0f)));
        auto text = ko("마이크 · 녹음 중인 마이크가 없습니다\n설정에서 물리 입력을 선택하세요.\n녹음   ·   입력 소리 듣기");
        if (dub) text += ko("\n오디오 파일 불러오기   ·   마이크도 녹음 (꺼짐)");
        g.drawFittedText(text, lowerArea.reduced(20), juce::Justification::centredLeft, 5);
    }
    if (showSettings)
    {
        const auto box = getLocalBounds().reduced(30).withTop(130).withHeight(195);
        g.setColour(Palette::bar); g.fillRoundedRectangle(box.toFloat(), 12.0f); g.setColour(Palette::accent); g.drawRoundedRectangle(box.toFloat(), 12.0f, 1.5f);
        g.setColour(Palette::text); g.setFont(juce::Font(juce::FontOptions(18.0f)));
        const auto& s = settings.get(); const auto device = s.asioDeviceId.isEmpty() ? ko("선택 안 함") : s.asioDeviceId;
        g.drawFittedText(ko("오디오 장치 · ") + device + ko("\n재생 출력 왼쪽 / 오른쪽 · 선택한 장치에서 확인\n카메라 · 캠1 / 캠2\n동기 보정 · ")
            + (s.calibration.calibrationDate.isEmpty() ? ko("보정 결과 없음") : ko("측정됨")), box.reduced(20), juce::Justification::centredLeft, 5);
    }
}
void MainComponent::TrackRows::paint(juce::Graphics& g)
{
    g.fillAll(Palette::card); g.setFont(juce::Font(juce::FontOptions(17.0f))); g.setColour(Palette::dimText);
    g.drawText(ko("타임라인 · 00:00:00 · 공통 시간축"), 14, 0, getWidth() - 28, 36, juce::Justification::centredLeft);
    const auto& p = document.getProject(); int y = 42;
    const auto row = [&](const juce::String& name, int top)
    { g.setColour(Palette::line); g.drawHorizontalLine(top + 50, 0, static_cast<float>(getWidth())); g.setColour(Palette::text); g.drawText(name, 12, top, 150, 48, juce::Justification::centredLeft); };
    if (p.tracks.empty()) { row(ko("캠1"), y); row(ko("캠2 · 사용 안 함"), y + 54); return; }
    const auto total = juce::jmax<Sample>(p.activeTimelineEnd(), static_cast<Sample>(p.Fs) * 10);
    for (const auto& t : p.tracks)
    {
        row(t.name, y);
        for (const auto& c : t.clips.items()) if (p.isActive(c))
        {
            const auto x = 170.0f + static_cast<float>(static_cast<double>(c.timelineStartSample) / static_cast<double>(total)) * static_cast<float>(getWidth() - 180);
            const auto width = juce::jmax(2.0f, static_cast<float>(static_cast<double>(c.lengthSamples) / static_cast<double>(total)) * static_cast<float>(getWidth() - 180));
            g.setColour(Palette::accent.withAlpha(t.mute ? 0.25f : 0.65f)); g.fillRoundedRectangle(x, static_cast<float>(y + 6), width, 36.0f, 4.0f);
        }
        y += 54;
    }
}
void MainComponent::projectMenu()
{
    juce::PopupMenu menu; menu.addItem(1, ko("새 프로젝트")); menu.addItem(2, ko("프로젝트 열기")); menu.addItem(3, ko("저장"));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(projectButton), [safe = juce::Component::SafePointer<MainComponent>(this)](int choice)
    { if (safe != nullptr) { if (choice == 1 || choice == 2) safe->chooseProject(choice == 1); if (choice == 3) safe->saveProject(); } });
}
void MainComponent::beforeSwitch(std::function<void()> action)
{
    if (document.isDirty() && (document.getProject().editRevision != 0 || !document.getProject().media->assets.empty()))
    { afterSave = std::move(action); saveProject(); }
    else action();
}
void MainComponent::chooseProject(bool create)
{
    beforeSwitch([this, create]
    {
        chooser = std::make_unique<juce::FileChooser>(create ? ko("새 프로젝트 폴더 선택") : ko("프로젝트 열기"), document.getFile().getParentDirectory(), "*" + ProductIdentity::projectExtension());
        chooser->launchAsync(juce::FileBrowserComponent::openMode | (create ? juce::FileBrowserComponent::canSelectDirectories : juce::FileBrowserComponent::canSelectFiles),
            [safe = juce::Component::SafePointer<MainComponent>(this), create](const juce::FileChooser& choice)
        {
            if (safe == nullptr || choice.getResult() == juce::File()) return;
            if (!create) { safe->openProject(choice.getResult()); return; }
            const auto folder = choice.getResult(); const auto target = folder.getChildFile(ProductIdentity::projectFileName());
            safe->fileWork = std::async(std::launch::async, [folder, target]
            {
                FileResult r; r.opening = true; r.file = target; r.loaded.name = folder.getFileName();
                r.result = target.exists() ? juce::Result::fail(ko("이미 프로젝트가 있는 폴더입니다. 프로젝트 열기를 사용하세요.")) : RecorderSerializer::writeCheckpoint(target, r.loaded); return r;
            }); safe->refresh();
        });
    });
}
void MainComponent::openProject(const juce::File& file)
{
    if (fileWork.valid()) return;
    beforeSwitch([this, file]
    {
        fileWork = std::async(std::launch::async, [file] { FileResult r; r.opening = true; r.file = file; r.result = RecorderSerializer::readCheckpoint(file, r.loaded, &r.info); return r; }); refresh();
    });
}
void MainComponent::saveProject()
{
    if (fileWork.valid()) return;
    if (document.getFile() != juce::File()) { saveTo(document.getFile()); return; }
    chooser = std::make_unique<juce::FileChooser>(ko("프로젝트 저장 폴더 선택"), juce::File(), "*");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safe = juce::Component::SafePointer<MainComponent>(this)](const juce::FileChooser& choice)
    {
        if (safe == nullptr) return;
        if (choice.getResult() == juce::File()) { safe->afterSave = {}; safe->closeAction = {}; return; }
        safe->saveTo(choice.getResult().getChildFile(ProductIdentity::projectFileName()));
    });
}
void MainComponent::saveTo(const juce::File& target)
{
    const auto snapshot = document.snapshot();
    fileWork = std::async(std::launch::async, [snapshot, target] { FileResult r; r.file = target; r.written = snapshot; r.result = RecorderSerializer::writeCheckpoint(target, *snapshot); return r; }); refresh();
}
void MainComponent::requestClose(std::function<void()> action) { closeAction = std::move(action); if (!fileWork.valid()) continueClose(); }
void MainComponent::continueClose()
{
    if (!closeAction || closingSettings) return;
    if (document.isDirty() && (document.getFile() != juce::File() || document.getProject().editRevision != 0 || !document.getProject().media->assets.empty())) { saveProject(); return; }
    if (settingsWork.valid()) return;
    closingSettings = true; settingsWork = settings.save();
}
void MainComponent::timerCallback()
{
    if (fileWork.valid() && fileWork.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        auto r = fileWork.get();
        if (r.result.wasOk())
        {
            banner.clear(); if (r.opening) document.adopt(std::move(r.loaded), r.file, r.info); else document.checkpointFinished(r.written, r.file, r.result);
            settings.rememberProject(r.file); settingsPending = true;
            if (!settingsWork.valid()) { settingsPending = false; settingsWork = settings.save(); }
            if (afterSave) { auto next = std::move(afterSave); afterSave = {}; next(); }
            continueClose();
        }
        else { if (r.written) document.checkpointFinished(r.written, r.file, r.result); showError(r.result.getErrorMessage()); afterSave = {}; closeAction = {}; }
        refresh();
    }
    if (settingsWork.valid() && settingsWork.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        const auto result = settingsWork.get();
        if (result.failed()) { showError(ko("설정을 저장할 수 없습니다: ") + result.getErrorMessage()); closeAction = {}; closingSettings = false; }
        else if (closingSettings && closeAction) { auto done = std::move(closeAction); closeAction = {}; done(); return; }
        else if (settingsPending) { settingsPending = false; settingsWork = settings.save(); }
        else continueClose();
    }
    const auto path = document.getFile() == juce::File() ? juce::File() : document.getFile().getParentDirectory();
    if (spaceWork.valid() && spaceWork.wait_for(std::chrono::seconds(0)) == std::future_status::ready) remainingBytes = spaceWork.get();
    if (--spacePollCountdown <= 0 && !spaceWork.valid())
    { spacePollCountdown = 25; spaceWork = std::async(std::launch::async, [path] { return path == juce::File() ? juce::int64(-1) : path.getBytesFreeOnVolume(); }); }
    statusLabel.setText(ko("00:00:00   ·   ") + (path == juce::File() ? ko("남은 공간 · 저장 위치 선택 전") : remainingBytes < 0 ? ko("남은 공간 확인 중") : ko("남은 공간 ") + juce::String(static_cast<double>(remainingBytes) / 1000000000.0, 1) + "GB")
        + "   ·   " + (fileWork.valid() ? ko("저장 중") : document.getStatusText()), juce::dontSendNotification);
}
}
