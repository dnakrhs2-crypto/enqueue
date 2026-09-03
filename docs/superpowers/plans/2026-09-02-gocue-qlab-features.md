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
- [x] 코덱스 read-only 리뷰(main→phase3, 25건: 치명 1·높음 8·중간 10·낮음 6) → **21건 반영**(phase6 워크트리에서, 커밋 bdae847): ① 패치 체인 map은 오디오 락 안에서만 삽입(getPatch*Chain·setPatches 대기 체인) ② setPatches/updatePatchLevels 락 안 복사·할당 제거(사전 sanitise+라우팅 계산 후 swap, 이전 값은 락 밖에서 파괴) ③ 33ch+ 장치 콜백은 사전 할당 scratch로(JUCE AudioBuffer 채널표 힙 할당 회피) ④ 오디션은 loaded 인스턴스 재사용 안 함 ⑤ 대체 패치가 사라지면 무음 오디션(엔진+컨트롤러) ⑥ 인서트 창은 구조 변경/편집기 종료 시 닫고, 구조 변경 전 인서트 상태를 문서에 캡처 ⑦ 플러그인 상태 캡처는 callback lock 안에서 ⑧ 프로젝트 열기/새로 만들기는 저장된 플러그인 상태 적용(applySavedStates) ⑨ 실행 취소가 레벨/트림도 라이브 반영 ⑩ 모노 큐 체인 뒤 L+R 합산(스테레오 이펙트 R 유실 방지) ⑪ MF: 스레드별 COM 초기화, 시크 실패 = 무음, ERROR/미디어타입 변경 플래그 처리, 프라이밍 중복 제거는 시크 직후만 ⑫ 레벨 한계 통일(+24/−120, 설정도 그 범위) ⑬ 파일 버전 4 ⑭ 패치 id 중복 시 재발급 ⑮ dB 문자열 엄격 파싱(쓰레기 = 기본값) ⑯ 겡: 더블클릭 리셋·Delete 무음·숫자 입력도 겡 전체 ⑰ 레벨 편집 커밋은 편집 시작한 큐(shownId)에, 편집 중 선택 변경 무시 ⑱ isAuditioning은 최신 인스턴스 기준.
- [ ] **보류(알려진 한계)**: ⑨ 재생 중 큐의 패치 변경은 다음 시작부터(런타임/폭 고정) ⑬ 10 ms 램프가 블록 단위 지수 수렴(청감 차이 없음) ⑳ seqlock payload는 전역 락 덕에 안전(락 제거 시 재설계) ㉕ MF_PD_DURATION 없는 파일 거부.
- [x] **릴리스 결정(2026-09-02)**: 0.4.0/0.5.0/0.6.0 중간 릴리스는 생략하고 3~6단계 + 코덱스 반영을 합쳐 **0.7.0 하나로 릴리스**(gom 요구는 "다 만들고 코덱스 검증"; 중간 설치 3회는 gom 시간만 씀). 릴리스 노트 0.4/0.5/0.6 문서는 0.7.0 노트에 합침.

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
- [x] 코덱스 read-only 리뷰(phase3→phase4, 21건: 높음 8·중간 9·낮음 4) → **20건 반영**(커밋 3ac7d3d): ① 페이드를 대상 인스턴스(startOrder)에 묶음 — 대상 정지/재시작 시 페이드 종료, ② 페이드 큐의 fadeStopOthers가 자기 대상 제외 + 실행 중 다른 페이드 큐도 정지, ③ 같은 셀에 새 페이드가 오면 이전 페이드는 그 lane을 놓음(끝난 뒤 되돌아가는 점프 제거), stop 완료 시 같은 대상 페이드 정리, ④ 새로/열기 시 `resetForNewProject`(페이드·revert·플레이리스트·대기·덕·played 전부 초기화), ⑤ revert = 그 페이드가 소유한 lane(활성 셀·rate·파라미터)만 복원, ⑥ 미리보기 중 대상 변경 → 먼저 복원, ⑦ 파라미터 패널: 자기 콜백 안에서 행 재구축 금지(비동기), raw 파라미터 포인터 제거(매번 재조회), ⑧ 붙여넣기: 복사 집합 내부 대상(fade/devamp/control·secondTarget) 재매핑, ⑨ 커스텀 커브 점 x 엄격 증가(최소 간격 0.001), ⑪ `stopCue` 단일 경로(리셋·삭제·활성 큐 x 버튼) — 페이드/그룹/대기/대상 향한 페이드까지, ⑫ 오디오 큐만 템플릿, 새 큐 타입/부모 보존, ⑬ Undo 시 다중 선택 + 플레이헤드 그대로(`restoreCursors`), ⑭ 닫을 때 큐 = isCueActive, ⑮ 커브 편집 무변경 mouse-up은 undo 단계 안 만듦, ⑯ 기본 탭 커밋 전부 shownId 기준, ⑰ 미러 잠금 시 오른쪽 점 편집 = 왼쪽 쌍 편집, ⑱ 셀 편집 stale 커밋의 포커스 탈취 방지, ⑲ 페이드도 played, ⑳ 앱이 포그라운드가 아니면 heldKeys 초기화, ㉑ Uuid null 초기화.
- [ ] **보류**: ⑩ 페이드 타이머(100 Hz 메시지 스레드)가 GUI 정지 시 늦어짐 + tick마다 엔진 락 안 복사 — 엔진 램프 커맨드로 옮기는 재설계는 추후.

---

## 단계 5 — v0.6.0: 슬라이스 · 속도/피치 · devamp

### Task 5.1: 슬라이스 모델 + 소스 (5)
- [x] `Slice{seconds, playCount}` + `AudioCueData::slices/firstSliceCount`(정렬·0.05 s 간격·클램프, 직렬화 "slices"/"firstSliceCount"), `sliceSequenceSeconds`, effectiveLength(무한 슬라이스 → -1). `RegionLoopSource` 재작성: run(파일 구간×횟수) 배열 = 가상 타임라인(seqlock 발행), 무한 run은 "끈끈이"(그 안에서 순환) → `finishCurrentPass(pos, stopAfter)`가 회차 해소 후 다음 run으로(또는 stopAfter면 이후 run 전부 건너뜀), `locate`/`virtualPositionFor`로 라이브 트림·시크가 파일 위치 유지. 플레이어 `setLiveSlices`, 엔진 `setLiveSlices`, 모든 슬라이스 0 = 재생 거부.
- [x] 테스트(RegionPlaybackTests): [0,0.2)×2·건너뜀·[0.4,0.6)×1 → 0.6 s 레벨 순서 확인, 무한 슬라이스 + devamp → 다음 슬라이스, 전부 0 거부, 정리·직렬화. 기존 3327 테스트 전부 유지.
- [x] 파형 뷰: M = 커서에 마커, 초록 마커선 + 상단 삼각 + 횟수 배지(첫 슬라이스는 구간 시작 배지), 드래그 이동(이웃·간격 제한), 더블클릭 = 횟수 입력(0 = 건너뜀, x/inf = 무한), Delete 삭제, 우클릭 메뉴: 마커 추가 / 전부 삭제 / **파일 큐 마커 가져오기**(WAV/AIFF NumCuePoints·CueNOffset). 커밋 588e332.

