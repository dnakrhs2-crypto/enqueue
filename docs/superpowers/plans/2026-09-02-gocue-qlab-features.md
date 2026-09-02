# GoCue 큐랩 오디오 기능 카피 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 큐랩 5 오디오 기능 후보 63개(원격 제어 제외) + 드래그앤드롭 전면을 GoCue에 단계별로 구현·릴리스하고 코덱스 리뷰를 통과시킨다.

**Architecture:** 평탄 `Cue` 구조체를 종류(type)·하위 데이터로 확장하고 트리(그룹)·다중 컨테이너(리스트/카트)로 키운다. 엔진은 큐당 `RegionLoopSource → 버퍼링 → 속도 → 정지 페이드 → VST3 → 레벨 매트릭스 → 패치 버스` 경로로 재구성하고, 메시지 스레드의 `CueController + Scheduler + FadeRunner`가 큐랩식 진행(웨이트·자동 진행·페이드 큐·그룹·devamp)을 담당한다. 실행 취소는 `Project` 스냅샷 스택.

**Tech Stack:** JUCE 8.0.15(CMake FetchContent, 로컬 체크아웃 `C:\Users\claude\JUCE`), MSVC 2022, C++17, Media Foundation(AAC), signalsmith-stretch(MIT, 피치 유지), Inno Setup + WinSparkle(기존 릴리스 파이프라인 `tools/release.py`).

**Spec:** `docs/superpowers/specs/2026-09-02-gocue-v2-design.md` (후보 목록 `docs/superpowers/specs/2026-09-02-qlab-audio-feature-candidates.md`)

## Global Constraints
- 빌드: `cd C:\Users\claude\gocue && cmake --preset local && cmake --build --preset local-release -- -m -v:m -nologo` (PATH에 `C:\Users\claude\tools\cmake\bin` 추가). Git Bash에서 `/m` 인자 금지 → `-m -v:m`.
- 테스트: `build\vs2022\tests\GoCueTests_artefacts\Release\GoCueTests.exe` (전부 통과해야 커밋). 테스트는 `tests/*.cpp`, JUCE `UnitTest` 카테고리 `"GoCue"`.
- 한글 문자열은 `ko("...")`(`String::fromUTF8`), `/utf-8` 컴파일. JUCE 8: `var::isObject()`는 배열도 true → `getDynamicObject()`로 판별. `createWriterFor`는 `AudioFormatWriterOptions` 버전.
- 프로젝트 파일 하위 호환: v1 `.gocue`는 반드시 읽혀야 한다(테스트 `ProjectSerializerTests`에 v1 고정 JSON 유지).
- 오디오 스레드: 락은 `AudioEngine::lock`만, 파일 IO·할당 금지. 모든 정지는 5 ms 디클릭.
- 정적 CRT, 설치 파일은 `python tools\release.py --repo dnakrhs2-crypto/gocue --key C:\Users\claude\SDKs\gocue_release\eddsa_priv.pem --publish` (버전은 `CMakeLists.txt` `project(GoCue VERSION x.y.z)`).
- 각 단계 끝: README 갱신, 버전 ↑, 릴리스, 코덱스 read-only 리뷰(`codex exec -m gpt-5.6-sol -c model_reasoning_effort=max -s read-only ...`, python subprocess + CREATE_NO_WINDOW), 지적 반영 후 재테스트.
- 커밋 메시지 끝: `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.

---

## 단계 1 — v0.2.0: 실행 취소 · 파형/트리밍/엔벨로프/루프 · 드래그 전면 · 2차 트리거/더블 GO · 패닉/일시정지/리셋 · 자동 백업

### Task 1.1: 실행 취소/다시 실행 (56)
**Files:** Create `src/app/ProjectHistory.h/.cpp`; Modify `src/app/ProjectDocument.h/.cpp`, `src/app/Commands.h`, `src/ui/MainComponent.cpp`(편집 메뉴, 모든 편집 경로를 `document.perform` 경유); Test `tests/ProjectHistoryTests.cpp`.
**Interfaces:**
```cpp
class ProjectHistory { public:
    void push (const Project& before, const juce::String& name, const juce::String& coalesceKey); // 같은 key가 700ms 안이면 합침
    bool canUndo() const; bool canRedo() const; juce::String getUndoName() const; juce::String getRedoName() const;
    std::optional<Project> undo (const Project& current); std::optional<Project> redo (const Project& current);
    void clear(); static constexpr int maxDepth = 200; };
