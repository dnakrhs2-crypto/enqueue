#include "ui/MainComponent.h"

#include "app/BackupManager.h"
#include "app/Commands.h"
#include "app/Updater.h"
#include "audio/CueFileInfo.h"
#include "model/CueNumbering.h"
#include "ui/AudioSettingsDialog.h"
#include "ui/PluginDialogs.h"
#include "ui/UiUtils.h"
#include "ui/PastePropertiesDialog.h"
#include "ui/PatchEditorDialog.h"
#include "ui/WorkspaceSettingsDialog.h"

#include <set>

namespace gocue
{

namespace
{
    constexpr int menuBarHeight = 24;
    constexpr int transportHeight = 112;
    constexpr int inspectorHeight = 330;
    constexpr int footerHeight = 30;
    constexpr double pluginChangeGraceMs = 1500.0;
}

bool MainComponent::HotkeyListener::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    const int code = key.getKeyCode();

    if (heldKeys.count (code) != 0)
        return owner.controller.handleHotkeyRepeat (key);   // auto-repeat of a held hotkey: swallowed

    const bool handled = owner.controller.handleHotkey (key);

    if (handled)
        heldKeys.insert (code);

    return handled;
}

bool MainComponent::HotkeyListener::keyStateChanged (bool, juce::Component*)
{
    for (auto it = heldKeys.begin(); it != heldKeys.end();)
    {
        if (! juce::KeyPress::isKeyCurrentlyDown (*it))
            it = heldKeys.erase (it);
        else
            ++it;
    }

    return false;
}

MainComponent::MainComponent (AudioEngine& e, AppSettings& s, juce::ApplicationCommandManager& cm)
    : engine (e),
      settings (s),
      commands (cm),
      controller (e, document, scheduler),
      menuBar (this),
      transport (cm),
      table (document.cues, e.getFormatManager(), cm),
      inspector (document, e, s, pluginWindows),
      activeCues (e, document.cues)
{
    setWantsKeyboardFocus (true);

    addAndMakeVisible (menuBar);
    addAndMakeVisible (transport);
    addAndMakeVisible (table);
    addAndMakeVisible (inspector);
    addAndMakeVisible (activeCues);
    addAndMakeVisible (footer);

    controller.onStatus = [this] (const juce::String& message, bool isError) { transport.showStatus (message, isError); };
    controller.onGoRejected = [this] { transport.flashGoRejected(); };

    table.onFilesDropped = [this] (const juce::StringArray& files, int insertAt) { addCuesFromFiles (files, insertAt); };
    table.onMoveRows = [this] (const std::vector<int>& rows, int insertIndex) { moveRows (rows, insertIndex); };
    table.onEditCues = [this] (const std::vector<int>& rows, const juce::String& name, const std::function<void (Cue&)>& mutator)
    {
        editCues (rows, name, mutator);
    };
    table.onEditNotes = [this] (int) { inspector.showNotes(); };
    table.hasPlayed = [this] (const juce::Uuid& id) { return controller.hasPlayed (id); };
    transport.describeFadeTarget = [this] (const Cue& fadeCue) -> juce::String
    {
        const int index = fadeCue.targetId().isNull() ? -1 : document.cues.indexOf (fadeCue.targetId());

        if (index < 0)
            return {};

        const auto& t = document.cues.get (index);
        return juce::String::fromUTF8 ("\xE2\x86\x92 ") + (t.number.isNotEmpty() ? t.number + " " : "#" + juce::String (index + 1) + " ") + t.name;
    };
    table.onEditDuration = [this] (int) { inspector.showTimeTab(); };

    inspector.onOpenPluginManager = [this] { showPluginManager(); };
    inspector.onPanic = [this] { controller.panicAll(); table.focusTable(); };
    inspector.onPreview = [this] { controller.preview(); };
    inspector.onResetCue = [this] { controller.resetSelected(); };

    footer.onShowModeChanged = [this] (bool mode) { setShowMode (mode); };
    footer.onWarningsClicked = [this] { showWarnings(); };

    document.snapshotDecorator = [this] (Project& project) { captureLivePluginStates (project); };
    document.onSnapshotRestored = [this] (const ProjectSnapshot& snapshot) { reconcileChainsAfterRestore (snapshot); };

    engine.setChainListener (&pluginWindows);
    pluginWindows.onChainChanged = [this] (PluginChain& chain)
    {
        document.markDirty();
        inspector.pluginChainChanged (&chain);
        PluginDialogs::chainChanged (&chain);
        PatchEditorDialog::chainChanged (&chain);
    };

    document.cues.addListener (this);
    document.addListener (this);

    commands.registerAllCommandsForTarget (this);
    commands.setFirstCommandTarget (this);
    addKeyListener (&hotkeyListener);            // cue hotkeys first ...
    addKeyListener (commands.getKeyMappings());  // ... then the command shortcuts
    setApplicationCommandManagerToWatch (&commands);

    setSize (1100, 820);
    updateTransportStandby();
    engine.setPatches (document.patches);   // the default patch of the empty project
    startTimerHz (30);
    scheduler.startTicking (1);   // pre-waits, post-waits, auto-follows
    controller.getFadeRunner().startTicking (10);   // fade cues at 100 Hz
}

MainComponent::~MainComponent()
{
    scheduler.stopTicking();
    controller.cancelPending();
    stopTimer();
    PluginDialogs::closeAll();
    WorkspaceSettingsDialog::closeIfOpen();
    AudioSettingsDialog::closeIfOpen();
    pluginWindows.closeAll();
    engine.setChainListener (nullptr);
    document.removeListener (this);
    document.cues.removeListener (this);
    commands.setFirstCommandTarget (nullptr);
    removeKeyListener (commands.getKeyMappings());
    removeKeyListener (&hotkeyListener);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    menuBar.setBounds (area.removeFromTop (menuBarHeight));
    transport.setBounds (area.removeFromTop (transportHeight));
    footer.setBounds (area.removeFromBottom (footerHeight));
    inspector.setBounds (area.removeFromBottom (inspectorHeight));

    activeCues.setVisible (activeCuesVisible);

    if (activeCuesVisible)
        activeCues.setBounds (area.removeFromRight (280));

    table.setBounds (area);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    if (dragOverWindow)
    {
        g.setColour (Palette::standby);
        g.drawRect (getLocalBounds(), 3);
    }
}

//==============================================================================
bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (juce::File (path).hasFileExtension (ProjectSerializer::fileExtension))
            return true;

    return ! showMode && containsAudioOrFolder (engine.getFormatManager(), files);
}

void MainComponent::fileDragEnter (const juce::StringArray&, int, int)
{
    dragOverWindow = true;
    repaint();
}

void MainComponent::fileDragExit (const juce::StringArray&)
{
    dragOverWindow = false;
    repaint();
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    dragOverWindow = false;
    repaint();

    for (const auto& path : files)
    {
        const juce::File file (path);

        if (file.existsAsFile() && file.hasFileExtension (ProjectSerializer::fileExtension))
        {
            confirmDiscardChangesThen ([this, file] { openProjectFile (file); });
            return;
        }
    }

    if (showMode)
        return;

    const auto audioFiles = collectAudioFiles (engine.getFormatManager(), files);

    if (! audioFiles.isEmpty())
        addCuesFromFiles (audioFiles, -1);
}

//==============================================================================
juce::ApplicationCommandTarget* MainComponent::getNextCommandTarget()
{
    return juce::JUCEApplication::getInstance();
}

void MainComponent::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    ids.addArray ({ CommandIDs::go, CommandIDs::pauseToggle, CommandIDs::fadeOutSelected,
                    CommandIDs::panicAll, CommandIDs::hardStopAll, CommandIDs::preview,
                    CommandIDs::auditionGo, CommandIDs::auditionPreview, CommandIDs::toggleAlwaysAudition,
                    CommandIDs::loadCue, CommandIDs::loadToTime, CommandIDs::resetCue, CommandIDs::resetAll,
                    CommandIDs::addCue, CommandIDs::addFadeCue, CommandIDs::addDevampCue, CommandIDs::revertFade, CommandIDs::fetchFadeLevels, CommandIDs::removeCue, CommandIDs::duplicateCue,
                    CommandIDs::moveCueUp, CommandIDs::moveCueDown, CommandIDs::selectAll,
                    CommandIDs::copyCues, CommandIDs::cutCues, CommandIDs::pasteCues, CommandIDs::pasteCueProperties,
                    CommandIDs::find, CommandIDs::findNext,
                    CommandIDs::renumber, CommandIDs::deleteNumbers, CommandIDs::findMissingFiles,
                    CommandIDs::saveCueTemplate, CommandIDs::clearCueTemplate,
                    CommandIDs::newProject, CommandIDs::openProject,
                    CommandIDs::saveProject, CommandIDs::saveProjectAs,
                    CommandIDs::undo, CommandIDs::redo, CommandIDs::toggleShowMode, CommandIDs::toggleActiveCues,
                    CommandIDs::audioSettings, CommandIDs::audioPatches, CommandIDs::pluginManager, CommandIDs::masterInserts,
                    CommandIDs::workspaceSettings,
                    CommandIDs::checkForUpdates, CommandIDs::about });
}

