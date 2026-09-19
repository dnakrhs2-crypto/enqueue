#include "app/ShortcutCatalog.h"
#include "app/Commands.h"

namespace gocue
{
namespace
{
using Category = ShortcutCategory;
using Scope = ShortcutScope;
using juce::KeyPress;
using juce::ModifierKeys;

juce::String ko (const char* text) { return juce::String::fromUTF8 (text); }
juce::KeyPress key (int code, int modifiers = 0) { return { code, modifiers, 0 }; }

ShortcutDefinition command (const char* id, juce::CommandID commandID, juce::String name,
                            juce::String description, Category category, Scope scope, bool repeat,
                            juce::String menuCategory, ShortcutKeys keys)
{
    int flags = commandID == CommandIDs::go ? juce::ApplicationCommandInfo::wantsKeyUpDownCallbacks : 0;
   #if ! JUCE_WINDOWS
    if (commandID == CommandIDs::panicAll)
        flags |= juce::ApplicationCommandInfo::wantsKeyUpDownCallbacks;
   #endif
    return { id, std::move (name), std::move (description), category, scope, repeat,
             commandID, std::move (keys), std::move (menuCategory), flags };
}

std::vector<ShortcutDefinition> makeCommands()
{
    return {
        command ("transport.go", CommandIDs::go, "GO", ko ("플레이헤드 큐 재생(시퀀스 포함) 후 다음 큐로 이동 (일시정지된 큐가 있으면 재개)"),
                 Category::playback, Scope::playback, false, ko ("재생"), { key (KeyPress::spaceKey, ModifierKeys::noModifiers) }),
        command ("transport.pauseToggle", CommandIDs::pauseToggle, ko ("일시정지 / 재개"), ko ("선택 큐(재생 중이 아니면 가장 최근 재생 큐)를 일시정지하거나 재개"),
                 Category::playback, Scope::playback, false, ko ("재생"), { key ('P', ModifierKeys::noModifiers) }),
        command ("transport.fadeOutSelected", CommandIDs::fadeOutSelected, ko ("페이드아웃 정지"), ko ("선택 큐(재생 중이 아니면 가장 최근 재생 큐)를 정지 페이드로 정지"),
                 Category::playback, Scope::playback, false, ko ("재생"), { key ('F', ModifierKeys::noModifiers) }),
        command ("transport.panicAll", CommandIDs::panicAll, ko ("전체 페이드 정지 (Esc)"), ko ("재생 중인 모든 큐를 설정된 시간(기본 2초) 동안 페이드아웃 후 정지. 0.5초 안에 두 번 누르면 즉시 정지"),
                 Category::playback, Scope::application, false, ko ("재생"), { key (KeyPress::escapeKey, ModifierKeys::noModifiers) }),
        command ("transport.hardStopAll", CommandIDs::hardStopAll, ko ("전체 즉시 정지"), ko ("페이드 없이 모든 큐를 바로 정지"),
                 Category::playback, Scope::playback, false, ko ("재생"), {  }),
        command ("transport.preview", CommandIDs::preview, ko ("미리듣기"), ko ("선택 큐만 재생 (프리웨이트·시퀀스 없이, 플레이헤드는 그대로)"),
                 Category::playback, Scope::playback, false, ko ("재생"), { key ('V', ModifierKeys::noModifiers) }),
        command ("transport.auditionGo", CommandIDs::auditionGo, ko ("오디션 GO"), ko ("프로젝트 설정의 오디션 방식(그대로 / 출력 없음 / 대체 패치)으로 GO"),
                 Category::playback, Scope::playback, false, ko ("재생"), { key (KeyPress::spaceKey, ModifierKeys::altModifier) }),
        command ("transport.auditionPreview", CommandIDs::auditionPreview, ko ("오디션 미리듣기"), ko ("선택 큐만 오디션 방식으로 재생"),
                 Category::playback, Scope::playback, false, ko ("재생"), { key ('V', ModifierKeys::altModifier) }),
        command ("transport.toggleAlwaysAudition", CommandIDs::toggleAlwaysAudition, ko ("항상 오디션"), ko ("켜면 모든 GO / 미리듣기가 오디션 방식으로 재생됩니다 (GO 버튼이 파랗게)"),
                 Category::playback, Scope::playback, false, ko ("재생"), {  }),
        command ("transport.loadCue", CommandIDs::loadCue, ko ("로드"), ko ("선택 큐를 미리 로드해 GO 지연을 없앰"),
                 Category::playback, Scope::playback, false, ko ("재생"), { key ('L', ModifierKeys::noModifiers) }),
        command ("transport.loadToTime", CommandIDs::loadToTime, ko ("시간으로 로드..."), ko ("선택 큐를 특정 위치에 로드 (음수 = 끝에서부터)"),
                 Category::playback, Scope::playback, false, ko ("재생"), { key ('T', ModifierKeys::commandModifier) }),
        command ("transport.resetCue", CommandIDs::resetCue, ko ("큐 리셋"), ko ("선택 큐를 정지하고 처음 상태로"),
                 Category::playback, Scope::playback, false, ko ("재생"), {  }),
        command ("transport.resetAll", CommandIDs::resetAll, ko ("전체 리셋"), ko ("모든 큐를 즉시 정지하고 플레이헤드를 첫 큐로"),
                 Category::playback, Scope::playback, false, ko ("재생"), {  }),
        command ("cue.addAudio", CommandIDs::addCue, ko ("큐 추가..."), ko ("오디오 파일을 골라 큐를 추가"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key (KeyPress::insertKey, ModifierKeys::noModifiers) }),
        command ("cue.addFade", CommandIDs::addFadeCue, ko ("페이드 인 큐 추가"), ko ("선택한 소리 큐를 대상으로: 실행하면 대상을 무음에서 시작해 설정 시간 동안 원래 레벨까지 올립니다"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('7', ModifierKeys::commandModifier) }),
        command ("cue.addFadeOut", CommandIDs::addFadeOutCue, ko ("페이드 아웃 큐 추가"), ko ("선택한 소리 큐를 대상으로: 실행하면 대상을 설정 시간 동안 무음까지 내리고 정지합니다"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('7', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("cue.addDevamp", CommandIDs::addDevampCue, ko ("디밴프 큐 추가"), ko ("선택한 오디오 큐를 대상으로: 실행하면 대상이 지금 도는 반복을 마치고 이어가거나 멈추고, 그 순간 다음 큐를 시작"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('8', ModifierKeys::commandModifier) }),
        command ("cue.addGroup", CommandIDs::addGroupCue, ko ("그룹 큐 추가"), ko ("빈 그룹을 선택 뒤에 추가 (자식은 그룹 아래로 끌어다 넣거나 Ctrl+G로 묶기)"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('0', ModifierKeys::commandModifier) }),
        command ("cue.groupSelected", CommandIDs::groupSelectedCues, ko ("선택한 큐 그룹으로 묶기"), ko ("선택한 큐(하위 포함)를 새 그룹 안에 넣음"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('G', ModifierKeys::commandModifier) }),
        command ("cue.ungroupSelected", CommandIDs::ungroupSelected, ko ("그룹 해제"), ko ("선택한 그룹을 없애고 자식을 한 단계 위로"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('G', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("cue.collapseAllGroups", CommandIDs::collapseAllGroups, ko ("모든 그룹 접기"), ko ("모든 그룹의 자식을 숨김"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("cue.addControl", CommandIDs::addControlCue, ko ("제어 큐 추가"), ko ("선택한 큐를 대상으로 하는 제어 큐 (시작/정지/일시정지/로드/리셋/이동/활성화/비활성화/대상 변경 — 종류는 인스펙터에서)"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('9', ModifierKeys::commandModifier) }),
        command ("cue.addWait", CommandIDs::addWaitCue, ko ("대기 큐 추가"), ko ("정해진 시간 동안 아무것도 하지 않는 큐 (자동 팔로우 앞에 시간을 둘 때)"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("cue.addMic", CommandIDs::addMicCue, ko ("마이크 큐 추가"), ko ("장치 입력을 레벨 매트릭스·인서트·패치로 보내는 큐 (정지할 때까지)"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('6', ModifierKeys::commandModifier) }),
        command ("cue.addMemo", CommandIDs::addMemoCue, ko ("메모 큐 추가"), ko ("아무것도 하지 않는 큐 (목록 안의 메모)"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("cue.addList", CommandIDs::addCueList, ko ("새 큐 리스트"), ko ("큐 리스트를 하나 더 추가 (위 탭)"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("cue.addCart", CommandIDs::addCart, ko ("새 카트"), ko ("버튼 격자 카트를 추가 — 클릭하면 바로 재생, 플레이헤드/자동 진행 없음"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("cue.nextContainer", CommandIDs::nextContainer, ko ("다음 리스트/카트"), ko ("오른쪽 탭으로"),
                 Category::cue, Scope::mainWindow, true, ko ("큐"), { key (juce::KeyPress::pageDownKey, ModifierKeys::commandModifier) }),
        command ("cue.previousContainer", CommandIDs::previousContainer, ko ("이전 리스트/카트"), ko ("왼쪽 탭으로"),
                 Category::cue, Scope::mainWindow, true, ko ("큐"), { key (juce::KeyPress::pageUpKey, ModifierKeys::commandModifier) }),
        command ("cue.renameContainer", CommandIDs::renameContainer, ko ("리스트/카트 이름 바꾸기..."), ko ("현재 탭의 이름"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("cue.removeContainer", CommandIDs::removeContainer, ko ("리스트/카트 삭제"), ko ("현재 탭을 큐와 함께 삭제 (실행 취소 가능)"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("cue.toggleSequenceRecording", CommandIDs::toggleSequenceRecording, ko ("시퀀스 녹음 시작..."), ko ("녹음 중 시작되는 큐와 시각을 기록해 정지할 때 타임라인 그룹(시작 큐 + 프리웨이트)으로 만듦"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('E', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("cue.expandAllGroups", CommandIDs::expandAllGroups, ko ("모든 그룹 펼치기"), ko ("모든 그룹의 자식을 표시"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("transport.revertFade", CommandIDs::revertFade, ko ("페이드 되돌리기"), ko ("가장 최근 페이드의 대상을 페이드 전 레벨로 되돌림"),
                 Category::playback, Scope::playback, false, ko ("재생"), { key ('R', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("cue.fetchFadeLevels", CommandIDs::fetchFadeLevels, ko ("대상에서 레벨 가져오기"), ko ("선택한 페이드 큐의 목표 레벨을 대상 큐의 현재 레벨로"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('T', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("cue.remove", CommandIDs::removeCue, ko ("큐 삭제"), ko ("선택 큐 삭제"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key (KeyPress::deleteKey, ModifierKeys::noModifiers) }),
        command ("cue.duplicate", CommandIDs::duplicateCue, ko ("큐 복제"), ko ("선택 큐를 플러그인 체인까지 바로 아래에 복제"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('D', ModifierKeys::commandModifier) }),
        command ("cue.moveUp", CommandIDs::moveCueUp, ko ("위로 이동"), ko ("선택 큐를 한 칸 위로"),
                 Category::cue, Scope::mainWindow, true, ko ("큐"), { key (KeyPress::upKey, ModifierKeys::commandModifier) }),
        command ("cue.moveDown", CommandIDs::moveCueDown, ko ("아래로 이동"), ko ("선택 큐를 한 칸 아래로"),
                 Category::cue, Scope::mainWindow, true, ko ("큐"), { key (KeyPress::downKey, ModifierKeys::commandModifier) }),
        command ("edit.selectAll", CommandIDs::selectAll, ko ("모두 선택"), ko ("모든 큐 선택"),
                 Category::edit, Scope::mainWindow, false, ko ("편집"), { key ('A', ModifierKeys::commandModifier) }),
        command ("edit.copy", CommandIDs::copyCues, ko ("큐 복사"), ko ("선택 큐를 플러그인 체인까지 클립보드에 복사"),
                 Category::edit, Scope::mainWindow, false, ko ("편집"), { key ('C', ModifierKeys::commandModifier) }),
        command ("edit.cut", CommandIDs::cutCues, ko ("큐 잘라내기"), ko ("선택 큐를 복사한 뒤 삭제"),
                 Category::edit, Scope::mainWindow, false, ko ("편집"), { key ('X', ModifierKeys::commandModifier) }),
        command ("edit.paste", CommandIDs::pasteCues, ko ("큐 붙여넣기"), ko ("복사한 큐를 선택 큐 아래에 새 큐로 붙여넣기"),
                 Category::edit, Scope::mainWindow, false, ko ("편집"), { key ('V', ModifierKeys::commandModifier) }),
        command ("edit.pasteProperties", CommandIDs::pasteCueProperties, ko ("큐 속성 붙여넣기..."), ko ("복사한 큐의 속성(색·시간·트리거·트림·레벨·플러그인)만 선택 큐들에 적용"),
                 Category::edit, Scope::mainWindow, false, ko ("편집"), { key ('V', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("edit.find", CommandIDs::find, ko ("찾기..."), ko ("번호·이름·파일·메모로 큐 찾기"),
                 Category::edit, Scope::mainWindow, false, ko ("편집"), { key ('F', ModifierKeys::commandModifier) }),
        command ("edit.findNext", CommandIDs::findNext, ko ("다음 찾기"), ko ("같은 검색어로 다음 큐 찾기"),
                 Category::edit, Scope::mainWindow, true, ko ("편집"), { key (KeyPress::F3Key, ModifierKeys::noModifiers) }),
        command ("cue.saveTemplate", CommandIDs::saveCueTemplate, ko ("선택 큐를 새 큐 기본값으로"), ko ("이후 추가하는 큐가 이 큐의 설정(페이드·게인·색·트리거·플러그인 등)을 물려받음 (프로젝트에 저장)"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("cue.clearTemplate", CommandIDs::clearCueTemplate, ko ("새 큐 기본값 초기화"), ko ("새 큐를 다시 기본 설정으로 추가"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("cue.renumber", CommandIDs::renumber, ko ("선택 큐 재번호..."), ko ("선택한 큐에 순서대로 번호를 매김 (시작·증가·접두·접미)"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), { key ('R', ModifierKeys::commandModifier) }),
        command ("cue.deleteNumbers", CommandIDs::deleteNumbers, ko ("선택 큐 번호 삭제"), ko ("선택한 큐의 번호를 지움"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("cue.findMissingFiles", CommandIDs::findMissingFiles, ko ("없어진 파일 찾기..."), ko ("폴더를 골라 같은 이름의 파일로 다시 연결"),
                 Category::cue, Scope::mainWindow, false, ko ("큐"), {  }),
        command ("file.new", CommandIDs::newProject, ko ("새 프로젝트"), ko ("빈 큐 리스트로 시작"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("파일"), { key ('N', ModifierKeys::commandModifier) }),
        command ("file.open", CommandIDs::openProject, ko ("열기..."), ko ("프로젝트 열기 (.enqueue, .gocue)"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("파일"), { key ('O', ModifierKeys::commandModifier) }),
        command ("file.save", CommandIDs::saveProject, ko ("저장"), ko ("프로젝트 저장 (플러그인 상태 포함)"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("파일"), { key ('S', ModifierKeys::commandModifier) }),
        command ("file.saveAs", CommandIDs::saveProjectAs, ko ("다른 이름으로 저장..."), ko ("프로젝트를 새 파일로 저장"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("파일"), { key ('S', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("settings.project", CommandIDs::workspaceSettings, ko ("프로젝트 설정..."), ko ("GO 간격, 전체 페이드 정지 시간, 자동 번호, 백업, 레벨 한계, 오디션 방식 (프로젝트에 저장)"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("설정"), { key (',', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("edit.undo", CommandIDs::undo, ko ("실행 취소"), ko ("마지막 편집을 되돌립니다"),
                 Category::edit, Scope::mainWindow, false, ko ("편집"), { key ('Z', ModifierKeys::commandModifier) }),
        command ("edit.redo", CommandIDs::redo, ko ("다시 실행"), ko ("되돌린 편집을 다시 적용합니다"),
                 Category::edit, Scope::mainWindow, false, ko ("편집"), { key ('Y', ModifierKeys::commandModifier), key ('Z', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("view.toggleShowMode", CommandIDs::toggleShowMode, ko ("쇼 모드로 (편집 잠금)"), ko ("쇼 모드에서는 큐 추가·삭제·이동·속성 편집이 잠깁니다 (재생·저장은 그대로)"),
                 Category::view, Scope::mainWindow, false, ko ("편집"), { key ('M', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("view.toggleActiveCues", CommandIDs::toggleActiveCues, ko ("활성 큐 패널 펴기"), ko ("재생 중인 큐 목록 (일시정지·스크럽·페이드 정지). 구분선을 끌면 너비가 바뀝니다"),
                 Category::view, Scope::mainWindow, false, ko ("편집"), { key ('L', ModifierKeys::commandModifier) }),
        command ("view.toggleInspector", CommandIDs::toggleInspector, ko ("인스펙터 접기"), ko ("아래 인스펙터 패널 접기 / 펴기. 구분선을 끌면 높이가 바뀝니다"),
                 Category::view, Scope::mainWindow, false, ko ("편집"), { key ('I', ModifierKeys::commandModifier) }),
        command ("settings.audio", CommandIDs::audioSettings, ko ("오디오 출력 설정..."), ko ("출력 장치(ASIO / WASAPI) 선택"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("설정"), { key (',', ModifierKeys::commandModifier) }),
        command ("settings.audioPatches", CommandIDs::audioPatches, ko ("오디오 패치..."), ko ("큐 출력 → 장치 출력 라우팅, 출력 이름, 스테레오 묶기, 출력 인서트"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("설정"), { key ('P', ModifierKeys::commandModifier | ModifierKeys::shiftModifier) }),
        command ("settings.pluginManager", CommandIDs::pluginManager, ko ("VST3 플러그인 관리..."), ko ("VST3 플러그인 스캔 / 목록"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("설정"), { key ('P', ModifierKeys::commandModifier) }),
        command ("settings.masterInserts", CommandIDs::masterInserts, ko ("마스터 버스 인서트..."), ko ("모든 큐가 통과하는 마스터 VST3 체인"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("설정"), { key ('M', ModifierKeys::commandModifier) }),
        command ("app.checkForUpdates", CommandIDs::checkForUpdates, ko ("업데이트 확인..."), ko ("GitHub Releases에서 새 버전 확인"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("도움말"), {  }),
        command ("app.showManual", CommandIDs::showManual, ko ("사용 설명서..."), ko ("기능 설명과 단축키"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("도움말"), { key (juce::KeyPress::F1Key, ModifierKeys::commandModifier) }),
        command ("app.feedbackChat", CommandIDs::feedbackChat, ko ("커뮤니티"), ko ("카카오톡 오픈채팅 열기"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("도움말"), {  }),
        command ("app.about", CommandIDs::about, ko ("앤큐 정보"), ko ("버전 정보"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("도움말"), {  }),
        command ("file.youtubeDownload", CommandIDs::youtubeDownload, ko ("유튜브 다운로드..."), ko ("유튜브 링크의 소리를 mp3로 받아 큐에 넣기"),
                 Category::fileSettings, Scope::mainWindow, false, ko ("유튜브다운"), {  }),
        command ("view.uiScale100", CommandIDs::uiScale100, ko ("100% (기본)"),
                 ko ("창 전체(글씨·버튼·행·간격)가 같은 비율로 커집니다. 프로젝트가 아니라 이 PC에 저장"),
                 Category::view, Scope::mainWindow, false, ko ("설정"), {}),
        command ("view.uiScale110", CommandIDs::uiScale110, ko ("110%"),
                 ko ("창 전체(글씨·버튼·행·간격)가 같은 비율로 커집니다. 프로젝트가 아니라 이 PC에 저장"),
                 Category::view, Scope::mainWindow, false, ko ("설정"), {}),
        command ("view.uiScale125", CommandIDs::uiScale125, ko ("125%"),
                 ko ("창 전체(글씨·버튼·행·간격)가 같은 비율로 커집니다. 프로젝트가 아니라 이 PC에 저장"),
                 Category::view, Scope::mainWindow, false, ko ("설정"), {}),
        command ("view.uiScale150", CommandIDs::uiScale150, ko ("150%"),
                 ko ("창 전체(글씨·버튼·행·간격)가 같은 비율로 커집니다. 프로젝트가 아니라 이 PC에 저장"),
                 Category::view, Scope::mainWindow, false, ko ("설정"), {}),
        command ("app.quit", juce::StandardApplicationCommandIDs::quit, ko ("종료"), ko ("앱 종료"),
                 Category::fileSettings, Scope::mainWindow, false, "Application", { key ('Q', ModifierKeys::commandModifier) })
    };
}

std::vector<ShortcutDefinition> makeFixedComponents()
{
    std::vector<ShortcutDefinition> result;
    const auto add = [&result] (const char* id, const char* name, Scope scope, ShortcutKeys keys, bool repeat = false)
    {
        result.push_back ({ id, ko (name), ko (name), Category::edit, scope, repeat, 0, std::move (keys), {}, 0 });
    };
    const int shift = ModifierKeys::shiftModifier, ctrl = ModifierKeys::ctrlModifier, alt = ModifierKeys::altModifier;
    add ("cueTable.editNumber", "번호 편집", Scope::cueTable, { key ('N') });
    add ("cueTable.editName", "이름 편집", Scope::cueTable, { key ('Q') });
    add ("cueTable.editPreWait", "프리웨이트 편집", Scope::cueTable, { key ('E') });
    add ("cueTable.editPostWait", "포스트웨이트 편집", Scope::cueTable, { key ('W') });
    add ("cueTable.cycleContinue", "진행 모드 변경", Scope::cueTable, { key ('C') });
    add ("cueTable.editNotes", "노트 편집", Scope::cueTable, { key ('O') });
    add ("cueTable.editDuration", "길이 편집", Scope::cueTable, { key ('D') });
    add ("cueTable.collapseGroup", "그룹 접기 / 부모 선택", Scope::cueTable, { key (KeyPress::leftKey) });
    add ("cueTable.expandGroup", "그룹 펼치기", Scope::cueTable, { key (KeyPress::rightKey) });
    add ("cueTable.delete", "선택 큐 삭제 (표의 고정 키)", Scope::cueTable,
         { key (KeyPress::deleteKey), key (KeyPress::backspaceKey), key (KeyPress::deleteKey, shift), key (KeyPress::backspaceKey, shift) });
    ShortcutKeys rows;
    for (const int code : { KeyPress::upKey, KeyPress::downKey, KeyPress::pageUpKey, KeyPress::pageDownKey, KeyPress::homeKey, KeyPress::endKey })
        for (const int modifiers : { 0, shift })
            rows.add (key (code, modifiers));
    add ("cueTable.selectRows", "행 이동 / 범위 선택", Scope::cueTable, rows, true);
    add ("cueTable.return", "선택 행 Enter (표 기본 처리)", Scope::cueTable, { key (KeyPress::returnKey), key (KeyPress::returnKey, shift) });

    // These modifier variants reflect existing component predicates, not new bindings.
    add ("waveform.trimStart", "커서로 트림 시작 설정", Scope::waveform, { key ('I', shift), key ('I', shift | alt) });
    add ("waveform.trimEnd", "커서로 트림 끝 설정", Scope::waveform, { key ('O', shift), key ('O', shift | alt) });
    add ("waveform.addSlice", "커서에 마커 추가", Scope::waveform, { key ('M') });
    ShortcutKeys sliceDelete;
    for (const int modifiers : { 0, shift, ctrl, alt, shift | ctrl, shift | alt, ctrl | alt, shift | ctrl | alt })
        for (const int code : { KeyPress::deleteKey, KeyPress::backspaceKey })
            sliceDelete.add (key (code, modifiers));
    add ("waveform.deleteSelection", "선택 마커 / 점 삭제", Scope::waveform, sliceDelete);
    add ("waveform.selectPoint", "이전 / 다음 점 선택", Scope::waveform, { key (KeyPress::leftKey), key (KeyPress::rightKey) }, true);
    ShortcutKeys movePoints, finePoints;
    for (const int code : { KeyPress::leftKey, KeyPress::rightKey, KeyPress::upKey, KeyPress::downKey })
        for (const int modifiers : { alt, alt | ctrl })
        {
            movePoints.add (key (code, modifiers));
            finePoints.add (key (code, modifiers | shift));
        }
    add ("waveform.movePoint", "선택 점 이동", Scope::waveform, movePoints, true);
    add ("waveform.movePointFine", "선택 점 미세 이동", Scope::waveform, finePoints, true);
    add ("waveform.zoomIn", "파형 확대", Scope::waveform, { key ('=', ctrl), key ('+', ctrl) }, true);
    add ("waveform.zoomOut", "파형 축소", Scope::waveform, { key ('-', ctrl) }, true);
    add ("levelMatrix.moveCell", "셀 이동", Scope::levelMatrix,
         { key (KeyPress::leftKey), key (KeyPress::rightKey), key (KeyPress::upKey), key (KeyPress::downKey) }, true);
    add ("levelMatrix.mute", "선택 셀 무음", Scope::levelMatrix, { key (KeyPress::deleteKey), key (KeyPress::backspaceKey) });
    add ("levelMatrix.edit", "셀 편집 시작", Scope::levelMatrix, { key (KeyPress::returnKey), key (KeyPress::F2Key) });
    add ("curveEditor.deletePoint", "선택 점 삭제", Scope::curveEditor, { key (KeyPress::deleteKey), key (KeyPress::backspaceKey) });
    ShortcutKeys children, preWait, finePreWait;
    for (const int modifiers : { 0, shift, ctrl, alt, shift | ctrl, shift | alt, ctrl | alt, shift | ctrl | alt })
        for (const int code : { KeyPress::upKey, KeyPress::downKey })
            children.add (key (code, modifiers));
    for (const int code : { KeyPress::leftKey, KeyPress::rightKey })
        for (const int modifiers : { alt, alt | ctrl })
        {
            preWait.add (key (code, modifiers));
            finePreWait.add (key (code, modifiers | shift));
        }
    add ("groupTimeline.selectChild", "이전 / 다음 자식 선택", Scope::groupTimeline, children, true);
    add ("groupTimeline.movePreWait", "프리웨이트 이동", Scope::groupTimeline, preWait, true);
    add ("groupTimeline.movePreWaitFine", "프리웨이트 미세 이동", Scope::groupTimeline, finePreWait, true);
    return result;
}
} // namespace

ShortcutCatalog::ShortcutCatalog() : commands (makeCommands()), fixedComponents (makeFixedComponents()) {}

ShortcutCatalog::ShortcutCatalog (std::vector<ShortcutDefinition> c, std::vector<ShortcutDefinition> f)
    : commands (std::move (c)), fixedComponents (std::move (f)) {}

const ShortcutCatalog& ShortcutCatalog::get()
{
    static const ShortcutCatalog catalog;
    return catalog;
}

const ShortcutDefinition* ShortcutCatalog::find (const juce::String& id) const
{
    for (const auto* entries : { &commands, &fixedComponents })
        for (const auto& entry : *entries)
            if (entry.id == id)
                return &entry;
    return nullptr;
}

const ShortcutDefinition* ShortcutCatalog::find (juce::CommandID commandID) const
{
    for (const auto& entry : commands)
        if (entry.commandID == commandID)
            return &entry;
    return nullptr;
}

bool ShortcutCatalog::getCommandInfo (juce::CommandID commandID, juce::ApplicationCommandInfo& result) const
{
    if (const auto* entry = find (commandID))
    {
        // Quit remains JUCE's existing menu text; the settings catalog has Korean labels.
        if (commandID == juce::StandardApplicationCommandIDs::quit)
            result.setInfo (TRANS ("Quit"), TRANS ("Quits the application"), "Application", entry->commandFlags);
        else
            result.setInfo (entry->name, entry->description, entry->menuCategory, entry->commandFlags);
        result.defaultKeypresses = entry->defaultKeys;
        return true;
    }
    return false;
}

juce::String ShortcutCatalog::categoryLabel (ShortcutCategory category)
{
    switch (category)
    {
        case Category::playback:     return ko ("재생");
        case Category::cue:          return ko ("큐");
        case Category::edit:         return ko ("편집");
        case Category::view:         return ko ("화면");
        case Category::fileSettings: return ko ("파일·설정");
    }
    return {};
}

juce::String ShortcutCatalog::scopeLabel (ShortcutScope scope)
{
    switch (scope)
    {
        case Scope::mainWindow:    return ko ("메인 작업 화면");
        case Scope::playback:      return ko ("앱 내 재생");
        case Scope::application:   return ko ("앱 전체 (패닉)");
        case Scope::cueTable:      return ko ("큐 표");
        case Scope::waveform:      return ko ("파형");
        case Scope::levelMatrix:   return ko ("레벨 매트릭스");
        case Scope::curveEditor:   return ko ("곡선 편집기");
        case Scope::groupTimeline: return ko ("그룹 타임라인");
    }
    return {};
}

} // namespace gocue
