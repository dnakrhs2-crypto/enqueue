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
- [x] 테스트(`tests/BackupManagerTests.cpp`): 파일명 형식 `show (Backup 2026-09-02_143000).gocue`, 같은 초 충돌 시 " 2", rotate 규칙(20/시간별/일별, 비백업 파일 보존), copyIntoProject(복사·"(2)" 충돌·프로젝트 안 파일은 그대로).
- [x] 주기 백업(`MainComponent::autoBackupIfDue`, 30 Hz 타이머에서 간격 검사, dirty+파일 있을 때만, 미저장 상태를 백업 폴더에 직렬화) + 저장 전 백업(1분 1회) + 추가 시 복사.
- [x] 커밋 `feat: automatic project backups and copy-into-project`.

### Task 1.9: 단계 1 마감
- [x] README(단축키·기능) 갱신, `CMakeLists.txt` 0.2.0, 전체 테스트, GUI 스모크(열기·GO·트림·엔벨로프·드롭·일시정지·패닉·실행 취소).
- [x] 코덱스 read-only 리뷰(26건: 높음 7) → 19건 반영(일시정지 중 정지/페이드 무시, 실행 취소 후 재생 중 플레이어 미반영, 속도 상승 시 오디오 스레드 할당·큰 블록 재할당, 라이브 트림 위치 점프+선독 캐시, 구간 atomic 찢김(seqlock), 경과시간 속도 점프, 테일 중 플레이헤드 튐, 엔벨로프 no-op undo, JSON int 좁힘 UB, 백업 파일명/회전, 저장 전 백업 파일별 제한, 설정 창 프로젝트 전환, 피치 유지 토글 비활성, 잠금 토글 길이 0 방어, formatTimeMs int64, 뷰 span 경계, 락 안 할당 제거, restore 체인 통째 swap) → 커밋.
- [ ] **보류한 코덱스 지적(알려진 한계, 나중 단계에서)**: ① 오디오 콜백이 전역 CriticalSection을 잡는 구조(lock-free 커맨드 큐로 개편 — 3단계 패치 버스 작업 때 같이) ② BufferingAudioSource가 디스크 읽기와 같은 락 사용(JUCE 설계) ③ 일시정지 램프 끝난 뒤 블록 나머지만큼 재개 위치 늦음(최대 1블록) ④ 로드 직후 1.5초 플러그인 변경 무시 ⑤ 자동 백업이 재생 중 getStateInformation 호출(DAW도 동일) ⑥ 플러그인 예외 경계 없음 ⑦ 동일 플러그인 두 개 순서 바꿈은 undo가 못 알아봄(이동 UI 없음) ⑧ 저장 상태로 되돌려도 dirty.
- [ ] 릴리스 `python tools\release.py ... --publish`, 설치 업데이트 확인, 메모리 `project_gocue.md` 현황 갱신.

---

## 단계 2 — v0.3.0: 큐 속성 · 웨이트/진행 모드/시퀀스 · 스케줄러 · 목록 UI 개편 · 여러 리스트

### Task 2.1: 큐 공통 속성 모델 (26·29·30·31·48·59)
**Files:** `src/model/Cue.*`, serializer, `tests/ProjectSerializerTests.cpp`.
- [x] 필드: number(String), notes, color, secondColor, useSecondColor, flagged, armed, skipIfDisarmed, autoLoad, preWaitSeconds, postWaitSeconds, continueMode, hotkey, wallClock, fadeStopOthers, duck. 템플릿 `settings.templates[type]`.
- [x] `CueNumbering`: `nextNumber(list, increment)`(마지막 숫자 번호 + 증가), `renumber(ids, start, increment, prefix, suffix)`, 유일성 검사.
- [x] 테스트: 왕복, 재번호 "1, 2, 3" / 접두 "A" / 소수 증가 0.5, 중복 거부.
- [x] 커밋.