void MainComponent::getCommandInfo (juce::CommandID commandID, juce::ApplicationCommandInfo& result)
{
    const auto playback = ko ("재생");
    const auto cueMenu  = ko ("큐");
    const auto fileMenu = ko ("파일");
    const auto editMenu = ko ("편집");
    const auto audio    = ko ("오디오");
    const bool hasSelection = document.cues.getSelected() != nullptr;
    const bool canEdit = ! showMode;
    const auto& selectedRows = document.cues.getSelectedIndices();
    const int firstSelected = selectedRows.empty() ? -1 : selectedRows.front();
    const int lastSelected = selectedRows.empty() ? -1 : selectedRows.back();
    using juce::KeyPress;
    using juce::ModifierKeys;

    switch (commandID)
    {
        case CommandIDs::go:
            result.setInfo ("GO", ko ("플레이헤드 큐 재생(시퀀스 포함) 후 다음 큐로 이동 (일시정지된 큐가 있으면 재개)"), playback, 0);
            result.addDefaultKeypress (KeyPress::spaceKey, ModifierKeys::noModifiers);
            result.flags |= juce::ApplicationCommandInfo::wantsKeyUpDownCallbacks;
            break;

        case CommandIDs::pauseToggle:
            result.setInfo (ko ("일시정지 / 재개"), ko ("선택 큐(재생 중이 아니면 가장 최근 재생 큐)를 일시정지하거나 재개"), playback, 0);
            result.addDefaultKeypress ('P', ModifierKeys::noModifiers);
            break;

        case CommandIDs::fadeOutSelected:
            result.setInfo (ko ("페이드아웃 정지"), ko ("선택 큐(재생 중이 아니면 가장 최근 재생 큐)를 정지 페이드로 정지"), playback, 0);
            result.addDefaultKeypress ('F', ModifierKeys::noModifiers);
            break;

        case CommandIDs::panicAll:
            result.setInfo (ko ("전체 페이드 정지"), ko ("재생 중인 모든 큐를 설정된 시간(기본 2초) 동안 페이드아웃 후 정지. 0.5초 안에 두 번 누르면 즉시 정지"), playback, 0);
            result.addDefaultKeypress (KeyPress::escapeKey, ModifierKeys::noModifiers);
            break;

        case CommandIDs::hardStopAll:
            result.setInfo (ko ("전체 즉시 정지"), ko ("페이드 없이 모든 큐를 바로 정지"), playback, 0);
            break;

        case CommandIDs::preview:
            result.setInfo (ko ("미리듣기"), ko ("선택 큐만 재생 (프리웨이트·시퀀스 없이, 플레이헤드는 그대로)"), playback, 0);
            result.addDefaultKeypress ('V', ModifierKeys::noModifiers);
            result.setActive (hasSelection);
            break;

        case CommandIDs::auditionGo:
            result.setInfo (ko ("오디션 GO"), ko ("프로젝트 설정의 오디션 방식(그대로 / 출력 없음 / 대체 패치)으로 GO"), playback, 0);
            result.addDefaultKeypress (KeyPress::spaceKey, ModifierKeys::altModifier);
            break;

        case CommandIDs::auditionPreview:
            result.setInfo (ko ("오디션 미리듣기"), ko ("선택 큐만 오디션 방식으로 재생"), playback, 0);
            result.addDefaultKeypress ('V', ModifierKeys::altModifier);
            result.setActive (hasSelection);
            break;

        case CommandIDs::toggleAlwaysAudition:
            result.setInfo (ko ("항상 오디션"), ko ("켜면 모든 GO / 미리듣기가 오디션 방식으로 재생됩니다 (GO 버튼이 파랗게)"), playback, 0);
            result.setTicked (document.settings.alwaysAudition);
            break;

        case CommandIDs::loadCue:
            result.setInfo (ko ("로드"), ko ("선택 큐를 미리 로드해 GO 지연을 없앰"), playback, 0);
            result.addDefaultKeypress ('L', ModifierKeys::noModifiers);
            result.setActive (hasSelection);
            break;

        case CommandIDs::loadToTime:
            result.setInfo (ko ("시간으로 로드..."), ko ("선택 큐를 특정 위치에 로드 (음수 = 끝에서부터)"), playback, 0);
            result.addDefaultKeypress ('T', ModifierKeys::commandModifier);
            result.setActive (hasSelection);
            break;

        case CommandIDs::resetCue:
            result.setInfo (ko ("큐 리셋"), ko ("선택 큐를 정지하고 처음 상태로"), playback, 0);
            result.setActive (hasSelection);
            break;

        case CommandIDs::resetAll:
            result.setInfo (ko ("전체 리셋"), ko ("모든 큐를 즉시 정지하고 플레이헤드를 첫 큐로"), playback, 0);
            break;

        case CommandIDs::addCue:
            result.setInfo (ko ("큐 추가..."), ko ("오디오 파일을 골라 큐를 추가"), cueMenu, 0);
            result.addDefaultKeypress (KeyPress::insertKey, ModifierKeys::noModifiers);
            result.setActive (canEdit);
            break;

        case CommandIDs::addFadeCue:
            result.setInfo (ko ("페이드 큐 추가"), ko ("선택한 오디오 큐를 대상으로 하는 페이드 큐를 아래에 추가 (레벨·속도·플러그인 파라미터를 시간에 걸쳐 변경)"), cueMenu, 0);
            result.addDefaultKeypress ('7', ModifierKeys::commandModifier);
            result.setActive (canEdit);
            break;

        case CommandIDs::addDevampCue:
            result.setInfo (ko ("디밴프 큐 추가"), ko ("선택한 오디오 큐를 대상으로: 실행하면 대상이 지금 도는 반복을 마치고 이어가거나 멈추고, 그 순간 다음 큐를 시작"), cueMenu, 0);
            result.addDefaultKeypress ('8', ModifierKeys::commandModifier);
            result.setActive (canEdit);
            break;

        case CommandIDs::revertFade:
            result.setInfo (ko ("페이드 되돌리기"), ko ("가장 최근 페이드의 대상을 페이드 전 레벨로 되돌림"), playback, 0);
            result.addDefaultKeypress ('R', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            result.setActive (controller.getFadeRunner().canRevert());
            break;

        case CommandIDs::fetchFadeLevels:
            result.setInfo (ko ("대상에서 레벨 가져오기"), ko ("선택한 페이드 큐의 목표 레벨을 대상 큐의 현재 레벨로"), cueMenu, 0);
            result.addDefaultKeypress ('T', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            result.setActive (canEdit && hasSelection && document.cues.getSelected()->isFade());
            break;

        case CommandIDs::removeCue:
            result.setInfo (selectedRows.size() > 1 ? ko ("큐 삭제 (") + juce::String (selectedRows.size()) + ")" : ko ("큐 삭제"),
                            ko ("선택 큐 삭제"), cueMenu, 0);
            result.addDefaultKeypress (KeyPress::deleteKey, ModifierKeys::noModifiers);
            result.setActive (canEdit && hasSelection);
            break;

        case CommandIDs::duplicateCue:
            result.setInfo (ko ("큐 복제"), ko ("선택 큐를 플러그인 체인까지 바로 아래에 복제"), cueMenu, 0);
            result.addDefaultKeypress ('D', ModifierKeys::commandModifier);
            result.setActive (canEdit && hasSelection);
            break;

        case CommandIDs::moveCueUp:
            result.setInfo (ko ("위로 이동"), ko ("선택 큐를 한 칸 위로"), cueMenu, 0);
            result.addDefaultKeypress (KeyPress::upKey, ModifierKeys::commandModifier);
            result.setActive (canEdit && firstSelected > 0);
            break;

        case CommandIDs::moveCueDown:
            result.setInfo (ko ("아래로 이동"), ko ("선택 큐를 한 칸 아래로"), cueMenu, 0);
            result.addDefaultKeypress (KeyPress::downKey, ModifierKeys::commandModifier);
            result.setActive (canEdit && hasSelection && lastSelected < document.cues.size() - 1);
            break;

        case CommandIDs::selectAll:
            result.setInfo (ko ("모두 선택"), ko ("모든 큐 선택"), editMenu, 0);
            result.addDefaultKeypress ('A', ModifierKeys::commandModifier);
            result.setActive (! document.cues.isEmpty());
            break;

        case CommandIDs::copyCues:
            result.setInfo (selectedRows.size() > 1 ? ko ("큐 복사 (") + juce::String (selectedRows.size()) + ")" : ko ("큐 복사"),
                            ko ("선택 큐를 플러그인 체인까지 클립보드에 복사"), editMenu, 0);
            result.addDefaultKeypress ('C', ModifierKeys::commandModifier);
            result.setActive (hasSelection);
            break;

        case CommandIDs::cutCues:
            result.setInfo (selectedRows.size() > 1 ? ko ("큐 잘라내기 (") + juce::String (selectedRows.size()) + ")" : ko ("큐 잘라내기"),
                            ko ("선택 큐를 복사한 뒤 삭제"), editMenu, 0);
            result.addDefaultKeypress ('X', ModifierKeys::commandModifier);
            result.setActive (canEdit && hasSelection);
            break;

        case CommandIDs::pasteCues:
            result.setInfo (clipboard.cues.size() > 1 ? ko ("큐 붙여넣기 (") + juce::String (clipboard.cues.size()) + ")" : ko ("큐 붙여넣기"),
                            ko ("복사한 큐를 선택 큐 아래에 새 큐로 붙여넣기"), editMenu, 0);
            result.addDefaultKeypress ('V', ModifierKeys::commandModifier);
            result.setActive (canEdit && ! clipboard.cues.empty());
            break;

        case CommandIDs::pasteCueProperties:
            result.setInfo (ko ("큐 속성 붙여넣기..."), ko ("복사한 큐의 속성(색·시간·트리거·트림·레벨·이펙트)만 선택 큐들에 적용"), editMenu, 0);
            result.addDefaultKeypress ('V', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            result.setActive (canEdit && hasSelection && ! clipboard.cues.empty());
            break;

        case CommandIDs::find:
            result.setInfo (ko ("찾기..."), ko ("번호·이름·파일·메모로 큐 찾기"), editMenu, 0);
            result.addDefaultKeypress ('F', ModifierKeys::commandModifier);
            result.setActive (! document.cues.isEmpty());
            break;

        case CommandIDs::findNext:
            result.setInfo (ko ("다음 찾기"), ko ("같은 검색어로 다음 큐 찾기"), editMenu, 0);
            result.addDefaultKeypress (KeyPress::F3Key, ModifierKeys::noModifiers);
            result.setActive (lastSearch.isNotEmpty() && ! document.cues.isEmpty());
            break;

        case CommandIDs::saveCueTemplate:
            result.setInfo (ko ("선택 큐를 새 큐 기본값으로"), ko ("이후 추가하는 큐가 이 큐의 설정(페이드·게인·색·트리거·이펙트 등)을 물려받음 (프로젝트에 저장)"), cueMenu, 0);
            result.setActive (canEdit && hasSelection);
            break;

        case CommandIDs::clearCueTemplate:
            result.setInfo (ko ("새 큐 기본값 초기화"), ko ("새 큐를 다시 기본 설정으로 추가"), cueMenu, 0);
            result.setActive (canEdit && document.settings.hasCueTemplate);
            break;

        case CommandIDs::renumber:
            result.setInfo (ko ("선택 큐 재번호..."), ko ("선택한 큐에 순서대로 번호를 매김 (시작·증가·접두·접미)"), cueMenu, 0);
            result.addDefaultKeypress ('R', ModifierKeys::commandModifier);
            result.setActive (canEdit && hasSelection);
            break;

        case CommandIDs::deleteNumbers:
            result.setInfo (ko ("선택 큐 번호 삭제"), ko ("선택한 큐의 번호를 지움"), cueMenu, 0);
            result.setActive (canEdit && hasSelection);
            break;

        case CommandIDs::findMissingFiles:
            result.setInfo (ko ("없어진 파일 찾기..."), ko ("폴더를 골라 같은 이름의 파일로 다시 연결"), cueMenu, 0);
            result.setActive (canEdit && countBrokenCues() > 0);
            break;

        case CommandIDs::newProject:
            result.setInfo (ko ("새 프로젝트"), ko ("빈 큐 리스트로 시작"), fileMenu, 0);
            result.addDefaultKeypress ('N', ModifierKeys::commandModifier);
            break;

        case CommandIDs::openProject:
            result.setInfo (ko ("열기..."), ko (".gocue 프로젝트 열기"), fileMenu, 0);
            result.addDefaultKeypress ('O', ModifierKeys::commandModifier);
            break;

        case CommandIDs::saveProject:
            result.setInfo (ko ("저장"), ko ("프로젝트 저장 (플러그인 상태 포함)"), fileMenu, 0);
            result.addDefaultKeypress ('S', ModifierKeys::commandModifier);
            break;

        case CommandIDs::saveProjectAs:
            result.setInfo (ko ("다른 이름으로 저장..."), ko ("프로젝트를 새 파일로 저장"), fileMenu, 0);
            result.addDefaultKeypress ('S', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;

        case CommandIDs::workspaceSettings:
            result.setInfo (ko ("프로젝트 설정..."), ko ("GO 간격, 전체 페이드 정지 시간, 자동 번호, 백업 등"), fileMenu, 0);
            result.addDefaultKeypress (',', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;

        case CommandIDs::undo:
            result.setInfo (document.canUndo() ? ko ("실행 취소: ") + document.getUndoName() : ko ("실행 취소"),
                            ko ("마지막 편집을 되돌립니다"), editMenu, 0);
            result.addDefaultKeypress ('Z', ModifierKeys::commandModifier);
            result.setActive (canEdit && document.canUndo());
            break;

        case CommandIDs::redo:
            result.setInfo (document.canRedo() ? ko ("다시 실행: ") + document.getRedoName() : ko ("다시 실행"),
                            ko ("되돌린 편집을 다시 적용합니다"), editMenu, 0);
            result.addDefaultKeypress ('Y', ModifierKeys::commandModifier);
            result.addDefaultKeypress ('Z', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            result.setActive (canEdit && document.canRedo());
            break;

        case CommandIDs::toggleShowMode:
            result.setInfo (showMode ? ko ("편집 모드로") : ko ("쇼 모드로 (편집 잠금)"),
                            ko ("쇼 모드에서는 큐 추가·삭제·이동·속성 편집이 잠깁니다 (재생·저장은 그대로)"), editMenu, 0);
            result.addDefaultKeypress ('M', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;

        case CommandIDs::toggleActiveCues:
            result.setInfo (activeCuesVisible ? ko ("활성 큐 패널 숨기기") : ko ("활성 큐 패널 보이기"),
                            ko ("재생 중인 큐 목록 (일시정지·스크럽·페이드 정지)"), editMenu, 0);
            result.addDefaultKeypress ('L', ModifierKeys::commandModifier);
            break;

        case CommandIDs::audioSettings:
            result.setInfo (ko ("오디오 출력 설정..."), ko ("출력 장치(ASIO / WASAPI) 선택"), audio, 0);
            result.addDefaultKeypress (',', ModifierKeys::commandModifier);
            break;

        case CommandIDs::audioPatches:
            result.setInfo (ko ("오디오 패치..."), ko ("큐 출력 → 장치 출력 라우팅, 출력 이름, 스테레오 묶기, 출력 인서트"), audio, 0);
            result.addDefaultKeypress ('P', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;

        case CommandIDs::pluginManager:
            result.setInfo (ko ("VST3 플러그인 관리..."), ko ("VST3 플러그인 스캔 / 목록"), audio, 0);
            result.addDefaultKeypress ('P', ModifierKeys::commandModifier);
            break;

        case CommandIDs::masterInserts:
            result.setInfo (ko ("마스터 버스 인서트..."), ko ("모든 큐가 통과하는 마스터 VST3 체인"), audio, 0);
            result.addDefaultKeypress ('M', ModifierKeys::commandModifier);
            break;

        case CommandIDs::checkForUpdates:
            result.setInfo (ko ("업데이트 확인..."), ko ("GitHub Releases에서 새 버전 확인"), ko ("도움말"), 0);
            result.setActive (Updater::isAvailable());
            break;

        case CommandIDs::about:
            result.setInfo (ko ("GoCue 정보"), ko ("버전 정보"), ko ("도움말"), 0);
            break;

        default:
            break;
    }
}

bool MainComponent::perform (const InvocationInfo& info)
{
    switch (info.commandID)
    {
        case CommandIDs::go:
            if (info.invocationMethod == InvocationInfo::fromKeyPress && ! info.isKeyDown)
            {
                controller.goKeyReleased();
            }
            else
            {
                controller.go();
                table.focusTable();

                if (info.invocationMethod != InvocationInfo::fromKeyPress)
                    controller.goKeyReleased();   // a button click has no key to release
            }
            break;

        case CommandIDs::pauseToggle:
            if (! controller.togglePause())
                transport.showStatus (ko ("재생 중인 큐가 없습니다"), false);
            break;

        case CommandIDs::fadeOutSelected:
            controller.fadeOutTarget();
            break;

        case CommandIDs::panicAll:
            controller.panicAll();
            break;

        case CommandIDs::hardStopAll:
            controller.hardStopAll();
            break;

        case CommandIDs::preview:
            controller.preview();
            break;

        case CommandIDs::auditionGo:
            controller.go (true);
            controller.goKeyReleased();
            table.focusTable();
            break;

        case CommandIDs::auditionPreview:
            controller.preview (true);
            break;

        case CommandIDs::toggleAlwaysAudition:
        {
            auto s = document.settings;
            s.alwaysAudition = ! s.alwaysAudition;
            document.setSettings (s);
            transport.setAuditionMode (s.alwaysAudition);
            commands.commandStatusChanged();
            transport.showStatus (s.alwaysAudition ? ko ("항상 오디션: 켜짐") : ko ("항상 오디션: 꺼짐"), false);
            break;
        }

        case CommandIDs::loadCue:
            controller.loadSelected (0.0);
            break;

        case CommandIDs::loadToTime:
            showLoadToTimeDialog();
            break;

        case CommandIDs::resetCue:
            controller.resetSelected();
            break;

        case CommandIDs::resetAll:
            controller.resetAll();
            break;

        case CommandIDs::addCue:
            addCueViaDialog();
            break;

        case CommandIDs::addFadeCue:
            addFadeCue();
            break;

        case CommandIDs::addDevampCue:
            addDevampCue();
            break;

        case CommandIDs::fetchFadeLevels:
            inspector.fetchFadeLevelsFromTarget();
            break;

        case CommandIDs::revertFade:
            if (controller.getFadeRunner().revertLast())
                transport.showStatus (ko ("페이드 되돌림"), false);
            else
                transport.showStatus (ko ("되돌릴 페이드가 없습니다 (대상이 재생 중이어야 합니다)"), true);
            break;

        case CommandIDs::removeCue:
            removeSelectedCues();
            break;

        case CommandIDs::duplicateCue:
            duplicateSelectedCue();
            break;

        case CommandIDs::moveCueUp:
            moveSelection (-1);
            break;

        case CommandIDs::moveCueDown:
            moveSelection (1);
            break;

        case CommandIDs::selectAll:
            document.cues.selectAll();
            break;

        case CommandIDs::copyCues:
            copySelectedCues();
            break;

        case CommandIDs::cutCues:
            cutSelectedCues();
            break;

        case CommandIDs::pasteCues:
            pasteCues();
            break;

        case CommandIDs::pasteCueProperties:
            pasteCueProperties();
            break;

        case CommandIDs::find:
            showFindDialog();
            break;

        case CommandIDs::findNext:
            findNext (false);
            break;

        case CommandIDs::saveCueTemplate:
            saveCueTemplate();
            break;

        case CommandIDs::clearCueTemplate:
            clearCueTemplate();
            break;

        case CommandIDs::renumber:
            showRenumberDialog();
            break;

        case CommandIDs::deleteNumbers:
            deleteNumbersOfSelection();
            break;

        case CommandIDs::findMissingFiles:
            findMissingFiles();
            break;

        case CommandIDs::newProject:
            confirmDiscardChangesThen ([this] { newProject(); });
            break;

        case CommandIDs::openProject:
            confirmDiscardChangesThen ([this] { openProjectViaDialog(); });
            break;

        case CommandIDs::saveProject:
            saveProject (false);
            break;

        case CommandIDs::saveProjectAs:
            saveProject (true);
            break;

        case CommandIDs::workspaceSettings:
            WorkspaceSettingsDialog::show (document, this);
            break;

        case CommandIDs::undo:
            if (document.undo())
                transport.showStatus (ko ("실행 취소"), false);
            break;

        case CommandIDs::redo:
            if (document.redo())
                transport.showStatus (ko ("다시 실행"), false);
            break;

        case CommandIDs::toggleShowMode:
            setShowMode (! showMode);
            break;

        case CommandIDs::toggleActiveCues:
            activeCuesVisible = ! activeCuesVisible;
            resized();
            commands.commandStatusChanged();
            break;

        case CommandIDs::audioSettings:
            AudioSettingsDialog::show (engine.getDeviceManager(), this);
            break;

        case CommandIDs::audioPatches:
            PatchEditorDialog::show (document, engine, pluginWindows, [this] { showPluginManager(); }, this);
            break;

        case CommandIDs::pluginManager:
            showPluginManager();
            break;

        case CommandIDs::masterInserts:
            PluginDialogs::showMasterInserts (engine, pluginWindows, [this] { showPluginManager(); },
                                              [this] (const juce::String& name, const std::function<void()>& edit)
                                              {
                                                  document.perform (name, edit, { {}, true });
                                              },
                                              this);
            break;

        case CommandIDs::checkForUpdates:
            Updater::checkForUpdatesWithUI();
            break;

        case CommandIDs::about:
            showAbout();
            break;

        default:
            return false;
    }

    return true;
}

//==============================================================================
juce::StringArray MainComponent::getMenuBarNames()
{
    return { ko ("파일"), ko ("편집"), ko ("큐"), ko ("재생"), ko ("오디오"), ko ("도움말") };
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;

    switch (topLevelMenuIndex)
    {
        case 0:
            menu.addCommandItem (&commands, CommandIDs::newProject);
            menu.addCommandItem (&commands, CommandIDs::openProject);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::saveProject);
            menu.addCommandItem (&commands, CommandIDs::saveProjectAs);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::workspaceSettings);
            menu.addSeparator();
            menu.addCommandItem (&commands, juce::StandardApplicationCommandIDs::quit);
            break;

        case 1:
            menu.addCommandItem (&commands, CommandIDs::undo);
            menu.addCommandItem (&commands, CommandIDs::redo);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::cutCues);
            menu.addCommandItem (&commands, CommandIDs::copyCues);
            menu.addCommandItem (&commands, CommandIDs::pasteCues);
            menu.addCommandItem (&commands, CommandIDs::pasteCueProperties);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::find);
            menu.addCommandItem (&commands, CommandIDs::findNext);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::selectAll);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::toggleShowMode);
            menu.addCommandItem (&commands, CommandIDs::toggleActiveCues);
            break;

        case 2:
            menu.addCommandItem (&commands, CommandIDs::addCue);
            menu.addCommandItem (&commands, CommandIDs::addFadeCue);
            menu.addCommandItem (&commands, CommandIDs::addDevampCue);
            menu.addCommandItem (&commands, CommandIDs::fetchFadeLevels);
            menu.addCommandItem (&commands, CommandIDs::removeCue);
            menu.addCommandItem (&commands, CommandIDs::duplicateCue);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::moveCueUp);
            menu.addCommandItem (&commands, CommandIDs::moveCueDown);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::renumber);
            menu.addCommandItem (&commands, CommandIDs::deleteNumbers);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::findMissingFiles);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::saveCueTemplate);
            menu.addCommandItem (&commands, CommandIDs::clearCueTemplate);
            break;

        case 3:
            menu.addCommandItem (&commands, CommandIDs::go);
            menu.addCommandItem (&commands, CommandIDs::preview);
            menu.addCommandItem (&commands, CommandIDs::auditionGo);
            menu.addCommandItem (&commands, CommandIDs::auditionPreview);
            menu.addCommandItem (&commands, CommandIDs::toggleAlwaysAudition);
            menu.addCommandItem (&commands, CommandIDs::loadCue);
            menu.addCommandItem (&commands, CommandIDs::loadToTime);
            menu.addCommandItem (&commands, CommandIDs::pauseToggle);
            menu.addCommandItem (&commands, CommandIDs::fadeOutSelected);
            menu.addCommandItem (&commands, CommandIDs::resetCue);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::panicAll);
            menu.addCommandItem (&commands, CommandIDs::hardStopAll);
            menu.addCommandItem (&commands, CommandIDs::resetAll);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::revertFade);
            break;

        case 4:
            menu.addCommandItem (&commands, CommandIDs::audioSettings);
            menu.addCommandItem (&commands, CommandIDs::audioPatches);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::pluginManager);
            menu.addCommandItem (&commands, CommandIDs::masterInserts);
            break;

        case 5:
            menu.addCommandItem (&commands, CommandIDs::checkForUpdates);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::about);
            break;

        default:
            break;
    }

    return menu;
}

