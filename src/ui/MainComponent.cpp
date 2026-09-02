#include "ui/MainComponent.h"

#include "app/BackupManager.h"
#include "app/Commands.h"
#include "app/Updater.h"
#include "audio/CueFileInfo.h"
#include "ui/AudioSettingsDialog.h"
#include "ui/PluginDialogs.h"
#include "ui/UiUtils.h"
#include "ui/WorkspaceSettingsDialog.h"

#include <set>

namespace gocue
{

namespace
{
    constexpr int menuBarHeight = 24;
    constexpr int transportHeight = 112;
    constexpr int inspectorHeight = 330;
    constexpr double pluginChangeGraceMs = 1500.0;
}

MainComponent::MainComponent (AudioEngine& e, AppSettings& s, juce::ApplicationCommandManager& cm)
    : engine (e),
      settings (s),
      commands (cm),
      controller (e, document, scheduler),
      menuBar (this),
      transport (cm),
      table (document.cues, e.getFormatManager(), cm),
      inspector (document, e, s, pluginWindows)
{
    setWantsKeyboardFocus (true);

    addAndMakeVisible (menuBar);
    addAndMakeVisible (transport);
    addAndMakeVisible (table);
    addAndMakeVisible (inspector);

    controller.onStatus = [this] (const juce::String& message, bool isError) { transport.showStatus (message, isError); };
    controller.onGoRejected = [this] { transport.flashGoRejected(); };

    table.onFilesDropped = [this] (const juce::StringArray& files, int insertAt) { addCuesFromFiles (files, insertAt); };
    inspector.onOpenPluginManager = [this] { showPluginManager(); };
    inspector.onPanic = [this] { controller.panicAll(); table.focusTable(); };
    inspector.onPreview = [this] { controller.preview(); };
    inspector.onResetCue = [this] { controller.resetSelected(); };

    document.snapshotDecorator = [this] (Project& project) { captureLivePluginStates (project); };
    document.onSnapshotRestored = [this] (const ProjectSnapshot& snapshot) { reconcileChainsAfterRestore (snapshot); };

    engine.setChainListener (&pluginWindows);
    pluginWindows.onChainChanged = [this] (PluginChain& chain)
    {
        document.markDirty();
        inspector.pluginChainChanged (&chain);
        PluginDialogs::chainChanged (&chain);
    };

    document.cues.addListener (this);
    document.addListener (this);

    commands.registerAllCommandsForTarget (this);
    commands.setFirstCommandTarget (this);
    addKeyListener (commands.getKeyMappings());
    setApplicationCommandManagerToWatch (&commands);

    setSize (1100, 780);
    updateTransportStandby();
    startTimerHz (30);
    scheduler.startTicking (1);   // pre-waits, post-waits, auto-follows
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
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    menuBar.setBounds (area.removeFromTop (menuBarHeight));
    transport.setBounds (area.removeFromTop (transportHeight));
    inspector.setBounds (area.removeFromBottom (inspectorHeight));
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

    return containsAudioOrFolder (engine.getFormatManager(), files);
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
                    CommandIDs::resetCue, CommandIDs::resetAll,
                    CommandIDs::addCue, CommandIDs::removeCue, CommandIDs::duplicateCue,
                    CommandIDs::moveCueUp, CommandIDs::moveCueDown,
                    CommandIDs::newProject, CommandIDs::openProject,
                    CommandIDs::saveProject, CommandIDs::saveProjectAs,
                    CommandIDs::undo, CommandIDs::redo,
                    CommandIDs::audioSettings, CommandIDs::pluginManager, CommandIDs::masterInserts,
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
    const int selected = document.cues.getSelectedIndex();
    using juce::KeyPress;
    using juce::ModifierKeys;

    switch (commandID)
    {
        case CommandIDs::go:
            result.setInfo ("GO", ko ("선택 큐 재생 후 다음 큐로 이동 (일시정지된 큐가 있으면 재개)"), playback, 0);
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
            result.setInfo (ko ("미리듣기"), ko ("선택 큐를 재생하되 플레이헤드는 그대로 둠"), playback, 0);
            result.addDefaultKeypress ('V', ModifierKeys::noModifiers);
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
            break;

        case CommandIDs::removeCue:
            result.setInfo (ko ("큐 삭제"), ko ("선택 큐 삭제"), cueMenu, 0);
            result.addDefaultKeypress (KeyPress::deleteKey, ModifierKeys::noModifiers);
            result.setActive (hasSelection);
            break;

        case CommandIDs::duplicateCue:
            result.setInfo (ko ("큐 복제"), ko ("선택 큐를 플러그인 체인까지 바로 아래에 복제"), cueMenu, 0);
            result.addDefaultKeypress ('D', ModifierKeys::commandModifier);
            result.setActive (hasSelection);
            break;

        case CommandIDs::moveCueUp:
            result.setInfo (ko ("위로 이동"), ko ("선택 큐를 한 칸 위로"), cueMenu, 0);
            result.addDefaultKeypress (KeyPress::upKey, ModifierKeys::commandModifier);
            result.setActive (selected > 0);
            break;

        case CommandIDs::moveCueDown:
            result.setInfo (ko ("아래로 이동"), ko ("선택 큐를 한 칸 아래로"), cueMenu, 0);
            result.addDefaultKeypress (KeyPress::downKey, ModifierKeys::commandModifier);
            result.setActive (hasSelection && selected < document.cues.size() - 1);
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
            result.setActive (document.canUndo());
            break;

        case CommandIDs::redo:
            result.setInfo (document.canRedo() ? ko ("다시 실행: ") + document.getRedoName() : ko ("다시 실행"),
                            ko ("되돌린 편집을 다시 적용합니다"), editMenu, 0);
            result.addDefaultKeypress ('Y', ModifierKeys::commandModifier);
            result.addDefaultKeypress ('Z', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            result.setActive (document.canRedo());
            break;

        case CommandIDs::audioSettings:
            result.setInfo (ko ("오디오 출력 설정..."), ko ("출력 장치(ASIO / WASAPI) 선택"), audio, 0);
            result.addDefaultKeypress (',', ModifierKeys::commandModifier);
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
    const int selected = document.cues.getSelectedIndex();

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

        case CommandIDs::resetCue:
            controller.resetSelected();
            break;

        case CommandIDs::resetAll:
            controller.resetAll();
            break;

        case CommandIDs::addCue:
            addCueViaDialog();
            break;

        case CommandIDs::removeCue:
            removeSelectedCue();
            break;

        case CommandIDs::duplicateCue:
            duplicateSelectedCue();
            break;

        case CommandIDs::moveCueUp:
            if (selected > 0)
                document.perform (ko ("큐 이동"), [this, selected] { document.cues.move (selected, selected - 1); });
            break;

        case CommandIDs::moveCueDown:
            if (selected >= 0 && selected < document.cues.size() - 1)
                document.perform (ko ("큐 이동"), [this, selected] { document.cues.move (selected, selected + 1); });
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

        case CommandIDs::audioSettings:
            AudioSettingsDialog::show (engine.getDeviceManager(), this);
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
            break;

        case 2:
            menu.addCommandItem (&commands, CommandIDs::addCue);
            menu.addCommandItem (&commands, CommandIDs::removeCue);
            menu.addCommandItem (&commands, CommandIDs::duplicateCue);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::moveCueUp);
            menu.addCommandItem (&commands, CommandIDs::moveCueDown);
            break;

        case 3:
            menu.addCommandItem (&commands, CommandIDs::go);
            menu.addCommandItem (&commands, CommandIDs::preview);
            menu.addCommandItem (&commands, CommandIDs::pauseToggle);
            menu.addCommandItem (&commands, CommandIDs::fadeOutSelected);
            menu.addCommandItem (&commands, CommandIDs::resetCue);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::panicAll);
            menu.addCommandItem (&commands, CommandIDs::hardStopAll);
            menu.addCommandItem (&commands, CommandIDs::resetAll);
            break;

        case 4:
            menu.addCommandItem (&commands, CommandIDs::audioSettings);
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
    if (files.isEmpty())
        return;

    const bool copyIn = document.settings.copyFilesIntoProject && document.hasFile();
    const auto projectDir = document.getFile().getParentDirectory();

    document.perform (files.size() == 1 ? ko ("큐 추가") : ko ("큐 추가 (") + juce::String (files.size()) + ")", [this, files, insertAt, copyIn, projectDir]
    {
        int index = insertAt;
        int last = -1;

        for (const auto& path : files)
        {
            const juce::File file = copyIn ? BackupManager::copyIntoProject (juce::File (path), projectDir) : juce::File (path);

            Cue cue;
            cue.name = file.getFileNameWithoutExtension();
            cue.file = file;
            refreshCueFileInfo (engine.getFormatManager(), cue);

            last = document.cues.add (std::move (cue), index);
            index = last + 1;
        }

        if (last >= 0)
            document.cues.setSelectedIndex (last);
    });

    settings.setLastAudioDirectory (juce::File (files[0]).getParentDirectory());
}

void MainComponent::addCueViaDialog()
{
    if (chooser != nullptr)
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

void MainComponent::removeSelectedCue()
{
    const int index = document.cues.getSelectedIndex();

    if (! document.cues.isValidIndex (index))
        return;

    const auto id = document.cues.get (index).id;

    document.perform (ko ("큐 삭제"), [this, index, id]
    {
        engine.stop (id);
        engine.removeCueChain (id);
        document.cues.remove (index);
    }, { {}, true });
}

void MainComponent::duplicateSelectedCue()
{
    const int index = document.cues.getSelectedIndex();

    if (! document.cues.isValidIndex (index))
        return;

    document.perform (ko ("큐 복제"), [this, index]
    {
        const auto sourceId = document.cues.get (index).id;
        const int newIndex = document.cues.duplicate (index);

        if (newIndex < 0)
            return;

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

void MainComponent::newProject()
{
    pluginWindows.closeAll();
    engine.stopAll();
    engine.clearCueChains();
    engine.getMasterChain().clear();
    document.newProject();
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

    pluginWindows.closeAll();
    engine.stopAll();
    engine.clearCueChains();
    engine.getMasterChain().clear();
    document.adopt (std::move (candidate), file);

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
}

void MainComponent::restorePluginChainsFromDocument (juce::StringArray& errors)
{
    const auto factory = engine.makePluginFactory();

    for (const auto& cue : document.cues.getAll())
        if (! cue.plugins.empty())
            errors.addArray (engine.getCueChain (cue.id).restore (cue.plugins, factory));

    if (! document.masterPlugins.empty())
        errors.addArray (engine.getMasterChain().restore (document.masterPlugins, factory));
}

void MainComponent::captureLivePluginStates (Project& project)
{
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
        else if (snapshot.pluginStatesCaptured && ! chain->matchesStructure (cue.plugins))
        {
            errors.addArray (chain->restore (cue.plugins, factory));
        }
    }

    for (const auto& id : engine.getCueChainIds())
        if (liveIds.count (id.toString()) == 0)
            engine.removeCueChain (id);

    if (snapshot.pluginStatesCaptured && ! engine.getMasterChain().matchesStructure (document.masterPlugins))
        errors.addArray (engine.getMasterChain().restore (document.masterPlugins, factory));

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

    auto* alert = new juce::AlertWindow (ko ("변경 사항 저장"),
                                         "\"" + document.getDisplayName() + "\"" + ko ("에 저장하지 않은 변경 사항이 있습니다. 저장할까요?"),
                                         juce::MessageBoxIconType::QuestionIcon, this);
    alert->addButton (ko ("저장"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("저장 안 함"), 2);
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    alert->setVisible (true);

    alert->enterModalState (true, juce::ModalCallbackFunction::create ([this, action] (int result)
    {
        if (result == 1)
            saveProject (false, [action] (bool ok) { if (ok && action) action(); });
        else if (result == 2 && action)
            action();
    }), true);
}

void MainComponent::backupBeforeSave (const juce::File& file)
{
    const auto& s = document.settings;

    if (! s.backupBeforeSave || ! file.existsAsFile())
        return;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();

    if (nowMs - lastSaveBackupMs < 60 * 1000.0)   // at most one pre-save backup per minute
        return;

    const auto result = BackupManager::copyToBackups (file, juce::Time::getCurrentTime());

    if (result.failed())
    {
        transport.showStatus (ko ("백업 실패: ") + result.getErrorMessage(), true);
        return;
    }

    lastSaveBackupMs = nowMs;

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

    const auto target = BackupManager::backupFileFor (document.getFile(), juce::Time::getCurrentTime());
    const auto result = ProjectSerializer::save (project, target);

    if (result.failed())
    {
        transport.showStatus (ko ("자동 백업 실패: ") + result.getErrorMessage(), true);
        return;
    }

    if (s.rotateBackups)
        BackupManager::rotate (target.getParentDirectory(), juce::Time::getCurrentTime());
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
    transport.setStandbyCue (document.cues.getSelectedIndex(), document.cues.getSelected());
}

//==============================================================================
void MainComponent::timerCallback()
{
    auto playing = engine.getPlayingCues();
    int paused = 0;

    for (const auto& p : playing)
        if (p.paused)
            ++paused;

    transport.setPlayingCount ((int) playing.size(), paused);
    transport.setGoLocked (controller.isGoLocked());
    inspector.setPlayback (playing);
    table.setPlayingCues (std::move (playing));

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
    if (index == document.cues.getSelectedIndex())
        updateTransportStandby();
}

void MainComponent::cueSelectionChanged (int)
{
    updateTransportStandby();
    commands.commandStatusChanged();
}

void MainComponent::documentStateChanged()
{
    unsavedChanges.store (document.isDirty(), std::memory_order_relaxed);
    commands.commandStatusChanged();   // undo / redo names and availability

    if (onWindowTitleChanged)
        onWindowTitleChanged (document.getWindowTitle());
}

} // namespace gocue