### Task 5.2: 속도 + 피치 유지 (6·22)
- [x] `third_party/signalsmith-stretch`(1.3.2, MIT) + `signalsmith-linear`(0.3.1) **헤더 벤더링**(FetchContent 대신 — 네트워크 없이 빌드). `src/audio/StretchSource`: 읽기선행 뒤·리샘플러 앞에서 파일 샘플 단위로 rate배 입력을 당겨 시간 신장, 위치 변경 시 `outputSeek` 프리롤(정렬), 라이브 rate 원자적, 범위 0.25~4배(초과는 가장자리). CuePlayer: `preservePitch`면 체인에 삽입(리샘플러는 SR 변환만), `advanceFor`(가상 위치 진행 = 실효 rate). 시간·루프 탭 "피치 유지" 토글 활성(다음 재생부터).
- [x] 테스트: 2 s 파일 rate 2.0 — 피치 유지: 길이 ≈1.0 s + 440 Hz 에너지 ≫ 880 (Goertzel) / 바리스피드: 880 ≫ 440. 3365 tests.
- [x] 커밋 f0633cd.

### Task 5.3: devamp 큐 (53)
- [x] `CueType::devamp` + `DevampCueData{targetId, startNextCue, stopTarget}`(직렬화 "devamp"). 엔진 `finishCurrentPass(id, stopAfter)`(소스가 무한 run 해소 + stopAfter면 이후 run 건너뜀 → 회차 경계에서 정지) + `getSecondsToPassEnd`(플레이어 `getCurrentPassEnd` = 회차 끝 가상 위치). 컨트롤러 `trigger`: 대상 없음/재생 안 함 → failed 상태, 아니면 finishCurrentPass + startNextCue면 **스케줄러 1 ms 타이머**로 회차 끝 시각에 다음 큐 fireSequence(설계의 "엔진 콜백 이벤트 큐" 대신 — 정확도 ≈ 1 ms + 오디오 블록, 실사용엔 충분). "bySlice"는 소스가 무한 슬라이스 안이면 슬라이스 회차, 아니면 전체 회차로 자동 처리.
- [x] UI: 큐 > 디밴프 큐 추가(Ctrl+8, 선택 오디오 큐 대상), 인스펙터 탭 세트(기본/**디밴프**/트리거: 대상 콤보·다음 큐 시작·대상 정지), 표 아이콘(루프 호 + 막대, 대상 없으면 빨강)·"→ 대상" 열, 트랜스포트 대기 정보, 경고 창, 속성 붙여넣기 그룹.
- [x] 테스트: 0.5 s 무한 루프 + devamp(stop) → 회차 끝(getSecondsToPassEnd)에 정지(±0.03 s), stop 없이 단일 구간 → 역시 끝, 컨트롤러: 디밴프 GO → 회차 끝에서 대상 정지 + 다음 큐 시작, 대상 없음 실패. 3385 tests. 커밋 f0633cd.
- [x] GUI 스모크(슬라이스 마커 표시·디밴프 탭·무한 슬라이스 → 디밴프 → 이어감 + 다음 큐 시작) + 발견 버그 수정: 인스펙터 탭 전환이 첫 입력칸에 포커스를 줘 Space가 GO가 안 되던 문제(`InspectorTabs::onReturnFocus`), 페이드/디밴프 첫 trigger 실패가 "GO:"로 덮이던 문제(`firstTriggerResult`).
- [x] 코덱스 read-only 리뷰(phase4→phase5, 19건: 높음 8·중간 7·낮음 4) → **17건 반영**: ① 레이아웃 발행 = SpinLock(seqlock UB·스핀 제거), ② 자체 `ReadAheadSource`(invalidate: 라이브 트림/슬라이스/디밴프 뒤 옛 캐시 재생 없음, 피치 모드 프리롤 캐시), ③ 트림 시작 이전/정확히 시작점 마커의 횟수를 첫 슬라이스가 상속(엔진·모델 동일 규칙 `firstCountFor`), ④ 제어 함수는 `controlPosition()`(미적용 점프 포함) 기준, ⑤ 편집 시 sequencePass 보존(`virtualPositionFor(Location)`), ⑥ 디밴프가 실제 경계(시퀀스 무한이면 시퀀스 회차 끝)를 돌려주고 끝낼 게 없으면 실패, ⑦ 다음 큐는 대상 인스턴스·가상 위치를 watch(일시정지/속도 변경 반영, 정지/재시작 시 취소, 1 s 데드라인), ⑬ startNext 디밴프는 자기 continue 모드 무시, ⑩ 디밴프/회차 끝 = 최신 인스턴스, ⑪ `effectiveRate()`(피치 모드 0.25~4) 모델·플레이어·컨트롤러 공통, ⑫ 확정 횟수는 run 파일 위치 키, ⑧ 스트레처 prepare 뒤 pending seek 소비, ⑯ spare 출력 버퍼, ⑭ 시간 탭 라이브 반영 종류별 분리(피치/엔벨로프 = 다음 시작, 슬라이스 = setLiveSlices만), ⑱ 슬라이스 undo 단계(드래그만 coalesce), ⑰ 횟수 입력 엄격(-1/x = 무한, 비숫자 = 유지), ⑮ Undo가 슬라이스도 라이브 반영, ⑲ 길이 표시에 슬라이스 반영.
- [ ] **보류**: ⑧ 라이브 seek의 stretcher 재분석이 오디오 스레드에서 실행(프리롤 ~수천 샘플; 드롭아웃은 시스템 의존) ⑨ 피치 모드 시작 프리롤이 캐시 밖이면 무음으로 분석(프리롤 캐시로 대부분 해결).

---

## 단계 6 — v0.7.0: 그룹 · 카트 · 트랜스포트 계열 큐 · 시퀀스 녹음