void MainComponent::menuItemSelected (int, int)
{
    // Command items dispatch themselves through the command manager.
}

//==============================================================================
void MainComponent::addCuesFromFiles (const juce::StringArray& files, int insertAt)
{
    if (files.isEmpty() || showMode)
        return;

    const bool copyIn = document.settings.copyFilesIntoProject && document.hasFile();
    const auto projectDir = document.getFile().getParentDirectory();
    const bool autoNumber = document.settings.autoNumber;
    const double increment = document.settings.numberIncrement;
    const bool autoLoad = document.settings.autoLoadNewCues;
    const WorkspaceSettings templateSettings = document.settings;   // hasCueTemplate / cueTemplate
    const bool templateHasPlugins = templateSettings.hasCueTemplate && ! templateSettings.cueTemplate.plugins.empty();

    document.perform (files.size() == 1 ? ko ("큐 추가") : ko ("큐 추가 (") + juce::String (files.size()) + ")",
                      [this, files, insertAt, copyIn, projectDir, autoNumber, increment, autoLoad, templateSettings]
    {
        int index = insertAt;
        int last = -1;
        std::vector<juce::Uuid> added;

        for (const auto& path : files)
        {
            const juce::File file = copyIn ? BackupManager::copyIntoProject (juce::File (path), projectDir) : juce::File (path);

            Cue cue;
            cue.name = file.getFileNameWithoutExtension();
            cue.file = file;
            cue.autoLoad = autoLoad;
            templateSettings.applyTemplate (cue);          // before the file info so the trim is clamped to this file
            refreshCueFileInfo (engine.getFormatManager(), cue);

            const int at = index < 0 ? document.cues.size() : index;

            if (autoNumber)
                cue.number = CueNumbering::next (document.cues.getAll(), at, increment);

            added.push_back (cue.id);
            last = document.cues.add (std::move (cue), index);
            index = last + 1;
        }

        if (last >= 0)
            document.cues.setSelectedIndex (last);

        restoreChainsForCues (added);
    }, { {}, templateHasPlugins });

    settings.setLastAudioDirectory (juce::File (files[0]).getParentDirectory());
}