// ProjectDocument
void perform (const juce::String& name, const std::function<void (Project&)>& edit, const juce::String& coalesceKey = {});
bool undo(); bool redo();  ProjectHistory history;
```
- [x] 테스트: push→undo→redo 왕복, 200개 상한, coalesce(같은 key 700 ms 내 두 번 push는 하나), undo 후 새 편집이 redo 스택을 비움.
- [x] 구현 + `ProjectDocument::perform`은 `toProject()` 스냅샷 → edit → `adopt`가 아닌 in-place 반영(`cues.replaceAll` + master) → dirty.
- [x] MainComponent: 이름 편집·페이드·게인·추가·삭제·복제·이동·드롭·플러그인 슬롯 추가/삭제/이동/바이패스 전부 `perform`으로. 편집 메뉴 "실행 취소 (Ctrl+Z)/다시 실행 (Ctrl+Y)"에 동작 이름 표시.
- [x] 빌드·테스트·커밋 `feat: undo/redo via project snapshots`. (2026-09-02, 469 tests)

### Task 1.2: 모델 확장 1차 + 파일 형식 v2 (트림·루프·엔벨로프·정지 페이드)
**Files:** Modify `src/model/Cue.h/.cpp`, `src/model/ProjectSerializer.h/.cpp`; Create `src/model/Envelope.h`; Test `tests/ProjectSerializerTests.cpp`, `tests/EnvelopeTests.cpp`.
**Interfaces:**
```cpp
struct EnvelopePoint { double seconds; double level; };   // level 0..1, seconds는 트림 시작 기준
struct Envelope { bool enabled = false; bool linear = false; bool lockToTrim = true; std::vector<EnvelopePoint> points;
    float levelAt (double seconds, double regionLength) const;   // lockToTrim이면 points의 시간축을 regionLength에 맞춰 늘림
    static Envelope fromFades (double fadeInSec, double fadeOutSec, double regionLength); };
struct AudioCueData { double startSeconds = 0; double endSeconds = -1; int playCount = 1; bool infiniteLoop = false;
    double rate = 1.0; bool preservePitch = false; Envelope envelope; };