### Task 6.1: 그룹 큐 (51)
- [x] `CueType::group` + `GroupCueData{mode(timeline/playlist/startFirstEnter/startFirst/random), collapsed, shuffle, loop, crossfade, crossfadeSeconds}`(크로스페이드 커브는 생략 — 현재 자식의 정지 페이드 + 다음 자식의 자기 페이드인). 트리 = `Cue::parentId` + pre-order 연속성(CueList: depthOf/subtreeEnd/childrenOf/nextSibling/isRowVisible/nextVisible/addAfter/wrapInGroup/ungroup/moveSubtrees, remove/duplicate/move가 서브트리 단위, sanitiseTree). 표: 들여쓰기·삼각형(클릭/←→)·모드 색 막대(타임라인 초록·플레이리스트 주황·첫큐 파랑·랜덤 보라)·접기. 직렬화 "parent"/"group".
- [x] 컨트롤러: 타임라인(자식 전부 각자 프리웨이트로 동시), 플레이리스트(순차 watch·크로스페이드=남은 시간 ≤ xf에 다음 시작+현재 페이드아웃·반복·셔플·두 번째 GO = 다음), 첫 큐 시작 후 진입(플레이헤드가 그룹 안으로), 첫 큐 시작, 랜덤(한 바퀴 한 번씩). stopGroup/패닉은 자식 전체. 시퀀스는 형제 범위로 제한.
- [x] 그룹 탭 타임라인 막대 편집기: 드래그(0.1 s 격자, Shift = 자유), Alt+←/→ 0.1 s, Shift+Alt 0.01 s, ↑↓ 자식 선택. 명령: Ctrl+0 그룹 추가, Ctrl+G 묶기, Ctrl+Shift+G 해제, 모두 접기/펼치기.
- [x] 테스트(CueListTests 트리 5건, CueControllerTests 4건: 타임라인 프리웨이트, 플레이리스트 순차/skip/반복, 크로스페이드 겹침, 첫큐 진입/랜덤 라운드로빈). 커밋 ee9ffe3·a12358c·758ab0b·3b2e72a.

### Task 6.2: 카트 + 여러 큐 리스트 (52 + 2단계에서 이월)
- [x] 모델: `CueContainer{id,name,isCart,cartRows,cartCols,cues}`, `Project::lists/activeList`(`cues()` = 메인 리스트 접근자), 파일 버전 5("lists"+"activeList", "cues"는 호환용 메인 리스트, v4 이하 = 단일 리스트). 문서: 비활성 리스트는 자기 CueList에 큐+커서 보관, 활성 리스트는 `document.cues`로 swap(setActiveContainer — dirty 아님), add/remove/rename/setContainerCart(undo는 perform으로), listContaining/containerOf/findCueAnywhere/forEachList, 스냅샷에 전 리스트. 컨트롤러 id 경로 전부 리스트 횡단(핫키·벽시계·팔로우·제어 대상·goto는 리스트 전환), 카트에서 GO 없음, fadeStopOthers 범위 = peers/list/all 실제 의미.
- [x] UI: ContainerTabs(탭·+ 메뉴·우클릭 이름/카트 전환/격자 크기/삭제), CueCartView(격자 1×1~15×15, 버튼 번호·이름·색·진행·남은 시간, 클릭 = 트리거(2차 트리거 규칙), 우클릭 = 정지, 슬롯에 파일 드롭), Ctrl+PageUp/PageDown. **생략**: 카트 진입 시 전체 로드(디스크 부담, 필요하면 로드 큐로), 버튼 크기 소/중/대(격자 크기로 대신), 카트에서 인스펙터 숨김(그대로 둠 — 진행 모드는 카트에서 무시).
- [x] 테스트: ProjectSerializer(리스트/카트 왕복, v4 레거시), ProjectDocumentTests 5건, CueControllerTests 리스트 횡단 1건. GUI 스모크(탭 전환·카트 버튼 재생·Esc). 커밋 98fd95c·7fe1d90·dd80509·7590050.

### Task 6.3: 트랜스포트·goto·wait·memo·arm/disarm·target 큐 (54)
- [x] 한 종류 `CueType::control` + `ControlCueData{kind, targetId, secondTargetId, seconds}`(11 kind): start(일시정지면 재개, 아니면 대상 시퀀스)/stop/pause/load(초)/reset/goto(플레이헤드, 다른 리스트면 전환)/wait(초 동안 active)/memo/arm/disarm/target(대상의 대상 교체). 동작 탭(종류·대상·새 대상·시간), 표 글리프 11종, 트랜스포트 정보, 경고(대상 없음), 속성 붙여넣기. 명령 Ctrl+9(시작), 대기 큐/메모 큐 메뉴.
- [x] 테스트 CueControllerTests 1건(11종 전부 + 대상 없음 실패). 커밋 b59089b.

### Task 6.4: 시퀀스 녹음 (61)
- [x] 큐 메뉴 "시퀀스 녹음 시작/정지"(Ctrl+Shift+E, 체크 표시 + 기록 수): 녹음 중 시작된 큐와 시각 → 정지 시 선택 뒤에 타임라인 그룹(자식 = 시작 제어 큐, 프리웨이트 = 상대 시각). 테스트 1건. 커밋 49b33c0.
- [x] 코덱스 read-only 리뷰(phase5→phase6, 29건: 치명 2·높음 17·중간 10) → **27건 반영**(커밋 240a44a·part B): ① 자기 자신을 시작하는 순환(제어 큐 A→B→A, 그룹 안의 자기 시작 큐) = dispatch 스택으로 거부 + 깊이 32 제한 ② 다른 리스트로의 goto는 최상위 dispatch가 끝난 뒤 적용(시퀀스 도중 리스트 교체 금지), 카트는 큐 복사본으로 실행 ③ moveSubtrees: 이동 전 부모 결정, 자기 행/자기 서브트리에 드롭 = no-op ④ 복제 시 서브트리 내부 대상(fade/control) 재매핑·핫키 비움·모든 행 번호/플러그인 체인 복제 ⑤ 플레이리스트 감시 = isCueActive(중첩 그룹·대기·페이드), 시작 실패 무한 재시도 방지, skip 시 프리웨이트 중 항목도 취소, 크로스페이드가 다음 프리웨이트 반영 ⑦ 그룹의 fadeStopOthers/덕이 자기 자식 제외 ⑧ cancelPendingFor가 덕 기여 해제 ⑨ 그룹 활성 판단·정지에 자식 대기 포함 ⑪ enter 인덱스를 리스트에 묶음 ⑫⑬ 열기/undo 복원이 모든 리스트의 체인 대상 ⑭ 컨테이너 삭제 시 큐 정지+체인 제거 ⑮ 리스트 전환 전 셀 편집 동기 커밋 ⑯ 번호/핫키 유일성 프로젝트 전역 ⑰ 페이드/디밴프/제어 대상 콤보·경고·FadeRunner가 모든 리스트 ⑱ ReadAheadSource 세대 검사 ⑲ applyStates가 체인 락+콜백 락 ㉑ 리스트 전환 한 번 통지(자동 로드 2회 방지) ㉒ 깊이 32 제한 ㉓ 묶기/해제 플레이헤드 보존 ㉔ 접을 때 숨은 플레이헤드는 그룹 행으로 ㉖ 녹음: 제어 큐 아래 시작은 기록 안 함, 다른 리스트 큐도 그룹화 ㉘ 비활성 리스트 편집도 dirty ㉙ 카트 버튼 = controller.fire(덕/페이드정지 포함), 우클릭 = stopCue(fade).
- [ ] **보류**: ⑳ 그룹 일시정지가 프리웨이트/대기/페이드/플레이리스트 진행까지 멈추진 않음(오디오 자식만) ㉗ undo 스냅샷에 비활성 리스트 커서 없음(첫 행으로) ⑥ 실패 항목 backoff 대신 한 바퀴 전부 실패 시 종료.
- [x] 단계 6 마감: 0.8.0 릴리스(3~7단계 합본, 2026-09-02 저녁) — 최종 전체 코덱스 리뷰는 90분 제한을 넘겨 결과 없이 종료 → 범위를 좁혀 재실행하고 결과는 0.8.1로 반영.

