#include "app/Links.h"
#include "ui/MainComponent.h"

#include "ui/SplitLayout.h"

#include <array>

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
    void focusAlertEditor (juce::AlertWindow& alert, const juce::String& editorName);   // defined further down

    constexpr int menuBarHeight = 30;   // 17 pt menu titles
    constexpr int transportHeight = 148;
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
      activeCues (e, document.cues),
      containerTabs (document),
      cart (document.cues, e.getFormatManager())
{
    setWantsKeyboardFocus (true);

    addAndMakeVisible (menuBar);
    addAndMakeVisible (transport);
    addAndMakeVisible (table);
    addAndMakeVisible (inspector);
    addAndMakeVisible (activeCues);
    addAndMakeVisible (footer);

    inspectorFraction = settings.getInspectorFraction();
    inspectorCollapsed = settings.getInspectorCollapsed();
    activeCuesFraction = settings.getActiveCuesFraction();
    activeCuesVisible = ! settings.getActiveCuesCollapsed();

    inspectorDivider.onDragStart = [this] { dragStartSize = inspector.getHeight(); };
    inspectorDivider.onDrag = [this] (int delta)
    {
        inspectorFraction = SplitLayout::fractionFor (dragStartSize - delta, splitHeight, inspectorFraction);   // dragging up = taller inspector
        resized();
    };
    inspectorDivider.onDragEnd = [this] { settings.setInspectorFraction (inspectorFraction); };
    inspectorDivider.onToggle = [this] { commands.invokeDirectly (CommandIDs::toggleInspector, true); };
    addAndMakeVisible (inspectorDivider);

    activeCuesDivider.onDragStart = [this] { dragStartSize = activeCues.getWidth(); };
    activeCuesDivider.onDrag = [this] (int delta)
    {
        activeCuesFraction = SplitLayout::fractionFor (dragStartSize - delta, splitWidth, activeCuesFraction);   // dragging left = wider panel
        resized();
    };
    activeCuesDivider.onDragEnd = [this] { settings.setActiveCuesFraction (activeCuesFraction); };
    activeCuesDivider.onToggle = [this] { commands.invokeDirectly (CommandIDs::toggleActiveCues, true); };
    addAndMakeVisible (activeCuesDivider);

    controller.onStatus = [this] (const juce::String& message, bool isError) { transport.showStatus (message, isError); };
    controller.onGoRejected = [this] { transport.flashGoRejected(); };

    table.onFilesDropped = [this] (const juce::StringArray& files, int insertAt) { addCuesFromFiles (files, insertAt); };
    table.onMoveRows = [this] (const std::vector<int>& rows, int insertIndex) { moveRows (rows, insertIndex); };
    table.onToggleCollapse = [this] (int index, bool collapsed) { document.cues.setCollapsed (index, collapsed); };

    // cue lists / carts
    addAndMakeVisible (containerTabs);
    addChildComponent (cart);
    containerTabs.onSelect = [this] (int index) { document.setActiveContainer (index); };
    containerTabs.onAddList = [this] { addContainer (false); };
    containerTabs.onAddCart = [this] { addContainer (true); };
    containerTabs.onRename = [this] (int index) { renameContainer (index); };
    containerTabs.onRemove = [this] (int index) { removeContainer (index); };
    containerTabs.onToggleCart = [this] (int index) { toggleContainerCart (index); };
    containerTabs.onGridSize = [this] (int index) { setContainerGrid (index); };
    cart.onTrigger = [this] (const Cue& cue)
    {
        const Cue copy = cue;   // the model row may move / vanish while the cue starts (a goto, a list switch)
        const auto result = controller.fire (copy.id);   // like a hotkey: the cue alone, with its fade-stop-others and duck

        if (result == CueController::GoResult::started)
            transport.showStatus (ko ("카트: ") + copy.name, false);
    };
    cart.onFilesDropped = [this] (const juce::StringArray& files, int slot) { addCuesFromFiles (files, slot); };
    cart.onStop = [this] (const juce::Uuid& id) { controller.stopCue (id, true); };   // pending follows go too
    table.isNumberTaken = [this] (const juce::String& number, const juce::Uuid& exceptId) { return document.isNumberTaken (number, exceptId); };
    document.onPatchesChanged = [this] { inspector.refreshDeviceDependent(); };   // dead output columns follow the patch
    document.onBeforeContainerSwitch = [this] { table.finishEditing(); inspector.finishEditing(); };   // a half-typed field belongs to the list that is leaving
    table.onEditCues = [this] (const std::vector<int>& rows, const juce::String& name, const std::function<void (Cue&)>& mutator)
    {
        editCues (rows, name, mutator);
    };
    table.onSetNumber = [this] (const juce::Uuid& id, const juce::String& number)
    {
        if (! showMode)
            document.setCueNumber (id, number);
    };
    table.onStatus = [this] (const juce::String& message) { transport.showStatus (message, false); };
    table.onEditNotes = [this] (int) { ensureInspectorShown(); inspector.showNotes(); };
    table.hasPlayed = [this] (const juce::Uuid& id) { return controller.hasPlayed (id); };
    transport.describeGroup = [this] (const Cue& groupCue) -> juce::String
    {
        const int index = document.cues.indexOf (groupCue.id);

        if (index < 0)
            return {};

        const int count = (int) document.cues.childrenOf (index).size();
        const double length = document.cues.effectiveLengthOf (index);
        juce::String text = ko ("자식 ") + juce::String (count) + ko ("개");

        if (length < 0.0)
            text << ko ("   길이 ") << juce::String::fromUTF8 ("\xE2\x88\x9E");
        else if (length > 0.0)
            text << ko ("   길이 ") << formatSeconds (length);

        return text;
    };

    transport.describeFadeTarget = [this] (const Cue& fadeCue) -> juce::String
    {
        const int index = fadeCue.targetId().isNull() ? -1 : document.cues.indexOf (fadeCue.targetId());

        if (index < 0)
            return {};

        const auto& t = document.cues.get (index);
        return juce::String::fromUTF8 ("\xE2\x86\x92 ") + (t.number.isNotEmpty() ? t.number + " " : "#" + juce::String (index + 1) + " ") + t.name;
    };
    table.onEditDuration = [this] (int) { ensureInspectorShown(); inspector.showTimeTab(); };

    inspector.onOpenPluginManager = [this] { showPluginManager(); };
    inspector.onPanic = [this] { controller.panicAll(); table.focusTable(); };
    inspector.onReturnFocus = [this] { table.focusTable(); };
    activeCues.onStopRequested = [this] (const juce::Uuid& id)
    {
        // an audio cue fades out over its stop fade; anything else (fade / group / wait) is stopped by the controller
        if (engine.isPlaying (id))
            engine.fadeOutAndStop (id);
        else
            controller.stopCue (id);
    };
    transport.onPanicSettings = [this] (juce::Point<int> screenPosition) { showPanicSecondsMenu (screenPosition); };
    inspector.onPreview = [this] { controller.preview(); };
    inspector.onSeekPlay = [this] (double fileSeconds)
    {
        const auto* cue = document.cues.getSelected();

        if (cue == nullptr || ! cue->isAudio())
            return;

        const double length = cue->regionLength();
        const double offset = length > 0.0 ? juce::jlimit (0.0, length, fileSeconds - cue->regionStart())
                                           : juce::jmax (0.0, fileSeconds - cue->regionStart());
        AudioEngine::LiveState live;

        if (engine.getLiveState (cue->id, live))
            engine.seekToFileSeconds (cue->id, cue->regionStart() + offset);   // running: jump there, inside the current pass
        else
            controller.previewFrom (offset);                                   // otherwise play from there
    };
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
    engine.setPatches (document.patches, true);   // the default patch of the empty project
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
    PatchEditorDialog::closeIfOpen();   // references the document, engine and plugin windows: before they go
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

    // below the transport: [ cue list | active cues ] over a divider over the inspector. The two secondary panes
    // take a share of the area (not a fixed size), so a resized window keeps the proportions.
    splitHeight = area.getHeight();
    const int inspectorH = SplitLayout::inspectorHeight (splitHeight, inspectorFraction, inspectorCollapsed, SplitDivider::thickness);
    inspector.setVisible (! inspectorCollapsed);
    inspector.setBounds (area.removeFromBottom (inspectorH));
    inspectorDivider.setCollapsed (inspectorCollapsed);
    inspectorDivider.setBounds (area.removeFromBottom (SplitDivider::thickness));

    splitWidth = area.getWidth();
    const int activeW = SplitLayout::activeCuesWidth (splitWidth, activeCuesFraction, ! activeCuesVisible, SplitDivider::thickness);
    activeCues.setVisible (activeCuesVisible);
    activeCues.setBounds (area.removeFromRight (activeW));
    activeCuesDivider.setCollapsed (! activeCuesVisible);
    activeCuesDivider.setBounds (area.removeFromRight (SplitDivider::thickness));

    containerTabs.setBounds (area.removeFromTop (ContainerTabs::height));
    table.setBounds (area);
    cart.setBounds (area);
}