void MainComponent::addFadeCue()
{
    if (showMode)
        return;

    const int selected = document.cues.getSelectedIndex();
    const auto* selectedCue = document.cues.getSelected();
    const bool autoNumber = document.settings.autoNumber;
    const double increment = document.settings.numberIncrement;
    const juce::Uuid target = selectedCue != nullptr && selectedCue->isAudio() ? selectedCue->id
                            : selectedCue != nullptr && selectedCue->isFade() ? selectedCue->fade.targetId : juce::Uuid::null();

    document.perform (ko ("페이드 큐 추가"), [this, selected, target, autoNumber, increment]
    {
        Cue fade;
        fade.type = CueType::fade;
        fade.fade.targetId = target;
        fade.name = ko ("페이드");

        if (const auto* t = document.cues.findById (target))
        {
            fade.name = ko ("페이드: ") + t->name;
            fade.fade.levels = t->levels;
            fade.fade.mainDb = t->gainDb;
            fade.fade.levels.resize (t->numChannels > 0 ? t->numChannels : 2, document.cueOutputsFor (*t));
            fade.fade.resizeActive (fade.fade.levels.numInputs(), fade.fade.levels.numOutputs());
            fade.fade.mainActive = true;   // the usual fade: just the main level
        }

        const int at = selected >= 0 ? selected + 1 : document.cues.size();

        if (autoNumber)
            fade.number = CueNumbering::next (document.cues.getAll(), at, increment);

        const int index = document.cues.add (std::move (fade), at);
        document.cues.setSelectedIndex (index);
    });
}

void MainComponent::addDevampCue()
{
    if (showMode)
        return;

    const int selected = document.cues.getSelectedIndex();
    const auto* selectedCue = document.cues.getSelected();
    const bool autoNumber = document.settings.autoNumber;
    const double increment = document.settings.numberIncrement;
    const juce::Uuid target = selectedCue != nullptr ? (selectedCue->isAudio() ? selectedCue->id : selectedCue->targetId()) : juce::Uuid::null();

    document.perform (ko ("디밴프 큐 추가"), [this, selected, target, autoNumber, increment]
    {
        Cue devamp;
        devamp.type = CueType::devamp;
        devamp.devamp.targetId = target;
        devamp.name = ko ("디밴프");

        if (const auto* t = document.cues.findById (target))
            devamp.name = ko ("디밴프: ") + t->name;

        const int at = selected >= 0 ? selected + 1 : document.cues.size();

        if (autoNumber)
            devamp.number = CueNumbering::next (document.cues.getAll(), at, increment);

        const int index = document.cues.add (std::move (devamp), at);
        document.cues.setSelectedIndex (index);
    });
}