### Task 2.2: CueList 다중 선택 + 플레이헤드 분리 (46·58)
**Files:** `src/model/CueList.*`, `tests/CueListTests.cpp`.
- [x] API: `selection()`(vector<Uuid>), `setSelection`, `addToSelection`, `primarySelected()`, `playhead()`, `setPlayhead`, `movePlayheadNext/Prev`, `movePlayheadToNextSequence`(다음 "수동 진행" 큐), `lockPlayheadToSelection` 반영(선택 이동 시 플레이헤드 동행).
- [x] 테스트: 잠금 on/off 동작, 시퀀스 단위 이동(자동 계속 묶음 건너뜀), 삭제 시 선택/플레이헤드 보정.
- [x] 커밋.

### Task 2.3: 스케줄러 + 진행 모드 + 시퀀스 (27·28)
**Files:** Create `src/app/Scheduler.h/.cpp`; Modify `CueController`; Test `tests/SchedulerTests.cpp`, `tests/CueControllerTests.cpp`.
```cpp
class Scheduler { public: using Clock = std::function<double()>;   // 초
    explicit Scheduler (Clock clock); int schedule (double atSeconds, std::function<void()> fn); void cancel (int id); void cancelAll();
    void tick(); /* 1 ms 타이머에서 호출 */ int pendingCount() const; };
```
- [x] 컨트롤러: GO → 플레이헤드 큐 시작 → 자동 계속(postWait 뒤 다음 큐; 0이면 즉시 같은 호출) / 자동 팔로우(끝나면 다음) / 수동. 프리웨이트는 시작을 스케줄. 시퀀스 전체 시작 후 플레이헤드는 시퀀스 다음 큐로. 미리듣기는 웨이트·진행 무시.
- [x] 테스트(가짜 시계): 프리웨이트 1 s 뒤 재생, 자동 계속 postWait 0.5 s 뒤 다음 큐, 자동 팔로우 큐 길이 뒤 다음, 시퀀스 후 플레이헤드 위치, 비활성 큐 건너뛰기(skipIfDisarmed) vs 정지(armed=false: 재생 안 하고 진행).
- [x] 커밋.

### Task 2.4: 페이드&정지 타인 · 덕/부스트 · 핫키 · 벽시계 (35·36·37·38)
- [x] 엔진 `setDuckDb(id, db, seconds)` 램프 + 컨트롤러가 시작 시 같은 리스트의 다른 실행 큐에 적용, 끝나면 복원. fadeStopOthers 범위(peers/list/all) 적용.
- [x] 핫키: `KeyListener`가 큐 핫키 맵 조회(입력창 편집 중 제외), 키 뗄 때 2차 트리거. 벽시계: 스케줄러가 매 초 검사.
- [x] 테스트: 덕 -6 dB 적용/복원 렌더, fadeStopOthers가 대상만 페이드, 벽시계 요일 마스크.
- [x] 커밋.

### Task 2.5: 로드·시간으로 로드 · 리셋 확장 · 활성 큐 패널 (42·43)
- [x] 엔진 `load(cue, seconds)`(준비된 플레이어, 상태 loaded, GO 시 즉시 시작), 컨트롤러 `loadSelected`, `loadToTime` 다이얼로그(초, 음수 허용, 스크럽 슬라이더).
- [x] `ActiveCuesPanel`(우측 사이드바, Ctrl+L): 행마다 일시정지/재개 버튼·번호·이름·경과/남은·진행바(초록/노랑)·패닉 버튼·드래그 스크럽(`engine.seek(id, seconds)`), 정렬 방향 설정.
- [x] 커밋.