---

## 단계 7 — v0.8.0: 마이크 큐 (17)
- [x] `CueType::mic` + `MicCueData{firstInput, numInputs}`; `CuePlayer` 마이크 모드(파일 대신 엔진이 블록마다 넘겨주는 장치 입력 포인터 `setInputBlock`; 엔벨로프·일시정지·게인·덕·체인·매트릭스 그대로, 길이 ∞); `AudioEngine::renderBlock(output, n, inputs, numInputs)` + 콜백이 입력 전달, `setInputsWanted(n)`(프로젝트의 마이크 큐가 필요로 하는 만큼 장치 입력을 열고 재시작 — `documentStateChanged`마다 no-op 검사), `getNumDeviceInputs`, 오디오 설정 창에 입력 채널(최대 32). 인스펙터 입력 탭(첫 채널·채널 수·장치 입력 상태), 표 마이크 글리프/"입력 1-2"/∞, 트랜스포트, 경고(입력 부족), Ctrl+M, 페이드/디밴프 대상 콤보에 마이크 큐 포함. 테스트: 오프라인 입력 버퍼 주입 → 행별 출력 라우팅·무입력 시 무음·정지. GUI 스모크(입력 없는 장치에서 경고 1 + 무음 재생 확인). 커밋 b9f9986.
- [x] 단계 7 마감: 0.8.0에 포함(마이크 큐), 최종 리뷰는 0.8.1로.

## 최종 코덱스 리뷰(범위 좁힌 재실행: 오디오 스레드 + 마이크 큐) → v0.8.1
- [x] 15건 중 13건 반영: (1) 마이크 큐 일시정지 시 source 널 참조 크래시 (2) 읽기선행 링: 재생 위치를 복사 후에 발행 + 여유 8192 샘플 (3) invalidate가 호출 스레드(엔진 락 안)에서 디스크를 읽지 않음 — 읽기선행 스레드가 즉시 채움 (4) setInputsWanted: 이전 설정 보존·실패 시 복원·오류 보고, audioDeviceStopped에서 입력 수 0 (5) 입력 1..N 접두 마스크 검사(개수 아님) (6) 콜백 인라인 경로 `< 32`, 32ch 이상은 scratch + 고정 입력 포인터 배열 (7) setLiveLevels/getLiveState/setLiveSlices/setLiveRegion: 락 안에서 플레이어만 찾고 작업은 밖에서 (8) 오디오 스레드의 triggerAsyncUpdate → 원자 플래그 + UI 타이머 `reapIfNeeded` (9) 플러그인 체인 map 노드는 밖에서 만들고 락 안에서 extract/insert (10) 마이크 firstInput ≤ 31, first+num ≤ 32(sanitise·MicPanel) (11) 편집 후 재로드는 makesSound(마이크 포함), Load 제어 큐도 마이크 로드 (13) 디밴프 대상 콤보에서 마이크 제외 + 두 번째 GO 디밴프가 반복 없으면 안내 (14) FadePanel 대상 조회 findCueAnywhere, 마이크 선택 후 페이드 추가 시 대상 지정 (15) 경고 수는 전 리스트 합산.
- [x] 수용(보류) 2건: (12) 재생 중 마이크 입력 변경은 다음 시작부터(MicPanel 힌트로 안내) (15의 상세) 경고 목록 상세 행은 활성 리스트 기준(수는 전체) — 컨테이너 이름 표기는 다음 릴리스.
- [x] 테스트 3711개 통과, 마이크 큐 GO→P→P→Esc 스모크 통과. → **v0.8.1 릴리스**.

## gom QA 1차 (2026-09-02 밤) → v0.8.2
- [x] 인스펙터 "시간·루프" 탭 → "재생".
- [x] 이펙트 슬롯 "N번 이름" + `<` `>` 순서 변경(PluginChainComponent::moveSlot, 실행 취소 경로), 힌트에 직렬 처리 설명. 체인 자체는 원래 순서대로 직렬(PluginChain::process)이었음.
- [x] 창 비율: 인스펙터 높이·활성 큐 패널 너비를 분할 영역의 비율로(SplitLayout.h, 테스트 16개), 구분선(SplitDivider) 드래그로 크기 조절 + 화살표/더블클릭으로 접기, Ctrl+I 인스펙터 접기, Ctrl+L 활성 큐 접기, AppSettings에 저장. GO 버튼은 높이×1.7 고정 너비. 창 최소 높이 640.
- [x] 메뉴 글자: GoCueLookAndFeel(15pt, 단축키 글자 항목과 같은 크기).
- [x] 도움말 > 사용 설명서(Ctrl+F1): ManualWindow, 8개 탭.
- [x] 코덱스 리뷰(codex_review_qa1, 5건) 전부 반영: 종료 시 패치 편집기 창 먼저 닫기 / moveSlot이 슬롯 객체 주소까지 검증 / LookAndFeel 해제 전 dismissAllActiveMenus / 슬롯 이름 더블클릭이 에디터를 열도록 라벨 클릭 통과 / 설명서 쇼 모드 설명 수정. 추가로 마이크 큐 단축키 Ctrl+M 충돌(마스터 버스 인서트) → Ctrl+6. 테스트 3727개. → 0.8.2 릴리스.

## gom QA 2차: 디자인 (2026-09-02 밤 → 09-03) → v0.8.3
- [x] 디자인 시안 20종을 HTML로 앱 레이아웃 그대로 그려 헤드리스 크롬으로 렌더링(`scratchpad/mock/make_mocks.py`, 바탕화면 `GoCue 디자인 시안/` 21장). gom 선택 = **10 큐랩 스타일**.
- [x] 적용: `Palette` 교체(+header/button/field/selected/playingRow/fadingRow/pausedRow/cornerRadius/buttonGradient), `GoCueLookAndFeel` ColourScheme + colourId 지정 + `drawButtonBackground`(5 px, 그라데이션), GoButton 8 px 그라데이션, 상태 행 색 분리(밝은 상태색 vs 어두운 행색). 코덱스 리뷰(codex_review_theme) → 반영 → 0.8.3.
- [x] 테마 코덱스 리뷰 11건 반영(대비: 주황/초록 버튼 어두운 글자, 선택 행 밝은 테두리, 재생 행·카트 어두운 상태색 + 하단 진행 막대, 큐 색 틴트 어둡게, 그룹 띠 오버레이 위, 스크롤바·오류색, 포커스 링, 연결 모서리). 메뉴 18pt/막대 17pt(30px).