void MainComponent::addCueViaDialog()
{
    if (chooser != nullptr || showMode)
        return;

    auto startDir = settings.getLastAudioDirectory();

    if (startDir == juce::File())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    chooser = std::make_unique<juce::FileChooser> (ko ("큐로 추가할 오디오 파일 선택"), startDir,
                                                   engine.getFormatManager().getWildcardForAllFormats());

    const int browseFlags = juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::canSelectMultipleItems;

    chooser->launchAsync (browseFlags, [this] (const juce::FileChooser& fc)
    {
        juce::StringArray files;

        for (const auto& f : fc.getResults())
            files.add (f.getFullPathName());

        chooser.reset();

        if (files.isEmpty())
            return;

        const int selected = document.cues.getSelectedIndex();
        addCuesFromFiles (files, selected >= 0 ? selected + 1 : -1);
    });
}

void MainComponent::removeSelectedCues()
{
    const auto rows = document.cues.getSelectedIndices();

    if (rows.empty() || showMode)
        return;

    std::vector<juce::Uuid> ids;

    for (int row : rows)
        ids.push_back (document.cues.get (row).id);

    document.perform (rows.size() == 1 ? ko ("큐 삭제") : ko ("큐 삭제 (") + juce::String (rows.size()) + ")", [this, rows, ids]
    {
        for (const auto& id : ids)
        {
            engine.stop (id);
            engine.removeCueChain (id);
        }

        document.cues.removeIndices (rows);
    }, { {}, true });
}

void MainComponent::duplicateSelectedCue()
{
    const int index = document.cues.getSelectedIndex();

    if (! document.cues.isValidIndex (index) || showMode)
        return;

    const bool autoNumber = document.settings.autoNumber;
    const double increment = document.settings.numberIncrement;

    document.perform (ko ("큐 복제"), [this, index, autoNumber, increment]
    {
        const auto sourceId = document.cues.get (index).id;
        const int newIndex = document.cues.duplicate (index);

        if (newIndex < 0)
            return;

        if (autoNumber)
        {
            const auto number = CueNumbering::next (document.cues.getAll(), newIndex, increment);
            document.cues.update (newIndex, [number] (Cue& c) { c.number = number; });
        }
        else
        {
            document.cues.update (newIndex, [] (Cue& c) { c.number.clear(); });   // numbers must stay unique
        }

        if (auto* source = engine.findCueChain (sourceId); source != nullptr && source->getNumSlots() > 0)
        {
            const auto errors = engine.getCueChain (document.cues.get (newIndex).id)
                                    .restore (source->getStates(), engine.makePluginFactory());

            if (! errors.isEmpty())
                showAlert (ko ("일부 플러그인을 복제하지 못했습니다"), errors.joinIntoString ("\n"), false);
        }

        document.cues.setSelectedIndex (newIndex);
        inspector.refreshPlugins();
    }, { {}, true });
}

void MainComponent::moveSelection (int delta)
{
    const auto rows = document.cues.getSelectedIndices();

    if (rows.empty() || showMode)
        return;

    const int first = rows.front();
    const int last = rows.back();
    int to;

    if (delta < 0)
    {
        if (first <= 0)
            return;

        to = first - 1;
    }
    else
    {
        if (last >= document.cues.size() - 1)
            return;

        to = last + 2 - (int) rows.size();
    }

    document.perform (ko ("큐 이동"), [this, rows, to] { document.cues.moveIndices (rows, to); });
}

void MainComponent::moveRows (const std::vector<int>& rows, int insertIndex)
{
    if (rows.empty() || showMode)
        return;

    int to = insertIndex;

    for (int row : rows)
        if (row < insertIndex)
            --to;

    document.perform (ko ("큐 이동"), [this, rows, to] { document.cues.moveIndices (rows, to); });
}

void MainComponent::editCues (const std::vector<int>& rows, const juce::String& name, const std::function<void (Cue&)>& mutator)
{
    if (rows.empty() || showMode)
        return;

    document.perform (name, [this, rows, mutator]
    {
        for (int row : rows)
            document.cues.update (row, mutator);
    });
}

void MainComponent::setShowMode (bool shouldBeShowMode)
{
    showMode = shouldBeShowMode;
    table.setEditable (! showMode);
    inspector.setEditable (! showMode);
    footer.setShowMode (showMode);
    commands.commandStatusChanged();
    transport.showStatus (showMode ? ko ("쇼 모드: 편집 잠김") : ko ("편집 모드"), false);
    table.focusTable();
}

namespace
{
    /** An AlertWindow shown with enterModalState() focuses itself, not its text editor: typing would go nowhere. */
    void focusAlertEditor (juce::AlertWindow& alert, const juce::String& editorName)
    {
        alert.toFront (true);

        if (auto* editor = alert.getTextEditor (editorName))
            editor->grabKeyboardFocus();
    }
}

void MainComponent::showLoadToTimeDialog()
{
    const auto* cue = document.cues.getSelected();

    if (cue == nullptr)
        return;

    auto* alert = new juce::AlertWindow (ko ("시간으로 로드"),
                                         ko ("시작 위치 (초 또는 m:ss.mmm, 음수 = 끝에서부터). 로드된 큐는 다음 GO에서 그 위치부터 재생됩니다."),
                                         juce::MessageBoxIconType::NoIcon, this);
    alert->addTextEditor ("time", "0:00.000", ko ("시작 위치"));
    alert->addButton (ko ("로드"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    alert->setVisible (true);

    juce::Component::SafePointer<MainComponent> safeThis (this);
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, alert] (int result)
    {
        if (safeThis == nullptr || result != 1)
            return;

        const auto text = alert->getTextEditorContents ("time").trim();
        const bool fromEnd = text.startsWithChar ('-');
        double seconds = parseTimeText (fromEnd ? text.substring (1) : text);

        if (seconds < 0.0)
        {
            safeThis->transport.showStatus (ko ("시간 형식을 읽을 수 없습니다: ") + text, true);
            return;
        }

        const auto* c = safeThis->document.cues.getSelected();

        if (c == nullptr)
            return;

        // the dialog speaks cue-timeline seconds (rate applied); the engine wants file seconds inside the region
        const double total = c->effectiveLength();

        if (fromEnd)
        {
            if (total < 0.0)
            {
                safeThis->transport.showStatus (ko ("무한 루프 큐는 끝에서부터 로드할 수 없습니다"), true);
                return;
            }

            seconds = juce::jmax (0.0, total - seconds);
        }

        if (total >= 0.0 && seconds >= total)
        {
            safeThis->transport.showStatus (ko ("큐 길이를 넘는 위치입니다: ") + formatTimeMs (seconds), true);
            return;
        }

        const double passSeconds = c->passLength() > 0.0 ? std::fmod (seconds, c->passLength()) : 0.0;   // inside the first pass
        safeThis->controller.loadSelected (passSeconds * c->audio.rate);
    }), true);

    focusAlertEditor (*alert, "time");
}

void MainComponent::showRenumberDialog()
{
    const auto rows = document.cues.getSelectedIndices();

    if (rows.empty() || showMode)
        return;

    auto* alert = new juce::AlertWindow (ko ("선택 큐 재번호"), ko ("선택한 큐 ") + juce::String (rows.size()) + ko ("개에 순서대로 번호를 매깁니다."),
                                         juce::MessageBoxIconType::NoIcon, this);
    alert->addTextEditor ("start", "1", ko ("시작"));
    alert->addTextEditor ("increment", juce::String (document.settings.numberIncrement, 2), ko ("증가"));
    alert->addTextEditor ("prefix", "", ko ("접두"));
    alert->addTextEditor ("suffix", "", ko ("접미"));
    alert->addButton (ko ("적용"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    alert->setVisible (true);

    juce::Component::SafePointer<MainComponent> safeThis (this);
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, alert, rows] (int result)
    {
        if (safeThis == nullptr || result != 1)
            return;

        CueNumbering::RenumberOptions options;
        options.start = alert->getTextEditorContents ("start").getDoubleValue();
        options.increment = alert->getTextEditorContents ("increment").getDoubleValue();
        options.prefix = alert->getTextEditorContents ("prefix").trim();
        options.suffix = alert->getTextEditorContents ("suffix").trim();

        if (! (options.increment > 0.0))
            options.increment = 1.0;

        const auto numbers = CueNumbering::generate ((int) rows.size(), options);

        safeThis->document.perform (ko ("재번호"), [safeThis, rows, numbers]
        {
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const auto number = numbers[i];
                safeThis->document.cues.update (rows[i], [number] (Cue& c) { c.number = number; });
            }
        });
    }), true);

    focusAlertEditor (*alert, "start");
}

void MainComponent::deleteNumbersOfSelection()
{
    editCues (document.cues.getSelectedIndices(), ko ("번호 삭제"), [] (Cue& c) { c.number.clear(); });
}