### Task 2.6: 목록 UI 개편 — CueListView (32·50·63·45·49)
**Files:** Create `src/ui/CueListView.h/.cpp`(CueTable 대체), `src/ui/StatusIcons.h`, `src/ui/WarningsWindow.*`, `src/ui/FileRelinkDialog.*`; Modify MainComponent.
- [x] 열: 상태 아이콘(스탠바이 삼각·재생 초록·일시정지 노란 바·로드 노란 원·테일 노란 경사·깨짐 빨간 X·경고 노란 삼각·깃발·오디션 괄호), 번호, 이름, 대상, 프리웨이트, 길이, 포스트웨이트, 진행(→ / 깃발 화살표). 행 색(큐 색·두 번째 색), 비활성 빗금, 행 크기 소/중/대.
- [x] 빠른 편집: N Q O T E D W C, 더블클릭 셀 편집, C는 순환. 다중 선택(Shift/Ctrl 클릭), 내부 드래그로 이동(다중), 파일 드롭 위치 삽입.
- [x] 쇼 모드: 편집 잠금(인스펙터 비활성·추가/삭제/이동/붙여넣기 차단, Esc·저장·종료 허용), 푸터 토글(Ctrl+Shift+[ / ]).
- [x] 경고 창(Ctrl+B): 깨진 큐 목록(파일 없음·대상 없음·플러그인 없음 등) + "파일 다시 찾기": 폴더 지정 → 같은 이름 파일 재연결.
- [x] 커밋.

### Task 2.7: 여러 리스트 · 찾기 · 속성 붙여넣기 · 템플릿 UI (55·57·59·60)

- [ ] **→ 6단계로 이동(2026-09-02 결정)**: 여러 큐 리스트(`CueContainer`·`ContainerTabs`)는 6단계 카트와 같은 컨테이너 구조라 한 번에 개편. "대상으로 점프·관련 큐 강조"는 대상이 생기는 4단계(페이드 큐)에서.
- [x] 찾기(Ctrl+F): 번호·이름·파일 이름·메모 검색(대소문자 무시, 선택 다음부터 순환), F3 다음 찾기. AlertWindow 텍스트 필드에 포커스 helper(`focusAlertEditor`: toFront + grabKeyboardFocus — 시간으로 로드·재번호 다이얼로그도 같은 수정).
- [x] 클립보드: Ctrl+C 복사(플러그인 체인 라이브 상태 포함)·Ctrl+X 잘라내기·Ctrl+V 붙여넣기(새 Uuid, 핫키 제거, 자동 번호 or 중복 번호 제거, 체인 restore). Ctrl+Shift+V 큐 속성 붙여넣기 다이얼로그(기본·시간·트리거·시간루프·레벨·이펙트 체크, `CuePropertyPaste::apply`가 모델 레이어 — 다른 파일이면 트림 클램프). 레벨 매트릭스·페이드 모양 항목은 3·4단계에서 추가.
- [x] 큐 템플릿: 설정 `hasCueTemplate/cueTemplate`(직렬화 = cueToVar 재사용), 메뉴 큐 > "선택 큐를 새 큐 기본값으로 / 초기화", `WorkspaceSettings::applyTemplate`(id·이름·번호·파일·길이·핫키·시계 제외) → 추가 시 체인 복원(`restoreChainsForCues`). 종류별 템플릿 탭은 큐 종류가 늘어나는 4·6단계에서.
- [x] 행 크기 설정(일반 탭 콤보) · 닫을 때 큐 시작(`fireCloseCueThen`: 큐 끝나면 종료, 2분 데드라인).
- [x] 테스트: `CuePropertyPasteTests`(전체/부분/트림 클램프/템플릿), 설정 왕복(템플릿·rowSize·startOnClose). 930 tests. GUI 스모크: 인라인 편집·활성 큐 패널·복사/붙여넣기·속성 붙여넣기(이펙트 포함)·찾기.
- [x] 커밋 590fddd.