## gom QA 3차 (2026-09-03 00:00~01:20) → v0.8.3
- [x] 표 글자 16/15pt·번호 굵게·행 높이 {28,34,42}, 시간 칸 가운데 정렬(헤더 LAF 오버라이드). 두 번째 색 UI 제거. 활성 큐 글자 17/14pt.
- [x] 번호 바꾸면 자동 정렬: `CueNumbering::compare`, `CueList::placeByNumber`(마지막 작은 형제 뒤/첫 큰 형제 앞), `ProjectDocument::setCueNumber`(perform 1회), 표·인스펙터 경로, `CueInspector::finishEditing`(리스트 전환 전 커밋). 테스트 `CueOrderTests`.
- [x] 재생 탭: 파형 클릭 → `onSeekPlay` → 재생 중이면 `engine.seekToFileSeconds`(현재 pass 안, `RegionLoopSource::locationFor`+`virtualPositionFor`), 아니면 `controller.previewFrom`(startOffset을 첫 pass 가상 위치로 변환; 로드된 인스턴스도 seek). 미리듣기 버튼 제거, 확대 +/-(Ctrl+휠), 세로 크기 +/-(`zoomVertical`).
- [x] 전체 페이드 정지 기본 1초 + 톱니바퀴 메뉴(0.5~10초/직접 입력, `applyPanicSeconds`), 버튼에 시간 표시. 레벨 Alt+더블클릭 0 dB(활성 칸 모드 제외). 새 패치 큐 출력 16. 플러그인 메뉴 제조사별.
- [x] "이펙트"→"플러그인", 슬롯 번호 배지 + 화살표, 활성(초록)/비활성(주황) 토글.
- [x] 기본값: 파일 복사 켬, 자동 백업 5분, 최근 15개 보관(`BackupManager::rotate` 단순화).
- [x] **드래그 버그 근본 원인**: 드롭 대상 클래스들이 `FileDragAndDropTarget`/`DragAndDropTarget`을 private 상속 → JUCE `dynamic_cast` 실패 → 파일 드롭·행 드래그 전부 불가. public 상속으로 수정. 검증 = 실제 탐색기 창 드래그(`tools/gocue_explorerdrag.ps1`) — WinForms 시뮬레이터(`gocue_droptest.ps1`)는 프로세스 간 드롭을 못 넘겨 무효였음.
- [x] 코덱스 리뷰: QA3 9건(1~6,8,9 반영, 7 = 비활성 리스트 커서 스냅샷은 보류), 플러그인 UI 4건 반영, 최종(codex_review_final3) → 0.8.3 릴리스.
- [x] exclusive 모드 멀티 출력(gom 선택 3번): JUCE `juce_WASAPI_windows.cpp`의 채널 탐색·열기가 "하위 N비트" 마스크만 시도하던 것을 표준 스피커 마스크(5.1/7.1 등)+0까지 시도하게 패치(로컬 JUCE 커밋 + `tools/juce-patches/0001-…patch`). 이 PC의 HDMI(캡처보드)·S/PDIF는 원래 2ch라 다채널 효과는 gom 인터페이스로 확인 필요, 회귀 없음(probe: 2ch 그대로 열림). `tests/DeviceProbeTests.cpp`(GOCUE_PROBE_DEVICES=1) → **v0.8.4**.


## 최종
- [ ] 코덱스 전체 리뷰(전체 소스, 성능·스레드 안전·메모리) → 반영 → 전체 테스트·GUI 스모크 → 릴리스 → gom 보고(무엇이 바뀌었는지, 남은 한계: 장치 1개, CAF 없음, MIDI/타임코드/원격 제외).

## v0.8.5 — 업데이트 안내 (2026-09-03 새벽, gom "업데이트 된 건지 안 된 건지 헷갈림")
- [x] 창 제목에 버전("GoCue 0.8.5 - 프로젝트"), 새 버전 첫 실행 시 안내창(`announceVersionChange`, AppSettings lastRunVersion), 설치기 [Run]에서 skipifsilent 제거(자동 업데이트 후 자동 재실행) + `/NORUN=1`로 스크립트 설치는 재실행 안 함.

## v0.8.6 — 활성 큐 큰 카드 (2026-09-03 10:19)
- [x] gom 선택 시안 01: `ActiveCuesPanel` Row 124px, 이름 22pt, 남은 시간 30pt 상태색, 위치/길이, 14px 진행 막대, 왼쪽 상태 띠. 패널 기본 비율 0.27, 최소 260.

## v0.8.7 — 고품질 리샘플러 + 실시간 엔벨로프 (2026-09-03 오전)
- [x] `src/audio/HighQualityResampler.*`: r8brain(`third_party/r8brain`, MIT, 헤더 온리) 고정 비율 SRC(파일→장치). 체인 = RegionLoop → ReadAhead → (Stretch) → HQ SRC → JUCE 리샘플러(속도만, `ratioFor = stretch ? 1 : rate`). 같은 레이트면 바이패스. 시크 시 `reset()`. 테스트 `tests/ResamplerTests.cpp`(48↔44.1k 음정·레벨 유지, 바이패스).
- [x] 엔벨로프 실시간: `RegionLoopSource::setLiveEnvelope`(SpinLock swap), `CuePlayer::setLiveEnvelope`(+읽기선행 캐시 즉시 갱신), `AudioEngine::setLiveEnvelope`, `TimeLoopsPanel` LiveApply::envelope(+regionAndRate에서도 전송). 테스트 추가.
- [x] 코덱스 리뷰(codex_review_hq, 9건): 반영 = 루프 상한 제거(무진행 pull만 제한, 큰 버퍼·고속 재생 무음 방지), FIFO 크기 `getMaxOutLen`+assert, prepare에서 프라임(첫 콜백 스파이크 방지), `ReadAheadSource::invalidateCurrent`(위치 유지, 되감기 경쟁 제거), 엔벨로프 토글 `LiveApply::envelope` 명시(불필요한 시크 제거), 드래그 중 라이브 푸시 40 ms 스로틀, 큰 요청·연속성 테스트. 수용 = SRC/속도 단계에 남은 ≤512 입력 샘플 분량의 옛 엔벨로프(≤ 46 ms), seek 직후 SRC 워밍업 무음(수십 ms, 기존 읽기선행 갭과 같은 급), 스타트업 구간 impulse 테스트 없음. → 0.8.7 릴리스.

## v0.8.8 — 단일 인스턴스 (2026-09-03)
- [x] gom "세션 파일을 열면 기존 창은 닫히고 세션 창만": `moreThanOneInstanceAllowed()=false` + `anotherInstanceStarted`가 창을 앞으로 올리고 `openProjectFromCommandLine`. 검증: 두 번 실행 → 프로세스 1개, 제목이 두 번째 파일로 바뀜.