//==============================================================================
void MainComponent::copySelectedCues()
{
    const auto rows = document.cues.getSelectedIndices();

    if (rows.empty())
        return;

    clipboard.cues.clear();
    clipboard.plugins.clear();

    for (int row : rows)
    {
        const auto& cue = document.cues.get (row);
        clipboard.cues.push_back (cue);

        if (auto* chain = engine.findCueChain (cue.id))
            clipboard.plugins.push_back (chain->getStates());
        else
            clipboard.plugins.push_back (cue.plugins);
    }

    commands.commandStatusChanged();
    transport.showStatus (rows.size() == 1 ? ko ("큐 복사: ") + clipboard.cues.front().name
                                           : ko ("큐 복사: ") + juce::String (rows.size()) + ko ("개"), false);
}

void MainComponent::cutSelectedCues()
{
    if (showMode || document.cues.getSelectedIndices().empty())
        return;

    copySelectedCues();
    removeSelectedCues();
}

void MainComponent::pasteCues()
{
    if (clipboard.cues.empty() || showMode)
        return;

    const int selected = document.cues.getSelectedIndex();
    const int insertAt = selected >= 0 ? selected + 1 : document.cues.size();
    const bool autoNumber = document.settings.autoNumber;
    const double increment = document.settings.numberIncrement;
    const auto copies = clipboard.cues;
    const auto plugins = clipboard.plugins;
    bool anyPlugins = false;

    for (const auto& p : plugins)
        anyPlugins = anyPlugins || ! p.empty();

    document.perform (copies.size() == 1 ? ko ("큐 붙여넣기") : ko ("큐 붙여넣기 (") + juce::String (copies.size()) + ")",
                      [this, copies, plugins, insertAt, autoNumber, increment]
    {
        int index = insertAt;
        int last = -1;
        juce::StringArray errors;

        for (size_t i = 0; i < copies.size(); ++i)
        {
            Cue cue = copies[i];
            cue.id = juce::Uuid();                 // a new identity: the original may still exist
            cue.hotkey.clear();                    // hotkeys stay unique
            cue.plugins = plugins[i];

            if (autoNumber)
                cue.number = CueNumbering::next (document.cues.getAll(), index, increment);
            else if (cue.number.isNotEmpty() && findCueIndexByNumber (cue.number) >= 0)
                cue.number.clear();                // numbers stay unique

            const auto id = cue.id;
            last = document.cues.add (std::move (cue), index);
            index = last + 1;

            if (! plugins[i].empty())
                errors.addArray (engine.getCueChain (id).restore (plugins[i], engine.makePluginFactory()));
        }

        if (last >= 0)
            document.cues.setSelectedIndex (last);

        inspector.refreshPlugins();

        if (! errors.isEmpty())
            showAlert (ko ("일부 플러그인을 붙여넣지 못했습니다"), errors.joinIntoString ("\n"), false);
    }, { {}, anyPlugins });
}

void MainComponent::pasteCueProperties()
{
    const auto rows = document.cues.getSelectedIndices();

    if (rows.empty() || clipboard.cues.empty() || showMode)
        return;

    const Cue source = clipboard.cues.front();
    const auto sourcePlugins = clipboard.plugins.front();
    juce::Component::SafePointer<MainComponent> safeThis (this);

    PastePropertiesDialog::show (this, source.name, (int) rows.size(),
                                 [safeThis, source, sourcePlugins] (const PastePropertiesDialog::Selection& sel)
    {
        if (safeThis == nullptr || safeThis->showMode)
            return;

        auto& self = *safeThis;
        const auto targets = self.document.cues.getSelectedIndices();   // re-read: the list may have changed meanwhile

        if (targets.empty())
            return;

        self.document.perform (targets.size() == 1 ? ko ("큐 속성 붙여넣기") : ko ("큐 속성 붙여넣기 (") + juce::String (targets.size()) + ")",
                               [&self, targets, source, sourcePlugins, sel]
        {
            juce::StringArray errors;

            for (int row : targets)
            {
                self.document.cues.update (row, [&] (Cue& c)
                {
                    PastePropertiesDialog::applyProperties (source, c, sel);

                    if (sel.effects)
                        c.plugins = sourcePlugins;
                });

                if (sel.effects)
                    errors.addArray (self.engine.getCueChain (self.document.cues.get (row).id)
                                         .restore (sourcePlugins, self.engine.makePluginFactory()));
            }

            if (sel.effects)
                self.inspector.refreshPlugins();

            if (! errors.isEmpty())
                self.showAlert (ko ("일부 플러그인을 붙여넣지 못했습니다"), errors.joinIntoString ("\n"), false);
        }, { {}, sel.effects });

        self.transport.showStatus (ko ("속성 붙여넣기: ") + juce::String (targets.size()) + ko ("개 큐"), false);
    });
}

void MainComponent::showFindDialog()
{
    if (document.cues.isEmpty())
        return;

    auto* alert = new juce::AlertWindow (ko ("찾기"), ko ("번호·이름·파일 이름·메모에서 찾습니다 (대소문자 무시). F3 = 다음 찾기"),
                                         juce::MessageBoxIconType::NoIcon, this);
    alert->addTextEditor ("text", lastSearch, ko ("찾을 내용"));
    alert->addButton (ko ("찾기"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    alert->setVisible (true);

    juce::Component::SafePointer<MainComponent> safeThis (this);
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, alert] (int result)
    {
        if (safeThis == nullptr || result != 1)
            return;

        safeThis->lastSearch = alert->getTextEditorContents ("text").trim();
        safeThis->commands.commandStatusChanged();
        safeThis->findNext (true);
    }), true);

    focusAlertEditor (*alert, "text");
}

void MainComponent::findNext (bool includeCurrent)
{
    const int n = document.cues.size();

    if (lastSearch.isEmpty() || n == 0)
        return;

    const auto matches = [this] (const Cue& c)
    {
        return c.number.containsIgnoreCase (lastSearch) || c.name.containsIgnoreCase (lastSearch)
            || c.file.getFileName().containsIgnoreCase (lastSearch) || c.notes.containsIgnoreCase (lastSearch);
    };

    const int current = document.cues.getSelectedIndex();
    const int begin = current < 0 ? 0 : (includeCurrent ? current : current + 1);

    for (int step = 0; step < n; ++step)
    {
        const int i = (begin + step) % n;
        const auto& c = document.cues.get (i);

        if (! matches (c))
            continue;

        document.cues.setSelectedIndex (i);
        table.focusTable();
        transport.showStatus (ko ("찾음: ") + (c.number.isNotEmpty() ? c.number + " " : juce::String()) + c.name, false);
        return;
    }

    transport.showStatus (ko ("찾을 수 없음: ") + lastSearch, true);
}

void MainComponent::saveCueTemplate()
{
    const auto* cue = document.cues.getSelected();

    if (cue == nullptr || showMode)
        return;

    Cue t = *cue;

    if (auto* chain = engine.findCueChain (cue->id))
        t.plugins = chain->getStates();

    auto s = document.settings;
    s.hasCueTemplate = true;
    s.cueTemplate = t;
    document.setSettings (s);
    commands.commandStatusChanged();
    transport.showStatus (ko ("새 큐 기본값으로 저장: ") + cue->name, false);
}

void MainComponent::clearCueTemplate()
{
    if (showMode || ! document.settings.hasCueTemplate)
        return;

    auto s = document.settings;
    s.hasCueTemplate = false;
    s.cueTemplate = Cue();
    document.setSettings (s);
    commands.commandStatusChanged();
    transport.showStatus (ko ("새 큐 기본값 초기화"), false);
}

int MainComponent::findCueIndexByNumber (const juce::String& number) const
{
    if (number.isEmpty())
        return -1;

    for (int i = 0; i < document.cues.size(); ++i)
        if (document.cues.get (i).number == number)
            return i;

    return -1;
}

void MainComponent::restoreChainsForCues (const std::vector<juce::Uuid>& ids)
{
    juce::StringArray errors;

    for (const auto& id : ids)
    {
        const int index = document.cues.indexOf (id);

        if (index < 0)
            continue;

        const auto& cue = document.cues.get (index);

        if (! cue.plugins.empty())
            errors.addArray (engine.getCueChain (cue.id).restore (cue.plugins, engine.makePluginFactory()));
    }

    if (! ids.empty())
        inspector.refreshPlugins();

    if (! errors.isEmpty())
        showAlert (ko ("일부 플러그인을 기본값에서 복원하지 못했습니다"), errors.joinIntoString ("\n"), false);
}

void MainComponent::fireCloseCueThen (std::function<void()> then)
{
    if (closeContinuation != nullptr)   // already closing
        return;

    const int index = document.settings.startOnClose ? findCueIndexByNumber (document.settings.startOnCloseCue) : -1;

    if (index < 0)
    {
        then();
        return;
    }

    closeContinuation = std::move (then);
    closeDeadlineMs = juce::Time::getMillisecondCounterHiRes() + 120000.0;
    closeCueId = document.cues.get (index).id;
    controller.fireSequence (index);
    transport.showStatus (ko ("닫을 때 큐 재생 중: ") + document.settings.startOnCloseCue + ko (" (끝나면 종료)"), false);
}

int MainComponent::countBrokenCues() const
{
    int count = 0;

    for (const auto& cue : document.cues.getAll())
    {
        if (cue.isFade() || cue.isDevamp())
        {
            if (cue.targetId().isNull() || document.cues.indexOf (cue.targetId()) < 0)
                ++count;
        }
        else if (cue.file == juce::File() || cue.fileMissing)
        {
            ++count;
        }
    }

    return count;
}