### Task 2.8: 단계 2 마감 — README, 0.3.0 릴리스, 코덱스 리뷰, 메모리.
- [x] 코덱스 read-only 리뷰(main→phase2 diff, 29건: 높음 15·중간 10·낮음 4) → **25건 반영**: ① Scheduler tick이 timed 시작을 먼저 돌린 뒤 watch 판정(+action 직전 재확인, cancelAll 중단, isPending) ② auto-follow 다음 큐를 UUID로 기억 ③ 예약 항목을 큐(owner)별로 묶어 재시작/2차 트리거 정지/리셋 시 그 큐 것만 취소(`cancelPendingFor`) ④ Undo 스냅샷에 다중 선택·플레이헤드 저장/복원 ⑤ 닫을 때 큐 = 그 큐의 재생+예약이 끝났을 때 종료 ⑥ stop-pending loaded 인스턴스는 GO/isLoaded 후보에서 제외 ⑦ 편집된 loaded 큐 재로드, 플레이헤드 이동 시 이전 자동 로드 해제 ⑧ 덕 중앙 관리(대상별 기여 합산, 끝난 큐 것만 제거) ⑨ 덕 램프 선형(지정 시간에 정확히 도달) ⑩ Undo 시 구조 같은 체인은 `applyStates`로 프리셋까지 복원 ⑪ 인스펙터/시간루프 패널 focus-lost 커밋은 편집 시작한 큐(shownId)에 적용 ⑫ 셀 편집 비동기 커밋 = cueId+generation ⑬ loaded 큐를 P/F/덕/페이드정지 대상에서 제외 ⑭ 셀 편집 Esc → 패닉 실행 ⑮ 벽시계 = 마지막 확인 이후 구간 검사(최대 5초) ⑰ 시간으로 로드 = 타임라인 초→파일 초 변환, 범위·무한루프 거부 ⑱ seek 시 리샘플러 flush(오디오 스레드에서) ⑲ PlayingCue.progress(가상 위치/총 길이)로 진행바 통일 ⑳ 번호 유일성 검사(인스펙터·셀 편집 거부, 경고음) ㉑ 핫키 검증(앱 예약 키·Ctrl/Alt 조합·중복 거부) + OS 키 반복 억제 ㉒ 플레이헤드 잠금 켤 때 즉시 동기화 ㉓ 파일 스키마 v3 ㉖ 두 번째 색 = 재생한 큐(`hasPlayed`) ㉗ pending 목록 정리 ㉘ tick 중 cancelAll ㉙ 종료 확인 창 단일화 + SafePointer. 회귀 테스트 8건 추가(991 tests).
- [x] **보류 4건(알려진 한계)**: ⑯ 고정 reserve 한도(플레이어 256·표시 64·수거 16 초과 시 락 안 재할당 — 자동 로드 해제로 누적 원인 제거) ㉔ 패닉 페이드 중 새 GO 허용(큐랩과 같음, 의도) ㉕ getStates()가 플러그인 callback lock 없이 상태 캡처(JUCE 호스트와 동일) ⑳-재번호 충돌 검사(수동 편집만 거부).

---

## 단계 3 — v0.4.0: 레벨 매트릭스 · 패치 · 멀티채널/포맷 · 오디션

### Task 3.1: LevelMatrix/TrimLevels 모델 + 엔진 매트릭스 (9·11·12·15)
- [x] 모델 `src/model/LevelMatrix.*`(inputs/outputs/crosspoints + 겡, "-inf" 직렬화, `gainFor` = input+cross+output, resize 기본 라우팅: 모노→1-2, 그 외 대각선) + `TrimLevels`. **설계 편차: 메인 레벨은 기존 `Cue::gainDb`를 그대로 씀**(52곳 이름 바꾸기 회피, 플레이어의 기존 램프 경로가 메인 담당). `LevelMatrixTests`.
- [x] 엔진: `RegionLoopSource`가 파일 채널 전부 읽음(`reader->read` 다채널 변형 + int→float 변환), `CuePlayer` N채널(≤24) 렌더 → 스테레오 체인(모노는 ch1로 복제, 3ch+는 앞 2ch만) → `mixIntoBus`(매트릭스×트림, ~10 ms 램프, seqlock으로 새 게인 수신) → 버스. `AudioEngine::setLiveLevels`.
- [x] 테스트(AudioEngineTests): 크로스포인트 -6 dB / 무음 행 / 출력 +6·트림 -6 상쇄 / 라이브 변경 램프 / 4ch 파일 3-4채널 라우팅. 1000 tests.
- [x] 커밋 b537be5.

