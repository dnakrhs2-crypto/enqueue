#include "MainComponent.h"

namespace gocue::recorder
{
namespace
{
class FormWindow : public juce::DocumentWindow
{
public:
    explicit FormWindow(const juce::String& title) : DocumentWindow(title, Palette::background, closeButton) { setUsingNativeTitleBar(true); setResizable(false, false); }
    std::function<void()> onClosed;
    void closeButtonPressed() override { setVisible(false); if (onClosed) onClosed(); }
};
class SettingsForm : public juce::Component
{
public:
    AudioSettingsPanel audio; CameraSettingsPanel camera;
    juce::TabbedComponent tabs {juce::TabbedButtonBar::TabsAtTop};
    juce::Viewport audioScroll, cameraScroll;
    juce::TextButton apply {ko("적용")}, close {ko("닫기")}; juce::Label error;
    SettingsForm(const UserSettings& s, const RecorderProject& p, const RecorderAudioEngine::DeviceInfo& d) : audio(s, p, d), camera(s, p)
    {
        audioScroll.setViewedComponent(&audio, false); cameraScroll.setViewedComponent(&camera, false);
        tabs.addTab(ko("오디오 장치"), Palette::bar, &audioScroll, false); tabs.addTab(ko("카메라"), Palette::bar, &cameraScroll, false);
        addAndMakeVisible(tabs); addAndMakeVisible(apply); addAndMakeVisible(close); addAndMakeVisible(error); error.setColour(juce::Label::textColourId, Palette::danger);
    }
    void resized() override
    {
        auto a = getLocalBounds().reduced(10); auto buttons = a.removeFromBottom(36); close.setBounds(buttons.removeFromRight(80).reduced(2)); apply.setBounds(buttons.removeFromRight(80).reduced(2));
        error.setBounds(a.removeFromBottom(44)); tabs.setBounds(a);
        audio.setSize(juce::jmax(500, audioScroll.getWidth() - 16), 588); camera.setSize(juce::jmax(500, cameraScroll.getWidth() - 16), 500);
    }
};
class NewProjectForm : public juce::Component
{
public:
    juce::Label nameLabel, folderLabel, fpsLabel, error;
    juce::TextEditor name, folder;
    juce::ComboBox fps;
    juce::TextButton browse {ko("폴더 선택")}, create {ko("새 프로젝트")}, cancel {ko("취소")};
    std::unique_ptr<juce::FileChooser> chooser;
    NewProjectForm()
    {
        for (auto* l : {&nameLabel, &folderLabel, &fpsLabel, &error}) addAndMakeVisible(l);
        nameLabel.setText(ko("이름"), juce::dontSendNotification); folderLabel.setText(ko("로컬 폴더"), juce::dontSendNotification); fpsLabel.setText(ko("프레임레이트"), juce::dontSendNotification);
        addAndMakeVisible(name); addAndMakeVisible(folder); addAndMakeVisible(fps); for (auto* b : {&browse, &create, &cancel}) addAndMakeVisible(b);
        name.setText(ko("새 프로젝트")); fps.addItem("30", 30); fps.addItem("60", 60); fps.setSelectedId(30);
        browse.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser>(ko("프로젝트를 저장할 로컬 폴더"), juce::File(), juce::String()); const juce::Component::SafePointer<NewProjectForm> safe(this);
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe](const juce::FileChooser& choice)
            { if (safe) { const auto path = choice.getResult(); if (path != juce::File()) safe->folder.setText(path.getFullPathName()); safe->chooser.reset(); } });
        };
    }
    void resized() override
    {
        auto a = getLocalBounds().reduced(20); auto row = a.removeFromTop(42); nameLabel.setBounds(row.removeFromLeft(118)); name.setBounds(row.reduced(2));
        row = a.removeFromTop(42); folderLabel.setBounds(row.removeFromLeft(118)); browse.setBounds(row.removeFromRight(112).reduced(2)); folder.setBounds(row.reduced(2));
        row = a.removeFromTop(42); fpsLabel.setBounds(row.removeFromLeft(118)); fps.setBounds(row.removeFromLeft(140).reduced(2)); error.setBounds(a.removeFromTop(40));
        row = a.removeFromBottom(40); cancel.setBounds(row.removeFromRight(86).reduced(2)); create.setBounds(row.removeFromRight(140).reduced(2));
    }
};
}
void MainComponent::showSettings()
{
    if (session.busy()) return; session.enterTimeline(false); audioPanel = nullptr; settingsError = nullptr; settingsWindow.reset();
    auto window = std::make_unique<FormWindow>(ko("설정"));
    window->onClosed = [this] { if (timeline) session.enterTimeline(true); }; settingsWindow = std::move(window);
    auto* content = new SettingsForm(settings.get(), document.getProject(), session.deviceInfo()); audioPanel = &content->audio; settingsError = &content->error;
    settingsWindow->setContentOwned(content, true); settingsWindow->centreAroundComponent(this, 720, 590);
    const auto configure = [this, content](UserSettings s)
    {
        if (session.configuring()) { pendingConfigure = s; content->error.setText(ko("이전 변경을 적용한 뒤 이어서 적용합니다."), juce::dontSendNotification); return; }
        const auto result = session.configure(s); content->error.setText(result.failed() ? result.getErrorMessage() : ko("장치를 연결하는 중입니다."), juce::dontSendNotification); if (result.wasOk()) banner.clear();
    };
    // Audio edits apply immediately with the saved camera choices, so running previews are left alone.
    content->audio.onChanged = [configure](UserSettings s) { configure(s); };
    content->audio.onControlPanel = [this, content]
    { session.stopPlayback(); const auto result = session.audioEngine().showControlPanel(); if (result.failed()) content->error.setText(result.getErrorMessage(), juce::dontSendNotification); else session.configure(settings.get()); };
    content->apply.onClick = [this, content, configure]
    {
        const auto s = content->camera.read(content->audio.read(settings.get()));
        auto result = s.asioDeviceId.isEmpty() ? juce::Result::ok() : validateAudioSettings(s, session.deviceInfo(), document.getProject()); // cameras apply without an ASIO device
        if (result.wasOk()) result = content->camera.scanning() ? juce::Result::fail(ko("카메라 목록을 확인하는 중입니다.")) : validateCameraSettings(s, content->camera.catalog());
        if (result.failed()) content->error.setText(result.getErrorMessage(), juce::dontSendNotification); else configure(s);
    };
    content->close.onClick = [this] { settingsWindow->closeButtonPressed(); }; settingsWindow->setVisible(true);
}
void MainComponent::newProjectDialog()
{
    projectWindow = std::make_unique<FormWindow>(ko("새 프로젝트")); auto* content = new NewProjectForm(); projectWindow->setContentOwned(content, true); projectWindow->centreAroundComponent(this, 700, 270);
    content->cancel.onClick = [this] { projectWindow->setVisible(false); };
    content->create.onClick = [this, content]
    {
        const auto path = content->folder.getText().trim(), name = content->name.getText().trim();
        if (name.isEmpty() || !juce::File::isAbsolutePath(path) || path.startsWith("\\\\") || path.startsWith("//")) { content->error.setText(ko("프로젝트 이름과 로컬 폴더를 입력하세요."), juce::dontSendNotification); return; }
        createProject(name, juce::File(path), unsigned(content->fps.getSelectedId())); projectWindow->setVisible(false);
    }; projectWindow->setVisible(true);
}
void MainComponent::beforeSwitch(std::function<void()> action)
{
    if (session.busy() || fileWork.valid()) { showError(ko("녹화와 저장이 끝난 뒤 프로젝트를 변경하세요.")); return; }
    session.stopPlayback(); if (document.isDirty() && document.getFile() != juce::File()) { afterSave = std::move(action); saveProject(); } else action();
}
void MainComponent::projectMenu()
{
    juce::PopupMenu menu; menu.addItem(1, ko("새 프로젝트")); menu.addItem(2, ko("프로젝트 열기")); menu.addItem(3, ko("저장")); menu.addSeparator();
    int i = 100; for (const auto& file : settings.get().recentProjects) menu.addItem(i++, juce::File(file).getParentDirectory().getFileName());
    const juce::Component::SafePointer<MainComponent> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(recordView.projectButton), [safe](int id)
    {
        if (!safe) return; if (id == 1) safe->beforeSwitch([safe] { if (safe) safe->newProjectDialog(); });
        else if (id == 2) safe->chooseOpen(); else if (id == 3) safe->saveProject();
        else if (id >= 100 && id - 100 < safe->settings.get().recentProjects.size()) safe->openProject(juce::File(safe->settings.get().recentProjects[id - 100]));
    });
}
void MainComponent::createProject(const juce::String& name, const juce::File& folder, unsigned fps)
{
    if (session.busy() || fileWork.valid()) return; const auto path = folder.getFullPathName();
    if (name.trim().isEmpty() || path.startsWith("\\\\") || path.startsWith("//") || (fps != 30 && fps != 60)) { showError(ko("프로젝트 이름·로컬 폴더·프레임레이트를 확인하세요.")); return; }
    fileWork = std::async(std::launch::async, [name, folder, fps]
    {
        FileResult r; r.opening = true; r.file = folder.getChildFile(ProductIdentity::projectFileName()); r.loaded.name = name; r.loaded.fps = {fps, 1};
        if (r.file.exists() || folder.getChildFile("media").exists() || folder.getChildFile("journal").exists()) r.result = juce::Result::fail(ko("이미 프로젝트 자료가 있는 폴더입니다. 새 폴더를 선택하세요."));
        else r.result = RecorderSerializer::writeCheckpoint(r.file, r.loaded); return r;
    }); refreshPending = true;
}
void MainComponent::chooseOpen()
{
    if (chooser) return; chooser = std::make_unique<juce::FileChooser>(ko("프로젝트 열기"), juce::File(), "*" + ProductIdentity::projectExtension()); const juce::Component::SafePointer<MainComponent> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe](const juce::FileChooser& c) { if (safe) { const auto path = c.getResult(); safe->chooser.reset(); if (path != juce::File()) safe->openProject(path); } });
}
void MainComponent::openProject(const juce::File& path)
{ beforeSwitch([this, path] { fileWork = std::async(std::launch::async, [path] { FileResult r; r.opening = true; r.file = path; r.result = RecorderSerializer::readCheckpoint(path, r.loaded, &r.info); return r; }); refreshPending = true; }); }
void MainComponent::saveProject() { if (document.getFile() == juce::File()) newProjectDialog(); else saveTo(document.getFile()); }
void MainComponent::saveTo(const juce::File& path)
{
    if (fileWork.valid() || session.busy()) return; const auto snapshot = document.snapshot();
    fileWork = std::async(std::launch::async, [snapshot, path] { FileResult r; r.file = path; r.written = snapshot; r.result = RecorderSerializer::writeCheckpoint(path, *snapshot); return r; }); refreshPending = true;
}
}