## v0.8.9 — 다채널 출력은 ASIO에서만 (2026-09-03)
- [x] gom "멀티채널 지원은 ASIO만, 다른 모드일 땐 투트랙만": `AudioEngine::enforceOutputLimit()`이 ASIO가 아닌 장치 타입(Windows Audio 공유/독점/저지연, DirectSound)의 활성 출력을 1-2로 되돌림 — `initialise()` 직후와 장치 변경 콜백(Main.cpp)마다. 저장돼 있던 8채널 WASAPI 설정도 1-2로 열림.
- [x] 오디오 출력 설정: `SelectorHost`가 장치 타입 변경 시 selector를 다시 만듦 — ASIO는 스테레오 쌍 최대 64, 그 밖은 출력 목록 숨김(min=max=64) + 안내 문구.
- [x] 테스트 `tests/OutputLimitTests.cpp`: 가짜 장치 타입(하드웨어 없음)으로 비ASIO 8채널 → 2, ASIO 8채널 → 8, 저장 상태 두 경우, 3-4 선택 후 강제 복귀, 재열기 없음, 장치 없음.
- [x] 코덱스 1차 리뷰 반영: ①타입 전환 직후 트림 전 블록이 3-8로 나가는 틈 → `audioDeviceAboutToStart`가 타입명으로 `outputChannelLimit` 설정, 콜백이 한도 밖 채널 clear + `numDeviceOutputs`도 한도로. ②비ASIO가 64채널로 먼저 열려야 트림됨(거부 장치는 실패) → `normaliseDeviceState()`가 저장 XML 비ASIO 타입에 `audioDeviceOutChans="11"` → 첫 open부터 1-2, 그래도 실패하면 기본 장치명으로 1-2 재시도. ③재열기 오류 무시 → `enforceOutputLimit()` String 반환, initialise 합침, Main 콜백 AlertWindow. ④재생 중 재-prepare가 SRC 재프라임으로 수십 ms 건너뜀 → `AudioEngine::prepare` 같은 포맷이면 플레이어/체인 prepare 생략. ⑤JUCE `numOutputChansNeeded`가 트림으로 2가 되어 ASIO 복귀 시 2채널 → ASIO+useDefault면 전체 채널로 넓힘. 보류: fallback 후 낡은 XML(인터페이스 잠깐 빠졌을 때 ASIO 선택을 지키는 쪽이 낫다고 판단, `treatAsChosenDevice=false`). 테스트 16개(open 이력·거부 모드·타입 전환·64채널 콜백·XML 라운드트립).
- [x] 코덱스 2차 리뷰 반영: ①JUCE는 활성 채널 포인터만 0번부터 압축해 넘기므로 비ASIO가 물리 3-4로만 열린 순간엔 1-2로 매핑 불가 → `outputMaskOutOfRange`면 콜백이 전부 무음(큐는 계속 진행), 트림 후 정상. ②타입 전환의 넓은 open 실패는 JUCE가 오류를 버려 장치가 조용히 사라짐 → `enforceOutputLimit`가 타입이 바뀌었는데 장치가 없으면 기본 장치를 1-2로 재시도(같은 타입에서 '없음' 선택은 존중). ③ASIO 넓히기 실패 시 원래 채널로 롤백. ④타입 없는 XML은 장치명으로 타입 판별. 테스트 19개.

## 베타 준비 — 쿠팡 바로가기 설치 옵션 + 피드백 메뉴 (2026-09-03, 미배포)
- [x] gom 수익화 안: 설치 시 쿠팡 파트너스 바로가기(반디집식 체크박스). 앱 내 광고·광고 제거 9,900원은 반대해서 보류.
- [x] 리다이렉트 페이지: GitHub Pages `gh-pages` 브랜치 → https://dnakrhs2-crypto.github.io/gocue/coupang/ (meta refresh + JS → 파트너스 간편링크 `link.coupang.com/a/gJIOd2FuJU`, 대가성 문구 표시). letsplax 도메인은 회사 주소라 쓰지 않음(gom).
- [x] `installer/GoCue.iss`: [Tasks] `coupang`(기본 체크, GroupDescription = 대가성 문구 + 삭제 방법), `coupang.ico`(쿠팡 로고, gom "그대로 써도 됨") 설치, [Code]가 `{userdesktop}\쿠팡.url` 작성 — 대화형 설치이거나 `/TASKS=coupang`일 때만(자동 업데이트 `/SILENT`는 지운 바로가기를 되살리지 않음), 제거 시 삭제. 문자열은 `installer/GoCue.messages.iss`(UTF-8 BOM, `#include`) — GoCue.iss 자체는 ASCII 유지.
- [x] 검증: ISCC 컴파일 OK, `/VERYSILENT /TASKS=coupang` 설치 → `쿠팡.url`(URL·IconFile·IconIndex) 생성, 제거 → 삭제, 0.8.9 재설치(/TASKS 없음) → 생성 안 됨.
- [x] 도움말 > "커뮤니티" 메뉴 = 카카오톡 오픈채팅 https://open.kakao.com/o/pST4IRLi (`src/app/Links.h`의 `feedbackChat`; 사이트 latest.json에도 같은 링크).
- [ ] 코드 서명(인증서 필요), 베타 버전 0.9.0 배포.

## v0.9.0 — 이름 변경 GoCue → Enqueue(앤큐), 회사 곰튀김 (2026-09-03, 미배포)
- [x] gom 결정: 프로그램 이름 Enqueue(한글 앤큐), 회사/브랜드 곰튀김, 사이트 도메인 곰튀김.com(구매 예정), 아이콘은 기존 GO 유지.
- [x] CMake project/타깃 Enqueue(EnqueueTests), 창 제목·정보 창·메뉴·설명서 문구, ProjectDocument 제목, WinSparkle 회사명 Gomtwigim.
- [x] 설정 폴더 `%APPDATA%\Enqueue` + 첫 실행 이전(`src/app/SettingsMigration.cpp`, 테스트 3개). 프로젝트 확장자 `.enqueue`, 열기는 `.enqueue;.gocue`, 옛 파일 저장 시 확장자 유지, 백업은 프로젝트 확장자 따라감.
- [x] 설치: `installer/Enqueue.iss`(UTF-8 BOM, 게시자 곰튀김, AppId 동일 → 제자리 업그레이드, GoCue.exe·옛 바로가기 삭제, .enqueue/.gocue 연결, 옛 ProgId 삭제). 이 PC에서 GoCue 0.8.9 → Enqueue 0.9.0 조용한 업그레이드 확인.
- [x] release.py 이름/경로, 사이트 표기, README, 아이콘 생성기 시안 10종(`바탕화면\앤큐 아이콘 시안`).
- [x] 코덱스 리뷰 10건 중 9건 반영(보류: 옛 '모든 사용자' 설치). 저장소 이름 enqueue로 변경, 도메인 곰튀김.com 연결(HTTPS), 사이트에 카카오톡 오픈채팅 큰 버튼. **0.9.0 배포 완료(2026-09-03 15:15)**.

