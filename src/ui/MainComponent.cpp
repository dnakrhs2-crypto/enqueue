#include "ui/MainComponent.h"

#include "app/Commands.h"
#include "audio/CueFileInfo.h"
#include "ui/AudioSettingsDialog.h"
#include "ui/UiUtils.h"

namespace gocue
{

namespace
{
    constexpr int menuBarHeight = 24;
    constexpr int transportHeight = 112;
    constexpr int inspectorHeight = 170;
}

MainComponent::MainComponent (AudioEngine& e, AppSettings& s, juce::ApplicationCommandManager& cm)
    : engine (e),
      settings (s),
      commands (cm),
      menuBar (this),
      transport (cm),
      table (document.cues, e.getFormatManager(), cm),
      inspector (document.cues, e.getFormatManager(), s)
{
    setWantsKeyboardFocus (true);

    addAndMakeVisible (menuBar);
    addAndMakeVisible (transport);
    addAndMakeVisible (table);
    addAndMakeVisible (inspector);

    table.onFilesDropped = [this] (const juce::StringArray& files, int insertAt) { addCuesFromFiles (files, insertAt); };

    document.cues.addListener (this);
    document.addListener (this);

    commands.registerAllCommandsForTarget (this);
    commands.setFirstCommandTarget (this);
    addKeyListener (commands.getKeyMappings());
    setApplicationCommandManagerToWatch (&commands);

    setSize (1100, 720);
    updateTransportStandby();
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
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

//==============================================================================
juce::ApplicationCommandTarget* MainComponent::getNextCommandTarget()
{
    return juce::JUCEApplication::getInstance();
}

void MainComponent::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    ids.addArray ({ CommandIDs::go, CommandIDs::stopSelected, CommandIDs::stopAll,
                    CommandIDs::fadeOutSelected, CommandIDs::fadeOutAll,
                    CommandIDs::addCue, CommandIDs::removeCue, CommandIDs::duplicateCue,
                    CommandIDs::moveCueUp, CommandIDs::moveCueDown,
                    CommandIDs::newProject, CommandIDs::openProject,
                    CommandIDs::saveProject, CommandIDs::saveProjectAs,
                    CommandIDs::audioSettings });
}

void MainComponent::getCommandInfo (juce::CommandID commandID, juce::ApplicationCommandInfo& result)
{
    const auto playback = ko ("재생");
    const auto cueMenu  = ko ("큐");
    const auto fileMenu = ko ("파일");
    const auto audio    = ko ("오디오");
    const bool hasSelection = document.cues.getSelected() != nullptr;
    const int selected = document.cues.getSelectedIndex();
    using juce::KeyPress;
    using juce::ModifierKeys;

    switch (commandID)
    {
        case CommandIDs::go:
            result.setInfo ("GO", ko ("선택 큐 재생 후 다음 큐로 이동"), playback, 0);
            result.addDefaultKeypress (KeyPress::spaceKey, ModifierKeys::noModifiers);
            result.setActive (hasSelection);
            break;

        case CommandIDs::stopSelected:
            result.setInfo (ko ("정지"), ko ("선택 큐(재생 중이 아니면 가장 최근 재생 큐) 즉시 정지"), playback, 0);
            result.addDefaultKeypress ('S', ModifierKeys::noModifiers);
            break;

        case CommandIDs::stopAll:
            result.setInfo (ko ("전체 정지"), ko ("재생 중인 모든 큐 즉시 정지"), playback, 0);
            result.addDefaultKeypress (KeyPress::escapeKey, ModifierKeys::noModifiers);
            break;

        case CommandIDs::fadeOutSelected:
            result.setInfo (ko ("페이드아웃 정지"), ko ("선택 큐(재생 중이 아니면 가장 최근 재생 큐)를 페이드아웃 후 정지"), playback, 0);
            result.addDefaultKeypress ('F', ModifierKeys::noModifiers);
            break;

        case CommandIDs::fadeOutAll:
            result.setInfo (ko ("전체 페이드아웃 정지"), ko ("재생 중인 모든 큐를 각자의 페이드아웃 시간으로 정지"), playback, 0);
            result.addDefaultKeypress ('F', ModifierKeys::shiftModifier);
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
            result.setInfo (ko ("큐 복제"), ko ("선택 큐를 바로 아래에 복제"), cueMenu, 0);
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
            result.setInfo (ko ("저장"), ko ("프로젝트 저장"), fileMenu, 0);
            result.addDefaultKeypress ('S', ModifierKeys::commandModifier);
            break;

        case CommandIDs::saveProjectAs:
            result.setInfo (ko ("다른 이름으로 저장..."), ko ("프로젝트를 새 파일로 저장"), fileMenu, 0);
            result.addDefaultKeypress ('S', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;

        case CommandIDs::audioSettings:
            result.setInfo (ko ("오디오 출력 설정..."), ko ("출력 장치(ASIO / WASAPI) 선택"), audio, 0);
            result.addDefaultKeypress (',', ModifierKeys::commandModifier);
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
            go();
            break;

        case CommandIDs::stopSelected:
            if (const auto id = resolveTargetCue (false); ! id.isNull())
                engine.stop (id);
            break;

        case CommandIDs::stopAll:
            engine.stopAll();
            break;

        case CommandIDs::fadeOutSelected:
            if (const auto id = resolveTargetCue (true); ! id.isNull())
                engine.fadeOutAndStop (id);
            break;

        case CommandIDs::fadeOutAll:
            engine.fadeOutAndStopAll();
            break;

        case CommandIDs::addCue:
            addCueViaDialog();
            break;

        case CommandIDs::removeCue:
            removeSelectedCue();
            break;

        case CommandIDs::duplicateCue:
            if (const int index = document.cues.duplicate (selected); index >= 0)
                document.cues.setSelectedIndex (index);
            break;

        case CommandIDs::moveCueUp:
            document.cues.move (selected, selected - 1);
            break;

        case CommandIDs::moveCueDown:
            document.cues.move (selected, selected + 1);
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

        case CommandIDs::audioSettings:
            AudioSettingsDialog::show (engine.getDeviceManager(), this);
            break;

        default:
            return false;
    }

    return true;
}

//==============================================================================
juce::StringArray MainComponent::getMenuBarNames()
{
    return { ko ("파일"), ko ("큐"), ko ("재생"), ko ("오디오") };
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
            menu.addCommandItem (&commands, juce::StandardApplicationCommandIDs::quit);
            break;

        case 1:
            menu.addCommandItem (&commands, CommandIDs::addCue);
            menu.addCommandItem (&commands, CommandIDs::removeCue);
            menu.addCommandItem (&commands, CommandIDs::duplicateCue);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::moveCueUp);
            menu.addCommandItem (&commands, CommandIDs::moveCueDown);
            break;

        case 2:
            menu.addCommandItem (&commands, CommandIDs::go);
            menu.addCommandItem (&commands, CommandIDs::stopSelected);
            menu.addCommandItem (&commands, CommandIDs::fadeOutSelected);
            menu.addSeparator();
            menu.addCommandItem (&commands, CommandIDs::stopAll);
            menu.addCommandItem (&commands, CommandIDs::fadeOutAll);
            break;

        case 3:
            menu.addCommandItem (&commands, CommandIDs::audioSettings);
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
void MainComponent::go()
{
    const int index = document.cues.getSelectedIndex();
    const auto* cue = document.cues.getSelected();

    if (cue == nullptr)
        return;

    juce::String error;

    if (engine.play (*cue, &error))
        transport.showStatus (ko ("GO: #") + juce::String (index + 1) + " " + cue->name, false);
    else
        transport.showStatus (error, true);

    document.cues.selectNext();
    table.focusTable();
}

juce::Uuid MainComponent::resolveTargetCue (bool ignoreFadingOut) const
{
    if (const auto* selected = document.cues.getSelected())
        for (const auto& p : engine.getPlayingCues())
            if (p.id == selected->id && ! (ignoreFadingOut && p.fadingOut))
                return p.id;

    return engine.getMostRecentlyStartedCue (ignoreFadingOut);
}

void MainComponent::addCuesFromFiles (const juce::StringArray& files, int insertAt)
{
    int index = insertAt;
    int last = -1;

    for (const auto& path : files)
    {
        const juce::File file (path);

        Cue cue;
        cue.name = file.getFileNameWithoutExtension();
        cue.file = file;
        refreshCueFileInfo (engine.getFormatManager(), cue);

        last = document.cues.add (std::move (cue), index);
        index = last + 1;
    }

    if (last >= 0)
    {
        document.cues.setSelectedIndex (last);
        settings.setLastAudioDirectory (juce::File (files[0]).getParentDirectory());
    }
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

    engine.stop (document.cues.get (index).id);
    document.cues.remove (index);
}

void MainComponent::newProject()
{
    engine.stopAll();
    document.newProject();
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
    engine.stopAll();

    juce::StringArray warnings;
    const auto result = document.load (file, &warnings);

    if (result.failed())
    {
        showAlert (ko ("프로젝트 열기 실패"), result.getErrorMessage(), true);
        return;
    }

    settings.setLastProjectFile (file);
    refreshFileInfoForAllCues();
    transport.showStatus (ko ("열림: ") + file.getFileName(), false);

    if (! warnings.isEmpty())
        showAlert (ko ("프로젝트를 열었지만 확인이 필요합니다"), warnings.joinIntoString ("\n"), false);
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

        const auto result = document.save (file);

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

void MainComponent::refreshFileInfoForAllCues()
{
    const bool wasDirty = document.isDirty();
    auto& formats = engine.getFormatManager();

    for (int i = 0; i < document.cues.size(); ++i)
        document.cues.update (i, [&formats] (Cue& c) { refreshCueFileInfo (formats, c); });

    if (! wasDirty)
        document.markClean();
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
    transport.setPlayingCount ((int) playing.size());
    table.setPlayingCues (std::move (playing));
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
    if (onWindowTitleChanged)
        onWindowTitleChanged (document.getWindowTitle());
}

} // namespace gocue