### Task 3.2: 패치 모델 + 패치 버스 렌더 + 장치 M채널 (13·14)
- [x] `src/model/AudioPatch.*`(id·이름·큐 출력 수 기본 8·출력 이름·라우팅 K×M 지연 확장·메인·큐/장치 출력 인서트·스테레오 묶기) + `Project::patches`(항상 ≥1, [0]=기본) + 직렬화 + `ProjectDocument::setPatches/patchForCue/cueOutputsFor`(실행 취소 밖). `AudioPatchTests`, 직렬화 왕복 테스트.
- [x] 엔진 `PatchRuntime`(버스 K×block, routed M×block, 큐/장치 출력 체인 map, 라우팅 게인 램프): `setPatches`(체인은 락 밖에서 restore, 사라진 패치의 플레이어는 기본 패치로), `updatePatchLevels`, `getPatch*Chain`, `capturePatchInsertStates`, 장치 출력 `initialise(0, 64)` + `prepare(sr, block, M)` + 설정 창 최대 64채널. 플레이어는 `busTag`로 자기 버스를 앎. 마스터 체인은 장치 1-2에 유지.
- [x] 테스트: 3-4채널 라우팅·패치 메인·라이브 라우팅 변경·스테레오 묶음 인서트·장치 출력 인서트·상태 캡처·미지 패치 → 기본·패치 교체 시 플레이어 이동. 1075 tests. MainComponent: 열기/새 프로젝트/저장 시 `setPatches`·`capturePatchInsertStates`.
- [x] 커밋 8e917f9, 05bf63d.

### Task 3.3: LevelMatrixComponent + 레벨/트림 탭 + 겡 (9·10·11·12)
- [x] `src/ui/LevelMatrixComponent.*`: 메인/입력/출력/크로스포인트 셀, 세로 드래그(Shift=0.1 dB, 무음은 아래로 끌어도 무음), 더블클릭 기본값, 숫자 타이핑(부호 없으면 음수, 빈칸/-inf=무음), 화살표 이동, Delete=무음, 우클릭 겡 1~8(색 배경, 같은 겡은 같이 움직임), 장치에 안 닿는 출력 열은 흐리게+귀퉁이 표시, 레벨 한계(설정) 클램프. 입력 이름 편집은 생략(채널 번호 표시).
- [x] 인스펙터 **레벨** 탭(패치 콤보 + 기본 레벨로 / 전부 무음 + 그리드, 재생 중 즉시 반영, coalesce "levels:id") · **트림** 탭(메인 + 출력별 세로 슬라이더).
- [x] 커밋 e1d5f49. (GUI 스모크는 gom 사용 중이라 보류 — 유휴 확인 후 실행)

### Task 3.4: 패치 편집기 다이얼로그 (13·14)
- [x] `src/ui/PatchEditorDialog.*`(오디오 > 오디오 패치..., Ctrl+Shift+P): 패치 목록(추가·복제·삭제, ★=기본), 이름, 큐 출력 개수, 장치 채널 수 표시, 탭 큐 출력(이름·다음과 스테레오·인서트 창) / 패치 라우팅(`LevelMatrixComponent` 재사용: 행=큐 출력, 열=장치 출력, 메인, 가장자리 레벨 숨김 `setEdgeLevelsVisible`) / 장치 출력(인서트 창). "기본 라우팅으로". 인서트 편집은 실행 취소 밖(markDirty). "다른 패치에서 복사"는 생략(복제로 대체).
- [x] 설정 창 **오디오** 탭: 레벨 상한/하한. 패치 선택은 인스펙터 **레벨** 탭 상단 콤보(기본 탭이 아니라 레벨 탭 — 자리 문제).
- [x] 커밋 e1d5f49.