## v0.9.1 — 활성 큐 이름 찌그러짐, 초록 꽉 찬 아이콘, 설치 체크박스 기본값 (2026-09-03)
- [x] `ActiveCuesPanel` 카드: 이름이 첫 줄 전체(가로 압축 없음, 긴 이름은 말줄임), 일시정지 버튼은 시간 줄 왼쪽. 아이콘 `make_icon.py` go 변형 = 테두리 없이 초록 타일. 설치: desktopicon 기본 체크, 쿠팡 문구 gom 지정.

## v0.9.2 — 글씨 키우기, 장치에 없는 출력 열 죽이기 (2026-09-03)
- [x] CueTable 18/17pt·행 {32,38,46}, CueInspector 캡션 15·힌트 13-14·행 26·탭 28, TimeLoops/PluginChain/ContainerTabs/CurveEditor 동반, LevelMatrix 14pt·셀 24. `minInspectorHeight` 236.
- [x] LevelMatrixComponent: `outputConnected` false 열 = 헤더 취소선 + 칸 12% 알파(마지막에 적용). FadePanel도 계산. 장치 변경(ChangeListener)·패치 변경(`ProjectDocument::onPatchesChanged`) → `CueInspector::refreshDeviceDependent()`(AsyncUpdater로 합쳐 레벨·페이드 패널만). `audioDeviceStopped` → numDeviceOutputs 2.
- [x] 코덱스 1차 9건·2차 2건 반영. 배포 0.9.2.

## v0.9.3 — 무한 루프/재생 횟수 실시간 반영 (2026-09-03)
- [x] gom 버그: 재생 중 무한 루프 토글이 정지 후에야 적용됨 → `CuePlayer::setLivePlayCount`(locate → setPlayCount → virtualPositionFor → jumpTo), `AudioEngine::setLivePlayCount`(락 밖 적용), `TimeLoopsPanel LiveApply::loop`. 코덱스 6건 반영(실행 취소 복원 경로, 편집 대상 큐로 라이브 적용, 스트레치 프리롤 `getPreRollSamples`, 락 밖 적용, 표시 갱신 제거, 테스트 강화). 테스트 `tests/LiveLoopTests.cpp` 4케이스.