// Cue: AudioCueData audio; (fadeInMs 삭제 → 로더가 envelope로 변환, fadeOutMs·gainDb 유지)
double Cue::regionLength() const;      // end(-1이면 duration) - start, 최소 0
double Cue::effectiveLength() const;   // regionLength / rate * playCount (무한이면 -1)
```
- [x] 테스트: v1 JSON(fadeInMs 250) 로드 → 페이드인 엔벨로프 2점(0,0)(0.25,1) (fadeOutMs는 정지 페이드로 유지 — v1에서도 파일 끝 페이드가 아니라 F키 페이드였음), v2 왕복, `levelAt` 선형·부드러운 보간·lockToTrim 늘림.
- [x] 구현: `currentVersion = 2`, 저장 시 `audio{}` 객체, 로드 시 v1 필드 변환.
- [x] 빌드·테스트·커밋 (1.1과 함께 커밋).

### Task 1.3: 엔진 — RegionLoopSource + 새 플레이어 경로 (1~4 기반)
**Files:** Create `src/audio/RegionLoopSource.h/.cpp`; Modify `src/audio/CuePlayer.h/.cpp`, `src/audio/AudioEngine.h/.cpp`; Test `tests/RegionLoopSourceTests.cpp`, `tests/AudioEngineTests.cpp`.
**Interfaces:**
```cpp
class RegionLoopSource : public juce::PositionableAudioSource {   // 파일 SR에서 동작
public:  RegionLoopSource (std::unique_ptr<juce::AudioFormatReader> reader);
    void setRegion (juce::int64 startSample, juce::int64 endSample); void setPlayCount (int count, bool infinite);
    void setEnvelope (Envelope env, double regionSeconds);   // 메시지 스레드, 재생 전
    void requestDevamp (bool wholeCue);                       // 현재 회차 끝에서 루프 탈출
    int getPassIndex() const; bool hasReachedEnd() const;
    // PositionableAudioSource: getNextAudioBlock는 회차 경계에서 seek, 엔벨로프는 샘플 단위 램프 적용
};
// CuePlayer 내부: RegionLoopSource → BufferingAudioSource(readAheadThread, readAheadSamples) → ResamplingAudioSource(ratio) → FadeEnvelope(정지) → gain → chain
struct PlayOptions { double startSeconds = 0; bool isPreview = false; };
bool AudioEngine::play (const Cue&, PlayOptions = {}, juce::String* error = nullptr);
```
- [x] 테스트(오프라인 렌더, `tests/RegionPlaybackTests.cpp`): 트림, playCount, infinite + finishCurrentPass(devamp), 엔벨로프 궤적, rate(라이브 변경 포함), pause/resume, 라이브 트림(끝이 앞으로 오면 즉시 종료), 시작 오프셋, 빈 구간 거부, 읽기선행 경로.
- [x] 구현: `RegionLoopSource`(가상 타임라인 pass*L+offset, 회차 경계 블록 안 처리) → BufferingAudioSource → ResamplingAudioSource(비율 블록 시작에 반영) → 정지 FadeEnvelope + pauseGate → 게인 → 체인. 엔진 API: PlayOptions.startSeconds, pause/resume(All), finishCurrentPass, setLiveRegion/Rate, fadeOutAndStop(ms), PlayingCue.filePosition/passIndex/paused.
- [x] 빌드·테스트·커밋 `feat: region/loop/envelope playback path`. (530 tests)

### Task 1.4: 파형 뷰 + 시간·루프 인스펙터 + 미리듣기/리셋 + 우클릭 (1·2·3·4·7·8)
**Files:** Create `src/ui/WaveformView.h/.cpp`, `src/ui/TimeLoopsPanel.h/.cpp`; Modify `src/ui/CueInspector.*`, `src/ui/MainComponent.*`, `src/app/Commands.h`.
**Interfaces:**
```cpp
class WaveformView : public juce::Component, public juce::ChangeListener {
public: WaveformView (juce::AudioFormatManager&, juce::AudioThumbnailCache&);
    void setCue (const Cue* cue);                    // 파일·트림·엔벨로프 표시
    void setPlaybackPosition (double seconds, bool playing);
    std::function<void (double start, double end, const char* coalesceKey)> onTrimChanged;
    std::function<void (Envelope)> onEnvelopeChanged;
    std::function<void()> onPreview, onReset;
    void zoomIn(); void zoomOut(); void zoomToFit(); void setViewChannel (int ch /* -1=합 */);
    bool keyPressed (const juce::KeyPress&) override;  // Shift+I/O, ←→ 점 선택, Alt+화살표 이동, Shift+Alt 미세, Delete 점 삭제
};
```
- [x] 파형: `juce::AudioThumbnail`(cache 64) 채널 합/개별, 회색 트림 핸들 드래그(재생 중이면 `onTrimChanged` 즉시 → 엔진 `setLiveRegion` 반영), 노란 엔벨로프(사용 체크 시) 클릭=점 추가, 드래그=이동, 곡선/직선 토글, "시작/끝에 잠금" 체크, Alt+휠 줌, 재생 위치선.
- [x] TimeLoopsPanel: 시작/끝 시간 텍스트(`m:ss.mmm`), 재생 횟수/무한, 속도(0.03~33)·피치 유지(6번 자리표시, 피치 유지는 5단계에서 활성), 엔벨로프 체크·곡선/직선·잠금, 미리듣기(V)·리셋 버튼, 줌 버튼.
- [x] 우클릭: 외부 편집기로 열기(`File::startAsProcess`), 탐색기에서 보기(`revealToUser`), 표시 채널.
- [x] 명령 `preview`(V: 플레이헤드 유지·프리웨이트 무시), `resetCue`. 인스펙터가 탭(`juce::TabbedComponent`: 기본 / 시간·루프 / 트리거 / 이펙트)으로 바뀜.
- [x] GUI 스모크(`tools/gocue_uitest.ps1` — drag/hotkey 액션 추가, ★키 자동화는 스캔코드 포함 `hotkey`만 Ctrl+글자 조합에 신뢰 가능): 트림 드래그·엔벨로프 점 추가·미리듣기 플레이헤드·Ctrl+Z 확인.
- [x] 커밋 (1.5~1.7과 함께).

### Task 1.5: 드래그앤드롭 전면
**Files:** Modify `src/ui/MainComponent.*`(FileDragAndDropTarget), `src/ui/CueInspector.*`(파일칸 드롭=교체), `src/ui/CueTable.*`(기존 유지); Test `tests/CueListTests.cpp`(드롭 삽입 인덱스 계산은 순수 함수로 분리해 테스트).
- [x] `MainComponent::isInterestedInFileDrag`: 오디오 확장자 또는 `.gocue`. 드롭: `.gocue` 1개면 열기(확인 후), 오디오면 맨 뒤 추가(목록 위는 CueTable이 먼저 받음). 인스펙터 파일 라벨 위 드롭 = 선택 큐 파일 교체(`perform("파일 교체")`).
- [x] 드롭 중 테두리 강조(전체 창 파란 테두리). 폴더 드롭 시 안의 오디오 파일을 이름순으로 추가(`collectAudioFiles`).
- [x] 커밋 (외부 OLE 드롭은 자동화 불가 — 코드 경로만 확인, gom QA에서 실물 확인 필요).

### Task 1.6: 워크스페이스 설정(일반) + 더블 GO/키업 + 2차 트리거 (33·34)
**Files:** Create `src/model/WorkspaceSettings.h`, `src/ui/WorkspaceSettingsDialog.h/.cpp`, `src/app/CueController.h/.cpp`; Modify serializer(settings 저장), `Cue`(secondTrigger, secondTriggerOnRelease), 인스펙터(트리거 탭), MainComponent(GO 경로를 컨트롤러로), TransportBar(더블 GO 빨간 테두리·깜빡임).
**Interfaces:**
```cpp
class CueController : private juce::Timer { public:
    CueController (AudioEngine&, ProjectDocument&);
    bool go (juce::int64 nowMs = juce::Time::getMillisecondCounter());   // 더블 GO 가드: false면 거부(깜빡임)
    void goKeyReleased();               // requireKeyUp
    void trigger (const juce::Uuid& id, bool fromRelease);  // 2차 트리거 판정: 실행 중이면 secondTrigger 동작
    void panicSelected(); void panicAll(); void hardStopSelected(); void hardStopAll();
    void pauseResumeSelected(); void pauseAll(); void resumeAll(); void resetAll();
    std::function<void()> onGoRejected; };