### Task 3.5: 멀티채널 + Media Foundation 포맷 (16)
- [x] N채널 경로는 3.1에서. **VST 체인 N채널 시도는 보류**(체인은 스테레오 고정: 모노 복제, 3ch+는 앞 2ch) — 4단계 이후 필요 시.
- [x] `src/audio/MediaFoundationAudioFormat.*`(.m4a/.aac/.mp4/.m4b; .wma는 JUCE WindowsMedia 포맷 유지): `IMFSourceReader` → float PCM(원본 SR/채널), 길이 `MF_PD_DURATION`, **시크 = 2프레임 프리롤 후 타임스탬프로 폐기**(첫 프레임은 overlap-add 없어 틀림), **프라이밍 프레임(ts 중복) 1버퍼 lookahead로 폐기** → ffmpeg 디코드와 샘플 정렬(상관 1.0000, lag 0), 시크=순차와 max diff 0. CMake: mfplat/mfreadwrite/mfuuid/propsys 링크. 엔진 ctor에서 등록.
- [x] 테스트 `MediaFoundationTests`(자산 `tests/assets/sweep.m4a` + `sweep_ref.wav`, ffmpeg 생성; 백색소음은 AAC PNS 때문에 비교 불가라 스윕 사용). 1095 tests.
- [x] 커밋 9c964d3.

### Task 3.6: 오디션 (47)
- [x] 설정 오디오 탭에 오디션 방식(그대로 / 출력 없음 / 대체 패치 + 패치 콤보), `PlayOptions{audition, silent, patchOverride}`(silent = 엔진의 `muteRuntime` 버스로), `CueController::go/preview/fireSequence/trigger(audition)` + `playOptions()`, 재생 메뉴 오디션 GO(Alt+Space)·오디션 미리듣기(Alt+V)·항상 오디션(체크, GO 버튼 파란 "GO (오디션)"). 오디션 중 일반 GO = 2차 트리거 규칙 건너뛰고 실제 출력으로 재시작(`engine.isAuditioning`).
- [x] ★버그 수정: `juce::Uuid` 기본 생성자는 난수 → 선택적 id(`Cue::patchId`, `PlayOptions::patchOverride`, `auditionPatchId`, `ProjectSnapshot::selectedId`)를 `Uuid::null()`로 초기화(안 하면 모든 재생이 오디션으로 표시되던 사고).
- [x] 테스트 CueControllerTests(출력 없음 / 일반 GO 재시작 / 대체 패치 라우팅 / 항상 오디션). 1111 tests. 커밋 f7fefca.
- [x] GUI 스모크(2026-09-02 15:45): 레벨 탭(2×8, 귀퉁이 표시), 트림 탭, 패치 편집기 3탭, 라우팅 매트릭스, 크로스포인트 드래그(-∞→-50 dB), Alt+Space 오디션 GO 상태 표시 확인.
- [ ] 단계 3 마감: README·릴리스 노트 0.4.0(작성됨), 코덱스 리뷰(2단계 리뷰 끝난 뒤), 릴리스, 메모리.

---

## 단계 4 — v0.5.0: 페이드 큐

### Task 4.1: FadeCurve + CurveEditor (20, 3과 공유)
- [x] `src/model/FadeCurve.*`: S커브(코사인), 파라메트릭(t^k, 대칭=양쪽 완만), 직선, 커스텀(단조 큐빅 Fritsch-Carlson, 좌우 점대칭 잠금), 도메인 보간(슬라이더=진폭 세제곱근, 데시벨, 리니어; 무음은 하한 레벨에서 출발/도착). `FadeCurveTests`(양끝·단조·도메인·이퀄파워·커스텀 점 편집·왕복).
- [x] `src/ui/CurveEditor.*`: 모양/강도/도메인/대칭/기본 모양 + 캔버스(클릭 추가, 드래그 이동, 더블클릭·Delete 삭제). **엔벨로프(1.4)에 커브 모양 적용은 보류**(엔벨로프는 점 기반 직선/곡선으로 충분, 필요 시 후속).
- [x] 커밋 7cdbffe, 8c5675c.