## v0.9.4 — 공연 안전성 감사 반영 (2026-09-03)
- [x] gom 요청 "안전성 문제될 거 없는지 코덱스랑 검토" → 코덱스 읽기 전용 감사 4건(오디오 A / 데이터 B / UI·운용 C / 플랫폼 D, 각 ~40분) + 내 검토. 치명·높음 항목을 세 묶음으로 수정, 코덱스 재리뷰 후 배포.
- [x] 엔진: 마스터 체인 뒤 최종 출력 게이트(`applyOutputGate`, 하드 정지 5 ms·페이드 정지 후 200 ms 램프, 다음 play/resume에서 열림, 완전히 닫혔으면 즉시), `stopAll()`이 락 전에 `hardPanicRequested`를 올려 콜백이 락 없이 즉시 무음, `PluginChain::process` try/catch → `faulted` 슬롯 우회 + `resetProcessing()`, `CuePlayer` 레벨 전달 `gainsLock`(SpinLock, 오디오 스레드 try-lock), `setLiveGainDb` 무할당, 종료 뒤 라이브 편집 시 `endedNaturally` 복구, 장치 없으면 `play()` 거부(`deviceExpected/deviceRunning`), 256개 한도·reserve, mfplat/mfreadwrite 지연 로드+LoadLibrary 프로브. `tests/PanicGateTests.cpp`.
- [x] 운용: `CueController` 패닉 페이드 중 모든 시작 거부(`panicLatchUntil`, 하드 정지·hardStopAll이 해제), 핫키 한 키 한 큐, `src/model/Hotkeys.h` 예약 키 공유 + 로더가 예약·중복 핫키 제거(경고), 쇼 모드 잠금 확대(새/열기/프로젝트 설정/오디오 설정/패치/플러그인/업데이트/탭 전환/스크럽), 재생 중·쇼 모드 종료·교체 확인, `MainComponent::OperationalKeys`(KeyListener를 다른 TopLevelWindow에 1초마다 부착: Esc=패닉, Space=GO), `installEscapePolicy`(인스펙터 TextEditor Esc), 자동 백업은 한가할 때, 저장 전 finishEditing, 안전 모드(Shift/--safe-mode: 장치 상태·플러그인 생략, `PluginHost::setSafeMode`), 폴백 장치 경고, 업데이트 후 프로젝트 재오픈(`AppSettings::setReopenProjectAfterUpdate`). `tests/HotkeyLoadTests.cpp` + CueControllerTests 추가.
- [x] 데이터: `ProjectSerializer::save` = `<name>.saving~`에 쓰고 상태·크기·역파싱 검증 후 `replaceFileIn`, 빈 파일·비프로젝트 JSON 거부, 상대 경로(fileRelative) 우선, `Updater` 키 미승인 시 init 안 함, 설치 `MinVersion=10.0`, release.py 더티 트리·태그 충돌 게이트(`--allow-dirty`). `tests/SafeSaveTests.cpp`. 전체 4046 통과.
- [ ] 보류(설계 변경 필요, 메모리에 기록): 워커 스레드 프리로드, 오디오 클록 기반 페이드 큐, 플러그인 프로세스 격리, 무제목 프로젝트 복구 저널, NAS 동시 편집, PDC, 루프 크로스페이드, 코드 서명(인증서), 옛 '모든 사용자' 설치.
- [x] 코덱스 재리뷰 16건 반영(2차): 출력 게이트 램프를 정확히 rampSamples로(이전엔 매 블록 전체 길이로 나눠 0에 못 닿음), 체인 리셋은 AsyncUpdater로 메시지 스레드에서, 패닉 잠금은 `trigger()`(카트·프리뷰 포함)와 재개에도, 플러그인 예외 시 스크래치의 드라이 입력 복원, 게이트는 확정된 시작에만 열림(load 캡 포함), 자연 종료 플레이어의 라이브 편집 복구(`hasPendingLiveEdit`), Esc는 `WH_KEYBOARD` 스레드 훅(Main.cpp, 네이티브 플러그인 창 포함, 키 소비 안 함, 메인 창 매핑과 50 ms 중복 제거)·에지 트리거, Space는 눌린 동안 1회, 인스펙터 `finishEditing` 동기 커밋, 조용한 실행(안전 모드·업데이트 복귀·폴백 장치·경고 있는 파일은 자동 시작 안 함), 쇼 모드 Ctrl+PageUp/Down·업데이트 잠금, 로더 강화(큐 배열 필수·꼬리 데이터·미래 버전 거부), `TemporaryFile` 저장+바이트 비교, `MinVersion=10.0.14393`(exe가 GetDpiForWindow 직접 임포트 — PE 임포트 테이블로 확인), release.py 태그 생성/검증(`--verify-tag`). 콤보박스 긴 이름 말줄임(gom: 페이드 대상 이름 찌그러짐).
- [x] gom 요청 4건(2026-09-03 저녁): ①페이드 큐 → 페이드 인(Ctrl+7)/페이드 아웃(Ctrl+Shift+7): `FadeMode {in,out,custom}`, 러너가 모드로 목표 결정(`applyFadeMode`), 페이드 인은 대상이 안 돌면 바닥 레벨(`minLevelDb`)로 시작(`PlayOptions::hasStartGain` → `CuePlayer::setInitialGainDb`) 후 원래 레벨까지, 페이드 아웃은 무음 후 정지. 새 `FadeInOutPanel`(대상·종류·시간), 탭은 기본/페이드/커브(트리거·파라미터 제거). 옛 파일은 custom으로 그대로. ②우클릭 메뉴 맨 위에 큐 추가 항목(빈 곳 우클릭도). ③그룹 행 가운데에 파일 드롭 → 그룹 마지막 자식(`CueList::addIntoGroup`, 행 하이라이트, 접힌 그룹 펼침). ④아밍+건너뛰기 → "비활성화" 스위치 하나(비활성 = GO·시퀀스가 지나침). 테스트 4113개.
- [x] 코덱스 3차 리뷰 19건 반영: 페이드 큐 미리듣기 거부(실제 대상 건드림), 패닉 리셋을 AsyncUpdater 대신 UI 타이머 폴링(`reapIfNeeded`)으로 + 정지 중인 옛 인스턴스 있으면 재시도 + `requestPanicFadeOut`(패닉 페이드 뒤 플러그인 테일 생략) + `resetInProgress` 동안 콜백 무음, `panicFromAnywhere`는 락 없는 `getNumPlayingLockFree`로 판단(아무것도 없으면 `silenceOutput`), Windows에선 키보드 훅이 유일한 Esc 경로(명령 단축키·인스펙터·표 편집기의 Esc 패닉 제거), `trigger()`가 비활성 큐 거부(카트·미리듣기·예약 시작·제어 큐), 컨트롤 start도 대상 armed 확인, 활성 큐 패널 재개는 `resumeCue`(패닉 잠금 적용), 하드 패닉 잠금 0.1 s, 로더 꼬리 데이터 스캐너(`hasTrailingData`)+app 표식 검사, 페이드 시간 편집기 중복 커밋/포커스 덮어쓰기 방지, 페이드 인 대상 상태 분기(일시정지=바닥에서 재개, 정지 중=새 인스턴스, 재생 중=현재 레벨에서)+대상 키 지정 바닥 요청, 페이드 인 자동 시작은 되돌리기 시 정지, 파일 버전 6, 게이트 예약 닫기 정확한 샘플 위치(`applyGateRange`), 포그라운드 잃으면 Space/Esc/GO 키 상태 초기화, 스냅샷 reserve(maxPlayers), 카트엔 그룹 없음(명령 비활성·카트 전환 시 그룹 해제·드롭 parent null), 드롭 위치 재계산. release.py: `--publish`는 `--allow-dirty/--skip-build/--skip-tests` 거부, 태그는 annotated만.
- [x] 코덱스 4차 리뷰 16건 중 15건 반영(보류 #9 메뉴 즉시 정지의 잠금 해제): 패닉 페이드 뒤 테일 종료, `settlePendingChainReset()`을 시작·재개 전에, 로드 인스턴스는 리셋 소유자 아님, 게이트 예약을 락 전에 발행, `committedRunning`+`mayBePlaying()`, 아무것도 없을 때 두 번째 Esc는 `stopAll()`, 페이드 미리듣기는 `auditionNow` 기준, 정지 중 대상엔 새 인스턴스, 정지 뒤 라이브 편집 부활 금지, 버전 6은 app 표식 필수, 알 수 없는 페이드 모드 경고, `CueList::flattenGroups()`(카트 변환·adopt·활성 카트 구조 변경).
- [x] 코덱스 5차 리뷰(앤큐 4건 + LiveMix 14건) 18건 중 17건 반영(보류 #18 비활성 리스트 커서 되돌리기): **앤큐** ①패닉 리셋 세대(`resetGeneration`/`resetAcknowledged`, `isResetOutstanding()`) — 리셋이 끝나기 전엔 `play()`/`resume()` 거부(메시지)·게이트 안 열림, 장치가 멈춰 있으면 메시지 스레드가 직접 리셋 ②잔향 중 소프트 패닉은 체인 뒤 선형 페이드(`tailFadeSamplesLeft`, ★JUCE `AudioBuffer::clear()`가 `isClear`를 세워 같은 뷰 객체의 `applyGainRamp`가 통째로 건너뛰어짐 → `fullBuffer` 채널별 적용) ③버전 6 파일의 `app:"GoCue"` 거부. **LiveMix**는 계획 파일 `2026-09-04-livemix.md` 참고. 테스트 4336.
- [x] 코덱스 6차 리뷰(5차 수정분) 14건 중 13건 반영(보류 #14=#18): `PluginChain` 소멸자는 chainChanged를 알리지 않음(퇴역 체인이 문서를 더럽히고 해제된 포인터로 서랍을 갱신하던 치명 버그), 장치 멈춘 채 패닉이면 플레이어를 `abandon()`하고 리셋, 패닉 페이드가 선언된 테일보다 우선·소스가 페이드 중 끝나도 체인 뒤 페이드 계속(`panicFadeSamplesLeft`), 열 때 시작은 리셋 완료를 기다림(`tryPendingStartOnOpen`, 3 s), LiveMix `openDevice`/`setBufferSize`(ASIO 타입·롤백·콜백, 안전 모드에서도 장치 열림), `pollPluginEdits()`로 노브 변경 즉시 반영·저장이 플래그 정리, 원자 dirty, WebDAV 취소 경쟁·startThread 실패·주소 검증 강화, 세션 파서 경고 유지, ChainDrawer revision. 테스트 4372.
- [x] 코덱스 7차 리뷰(6차 수정분) 9건 중 8건 반영(보류 #9=#18): 체인 서랍은 닫히거나 주인이 사라지면 체인을 비우고 rebind마다 revision 증가, 저장은 캡처 전에 플러그인 편집을 흡수하고 쓰기 뒤 다시 폴링(실패 시 dirty 유지), 자연 종료 뒤 라이브 편집이 걸린 플레이어도 정지·패닉 대상(`abandon()`은 편집 취소), 장치 멈춤 정리는 pending 여부와 무관·정지 요청된 로드 인스턴스 포함, 소프트 패닉이 스트리밍·마이크·일시정지 큐의 인서트도 끊지 않고 체인 뒤로 페이드(게이트 램프 200 ms), 열 때 시작은 장치·잠금까지 기다리고 실제 시작했을 때만 상태 표시, 첫 장치 열기 실패 시 반쯤 열린 장치 닫음, IPv6 주소 파싱. 테스트 4393.