void MainComponent::showWarnings()
{
    juce::StringArray lines;
    const auto& cues = document.cues;

    for (int i = 0; i < cues.size(); ++i)
    {
        const auto& cue = cues.get (i);
        const juce::String label = "#" + juce::String (i + 1) + (cue.number.isNotEmpty() ? " [" + cue.number + "]" : juce::String()) + " " + cue.name;

        if (cue.isFade() || cue.isDevamp())
        {
            if (cue.targetId().isNull())
                lines.add (label + ko (" - 대상 큐가 지정되지 않음"));
            else if (document.cues.indexOf (cue.targetId()) < 0)
                lines.add (label + ko (" - 대상 큐가 없음 (삭제됨)"));

            continue;
        }

        if (cue.file == juce::File())
            lines.add (label + ko (" - 파일이 지정되지 않음"));
        else if (cue.fileMissing)
            lines.add (label + ko (" - 파일 없음: ") + cue.file.getFullPathName());
    }

    if (lines.isEmpty())
        lines.add (ko ("문제 있는 큐가 없습니다."));
    else
        lines.add (juce::String() + "\n" + ko ("큐 > 없어진 파일 찾기... 로 다른 폴더에서 같은 이름의 파일을 다시 연결할 수 있습니다."));

    showAlert (ko ("경고 (") + juce::String (countBrokenCues()) + ")", lines.joinIntoString ("\n"), false);
}

void MainComponent::findMissingFiles()
{
    if (chooser != nullptr || showMode)
        return;

    auto startDir = settings.getLastAudioDirectory();

    if (startDir == juce::File())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    chooser = std::make_unique<juce::FileChooser> (ko ("없어진 파일을 찾을 폴더 선택 (하위 폴더까지 검색)"), startDir);
    const int browseFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

    chooser->launchAsync (browseFlags, [this] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        chooser.reset();

        if (dir == juce::File() || ! dir.isDirectory())
            return;

        std::vector<std::pair<int, juce::File>> found;

        for (int i = 0; i < document.cues.size(); ++i)
        {
            const auto& cue = document.cues.get (i);

            if (! cue.fileMissing || cue.file == juce::File())
                continue;

            const auto matches = dir.findChildFiles (juce::File::findFiles, true, cue.file.getFileName());

            if (! matches.isEmpty())
                found.emplace_back (i, matches[0]);
        }

        if (found.empty())
        {
            showAlert (ko ("없어진 파일 찾기"), ko ("이 폴더에서 같은 이름의 파일을 찾지 못했습니다."), false);
            return;
        }

        auto& formats = engine.getFormatManager();
        document.perform (ko ("파일 다시 연결"), [this, found, &formats]
        {
            for (const auto& [index, file] : found)
                document.cues.update (index, [file, &formats] (Cue& c) { c.file = file; refreshCueFileInfo (formats, c); });
        });

        showAlert (ko ("없어진 파일 찾기"), juce::String (found.size()) + ko ("개 파일을 다시 연결했습니다."), false);
    });
}

void MainComponent::newProject()
{
    WorkspaceSettingsDialog::closeIfOpen();   // it edits the document that is about to be replaced
    PatchEditorDialog::closeIfOpen();
    controller.cancelPending();
    pluginWindows.closeAll();
    engine.stopAll();
    engine.clearCueChains();
    engine.getMasterChain().clear();
    document.newProject();
    engine.setPatches (document.patches);
    controller.clearPlayed();
    autoLoadedId = juce::Uuid::null();
    ignorePluginChangesBriefly();
}

void MainComponent::openProjectViaDialog()
{
    if (chooser != nullptr)
        return;

    auto startDir = settings.getLastProjectFile().getParentDirectory();

    if (startDir == juce::File())
        startDir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    chooser = std::make_unique<juce::FileChooser> (ko ("프로젝트 열기"), startDir,
                                                   "*" + juce::String (ProjectSerializer::fileExtension));

    const int browseFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync (browseFlags, [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        chooser.reset();

        if (file != juce::File())
            openProjectFile (file);
    });
}

void MainComponent::openProjectFile (const juce::File& file)
{
    // Validate first: a broken file must leave the current project (and its plugin chains) untouched.
    juce::StringArray warnings;
    Project candidate;
    const auto result = ProjectDocument::parse (file, candidate, &warnings);

    if (result.failed())
    {
        showAlert (ko ("프로젝트 열기 실패"), result.getErrorMessage(), true);
        return;
    }

    WorkspaceSettingsDialog::closeIfOpen();
    PatchEditorDialog::closeIfOpen();
    controller.cancelPending();
    pluginWindows.closeAll();
    engine.stopAll();
    engine.clearCueChains();
    engine.getMasterChain().clear();
    document.adopt (std::move (candidate), file);
    controller.clearPlayed();
    autoLoadedId = juce::Uuid::null();
    document.cues.setLockPlayheadToSelection (document.settings.lockPlayheadToSelection);

    settings.setLastProjectFile (file);
    refreshFileInfoForAllCues();
    restorePluginChainsFromDocument (warnings);
    ignorePluginChangesBriefly();   // restoring saved plugin state is not an edit
    document.markClean();
    nextAutoBackupMs = juce::Time::getMillisecondCounterHiRes() + document.settings.backupIntervalSeconds * 1000.0;
    inspector.refreshPlugins();
    transport.showStatus (ko ("열림: ") + file.getFileName(), false);

    if (! warnings.isEmpty())
        showAlert (ko ("프로젝트를 열었지만 확인이 필요합니다"), warnings.joinIntoString ("\n"), false);

    if (document.settings.startOnOpen)
    {
        if (const int i = findCueIndexByNumber (document.settings.startOnOpenCue); i >= 0)
        {
            controller.fireSequence (i);
            transport.showStatus (ko ("열 때 시작: ") + document.settings.startOnOpenCue, false);
        }
    }
}

void MainComponent::restorePluginChainsFromDocument (juce::StringArray& errors)
{
    const auto factory = engine.makePluginFactory();

    for (const auto& cue : document.cues.getAll())
        if (! cue.plugins.empty())
            errors.addArray (engine.getCueChain (cue.id).restore (cue.plugins, factory));

    if (! document.masterPlugins.empty())
        errors.addArray (engine.getMasterChain().restore (document.masterPlugins, factory));

    errors.addArray (engine.setPatches (document.patches));
}

void MainComponent::captureLivePluginStates (Project& project)
{
    for (auto& patch : project.patches)
        engine.capturePatchInsertStates (patch);

    for (auto& cue : project.cues)
        if (auto* chain = engine.findCueChain (cue.id))
            cue.plugins = chain->getStates();

    project.masterPlugins = engine.getMasterChain().getStates();
}

void MainComponent::ignorePluginChangesBriefly()
{
    engine.consumePluginStateChanges();
    ignorePluginChangesUntilMs = juce::Time::getMillisecondCounterHiRes() + pluginChangeGraceMs;
}

void MainComponent::reconcileChainsAfterRestore (const ProjectSnapshot& snapshot)
{
    const auto factory = engine.makePluginFactory();
    juce::StringArray errors;
    std::set<juce::String> liveIds;

    for (const auto& cue : document.cues.getAll())
    {
        liveIds.insert (cue.id.toString());
        auto* chain = engine.findCueChain (cue.id);

        if (chain == nullptr)
        {
            if (! cue.plugins.empty())
                errors.addArray (engine.getCueChain (cue.id).restore (cue.plugins, factory));
        }
        else if (snapshot.pluginStatesCaptured)
        {
            if (chain->matchesStructure (cue.plugins))
                chain->applyStates (cue.plugins);   // same plugins, restored preset / parameters
            else
                errors.addArray (chain->restore (cue.plugins, factory));
        }
    }

    for (const auto& id : engine.getCueChainIds())
        if (liveIds.count (id.toString()) == 0)
            engine.removeCueChain (id);

    if (snapshot.pluginStatesCaptured)
    {
        if (engine.getMasterChain().matchesStructure (document.masterPlugins))
            engine.getMasterChain().applyStates (document.masterPlugins);
        else
            errors.addArray (engine.getMasterChain().restore (document.masterPlugins, factory));
    }

    // running players follow the restored model: orphans stop, the others take the restored live values
    for (const auto& p : engine.getPlayingCues())
    {
        const int index = document.cues.indexOf (p.id);

        if (index < 0)
        {
            engine.stop (p.id);
            continue;
        }

        const auto& cue = document.cues.get (index);
        engine.setLiveGainDb (p.id, cue.gainDb);
        engine.setLiveRate (p.id, cue.audio.rate);
        engine.setLiveRegion (p.id, cue.audio.startSeconds, cue.audio.endSeconds);
    }

    ignorePluginChangesBriefly();   // replaying saved state is not a new edit
    inspector.refreshPlugins();
    PluginDialogs::chainChanged (&engine.getMasterChain());

    if (! errors.isEmpty())
        showAlert (ko ("일부 플러그인을 되돌리지 못했습니다"), errors.joinIntoString ("\n"), false);
}

void MainComponent::openProjectFromCommandLine (const juce::String& commandLine)
{
    const juce::ArgumentList args ("GoCue", commandLine);

    for (const auto& arg : args.arguments)
    {
        const auto file = arg.resolveAsFile();

        if (file.existsAsFile() && file.hasFileExtension (ProjectSerializer::fileExtension))
        {
            openProjectFile (file);
            return;
        }
    }
}

void MainComponent::backupBeforeSave (const juce::File& file)
{
    const auto& s = document.settings;

    if (! s.backupBeforeSave || ! file.existsAsFile())
        return;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const auto key = file.getFullPathName();

    if (const auto it = lastSaveBackupByPath.find (key); it != lastSaveBackupByPath.end() && nowMs - it->second < 60 * 1000.0)
        return;   // at most one pre-save backup per minute for this file

    const auto result = BackupManager::copyToBackups (file, juce::Time::getCurrentTime());

    if (result.failed())
    {
        transport.showStatus (ko ("백업 실패: ") + result.getErrorMessage(), true);
        return;
    }

    lastSaveBackupByPath[key] = nowMs;

    if (s.rotateBackups)
        BackupManager::rotate (BackupManager::backupDirFor (file), juce::Time::getCurrentTime());
}