### Task 4.2: 페이드 큐 모델·러너 (18·19·21·22·23·24·25)
- [x] `CueType{audio, fade}` + `FadeCueData`(대상 id·시간·상대·완료 시 정지·레벨 목표+활성 플래그·속도·VST 파라미터 목표·커브) + 직렬화("type", "fade"). `Cue::passLength/effectiveLength`가 페이드는 시간을 돌려줌.
- [x] `src/app/FadeRunner.*`: 시작 시 대상 실시간 상태(`AudioEngine::getLiveState` — CuePlayer 메시지 스레드 미러)를 from으로, 목표(절대/상대·활성 칸만)를 to로 잡고 100 Hz tick에서 커브로 보간(레벨은 매 tick 현재 상태를 읽어 활성 칸만 덮어써서 다른 칸의 페이드와 공존, 속도는 기하 보간, 파라미터는 `setValueNotifyingHost`), 완료 시 대상 정지, 되돌리기 스택(20), stop/stopAll. 컨트롤러: `trigger`가 페이드 큐면 러너 시작(재시작=현재값에서), `isCueActive`(엔진 or 러너)로 auto-follow/덕 판정, panic/hardStop/reset이 페이드 취소, `getFadeRunner()`.
- [x] `FadeRunnerTests`(대상 없음/재생 안 함 거부, 절대 메인 궤적·완료·되돌리기, 상대·비활성 칸 보존, 크로스포인트 단일 칸 + 동시 2개, 속도 기하·완료 시 정지, stop, 직렬화 왕복, 컨트롤러 auto-follow·Esc). 3260 tests.
- [x] 커밋 7cdbffe.

### Task 4.3: 페이드 큐 UI
- [x] 큐 > 페이드 큐 추가(Ctrl+7: 선택한 오디오 큐를 대상으로, 목표 = 대상 레벨, 메인만 활성), 인스펙터 탭 세트 전환(오디오: 기본/시간루프/레벨/트림/트리거/이펙트 · 페이드: 기본/**페이드**/**커브**/트리거/**파라미터**), 기본 탭은 페이드 큐에서 파일·게인·정지 페이드·자동 로드 숨김. 페이드 탭: 대상 콤보(T 키 입력 대신 콤보), 시간, 상대, 완료 시 정지, 레벨 페이드 토글, 대상에서 레벨 가져오기(Ctrl+Shift+T), 전부 활성/비활성, 속도 페이드+목표, 라이브 미리보기 토글(켜면 목표가 재생 중 대상에 즉시, 끄면 복원), 매트릭스 활성 칸(노란 점/빗금, Alt+클릭·우클릭). 커브 탭 = CurveEditor. 파라미터 탭 = 대상 체인의 VST3 파라미터(체크+슬라이더+표시값). 재생 메뉴 페이드 되돌리기(Ctrl+Shift+R). 표: 페이드 행 아이콘(경사, 대상 없으면 빨강)·파일 열 "→ 대상"·길이=페이드 시간·실행 중 진행바(러너 정보를 재생 목록에 병합), 활성 큐 패널에도 표시, 경고 창에 대상 없음. 트랜스포트 대기 정보에 대상·시간·모드. 속성 붙여넣기에 "페이드" 그룹(대상 제외). **라이브 미리보기 단축키(Ctrl+Shift+P)는 패치 편집기가 쓰므로 토글 버튼만.**
- [x] GUI 스모크(2026-09-02 16:50): Ctrl+7 → 페이드 큐 생성·페이드 탭·커브 탭, 대상 GO 후 페이드 GO → 표/활성 큐 패널 진행 표시 확인. 커밋 8c5675c, 6a7bfd3.
- [ ] 단계 4 마감: README·릴리스 노트 0.5.0(작성됨), 코덱스 리뷰(3단계 리뷰 뒤), 릴리스, 메모리.

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