```
- [x] 테스트 `tests/CueControllerTests.cpp`(가짜 시계 주입): doubleGo 거부/허용, requireKeyUp, P 일시정지 + Space 재개, Esc 페이드/더블 Esc 즉시, 2차 트리거 nothing/hardStopRestart/devamp/hardStop, 전체 리셋.
- [x] 설정 창(일반 탭): GO 최소 간격(초), 키업 필요, 전체 페이드 정지 시간(초, 기본 2), 자동 번호(2단계에서 사용), 플레이헤드 잠금, 열 때/닫을 때 큐. 파일 탭: 프로젝트 폴더로 복사, 자동 백업·간격·저장 전 백업·회전. (설정은 실행 취소 대상 아님)
- [x] 커밋. **★gom 결정(9/2)으로 키 체계 변경**: Space=GO(일시정지된 큐 있으면 재개), P=일시정지/재개, F=선택 큐 정지 페이드, Esc=전체 페이드 정지(기본 2초, 0.5초 안 두 번=즉시), S·Shift+F·"전체 정지" 버튼 제거. 2차 트리거 기본값=즉시 정지 후 재시작(기존 동작 유지).

### Task 1.7: 패닉/S/F 정리 + 일시정지/재개 + 리셋 (39·40·41·44)
**Files:** Modify `src/audio/CuePlayer.*`(pause/resume 램프, `panic(seconds)`), `src/audio/AudioEngine.*`, `CueController`, `Commands.h`, MainComponent(메뉴 재생), TransportBar(버튼: GO · 일시정지 · 패닉 · 전체 패닉), CueTable(일시정지 노란 표시); Test `tests/AudioEngineTests.cpp`.
- [x] 엔진 테스트: pause → 출력 무음 + 위치 정지, resume → 이어서 재생, 5 ms 램프(RegionPlaybackTests), panic/더블 Esc(CueControllerTests). ★게인 실시간 반영(gom 지적 "게인값 안 먹는 버그" = 재생 중 인스턴스에 반영 안 되던 것) → `setLiveGainDb` 램프 + 테스트.
- [x] 키: 위 1.6 항목의 gom 체계. 전체 즉시 정지·전체 리셋은 재생 메뉴에만(키 없음). 표에서 일시정지 행은 노란색.
- [x] 커밋.

### Task 1.8: 자동 백업 + 프로젝트 폴더 복사 (62)
**Files:** Create `src/app/BackupManager.h/.cpp`; Modify `ProjectDocument`(save 훅), MainComponent(추가 시 복사); Test `tests/BackupManagerTests.cpp`(임시 폴더).
```cpp
class BackupManager { public: static juce::File backupDirFor (const juce::File& project);   // "<name>.gocue.backups"
    static juce::Result backupNow (const juce::File& project, const juce::String& tag /* "save"|"auto" */);
    static void rotate (const juce::File& dir, juce::Time now);  // 최근 1시간 20개, 24시간 시간별, 그 뒤 일별
    static juce::File copyIntoProject (const juce::File& audio, const juce::File& projectDir);  // <dir>/audio/<name>(중복 시 이름 뒤 번호) };
```
- [ ] 테스트: backupNow 파일명 형식 `show (Backup 2026-09-02_143000).gocue`, 저장 전 1분 1회 제한, rotate 규칙, copyIntoProject 중복 처리.
- [ ] 주기 백업 타이머(설정 간격, dirty이고 파일 있을 때만, 파일 미저장 프로젝트는 백업 없음 — 큐랩과 동일).
- [ ] 커밋 `feat: automatic project backups and copy-into-project`.

### Task 1.9: 단계 1 마감
- [ ] README(단축키·기능) 갱신, `CMakeLists.txt` 0.2.0, 전체 테스트, GUI 스모크(열기·GO·트림·엔벨로프·드롭·일시정지·패닉·실행 취소), 릴리스 `python tools\release.py ... --publish`, 설치 업데이트 확인.
- [ ] 코덱스 read-only 리뷰(범위: 단계 1 diff) → 지적 반영 → 재테스트 → 커밋 `Apply Codex review findings (phase 1)`.
- [ ] 메모리 `project_gocue.md` 현황 갱신.

---

## 단계 2 — v0.3.0: 큐 속성 · 웨이트/진행 모드/시퀀스 · 스케줄러 · 목록 UI 개편 · 여러 리스트

### Task 2.1: 큐 공통 속성 모델 (26·29·30·31·48·59)
**Files:** `src/model/Cue.*`, serializer, `tests/ProjectSerializerTests.cpp`.
- [ ] 필드: number(String), notes, color, secondColor, useSecondColor, flagged, armed, skipIfDisarmed, autoLoad, preWaitSeconds, postWaitSeconds, continueMode, hotkey, wallClock, fadeStopOthers, duck. 템플릿 `settings.templates[type]`.
- [ ] `CueNumbering`: `nextNumber(list, increment)`(마지막 숫자 번호 + 증가), `renumber(ids, start, increment, prefix, suffix)`, 유일성 검사.
- [ ] 테스트: 왕복, 재번호 "1, 2, 3" / 접두 "A" / 소수 증가 0.5, 중복 거부.
- [ ] 커밋.

### Task 2.2: CueList 다중 선택 + 플레이헤드 분리 (46·58)
**Files:** `src/model/CueList.*`, `tests/CueListTests.cpp`.
- [ ] API: `selection()`(vector<Uuid>), `setSelection`, `addToSelection`, `primarySelected()`, `playhead()`, `setPlayhead`, `movePlayheadNext/Prev`, `movePlayheadToNextSequence`(다음 "수동 진행" 큐), `lockPlayheadToSelection` 반영(선택 이동 시 플레이헤드 동행).
- [ ] 테스트: 잠금 on/off 동작, 시퀀스 단위 이동(자동 계속 묶음 건너뜀), 삭제 시 선택/플레이헤드 보정.
- [ ] 커밋.

### Task 2.3: 스케줄러 + 진행 모드 + 시퀀스 (27·28)
**Files:** Create `src/app/Scheduler.h/.cpp`; Modify `CueController`; Test `tests/SchedulerTests.cpp`, `tests/CueControllerTests.cpp`.
```cpp
class Scheduler { public: using Clock = std::function<double()>;   // 초
    explicit Scheduler (Clock clock); int schedule (double atSeconds, std::function<void()> fn); void cancel (int id); void cancelAll();
    void tick(); /* 1 ms 타이머에서 호출 */ int pendingCount() const; };
```
- [ ] 컨트롤러: GO → 플레이헤드 큐 시작 → 자동 계속(postWait 뒤 다음 큐; 0이면 즉시 같은 호출) / 자동 팔로우(끝나면 다음) / 수동. 프리웨이트는 시작을 스케줄. 시퀀스 전체 시작 후 플레이헤드는 시퀀스 다음 큐로. 미리듣기는 웨이트·진행 무시.
- [ ] 테스트(가짜 시계): 프리웨이트 1 s 뒤 재생, 자동 계속 postWait 0.5 s 뒤 다음 큐, 자동 팔로우 큐 길이 뒤 다음, 시퀀스 후 플레이헤드 위치, 비활성 큐 건너뛰기(skipIfDisarmed) vs 정지(armed=false: 재생 안 하고 진행).
- [ ] 커밋.

### Task 2.4: 페이드&정지 타인 · 덕/부스트 · 핫키 · 벽시계 (35·36·37·38)
- [ ] 엔진 `setDuckDb(id, db, seconds)` 램프 + 컨트롤러가 시작 시 같은 리스트의 다른 실행 큐에 적용, 끝나면 복원. fadeStopOthers 범위(peers/list/all) 적용.
- [ ] 핫키: `KeyListener`가 큐 핫키 맵 조회(입력창 편집 중 제외), 키 뗄 때 2차 트리거. 벽시계: 스케줄러가 매 초 검사.
- [ ] 테스트: 덕 -6 dB 적용/복원 렌더, fadeStopOthers가 대상만 페이드, 벽시계 요일 마스크.
- [ ] 커밋.

### Task 2.5: 로드·시간으로 로드 · 리셋 확장 · 활성 큐 패널 (42·43)
- [ ] 엔진 `load(cue, seconds)`(준비된 플레이어, 상태 loaded, GO 시 즉시 시작), 컨트롤러 `loadSelected`, `loadToTime` 다이얼로그(초, 음수 허용, 스크럽 슬라이더).
- [ ] `ActiveCuesPanel`(우측 사이드바, Ctrl+L): 행마다 일시정지/재개 버튼·번호·이름·경과/남은·진행바(초록/노랑)·패닉 버튼·드래그 스크럽(`engine.seek(id, seconds)`), 정렬 방향 설정.
- [ ] 커밋.

### Task 2.6: 목록 UI 개편 — CueListView (32·50·63·45·49)
**Files:** Create `src/ui/CueListView.h/.cpp`(CueTable 대체), `src/ui/StatusIcons.h`, `src/ui/WarningsWindow.*`, `src/ui/FileRelinkDialog.*`; Modify MainComponent.
- [ ] 열: 상태 아이콘(스탠바이 삼각·재생 초록·일시정지 노란 바·로드 노란 원·테일 노란 경사·깨짐 빨간 X·경고 노란 삼각·깃발·오디션 괄호), 번호, 이름, 대상, 프리웨이트, 길이, 포스트웨이트, 진행(→ / 깃발 화살표). 행 색(큐 색·두 번째 색), 비활성 빗금, 행 크기 소/중/대.
- [ ] 빠른 편집: N Q O T E D W C, 더블클릭 셀 편집, C는 순환. 다중 선택(Shift/Ctrl 클릭), 내부 드래그로 이동(다중), 파일 드롭 위치 삽입.
- [ ] 쇼 모드: 편집 잠금(인스펙터 비활성·추가/삭제/이동/붙여넣기 차단, Esc·저장·종료 허용), 푸터 토글(Ctrl+Shift+[ / ]).
- [ ] 경고 창(Ctrl+B): 깨진 큐 목록(파일 없음·대상 없음·플러그인 없음 등) + "파일 다시 찾기": 폴더 지정 → 같은 이름 파일 재연결.
- [ ] 커밋.

### Task 2.7: 여러 리스트 · 찾기 · 속성 붙여넣기 · 템플릿 UI (55·57·59·60)
- [ ] `CueContainer` 모델 + 서식 + `ContainerTabs`(리스트 추가/삭제/이름 변경, 리스트별 플레이헤드). 큐 번호 유일성은 워크스페이스 전체.
- [ ] 찾기(Ctrl+F): 번호·이름·메모·파일 경로 검색, 다음/이전. 대상으로 점프(Ctrl+Shift+J), 관련 큐 강조(도구 메뉴).
- [ ] 큐 속성 붙여넣기(Ctrl+Shift+V): 카테고리 체크(기본 속성·트리거·시간/루프·레벨·트림·이펙트·페이드 커브) + 프리셋(p=최근), "오디오 레벨 붙여넣기"·"페이드 모양 붙여넣기" 단축 항목.
- [ ] 큐 템플릿 탭(설정 창): 종류별 기본값 편집 = 인스펙터 재사용.
- [ ] 커밋.

### Task 2.8: 단계 2 마감 — README, 0.3.0 릴리스, 코덱스 리뷰, 메모리.

---

## 단계 3 — v0.4.0: 레벨 매트릭스 · 패치 · 멀티채널/포맷 · 오디션

### Task 3.1: LevelMatrix/TrimLevels 모델 + 엔진 매트릭스 (9·11·12·15)
- [ ] 모델(설계 1.4) + 직렬화("-inf") + 테스트(gainFor 합산, 무음 전파, resize 기본 대각선, 모노 파일 양쪽).
- [ ] 엔진: 플레이어 출력이 K채널 큐 출력 버스로, 10 ms 램프 레벨 스무딩, `setLiveLevels`. 레벨 한계(max/min)는 UI·페이드에서 클램프.
- [ ] 테스트: 2ch 파일 → 크로스포인트 대각선 -6 dB → 출력 RMS, 입력 -inf → 무음, 트림 +3 dB 후단 적용.
- [ ] 커밋.

### Task 3.2: 패치 모델 + 패치 버스 렌더 + 장치 M채널 (13·14)
- [ ] `AudioPatch` + 직렬화 + 기본 패치 이관. 엔진 `renderBlock`이 장치 출력 M채널을 쓰고, 패치별로 큐 출력 인서트 → 라우팅 → 장치 출력 인서트, 마스터 체인은 1-2에 유지.
- [ ] 테스트: 큐 출력 2 → 라우팅으로 장치 3-4에 보냄, 패치 메인 -6 dB, 큐 출력 인서트 스텁 플러그인(TestGainPlugin) 적용.
- [ ] 커밋.

### Task 3.3: LevelMatrixComponent + 레벨/트림 탭 + 겡 (9·10·11·12)
- [ ] 그리드: 좌상 메인, 행 입력 레벨, 열 출력 레벨(장치에 연결된 출력=노란 핸들, 아니면 회색), 크로스포인트, 드래그(0 dB 상한)·타이핑(부호 없으면 음수, 빈칸 -inf), 겡 색 배경, 도그이어 표시, "기본 레벨로"·"전부 무음"·"입력 이름"·"겡 지정" 버튼. 재생 중 즉시 반영.
- [ ] 트림 탭: 메인·출력별 오프셋.
- [ ] 커밋.

### Task 3.4: 패치 편집기 다이얼로그 (13·14)
- [ ] 이름·큐 출력 개수(1~128)·장치 표시, 탭: 큐 출력(이름·인서트 추가·스테레오 묶기) / 패치 라우팅(매트릭스, 메인) / 장치 출력(인서트). "다른 패치에서 복사/기본값". 설정 창 오디오 탭: 패치 목록(추가·삭제·복제·순서), 레벨 한계.
- [ ] 큐 I/O: 인스펙터 기본 탭에 출력 패치 선택.
- [ ] 커밋.

### Task 3.5: 멀티채널 + Media Foundation 포맷 (16)
- [ ] `CuePlayer` N채널(≤24) 경로, VST 체인 N채널 시도 후 실패 시 앞 2ch, 매트릭스 N행. `MediaFoundationAudioFormat`(m4a/aac/mp4/alac/wma 확장자, `IMFSourceReader`, 44.1k float PCM 변환, 길이 `MF_PD_DURATION`) 등록. 테스트: 4ch WAV 재생 라우팅, m4a 파일 리더 생성(테스트 자산 `tests/assets/tone.m4a`, 없으면 건너뜀).
- [ ] 커밋.

### Task 3.6: 오디션 (47)
- [ ] 설정 오디션 탭(그대로/출력 없음/대체 패치), Alt+Space 오디션 GO(플레이헤드 이동), Alt+V 오디션 미리듣기, "항상 오디션" 도구 메뉴 토글(GO 버튼 파란 "오디션" 라벨). 오디션 중 일반 재생 명령 → 즉시 일반 출력으로 재시작.
- [ ] 커밋. 단계 3 마감(README, 0.4.0, 코덱스, 메모리).

---

## 단계 4 — v0.5.0: 페이드 큐

### Task 4.1: FadeCurve + CurveEditor (20, 3과 공유)
- [ ] `FadeCurve::completion`: S커브(코사인 이징), 파라메트릭(강도 k: t^k / 대칭), 직선(점 선형), 커스텀(점 스플라인, 좌우 대칭 잠금). 도메인 보간. 테스트: 양끝 0/1, 단조성, 이퀄파워 중간 -3 dB.
- [ ] `CurveEditor` 컴포넌트(점 추가·드래그·삭제·기본 모양으로·대칭 잠금·강도 입력·도메인 선택·모양 선택). 엔벨로프 편집(1.4)도 이 커브 모양 선택지를 사용.
- [ ] 커밋.

### Task 4.2: 페이드 큐 모델·러너 (18·19·21·22·23·24·25)
- [ ] `FadeCueData` + 직렬화. `FadeRunner`(100 Hz): 시작 시 대상의 현재 레벨 기록(되돌리기용), 절대/상대, 활성 칸만, 완료 시 대상 정지 옵션, 속도 페이드(엔진 `setLiveRate`), VST 파라미터 페이드(대상 체인 슬롯 파라미터 보간, 출력 인서트는 불가). 서로 다른 칸을 다루는 페이드는 동시 실행.
- [ ] 테스트: 5 s 페이드 -inf→0 dB 곡선 궤적(가짜 시계), 상대 -6 dB, 완료 시 정지, 되돌리기, 겹치는 페이드(다른 칸)는 둘 다 적용.
- [ ] 커밋.

### Task 4.3: 페이드 큐 UI
- [ ] 큐 추가 메뉴 "페이드 큐"(Ctrl+7), 대상 선택(T: 큐 번호 입력 또는 목록 클릭), 탭: 레벨(매트릭스 활성/비활성 토글, 대상에서 레벨 가져오기 Ctrl+Shift+T, 전부 무음, 겡, 완료 시 대상 정지, 라이브 미리보기 토글) / 커브 / 이펙트(파라미터 체크·대상에서 가져오기·편집). 도구 메뉴: 라이브 페이드 미리보기(Ctrl+Shift+P), 페이드 되돌리기(Ctrl+Shift+R). 깨진 페이드 큐 경고(대상 없음·파라미터 없음).
- [ ] 커밋. 단계 4 마감(0.5.0, 코덱스, 메모리).

---

## 단계 5 — v0.6.0: 슬라이스 · 속도/피치 · devamp

### Task 5.1: 슬라이스 모델 + 소스 (5)
- [ ] `Slice{seconds, playCount}` (0=건너뜀, -1=무한), 최소 간격 0.05 s, WAV/AIFF 큐 마커 가져오기(`AudioFormatReader::metadataValues` "Cue" 항목). `RegionLoopSource`가 슬라이스 단위 회차 처리(devamp 슬라이스 = 현재 슬라이스 회차 끝에서 다음 슬라이스로).
- [ ] 테스트: 슬라이스 3개 [0,0.2)(×2) [0.2,0.4)(×0 건너뜀) [0.4,0.6)(×1) → 총 길이 0.6, 무한 슬라이스 + devamp 후 다음 슬라이스로, 모든 슬라이스 0 → 깨진 큐.
- [ ] 파형 뷰: M키 마커, 초록 핸들 드래그, 횟수 더블클릭 편집(글자=무한, 0=건너뜀), Delete/위로 드래그 삭제, "전부 삭제", 마커 가져오기.
- [ ] 커밋.

### Task 5.2: 속도 + 피치 유지 (6·22)
- [ ] `ResamplingAudioSource` 비율 실시간(이미 원자적), `signalsmith-stretch`(FetchContent, MIT) 경로: 피치 유지 시 스트레치 비율 1/rate, 레이턴시 보정. 속도 페이드는 4.2의 `setLiveRate` 램프.
- [ ] 테스트: rate 0.5 → 길이 2배, 피치 유지 rate 2.0 → 440 Hz 유지(FFT 피크), 길이 절반 ±5 %.
- [ ] 커밋.

### Task 5.3: devamp 큐 (53)
- [ ] 큐 종류 devamp{bySlice, startNext, stopTarget} + 컨트롤러: 대상의 현재 회차 끝 시각을 엔진에서 받아(`getPassEndTime`) 그 순간 다음 큐 시작/대상 정지 스케줄(샘플 정확: 엔진 콜백에서 이벤트 큐로 알림).
- [ ] 테스트: 무한 루프 0.5 s 큐 + devamp(startNext, stopTarget) → 회차 경계에서 정지 + 다음 큐 시작(오프라인 렌더로 경계 샘플 확인).
- [ ] 커밋. 단계 5 마감(0.6.0, 코덱스, 메모리).

---

## 단계 6 — v0.7.0: 그룹 · 카트 · 트랜스포트 계열 큐 · 시퀀스 녹음

### Task 6.1: 그룹 큐 (51)
- [ ] `GroupCueData{mode, playlist{shuffle, loop, crossfadeEnabled, crossfadeSeconds, fadeOutCurve, fadeInCurve}}`, 트리 편집(CueList id API), 목록 접기/펼치기(> <), 그룹 테두리 색(타임라인 초록·플레이리스트 주황·start-first 파랑·랜덤 보라).
- [ ] 컨트롤러: 타임라인(전부 동시, 자식 프리웨이트), 플레이리스트(순차·크로스페이드=다음 큐 미리 시작+두 페이드·루프·셔플·2차 트리거 다음/이전), 첫 큐 시작 후 진입, 첫 큐 시작, 랜덤(라운드로빈 메모리). 그룹 정지/패닉은 자식 전체.
- [ ] 타임라인 탭: 자식 프리웨이트를 막대 드래그·nudge(Alt+←→ 0.1 s, Shift+Alt 0.01 s)로 편집.
- [ ] 테스트: 각 모드 진행 순서(가짜 시계), 크로스페이드 중 두 큐 동시 재생, 라운드로빈 반복 없이 전부 한 번씩.
- [ ] 커밋.

### Task 6.2: 카트 (52)
- [ ] `CueContainer.isCart` + `CueCartView`(격자 1×1~15×15, 버튼: 번호·이름·색·재생 진행, 클릭=트리거(2차 트리거 규칙), 우상단 미리듣기), 카트 시작 시 전체 로드, 카트엔 그룹/자동 진행 없음(인스펙터에서 숨김), 설정 격자 크기 탭, 버튼 크기 소/중/대.
- [ ] 커밋.

### Task 6.3: 트랜스포트·goto·wait·memo·arm/disarm·target 큐 (54)
- [ ] 종류별 실행: start(재개 포함)/stop/pause/load(loadTime)/reset(대상 정지+임시값 초기화)/goto(플레이헤드 이동)/wait(길이만)/memo(아무것도 안 함)/arm·disarm(대상 armed 토글)/target(대상의 대상 교체). 대상 선택 UI 공통(`TargetPicker`), 대상 없으면 깨짐.
- [ ] 테스트: 각 종류 1개씩 동작(가짜 시계·엔진 오프라인).
- [ ] 커밋.

### Task 6.4: 시퀀스 녹음 (61)
- [ ] 도구 메뉴 "시퀀스 녹음…": 녹음 시작 후 큐 시작 시각을 기록 → 정지 시 타임라인 그룹(자식=start 큐, 프리웨이트=상대 시각) 생성.
- [ ] 커밋. 단계 6 마감(0.7.0, 코덱스, 메모리).

---

## 단계 7 — v0.8.0: 마이크 큐 (17)
- [ ] 장치 입력 채널 열기(마이크 큐가 있을 때만 0→N in), `MicCuePlayer`(입력 채널 → 레벨 매트릭스 → 패치), 인스펙터 레벨 탭 공유, 정지 페이드·패닉 적용. 테스트: 오프라인 입력 버퍼 주입 → 출력 라우팅.
- [ ] 단계 7 마감(0.8.0, 코덱스, 메모리).

## 최종
- [ ] 코덱스 전체 리뷰(전체 소스, 성능·스레드 안전·메모리) → 반영 → 전체 테스트·GUI 스모크 → 릴리스 → gom 보고(무엇이 바뀌었는지, 남은 한계: 장치 1개, CAF 없음, MIDI/타임코드/원격 제외).