void MainComponent::autoBackupIfDue()
{
    const auto& s = document.settings;

    if (! s.autoBackup || ! document.hasFile() || ! document.isDirty())
        return;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();

    if (nowMs < nextAutoBackupMs)
        return;

    nextAutoBackupMs = nowMs + s.backupIntervalSeconds * 1000.0;

    // the unsaved state itself goes into the backup folder, so a crash loses at most one interval
    auto project = document.toProject();
    captureLivePluginStates (project);

    const auto target = BackupManager::makeUniqueBackupFile (document.getFile(), juce::Time::getCurrentTime());
    const auto result = ProjectSerializer::save (project, target);

    if (result.failed())
    {
        transport.showStatus (ko ("자동 백업 실패: ") + result.getErrorMessage(), true);
        return;
    }

    if (s.rotateBackups)
        BackupManager::rotate (target.getParentDirectory(), juce::Time::getCurrentTime());
}

void MainComponent::saveProject (bool saveAs, std::function<void (bool)> then)
{
    auto writeTo = [this, then] (juce::File file)
    {
        if (! file.hasFileExtension (ProjectSerializer::fileExtension))
            file = file.withFileExtension (ProjectSerializer::fileExtension);

        backupBeforeSave (file);

        const auto result = document.save (file, [this] (Project& project) { captureLivePluginStates (project); });
        nextAutoBackupMs = juce::Time::getMillisecondCounterHiRes() + document.settings.backupIntervalSeconds * 1000.0;

        if (result.failed())
        {
            showAlert (ko ("저장 실패"), result.getErrorMessage(), true);

            if (then)
                then (false);

            return;
        }

        settings.setLastProjectFile (file);
        transport.showStatus (ko ("저장됨: ") + file.getFileName(), false);

        if (then)
            then (true);
    };

    if (! saveAs && document.hasFile())
    {
        writeTo (document.getFile());
        return;
    }

    if (chooser != nullptr)
    {
        if (then)
            then (false);

        return;
    }

    juce::File suggested;

    if (document.hasFile())
    {
        suggested = document.getFile();
    }
    else
    {
        auto dir = settings.getLastProjectFile().getParentDirectory();

        if (dir == juce::File())
            dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

        suggested = dir.getChildFile (ko ("새 프로젝트") + juce::String (ProjectSerializer::fileExtension));
    }

    chooser = std::make_unique<juce::FileChooser> (ko ("프로젝트 저장"), suggested,
                                                   "*" + juce::String (ProjectSerializer::fileExtension));

    const int browseFlags = juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser->launchAsync (browseFlags, [this, writeTo, then] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        chooser.reset();

        if (file == juce::File())
        {
            if (then)
                then (false);

            return;
        }

        writeTo (file);
    });
}

void MainComponent::confirmDiscardChangesThen (std::function<void()> action)
{
    if (! document.isDirty())
    {
        if (action)
            action();

        return;
    }

    if (discardDialog != nullptr)   // already asking (a second close request while the dialog is up)
    {
        discardDialog->toFront (true);
        return;
    }

    auto* alert = new juce::AlertWindow (ko ("변경 사항 저장"),
                                         "\"" + document.getDisplayName() + "\"" + ko ("에 저장하지 않은 변경 사항이 있습니다. 저장할까요?"),
                                         juce::MessageBoxIconType::QuestionIcon, this);
    alert->addButton (ko ("저장"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("저장 안 함"), 2);
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    alert->setVisible (true);
    discardDialog = alert;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, action] (int result)
    {
        if (safeThis == nullptr)
            return;

        if (result == 1)
            safeThis->saveProject (false, [action] (bool ok) { if (ok && action) action(); });
        else if (result == 2 && action)
            action();
    }), true);
}

void MainComponent::refreshFileInfoForAllCues()
{
    const bool wasDirty = document.isDirty();
    auto& formats = engine.getFormatManager();

    for (int i = 0; i < document.cues.size(); ++i)
        document.cues.update (i, [&formats] (Cue& c) { refreshCueFileInfo (formats, c); });

    if (! wasDirty)
        document.markClean();
}

void MainComponent::showPluginManager()
{
    PluginDialogs::showPluginManager (engine, settings, this);
}

void MainComponent::showAbout()
{
    juce::String text;
    text << "GoCue " << juce::JUCEApplication::getInstance()->getApplicationVersion() << "\n"
         << ko ("Windows용 오디오 큐 플레이어 (QLab 오디오 기능 부분집합)") << "\n\n"
         << "JUCE " << JUCE_MAJOR_VERSION << "." << JUCE_MINOR_VERSION << "." << JUCE_BUILDNUMBER << "\n"
         << ko ("ASIO: ") << (JUCE_ASIO ? ko ("지원") : ko ("미포함 (WASAPI 전용)")) << "\n"
         << ko ("자동 업데이트: ") << (Updater::isAvailable() ? Updater::getAppcastUrl() : ko ("비활성 (빌드 시 appcast 미설정)"));

    showAlert (ko ("GoCue 정보"), text, false);
}

void MainComponent::showAlert (const juce::String& title, const juce::String& message, bool isError)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (isError ? juce::MessageBoxIconType::WarningIcon
                                                             : juce::MessageBoxIconType::InfoIcon)
                                      .withTitle (title)
                                      .withMessage (message)
                                      .withButton (ko ("확인"))
                                      .withAssociatedComponent (this),
                                  [] (int) {});
}

void MainComponent::updateTransportStandby()
{
    transport.setStandbyCue (document.cues.getPlayheadIndex(), document.cues.getPlayhead());
}

//==============================================================================
void MainComponent::timerCallback()
{
    auto playing = engine.getPlayingCues();

    for (const auto& fade : controller.getFadeRunner().getRunning())
    {
        AudioEngine::PlayingCue p;
        p.id = fade.fadeId;
        p.positionSeconds = fade.elapsedSeconds;
        p.lengthSeconds = fade.durationSeconds;
        p.remainingSeconds = juce::jmax (0.0, fade.durationSeconds - fade.elapsedSeconds);
        p.progress = fade.durationSeconds > 0.0 ? juce::jlimit (0.0, 1.0, fade.elapsedSeconds / fade.durationSeconds) : 1.0;
        p.startOrder = std::numeric_limits<juce::int64>::max() / 2;   // after the audio cues
        playing.push_back (p);
    }

    int paused = 0, running = 0;

    for (const auto& p : playing)
    {
        if (p.loaded)
            continue;

        ++running;

        if (p.paused)
            ++paused;
    }

    transport.setPlayingCount (running, paused);
    transport.setGoLocked (controller.isGoLocked());
    inspector.setPlayback (playing);

    if (closeContinuation != nullptr)   // "start on close" cue: quit once its run has finished (or after the deadline)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const bool runOver = ! engine.isPlaying (closeCueId) && ! controller.hasPendingFor (closeCueId);

        if (runOver || now > closeDeadlineMs)
        {
            auto then = std::move (closeContinuation);
            closeContinuation = nullptr;
            then();
            return;
        }
    }

    if (activeCuesVisible)
        activeCues.setPlayingCues (playing);

    table.setPlayingCues (std::move (playing));
    footer.setCueCount (document.cues.size());
    footer.setWarningCount (countBrokenCues());
    controller.checkWallClock (juce::Time::getCurrentTime());

    // a knob moved in a plugin editor: the project needs saving (but not right after a restore)
    const bool changed = engine.consumePluginStateChanges();

    if (changed && juce::Time::getMillisecondCounterHiRes() >= ignorePluginChangesUntilMs)
        document.markDirty();

    autoBackupIfDue();
}

void MainComponent::cueListStructureChanged()
{
    updateTransportStandby();
    commands.commandStatusChanged();
}

void MainComponent::cueChanged (int index)
{
    if (index == document.cues.getPlayheadIndex())
        updateTransportStandby();

    // a loaded instance holds a copy of the cue: after an edit it would play the old settings
    if (document.cues.isValidIndex (index))
    {
        const auto& cue = document.cues.get (index);

        if (engine.isLoaded (cue.id))
        {
            engine.unload (cue.id);

            if (! cue.fileMissing && cue.file != juce::File())
                engine.load (cue, 0.0);
        }
    }
}

void MainComponent::cueSelectionChanged (int)
{
    commands.commandStatusChanged();
}

void MainComponent::playheadChanged (int index)
{
    updateTransportStandby();
    commands.commandStatusChanged();

    // auto-load: the standby cue is prepared as soon as the playhead lands on it; the previous one is let go
    const juce::Uuid standbyId = document.cues.isValidIndex (index) ? document.cues.get (index).id : juce::Uuid::null();

    if (! autoLoadedId.isNull() && autoLoadedId != standbyId)
    {
        engine.unload (autoLoadedId);
        autoLoadedId = juce::Uuid::null();
    }

    if (document.cues.isValidIndex (index))
    {
        const auto& cue = document.cues.get (index);

        if (cue.autoLoad && ! cue.fileMissing && cue.file != juce::File() && ! engine.isLoaded (cue.id) && ! engine.isPlaying (cue.id))
            if (engine.load (cue, 0.0))
                autoLoadedId = cue.id;
    }
}

void MainComponent::documentStateChanged()
{
    unsavedChanges.store (document.isDirty(), std::memory_order_relaxed);
    commands.commandStatusChanged();   // undo / redo names and availability
    document.cues.setLockPlayheadToSelection (document.settings.lockPlayheadToSelection);
    table.setRowSize (document.settings.rowSize);
    transport.setAuditionMode (document.settings.alwaysAudition);

    if (onWindowTitleChanged)
        onWindowTitleChanged (document.getWindowTitle());
}

} // namespace gocue