void MainComponent::showPanicSecondsMenu (juce::Point<int> screenPosition)
{
    const double current = document.settings.panicSeconds;
    const std::array<double, 6> presets { 0.5, 1.0, 2.0, 3.0, 5.0, 10.0 };
    juce::PopupMenu menu;
    menu.addSectionHeader (ko ("전체 페이드 정지 (Esc): 페이드아웃 시간"));

    for (size_t i = 0; i < presets.size(); ++i)
        menu.addItem ((int) i + 1, juce::String (presets[i], presets[i] < 1.0 ? 1 : 0) + ko ("초"), true, std::abs (current - presets[i]) < 0.001);

    menu.addSeparator();
    menu.addItem (100, ko ("직접 입력... (지금 ") + juce::String (current, 1) + ko ("초)"));

    juce::Component::SafePointer<MainComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ screenPosition.x, screenPosition.y, 1, 1 }),
                        [safeThis, presets, current] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        if (result >= 1 && result <= (int) presets.size())
        {
            safeThis->applyPanicSeconds (presets[(size_t) result - 1]);
            return;
        }

        auto* alert = new juce::AlertWindow (ko ("전체 페이드 정지 시간"),
                                             ko ("Esc를 눌렀을 때 재생 중인 모든 큐가 페이드아웃되는 시간 (초, 0 = 즉시 정지)"),
                                             juce::MessageBoxIconType::NoIcon);
        alert->addTextEditor ("seconds", juce::String (current, 1), ko ("초"));
        alert->addButton (ko ("확인"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, alert] (int r)
        {
            if (safeThis != nullptr && r == 1)
                safeThis->applyPanicSeconds (alert->getTextEditorContents ("seconds").getDoubleValue());
        }), true);
        focusAlertEditor (*alert, "seconds");
    });
}

void MainComponent::applyPanicSeconds (double seconds)
{
    auto updated = document.settings;
    updated.panicSeconds = juce::jlimit (0.0, WorkspaceSettings::maxPanicSeconds, seconds);
    document.setSettings (updated);
    transport.setPanicSeconds (updated.panicSeconds);
    transport.showStatus (ko ("전체 페이드 정지 시간: ") + juce::String (updated.panicSeconds, 1) + ko ("초"), false);
}

void MainComponent::ensureInspectorShown()
{
    if (! inspectorCollapsed)
        return;

    inspectorCollapsed = false;
    settings.setInspectorCollapsed (false);
    resized();
    commands.commandStatusChanged();
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
        if (juce::File (path).hasFileExtension (ProjectSerializer::openableExtensions))
            return true;

    return ! showMode && containsAudioOrFolder (engine.getFormatManager(), files);
}

void MainComponent::fileDragEnter (const juce::StringArray& files, int, int)
{
    dragOverWindow = true;
    repaint();
    transport.showStatus (ko ("파일 드래그 감지 (창): ") + juce::String (files.size()) + ko ("개"), false);
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

        if (file.existsAsFile() && file.hasFileExtension (ProjectSerializer::openableExtensions))
        {
            confirmReplaceProjectThen ([this, file] { openProjectFile (file); });
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
                    CommandIDs::addCue, CommandIDs::addFadeCue, CommandIDs::addDevampCue, CommandIDs::addGroupCue, CommandIDs::groupSelectedCues,
                    CommandIDs::ungroupSelected, CommandIDs::collapseAllGroups, CommandIDs::expandAllGroups,
                    CommandIDs::addControlCue, CommandIDs::addWaitCue, CommandIDs::addMemoCue, CommandIDs::addMicCue, CommandIDs::toggleSequenceRecording,
                    CommandIDs::addCueList, CommandIDs::addCart, CommandIDs::nextContainer, CommandIDs::previousContainer,
                    CommandIDs::renameContainer, CommandIDs::removeContainer,
                    CommandIDs::revertFade, CommandIDs::fetchFadeLevels, CommandIDs::removeCue, CommandIDs::duplicateCue,
                    CommandIDs::moveCueUp, CommandIDs::moveCueDown, CommandIDs::selectAll,
                    CommandIDs::copyCues, CommandIDs::cutCues, CommandIDs::pasteCues, CommandIDs::pasteCueProperties,
                    CommandIDs::find, CommandIDs::findNext,
                    CommandIDs::renumber, CommandIDs::deleteNumbers, CommandIDs::findMissingFiles,
                    CommandIDs::saveCueTemplate, CommandIDs::clearCueTemplate,
                    CommandIDs::newProject, CommandIDs::openProject,
                    CommandIDs::saveProject, CommandIDs::saveProjectAs,
                    CommandIDs::undo, CommandIDs::redo, CommandIDs::toggleShowMode, CommandIDs::toggleActiveCues, CommandIDs::toggleInspector,
                    CommandIDs::audioSettings, CommandIDs::audioPatches, CommandIDs::pluginManager, CommandIDs::masterInserts,
                    CommandIDs::workspaceSettings,
                    CommandIDs::checkForUpdates, CommandIDs::showManual, CommandIDs::feedbackChat, CommandIDs::about });
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

        case CommandIDs::addGroupCue:
            result.setInfo (ko ("그룹 큐 추가"), ko ("빈 그룹을 선택 뒤에 추가 (자식은 그룹 아래로 끌어다 넣거나 Ctrl+G로 묶기)"), cueMenu, 0);
            result.addDefaultKeypress ('0', ModifierKeys::commandModifier);
            result.setActive (canEdit);
            break;

        case CommandIDs::groupSelectedCues:
            result.setInfo (ko ("선택한 큐 그룹으로 묶기"), ko ("선택한 큐(하위 포함)를 새 그룹 안에 넣음"), cueMenu, 0);
            result.addDefaultKeypress ('G', ModifierKeys::commandModifier);
            result.setActive (canEdit && document.cues.getSelectedIndex() >= 0);
            break;

        case CommandIDs::ungroupSelected:
            result.setInfo (ko ("그룹 해제"), ko ("선택한 그룹을 없애고 자식을 한 단계 위로"), cueMenu, 0);
            result.addDefaultKeypress ('G', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            result.setActive (canEdit && document.cues.getSelected() != nullptr && document.cues.getSelected()->isGroup());
            break;

        case CommandIDs::collapseAllGroups:
            result.setInfo (ko ("모든 그룹 접기"), ko ("모든 그룹의 자식을 숨김"), cueMenu, 0);
            break;

        case CommandIDs::addControlCue:
            result.setInfo (ko ("제어 큐 추가"), ko ("선택한 큐를 대상으로 하는 제어 큐 (시작/정지/일시정지/로드/리셋/이동/활성화/비활성화/대상 변경 — 종류는 인스펙터에서)"), cueMenu, 0);
            result.addDefaultKeypress ('9', ModifierKeys::commandModifier);
            result.setActive (canEdit);
            break;

        case CommandIDs::addWaitCue:
            result.setInfo (ko ("대기 큐 추가"), ko ("정해진 시간 동안 아무것도 하지 않는 큐 (자동 팔로우 앞에 시간을 둘 때)"), cueMenu, 0);
            result.setActive (canEdit);
            break;

        case CommandIDs::addMicCue:
            result.setInfo (ko ("마이크 큐 추가"), ko ("장치 입력을 레벨 매트릭스·인서트·패치로 보내는 큐 (정지할 때까지)"), cueMenu, 0);
            result.addDefaultKeypress ('6', ModifierKeys::commandModifier);   // Ctrl+M is the master bus inserts
            result.setActive (canEdit);
            break;

        case CommandIDs::addMemoCue:
            result.setInfo (ko ("메모 큐 추가"), ko ("아무것도 하지 않는 큐 (목록 안의 메모)"), cueMenu, 0);
            result.setActive (canEdit);
            break;

        case CommandIDs::addCueList:
            result.setInfo (ko ("새 큐 리스트"), ko ("큐 리스트를 하나 더 추가 (위 탭)"), cueMenu, 0);
            result.setActive (canEdit);
            break;

        case CommandIDs::addCart:
            result.setInfo (ko ("새 카트"), ko ("버튼 격자 카트를 추가 — 클릭하면 바로 재생, 플레이헤드/자동 진행 없음"), cueMenu, 0);
            result.setActive (canEdit);
            break;

        case CommandIDs::nextContainer:
            result.setInfo (ko ("다음 리스트/카트"), ko ("오른쪽 탭으로"), cueMenu, 0);
            result.addDefaultKeypress (juce::KeyPress::pageDownKey, ModifierKeys::commandModifier);
            result.setActive (document.getNumContainers() > 1);
            break;

        case CommandIDs::previousContainer:
            result.setInfo (ko ("이전 리스트/카트"), ko ("왼쪽 탭으로"), cueMenu, 0);
            result.addDefaultKeypress (juce::KeyPress::pageUpKey, ModifierKeys::commandModifier);
            result.setActive (document.getNumContainers() > 1);
            break;

        case CommandIDs::renameContainer:
            result.setInfo (ko ("리스트/카트 이름 바꾸기..."), ko ("현재 탭의 이름"), cueMenu, 0);
            result.setActive (canEdit);
            break;

        case CommandIDs::removeContainer:
            result.setInfo (ko ("리스트/카트 삭제"), ko ("현재 탭을 큐와 함께 삭제 (실행 취소 가능)"), cueMenu, 0);
            result.setActive (canEdit && document.getNumContainers() > 1);
            break;

        case CommandIDs::toggleSequenceRecording:
            result.setInfo (controller.isRecording() ? ko ("시퀀스 녹음 정지 (") + juce::String (controller.getNumRecorded()) + ko ("개 기록됨)") : ko ("시퀀스 녹음 시작..."),
                            ko ("녹음 중 시작되는 큐와 시각을 기록해 정지할 때 타임라인 그룹(시작 큐 + 프리웨이트)으로 만듦"), cueMenu, 0);
            result.addDefaultKeypress ('E', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            result.setActive (canEdit || controller.isRecording());
            result.setTicked (controller.isRecording());
            break;

        case CommandIDs::expandAllGroups:
            result.setInfo (ko ("모든 그룹 펼치기"), ko ("모든 그룹의 자식을 표시"), cueMenu, 0);
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
            result.setInfo (ko ("큐 속성 붙여넣기..."), ko ("복사한 큐의 속성(색·시간·트리거·트림·레벨·플러그인)만 선택 큐들에 적용"), editMenu, 0);
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
            result.setInfo (ko ("선택 큐를 새 큐 기본값으로"), ko ("이후 추가하는 큐가 이 큐의 설정(페이드·게인·색·트리거·플러그인 등)을 물려받음 (프로젝트에 저장)"), cueMenu, 0);
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
            result.setActive (canEdit);   // show mode: the project, devices, patches, plugins and updates are locked
            result.addDefaultKeypress ('N', ModifierKeys::commandModifier);
            break;

        case CommandIDs::openProject:
            result.setInfo (ko ("열기..."), ko ("프로젝트 열기 (.enqueue, .gocue)"), fileMenu, 0);
            result.setActive (canEdit);   // show mode: the project, devices, patches, plugins and updates are locked
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
            result.setActive (canEdit);   // show mode: the project, devices, patches, plugins and updates are locked
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
            result.setInfo (activeCuesVisible ? ko ("활성 큐 패널 접기") : ko ("활성 큐 패널 펴기"),
                            ko ("재생 중인 큐 목록 (일시정지·스크럽·페이드 정지). 구분선을 끌면 너비가 바뀝니다"), editMenu, 0);
            result.addDefaultKeypress ('L', ModifierKeys::commandModifier);
            break;

        case CommandIDs::toggleInspector:
            result.setInfo (inspectorCollapsed ? ko ("인스펙터 펴기") : ko ("인스펙터 접기"),
                            ko ("아래 인스펙터 패널 접기 / 펴기. 구분선을 끌면 높이가 바뀝니다"), editMenu, 0);
            result.addDefaultKeypress ('I', ModifierKeys::commandModifier);
            break;

        case CommandIDs::audioSettings:
            result.setInfo (ko ("오디오 출력 설정..."), ko ("출력 장치(ASIO / WASAPI) 선택"), audio, 0);
            result.setActive (canEdit);   // show mode: the project, devices, patches, plugins and updates are locked
            result.addDefaultKeypress (',', ModifierKeys::commandModifier);
            break;

        case CommandIDs::audioPatches:
            result.setInfo (ko ("오디오 패치..."), ko ("큐 출력 → 장치 출력 라우팅, 출력 이름, 스테레오 묶기, 출력 인서트"), audio, 0);
            result.setActive (canEdit);   // show mode: the project, devices, patches, plugins and updates are locked
            result.addDefaultKeypress ('P', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;

        case CommandIDs::pluginManager:
            result.setInfo (ko ("VST3 플러그인 관리..."), ko ("VST3 플러그인 스캔 / 목록"), audio, 0);
            result.setActive (canEdit);   // show mode: the project, devices, patches, plugins and updates are locked
            result.addDefaultKeypress ('P', ModifierKeys::commandModifier);
            break;

        case CommandIDs::masterInserts:
            result.setInfo (ko ("마스터 버스 인서트..."), ko ("모든 큐가 통과하는 마스터 VST3 체인"), audio, 0);
            result.setActive (canEdit);   // show mode: the project, devices, patches, plugins and updates are locked
            result.addDefaultKeypress ('M', ModifierKeys::commandModifier);
            break;

        case CommandIDs::checkForUpdates:
            result.setInfo (ko ("업데이트 확인..."), ko ("GitHub Releases에서 새 버전 확인"), ko ("도움말"), 0);
            result.setActive (canEdit);   // show mode: the project, devices, patches, plugins and updates are locked
            result.setActive (Updater::isAvailable());
            break;

        case CommandIDs::showManual:
            result.setInfo (ko ("사용 설명서..."), ko ("기능 설명과 단축키"), ko ("도움말"), 0);
            result.addDefaultKeypress (juce::KeyPress::F1Key, ModifierKeys::commandModifier);
            break;

        case CommandIDs::feedbackChat:
            result.setInfo (ko ("커뮤니티"), ko ("카카오톡 오픈채팅 열기"), ko ("도움말"), 0);
            result.setActive (juce::String (Links::feedbackChat).isNotEmpty());
            break;

        case CommandIDs::about:
            result.setInfo (ko ("앤큐 정보"), ko ("버전 정보"), ko ("도움말"), 0);
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

        case CommandIDs::addGroupCue:
            addGroupCue();
            break;

        case CommandIDs::groupSelectedCues:
            groupSelectedCues();
            break;

        case CommandIDs::ungroupSelected:
            ungroupSelected();
            break;

        case CommandIDs::collapseAllGroups:
            setAllGroupsCollapsed (true);
            break;

        case CommandIDs::addControlCue:
            addControlCue (ControlKind::start);
            break;

        case CommandIDs::addWaitCue:
            addControlCue (ControlKind::wait);
            break;

        case CommandIDs::addMicCue:
            addMicCue();
            break;

        case CommandIDs::addMemoCue:
            addControlCue (ControlKind::memo);
            break;

        case CommandIDs::toggleSequenceRecording:
            toggleSequenceRecording();
            break;

        case CommandIDs::addCueList:
            addContainer (false);
            break;

        case CommandIDs::addCart:
            addContainer (true);
            break;

        case CommandIDs::nextContainer:
            document.setActiveContainer ((document.getActiveContainer() + 1) % juce::jmax (1, document.getNumContainers()));
            break;

        case CommandIDs::previousContainer:
            document.setActiveContainer ((document.getActiveContainer() + document.getNumContainers() - 1) % juce::jmax (1, document.getNumContainers()));
            break;

        case CommandIDs::renameContainer:
            renameContainer (document.getActiveContainer());
            break;

        case CommandIDs::removeContainer:
            removeContainer (document.getActiveContainer());
            break;

        case CommandIDs::expandAllGroups:
            setAllGroupsCollapsed (false);
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
            confirmReplaceProjectThen ([this] { newProject(); });
            break;

        case CommandIDs::openProject:
            confirmReplaceProjectThen ([this] { openProjectViaDialog(); });
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
            settings.setActiveCuesCollapsed (! activeCuesVisible);
            resized();
            commands.commandStatusChanged();
            break;

        case CommandIDs::toggleInspector:
            inspectorCollapsed = ! inspectorCollapsed;
            settings.setInspectorCollapsed (inspectorCollapsed);
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

        case CommandIDs::showManual:
            if (manualWindow == nullptr)
                manualWindow = std::make_unique<ManualWindow>();

            manualWindow->open();
            break;

        case CommandIDs::feedbackChat:
            if (juce::String (Links::feedbackChat).isNotEmpty())
                juce::URL (Links::feedbackChat).launchInDefaultBrowser();
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
            menu.addCommandItem (&commands, CommandIDs::toggleInspector);
            break;

        case 2:
            menu.addCommandItem (&commands, CommandIDs::addCue);
            menu.addCommandItem (&commands, CommandIDs::addFadeCue);
            menu.addCommandItem (&commands, CommandIDs::addDevampCue);
            menu.addCommandItem (&commands, CommandIDs::addGroupCue);
            menu.addCommandItem (&commands, CommandIDs::addControlCue);
            menu.addCommandItem (&commands, CommandIDs::addWaitCue);
            menu.addCommandItem (&commands, CommandIDs::addMemoCue);
            menu.addCommandItem (&commands, CommandIDs::addMicCue);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::toggleSequenceRecording);
            menu.addSeparator();

            {
                juce::PopupMenu lists;
                lists.addCommandItem (&commands, CommandIDs::addCueList);
                lists.addCommandItem (&commands, CommandIDs::addCart);
                lists.addCommandItem (&commands, CommandIDs::nextContainer);
                lists.addCommandItem (&commands, CommandIDs::previousContainer);
                lists.addCommandItem (&commands, CommandIDs::renameContainer);
                lists.addCommandItem (&commands, CommandIDs::removeContainer);
                menu.addSubMenu (ko ("큐 리스트 / 카트"), lists);
            }
            menu.addCommandItem (&commands, CommandIDs::groupSelectedCues);
            menu.addCommandItem (&commands, CommandIDs::ungroupSelected);
            menu.addCommandItem (&commands, CommandIDs::collapseAllGroups);
            menu.addCommandItem (&commands, CommandIDs::expandAllGroups);
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
            menu.addCommandItem (&commands, CommandIDs::showManual);
            menu.addCommandItem (&commands, CommandIDs::feedbackChat);
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
            cue.parentId = document.cues.parentForInsertion (at);   // dropped between a group's children: joins the group

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
    const juce::Uuid target = selectedCue != nullptr && selectedCue->makesSound() ? selectedCue->id
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

        const int at = selected >= 0 ? document.cues.subtreeEnd (selected) : document.cues.size();

        if (autoNumber)
            fade.number = CueNumbering::next (document.cues.getAll(), at, increment);

        const int index = document.cues.addAfter (std::move (fade), selected);
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
    const juce::Uuid target = selectedCue != nullptr ? (selectedCue->makesSound() ? selectedCue->id : selectedCue->targetId()) : juce::Uuid::null();

    document.perform (ko ("디밴프 큐 추가"), [this, selected, target, autoNumber, increment]
    {
        Cue devamp;
        devamp.type = CueType::devamp;
        devamp.devamp.targetId = target;
        devamp.name = ko ("디밴프");

        if (const auto* t = document.cues.findById (target))
            devamp.name = ko ("디밴프: ") + t->name;

        const int at = selected >= 0 ? document.cues.subtreeEnd (selected) : document.cues.size();

        if (autoNumber)
            devamp.number = CueNumbering::next (document.cues.getAll(), at, increment);

        const int index = document.cues.addAfter (std::move (devamp), selected);
        document.cues.setSelectedIndex (index);
    });
}

void MainComponent::addControlCue (ControlKind kind)
{
    if (showMode)
        return;

    const int selected = document.cues.getSelectedIndex();
    const auto* selectedCue = document.cues.getSelected();
    const bool autoNumber = document.settings.autoNumber;
    const double increment = document.settings.numberIncrement;
    const juce::Uuid target = selectedCue != nullptr && kind != ControlKind::wait && kind != ControlKind::memo ? selectedCue->id : juce::Uuid::null();

    document.perform (ko ("제어 큐 추가"), [this, selected, target, kind, autoNumber, increment]
    {
        Cue control;
        control.type = CueType::control;
        control.control.kind = kind;
        control.control.targetId = target;
        control.control.seconds = kind == ControlKind::wait ? 5.0 : 0.0;
        control.name = kind == ControlKind::wait ? ko ("대기") : kind == ControlKind::memo ? ko ("메모") : ko ("시작");

        if (const auto* t = document.cues.findById (target))
            control.name = ko ("시작: ") + t->name;

        const int at = selected >= 0 ? document.cues.subtreeEnd (selected) : document.cues.size();

        if (autoNumber)
            control.number = CueNumbering::next (document.cues.getAll(), at, increment);

        const int index = document.cues.addAfter (std::move (control), selected);
        document.cues.setSelectedIndex (index);
    });
}

void MainComponent::toggleSequenceRecording()
{
    if (! controller.isRecording())
    {
        if (showMode)
            return;

        controller.startRecording();
        commands.commandStatusChanged();
        return;
    }

    const auto starts = controller.stopRecording();
    commands.commandStatusChanged();

    if (starts.empty())
    {
        transport.showStatus (ko ("시퀀스 녹음: 기록된 큐가 없어 그룹을 만들지 않았습니다"), false);
        return;
    }

    const int selected = document.cues.getSelectedIndex();
    const bool autoNumber = document.settings.autoNumber;
    const double increment = document.settings.numberIncrement;

    document.perform (ko ("시퀀스 녹음 → 타임라인 그룹"), [this, starts, selected, autoNumber, increment]
    {
        Cue group;
        group.type = CueType::group;
        group.group.mode = GroupMode::timeline;
        group.name = ko ("녹음한 시퀀스 ") + juce::Time::getCurrentTime().formatted ("%H:%M");
        const int at = selected >= 0 ? document.cues.subtreeEnd (selected) : document.cues.size();

        if (autoNumber)
            group.number = CueNumbering::next (document.cues.getAll(), at, increment);

        const auto groupId = group.id;
        const int groupIndex = document.cues.addAfter (std::move (group), selected);
        int insertAt = groupIndex + 1;

        for (const auto& s : starts)
        {
            const auto* target = document.findCueAnywhere (s.cueId);

            if (target == nullptr)
                continue;

            Cue start;
            start.type = CueType::control;
            start.control.kind = ControlKind::start;
            start.control.targetId = s.cueId;
            start.preWaitSeconds = std::round (s.seconds * 1000.0) / 1000.0;
            start.name = ko ("시작: ") + target->name;
            start.parentId = groupId;
            insertAt = document.cues.add (std::move (start), insertAt) + 1;
        }

        document.cues.setSelectedIndex (groupIndex);
    });
}

void MainComponent::addMicCue()
{
    if (showMode)
        return;

    const int selected = document.cues.getSelectedIndex();
    const bool autoNumber = document.settings.autoNumber;
    const double increment = document.settings.numberIncrement;

    document.perform (ko ("마이크 큐 추가"), [this, selected, autoNumber, increment]
    {
        Cue mic;
        mic.type = CueType::mic;
        mic.name = ko ("마이크");
        mic.mic.firstInput = 0;
        mic.mic.numInputs = 2;
        mic.fadeOutMs = 100;
        const int at = selected >= 0 ? document.cues.subtreeEnd (selected) : document.cues.size();

        if (autoNumber)
            mic.number = CueNumbering::next (document.cues.getAll(), at, increment);

        const int index = document.cues.addAfter (std::move (mic), selected);
        document.cues.setSelectedIndex (index);
    });
}

void MainComponent::updateInputsWanted()
{
    int wanted = 0;

    document.forEachList ([&wanted] (CueList& list)
    {
        for (const auto& c : list.getAll())
            if (c.isMic())
                wanted = juce::jmax (wanted, c.mic.firstInput + c.mic.numInputs);
    });

    const auto error = engine.setInputsWanted (wanted);   // no-op when the first 'wanted' inputs are open (or none are needed)

    if (error.isNotEmpty())
        transport.showStatus (ko ("장치 입력을 열지 못했습니다: ") + error, true);
}

void MainComponent::addGroupCue()
{
    if (showMode)
        return;

    const int selected = document.cues.getSelectedIndex();
    const bool autoNumber = document.settings.autoNumber;
    const double increment = document.settings.numberIncrement;

    document.perform (ko ("그룹 큐 추가"), [this, selected, autoNumber, increment]
    {
        Cue group;
        group.type = CueType::group;
        group.name = ko ("그룹");
        const int at = selected >= 0 ? document.cues.subtreeEnd (selected) : document.cues.size();

        if (autoNumber)
            group.number = CueNumbering::next (document.cues.getAll(), at, increment);

        const int index = document.cues.addAfter (std::move (group), selected);
        document.cues.setSelectedIndex (index);
    });
}

void MainComponent::groupSelectedCues()
{
    const auto rows = document.cues.getSelectedIndices();

    if (rows.empty() || showMode)
        return;

    const bool autoNumber = document.settings.autoNumber;
    const double increment = document.settings.numberIncrement;

    document.perform (ko ("그룹으로 묶기"), [this, rows, autoNumber, increment]
    {
        Cue group;
        group.type = CueType::group;
        group.name = ko ("그룹");

        if (autoNumber)
            group.number = CueNumbering::next (document.cues.getAll(), rows.front(), increment);

        document.cues.wrapInGroup (rows, std::move (group));
    });
}

void MainComponent::ungroupSelected()
{
    const int index = document.cues.getSelectedIndex();

    if (! document.cues.isValidIndex (index) || ! document.cues.get (index).isGroup() || showMode)
        return;

    const auto id = document.cues.get (index).id;
    controller.stopGroup (id, 0);

    document.perform (ko ("그룹 해제"), [this, index]
    {
        document.cues.ungroup (index);
    });
}

void MainComponent::setAllGroupsCollapsed (bool collapsed)
{
    for (int i = 0; i < document.cues.size(); ++i)
        if (document.cues.get (i).isGroup())
            document.cues.setCollapsed (i, collapsed);
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

    for (int row : document.cues.withSubtrees (rows))   // a group takes its children along
        ids.push_back (document.cues.get (row).id);

    for (int row : rows)
        if (document.cues.get (row).isGroup())
            controller.stopGroup (document.cues.get (row).id, 0);

    document.perform (rows.size() == 1 ? ko ("큐 삭제") : ko ("큐 삭제 (") + juce::String (rows.size()) + ")", [this, rows, ids]
    {
        for (const auto& id : ids)
        {
            controller.stopCue (id);   // fades / groups / waits included
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
        // the originals of the whole subtree, in order: the copies come out in the same order
        std::vector<juce::Uuid> originals;

        for (int i = index, end = document.cues.subtreeEnd (index); i < end; ++i)
            originals.push_back (document.cues.get (i).id);

        const int newIndex = document.cues.duplicate (index);

        if (newIndex < 0)
            return;

        juce::StringArray errors;

        for (size_t k = 0; k < originals.size(); ++k)
        {
            const int row = newIndex + (int) k;

            if (! document.cues.isValidIndex (row))
                break;

            if (autoNumber)
            {
                const auto number = CueNumbering::next (document.cues.getAll(), row, increment);
                document.cues.update (row, [number] (Cue& c) { c.number = number; });
            }
            else
            {
                document.cues.update (row, [] (Cue& c) { c.number.clear(); });   // numbers must stay unique
            }

            // the live plugin chain of every copied row (a group's children included)
            if (auto* source = engine.findCueChain (originals[k]); source != nullptr && source->getNumSlots() > 0)
                errors.addArray (engine.getCueChain (document.cues.get (row).id).restore (source->getStates(), engine.makePluginFactory()));
        }

        if (! errors.isEmpty())
            showAlert (ko ("일부 플러그인을 복제하지 못했습니다"), errors.joinIntoString ("\n"), false);

        document.cues.setSelectedIndex (newIndex);
        inspector.refreshPlugins();
    }, { {}, true });
}

void MainComponent::moveSelection (int delta)
{
    const auto rows = document.cues.getSelectedIndices();

    if (rows.empty() || showMode)
        return;

    const auto& cues = document.cues;
    const int first = rows.front();
    const int last = rows.back();
    int insertIndex;

    if (delta < 0)
    {
        // in front of the previous visible row (a group's last child: leaves the group; an open group above: enters it as the last child)
        insertIndex = cues.previousVisible (first);

        if (insertIndex < 0)
            return;
    }
    else
    {
        // behind the next visible row (an open group: becomes its first child; a closed one is skipped whole)
        const int next = cues.subtreeEnd (last);

        if (next >= cues.size())
            return;

        const auto& nextCue = cues.get (next);
        insertIndex = nextCue.isGroup() && nextCue.group.collapsed ? cues.subtreeEnd (next) : next + 1;
    }

    document.perform (ko ("큐 이동"), [this, rows, insertIndex] { document.cues.moveSubtrees (rows, insertIndex); });
}

void MainComponent::moveRows (const std::vector<int>& rows, int insertIndex)
{
    if (rows.empty() || showMode)
        return;

    document.perform (ko ("큐 이동"), [this, rows, insertIndex] { document.cues.moveSubtrees (rows, insertIndex); });
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
    showModeFlag.store (shouldBeShowMode, std::memory_order_release);
    activeCues.setScrubEnabled (! showMode);   // a stray click on a progress bar must not move a running cue
    table.setEditable (! showMode);
    cart.setEditable (! showMode);
    containerTabs.setEditable (! showMode);
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

    for (int row : document.cues.withSubtrees (rows))   // a group is copied with its children
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
    const int insertAt = selected >= 0 ? document.cues.subtreeEnd (selected) : document.cues.size();
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
        const auto parentAtPoint = document.cues.parentForInsertion (insertAt);
        std::map<juce::Uuid, juce::Uuid> newIds;   // copied groups keep their copied children

        for (const auto& c : copies)
            newIds[c.id] = juce::Uuid();

        for (size_t i = 0; i < copies.size(); ++i)
        {
            Cue cue = copies[i];
            cue.id = newIds[cue.id];               // a new identity: the original may still exist
            cue.parentId = newIds.count (cue.parentId) != 0 ? newIds[cue.parentId] : parentAtPoint;

            // references inside the pasted set follow the copies (a fade pasted with its target fades the copy)
            if (const auto target = cue.targetId(); ! target.isNull() && newIds.count (target) != 0)
                cue.setTargetId (newIds[target]);

            if (cue.isControl() && newIds.count (cue.control.secondTargetId) != 0)
                cue.control.secondTargetId = newIds[cue.control.secondTargetId];
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

    std::vector<Cue> everyCue;   // every list / cart (a read-only walk: forEachList is non-const only because it hands out the lists)
    const_cast<ProjectDocument&> (document).forEachList ([&everyCue] (CueList& list) { for (const auto& c : list.getAll()) everyCue.push_back (c); });

    for (const auto& cue : everyCue)
    {
        if (cue.isGroup() || (cue.isControl() && ! cue.control.needsTarget()))
            continue;   // no file, no target

        if (cue.isMic())
        {
            if (cue.mic.firstInput + cue.mic.numInputs > engine.getNumDeviceInputs())
                ++count;

            continue;
        }

        if (cue.isFade() || cue.isDevamp() || cue.isControl())
        {
            if (cue.targetId().isNull() || document.findCueAnywhere (cue.targetId()) == nullptr)
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

        if (cue.isGroup() || (cue.isControl() && ! cue.control.needsTarget()))
            continue;

        if (cue.isMic())
        {
            if (cue.mic.firstInput + cue.mic.numInputs > engine.getNumDeviceInputs())
                lines.add (label + ko (" - 장치 입력이 ") + juce::String (cue.mic.firstInput + cue.mic.numInputs) + ko ("개 필요한데 ")
                           + juce::String (engine.getNumDeviceInputs()) + ko ("개만 열려 있음 (오디오 설정)"));

            continue;
        }

        if (cue.isFade() || cue.isDevamp() || cue.isControl())
        {
            if (cue.targetId().isNull())
                lines.add (label + ko (" - 대상 큐가 지정되지 않음"));
            else if (document.findCueAnywhere (cue.targetId()) == nullptr)
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
    controller.resetForNewProject();   // fades, revert history, playlists, waits: nothing of this project survives
    pluginWindows.closeAll();
    engine.stopAll();
    engine.clearCueChains();
    engine.getMasterChain().clear();
    document.newProject();
    engine.setPatches (document.patches, true);
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
                                                   "*" + juce::String (ProjectSerializer::fileExtension) + ";*.gocue");

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
    controller.resetForNewProject();   // fades, revert history, playlists, waits: nothing of this project survives
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

    document.forEachList ([&] (CueList& list)   // every list / cart: an inactive list's cues play from hotkeys too
    {
        for (const auto& cue : list.getAll())
            if (! cue.plugins.empty())
                errors.addArray (engine.getCueChain (cue.id).restore (cue.plugins, factory));
    });

    if (! document.masterPlugins.empty())
        errors.addArray (engine.getMasterChain().restore (document.masterPlugins, factory));

    errors.addArray (engine.setPatches (document.patches, true));
}

void MainComponent::captureLivePluginStates (Project& project)
{
    for (auto& patch : project.patches)
        engine.capturePatchInsertStates (patch);

    for (auto& list : project.lists)
        for (auto& cue : list.cues)
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

    std::vector<Cue> allCues;   // every list / cart, not only the active one
    document.forEachList ([&allCues] (CueList& list) { for (const auto& c : list.getAll()) allCues.push_back (c); });

    for (const auto& cue : allCues)
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
        const auto* cuePtr = document.findCueAnywhere (p.id);

        if (cuePtr == nullptr)
        {
            engine.stop (p.id);
            continue;
        }

        const auto& cue = *cuePtr;
        engine.setLiveGainDb (p.id, cue.gainDb);
        engine.setLiveLevels (p.id, cue.levels, cue.trim);
        engine.setLiveRate (p.id, cue.audio.rate);
        engine.setLiveRegion (p.id, cue.audio.startSeconds, cue.audio.endSeconds);
        engine.setLiveSlices (p.id, cue.audio.slices, cue.audio.firstSliceCount);
        engine.setLivePlayCount (p.id, cue.audio.playCount, cue.audio.infiniteLoop);
    }

    ignorePluginChangesBriefly();   // replaying saved state is not a new edit
    inspector.refreshPlugins();
    PluginDialogs::chainChanged (&engine.getMasterChain());

    if (! errors.isEmpty())
        showAlert (ko ("일부 플러그인을 되돌리지 못했습니다"), errors.joinIntoString ("\n"), false);
}

void MainComponent::openProjectFromCommandLine (const juce::String& commandLine)
{
    const juce::ArgumentList args ("Enqueue", commandLine);

    for (const auto& arg : args.arguments)
    {
        const auto file = arg.resolveAsFile();

        if (file.existsAsFile() && file.hasFileExtension (ProjectSerializer::openableExtensions))
        {
            confirmReplaceProjectThen ([this, file] { openProjectFile (file); });   // never silently over a running show
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

    // not while cues play or the show mode is on: capturing plugin states holds the callback lock, and the disk write
    // sits on the message thread. It runs at the next idle tick instead.
    if (engine.getNumPlaying() > 0 || showMode)
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
    table.finishEditing();       // what is being typed is what gets saved
    inspector.finishEditing();

    auto writeTo = [this, then] (juce::File file)
    {
        if (! file.hasFileExtension (ProjectSerializer::openableExtensions))
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

void MainComponent::confirmReplaceProjectThen (std::function<void()> action, const juce::String& question)
{
    const int playing = engine.getNumPlaying();

    if (playing == 0 && ! showMode)
    {
        confirmDiscardChangesThen (std::move (action));
        return;
    }

    const auto what = question.isNotEmpty() ? question : ko ("다른 프로젝트를 열까요?");
    const auto message = playing > 0 ? ko ("재생 중인 큐 ") + juce::String (playing) + ko ("개가 모두 멈춥니다. ") + what
                                     : ko ("쇼 모드입니다. ") + what;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle (playing > 0 ? ko ("재생 중인 큐가 있습니다") : ko ("쇼 모드"))
                                      .withMessage (message)
                                      .withButton (playing > 0 ? ko ("모두 멈추고 계속") : ko ("계속"))
                                      .withButton (ko ("취소"))
                                      .withAssociatedComponent (this),
                                  [safeThis, action] (int result)
                                  {
                                      if (safeThis != nullptr && result == 1)
                                          safeThis->confirmDiscardChangesThen (action);
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

    document.forEachList ([&formats] (CueList& list)
    {
        for (int i = 0; i < list.size(); ++i)
            list.update (i, [&formats] (Cue& c) { refreshCueFileInfo (formats, c); });
    });

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
    text << "Enqueue " << juce::JUCEApplication::getInstance()->getApplicationVersion() << ko (" (앤큐)") << "\n"
         << ko ("Windows용 오디오 큐 플레이어") << "\n\n"
         << "JUCE " << JUCE_MAJOR_VERSION << "." << JUCE_MINOR_VERSION << "." << JUCE_BUILDNUMBER << "\n"
         << ko ("ASIO: ") << (JUCE_ASIO ? ko ("지원") : ko ("미포함 (WASAPI 전용)")) << "\n"
         << ko ("자동 업데이트: ") << (Updater::isAvailable() ? Updater::getAppcastUrl() : ko ("비활성 (빌드 시 appcast 미설정)"));

    showAlert (ko ("앤큐 정보"), text, false);
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
bool MainComponent::OperationalKeys::keyPressed (const juce::KeyPress& key, juce::Component* origin)
{
    if (key.getModifiers().isAnyModifierKeyDown())
        return false;

    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        owner.commands.invokeDirectly (CommandIDs::panicAll, false);   // panic from anywhere
        return true;
    }

    if (key.getKeyCode() == juce::KeyPress::spaceKey)
    {
        // a text field that did not consume Space is not being typed into; anything else means GO
        if (origin != nullptr && (dynamic_cast<juce::TextEditor*> (origin) != nullptr || origin->findParentComponentOfClass<juce::TextEditor>() != nullptr))
            return false;

        owner.commands.invokeDirectly (CommandIDs::go, false);
        return true;
    }

    return false;
}

void MainComponent::attachOperationalKeysToWindows()
{
    keyedWindows.erase (std::remove_if (keyedWindows.begin(), keyedWindows.end(), [] (const auto& w) { return w == nullptr; }), keyedWindows.end());

    auto* ownWindow = getTopLevelComponent();

    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
    {
        auto* window = juce::TopLevelWindow::getTopLevelWindow (i);

        if (window == nullptr || window == ownWindow)
            continue;

        const bool known = std::any_of (keyedWindows.begin(), keyedWindows.end(), [window] (const auto& w) { return w.getComponent() == window; });

        if (known)
            continue;

        window->addKeyListener (&operationalKeys);
        keyedWindows.emplace_back (window);
    }
}

void MainComponent::installEscapePolicy (juce::Component& root)
{
    // every text field cancels its edit on Esc and passes the panic on (the inspector's own panels do this already;
    // this catches the rest, and fields created later)
    for (auto* child : root.getChildren())
    {
        if (auto* editor = dynamic_cast<juce::TextEditor*> (child))
        {
            if (! editor->onEscapeKey)
            {
                juce::Component::SafePointer<juce::TextEditor> safeEditor (editor);
                editor->onEscapeKey = [this, safeEditor]
                {
                    if (safeEditor != nullptr)
                        safeEditor->giveAwayKeyboardFocus();

                    commands.invokeDirectly (CommandIDs::panicAll, false);
                };
            }
        }
        else if (child != nullptr)
            installEscapePolicy (*child);
    }
}

void MainComponent::timerCallback()
{
    if (--windowScanCountdown <= 0)
    {
        windowScanCountdown = 30;   // once a second
        attachOperationalKeysToWindows();
        installEscapePolicy (inspector);
    }

    engine.reapIfNeeded();   // finished players are destroyed here, never from the audio thread's callback

    if (! juce::Process::isForegroundProcess())
        hotkeyListener.heldKeys.clear();   // key-ups missed while another app had the focus must not look like auto-repeat

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
        const bool runOver = ! controller.isCueActive (closeCueId) && ! controller.hasPendingFor (closeCueId);   // a fade / group counts too

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

    if (cart.isVisible())
        cart.setPlayingCues (playing);

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

            if (cue.makesSound() && ! cue.fileMissing && (cue.isMic() || cue.file != juce::File()))
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

void MainComponent::addContainer (bool isCart)
{
    if (showMode)
        return;

    const juce::String name = (isCart ? ko ("카트 ") : ko ("큐 리스트 ")) + juce::String (document.getNumContainers() + 1);
    document.perform (isCart ? ko ("카트 추가") : ko ("큐 리스트 추가"), [this, name, isCart]
    {
        document.setActiveContainer (document.addContainer (name, isCart));
    });
}

void MainComponent::renameContainer (int index)
{
    if (showMode || index < 0 || index >= document.getNumContainers())
        return;

    auto* alert = new juce::AlertWindow (ko ("이름 바꾸기"), ko ("리스트 / 카트 이름"), juce::MessageBoxIconType::NoIcon, this);
    alert->addTextEditor ("name", document.getContainerInfo (index).name);
    alert->addButton (ko ("확인"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safeThis (this);
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, alert, index] (int result)
    {
        if (safeThis == nullptr || result != 1)
            return;

        const auto name = alert->getTextEditorContents ("name").trim();

        if (name.isEmpty() || index >= safeThis->document.getNumContainers())
            return;

        safeThis->document.perform (ko ("이름 바꾸기"), [safeThis, index, name] { safeThis->document.renameContainer (index, name); });
    }), true);
    focusAlertEditor (*alert, "name");
}

void MainComponent::removeContainer (int index)
{
    if (showMode || index < 0 || index >= document.getNumContainers() || document.getNumContainers() <= 1)
        return;

    const auto info = document.getContainerInfo (index);
    const auto ids = document.cueIdsOf (index);   // active or not: hotkeys / control cues may have started them

    for (const auto& id : ids)
    {
        controller.stopCue (id);
        engine.removeCueChain (id);
    }

    document.perform (ko ("리스트/카트 삭제: ") + info.name, [this, index] { document.removeContainer (index); }, { {}, true });
}

void MainComponent::toggleContainerCart (int index)
{
    if (showMode || index < 0 || index >= document.getNumContainers())
        return;

    const auto info = document.getContainerInfo (index);
    document.perform (info.isCart ? ko ("큐 리스트로 전환") : ko ("카트로 전환"), [this, index, info]
    {
        document.setContainerCart (index, ! info.isCart, info.cartRows, info.cartCols);
    });
}

void MainComponent::setContainerGrid (int index)
{
    if (showMode || index < 0 || index >= document.getNumContainers())
        return;

    const auto info = document.getContainerInfo (index);
    auto* alert = new juce::AlertWindow (ko ("카트 격자 크기"), ko ("행 x 열 (1~15)"), juce::MessageBoxIconType::NoIcon, this);
    alert->addTextEditor ("rows", juce::String (info.cartRows), ko ("행"));
    alert->addTextEditor ("cols", juce::String (info.cartCols), ko ("열"));
    alert->addButton (ko ("확인"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safeThis (this);
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, alert, index] (int result)
    {
        if (safeThis == nullptr || result != 1 || index >= safeThis->document.getNumContainers())
            return;

        const int rows = juce::jlimit (1, CueContainer::maxGrid, alert->getTextEditorContents ("rows").getIntValue());
        const int cols = juce::jlimit (1, CueContainer::maxGrid, alert->getTextEditorContents ("cols").getIntValue());
        const auto current = safeThis->document.getContainerInfo (index);
        safeThis->document.perform (ko ("카트 격자 크기"), [safeThis, index, current, rows, cols]
        {
            safeThis->document.setContainerCart (index, current.isCart, rows, cols);
        });
    }), true);
    focusAlertEditor (*alert, "rows");
}

void MainComponent::updateContainerView()
{
    const bool isCart = document.isActiveCart();
    const auto info = document.getContainerInfo (document.getActiveContainer());
    cart.setGrid (info.cartRows, info.cartCols);
    cart.setVisible (isCart);
    table.setVisible (! isCart);
    containerTabs.refresh();
    commands.commandStatusChanged();

    if (isCart)
        cart.grabKeyboardFocus();
    else
        table.focusTable();
}

void MainComponent::containersChanged()
{
    updateContainerView();
}

void MainComponent::documentStateChanged()
{
    unsavedChanges.store (document.isDirty(), std::memory_order_relaxed);
    updateInputsWanted();
    commands.commandStatusChanged();   // undo / redo names and availability
    document.cues.setLockPlayheadToSelection (document.settings.lockPlayheadToSelection);
    table.setRowSize (document.settings.rowSize);
    transport.setPanicSeconds (document.settings.panicSeconds);
    transport.setAuditionMode (document.settings.alwaysAudition);

    if (onWindowTitleChanged)
        onWindowTitleChanged (document.getWindowTitle());
}

} // namespace gocue
