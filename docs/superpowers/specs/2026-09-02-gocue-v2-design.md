# GoCue v2 설계 — 큐랩 오디오 기능 카피 (2026-09-02)

gom 지시(2026-09-02): "큐랩 오디오 기능 후보 64개 **다 하고** 오디오 파일 드래그도 해. 원격 제어(64번)는 빼. 하고 나서 코덱스 검증해."
후보 목록: `2026-09-02-qlab-audio-feature-candidates.md`. 이 문서는 그 63개를 어떻게 GoCue 구조에 얹을지 정한다.

## 0. 범위 확정
- 포함: 1~63번 전부. 38번은 **벽시계 트리거만** (MIDI·타임코드 트리거는 외부 장치 입력 = 원격 제어 계열이라 제외, gom이 원하면 추가).
- 제외: 64번(OSC·MIDI/MSC·Stream Deck·원격 앱·협업).
- 추가: 오디오 파일 드래그앤드롭을 창 어디에나(큐 목록=끼워넣기, 인스펙터 파일칸=교체, 그 외=맨 뒤 추가). 현재는 큐 테이블 영역에만 반응한다.
- 검증: 단계마다 단위 테스트 + 빌드 + GUI 스모크, 단계 끝에 코덱스 read-only 리뷰, 마지막에 전체 리뷰.
- 오디오 장치는 **한 번에 하나**(AudioDeviceManager 1개). 패치는 그 장치의 채널들 위에 라우팅을 정의한다. 두 장치 동시 출력은 범위 밖(ASIO도 원래 한 장치).
- CAF 포맷은 윈도에서 디코더가 없어 제외. AAC/M4A/MP4/ALAC은 Media Foundation 리더를 직접 만들어 지원.

## 1. 모델 (src/model)

### 1.1 큐 종류
```cpp
enum class CueType { audio, fade, group, start, stop, pause, load, reset, goTo, wait, memo, arm, disarm, devamp, target };
enum class ContinueMode { none, autoContinue, autoFollow };
enum class SecondTriggerAction { nothing, panic, stop, hardStop, hardStopRestart, devamp };
enum class FadeStopScope { peers, list, all };
enum class GroupMode { timeline, playlist, startFirstEnter, startFirst, startRandom };
enum class CurveShape { sCurve, custom, parametric, linear };
enum class AudioDomain { slider, decibel, linear };
```
`Cue`는 하나의 구조체에 공통 필드 + 종류별 하위 구조체(`AudioCueData audio`, `FadeCueData fade`, `GroupCueData group`, 대상 큐 필드)를 모두 가진다. 종류에 안 맞는 필드는 무시·저장 안 함. `std::vector<Cue> children`은 C++17 불완전 타입 벡터로 허용.

### 1.2 공통 필드
번호(자유 텍스트, 워크스페이스 안 유일, 빈 값 허용) · 이름 · 메모 · 색(0~20) · 두 번째 색 · 깃발 · 아밍 · 비활성 시 건너뛰기 · 자동 로드 · 프리웨이트(초) · 포스트웨이트(초) · 진행 모드 · 핫키(`juce::KeyPress` 문자열) · 벽시계 트리거{사용, 시:분:초, 요일 비트} · 2차 트리거 동작 · 키 뗄 때 2차 트리거 · 시작 시 다른 큐 페이드&정지{사용, 초, 범위} · 덕/부스트{사용, dB, 초} · 대상 큐 id(트랜스포트·goto·arm·devamp·target·fade) · 로드 시각(load 큐, 음수=끝에서) · 대기 시간(wait 큐) · devamp{슬라이스 기준, 다음 큐 시작, 대상 정지}.

### 1.3 오디오 큐 데이터
파일(절대·상대) · 시작/끝(초, 끝 -1=파일 끝) · 재생 횟수(1, 0=무한 아님; `infiniteLoop` 별도) · 슬라이스 `{초, 재생 횟수(0=건너뜀, -1=무한)}` · 속도(0.03~33) · 피치 유지 · 엔벨로프{사용, 직선 여부, 시작/끝 잠금, 점들 `{초, 0~1}`} · 정지 페이드 ms(`fadeOutMs` 유지, F키·패닉이 아닌 개별 정지용, 0=워크스페이스 패닉 시간) · 레벨 `LevelMatrix` · 트림 `TrimLevels` · 패치 id · VST3 인서트(기존) · 캐시: 길이·채널 수·파일 없음.
v1의 `fadeInMs`는 엔벨로프 점 4개로 변환, `gainDb`는 `levels.mainDb`로 옮긴다.

### 1.4 레벨·트림·커브
```cpp
struct LevelMatrix {           // 행=파일 채널(N), 열=큐 출력(K). dB, silentDb(-inf) 이하=무음
    double mainDb = 0;  std::vector<double> inputDb, outputDb;  std::vector<std::vector<double>> crosspointDb;
    int mainGang = 0;   std::vector<int> inputGang, outputGang;  std::vector<std::vector<int>> crosspointGang;   // 0=없음, 1~8
    bool mainActive = true; std::vector<char> inputActive, outputActive; std::vector<std::vector<char>> crosspointActive; // 페이드 큐용
    static constexpr double silentDb = -120.0;   // 직렬화: "-inf"
    void resize (int numInputs, int numOutputs);   // 기본: 대각선 0 dB, 모노 파일은 두 출력 모두 0 dB
    float gainFor (int in, int out) const;         // main+input+crosspoint+output 합(dB→linear), 어느 하나 무음이면 0
};
struct TrimLevels { double mainDb = 0; std::vector<double> outputDb; };
struct CurvePoint { double x, y; };            // 0~1
struct FadeCurve { CurveShape shape = sCurve; double intensity = 1.0; std::vector<CurvePoint> points; bool mirror = false; AudioDomain domain = slider;
    double completion (double t) const;        // 0~1 → 0~1
    double interpolateDb (double fromDb, double toDb, double t, double minDb) const; };
```
슬라이더 도메인은 진폭의 세제곱근 공간에서 보간(콘솔 페이더 근사), 데시벨 도메인은 dB 선형, 리니어는 진폭 선형. 이퀄파워=파라메트릭+리니어, 이퀄게인=직선+리니어(문서 그대로).

### 1.5 패치·설정·컨테이너
```cpp
struct AudioPatch { juce::Uuid id; juce::String name; int numCueOutputs = 2; juce::StringArray cueOutputNames;
    std::vector<std::vector<double>> routingDb;             // K × M(장치 출력), 기본 대각선 0 dB
    double mainDb = 0;  std::vector<std::vector<PluginSlotState>> cueOutputInserts, deviceOutputInserts;
    std::vector<char> cueOutputStereoWithNext;                // 인서트가 2ch를 받도록 짝 묶기 };
struct WorkspaceSettings { double doubleGoSeconds = 0; bool requireKeyUp = false; double panicSeconds = 3;
    bool autoNumber = true; double numberIncrement = 1; bool autoLoadNewCues = false; bool lockPlayheadToSelection = true;
    bool startOnOpen = false; juce::String startOnOpenCue; bool startOnClose = false; juce::String startOnCloseCue;
    double maxLevelDb = 12, minLevelDb = -60;  bool copyFilesIntoProject = false;
    bool autoBackup = true; int backupIntervalSeconds = 60; bool backupBeforeSave = true; bool rotateBackups = true;
    enum class Audition { unchanged, none, alternatePatch } audition = unchanged; juce::Uuid auditionPatchId; bool alwaysAudition = false;
    int rowSize = 1; int cartButtonSize = 1; bool activeCuesNewestFirst = false;
    std::map<CueType, Cue> templates; };
struct CueContainer { juce::Uuid id; juce::String name; bool isCart = false; int cartRows = 4, cartCols = 4;
    std::vector<Cue> cues; bool collapsedByDefault = false; };
struct Project { juce::String name; std::vector<CueContainer> lists; std::vector<AudioPatch> patches; WorkspaceSettings settings;
    std::vector<PluginSlotState> masterPlugins; };
```
`CueList`(라이브 모델)는 활성 컨테이너의 트리 편집 façade: id 기반 API(`findById`, `insert(cue, parentId, index)`, `removeById`, `moveById`, `visit`, `flattenVisible(collapsed)`), 다중 선택(`std::vector<juce::Uuid>`) + 플레이헤드(Uuid). 기존 index API는 평탄화 인덱스 위에서 유지해 테스트·UI 전환을 단계적으로 한다.

### 1.6 실행 취소
`ProjectHistory`: `Project` 스냅샷 스택(최대 200). `ProjectDocument::perform(name, mutator)`가 편집 전 스냅샷을 쌓고 적용. 같은 이름+같은 큐 편집이 700 ms 안에 이어지면 합친다(슬라이더 드래그). 플러그인 내부 파라미터 변경은 되돌리지 않는다(큐랩도 안 함). `juce::String`이 COW라 스냅샷 복사는 싸다.

### 1.7 파일 형식 v2
`version: 2`. `lists[]`(컨테이너), `patches[]`, `settings{}`, `master{}`. v1(`cues[]`)은 메인 리스트 하나로 이관 + 기본 패치 생성 + fadeIn→엔벨로프, gainDb→mainDb. dB 값은 숫자 또는 `"-inf"`. 모르는 필드 무시, 더 높은 버전은 경고만.

## 2. 오디오 엔진 (src/audio)

### 2.1 신호 경로 (큐 하나)
파일(N ch) → `RegionLoopSource`(트림·슬라이스·루프·devamp 경계·**엔벨로프**를 파일 시간축에서 적용) → `BufferingAudioSource`(디스크 선독) → 속도 단계(`ResamplingAudioSource` 비율 = 파일SR×rate/장치SR; 피치 유지 시 `signalsmith::stretch` 시간 늘림) → 정지/패닉/덕 `FadeEnvelope`(장치 시간축) → 큐 VST3 체인(N ch 시도, 실패 시 앞 2ch) → 큐 레벨 매트릭스(N→K) + 트림 → 패치 버스(K).
패치: 큐 출력 인서트(K) → 라우팅 매트릭스(K→M) + 패치 메인 → 장치 출력 인서트(M) → (기존) 마스터 체인은 장치 출력 1-2 위 스테레오 인서트로 유지 → 장치.

### 2.2 엔진 API 추가 (메시지 스레드)
`play(cue, PlayOptions{startSeconds, patchOverride, isPreview})`, `load(cue, startSeconds)`(준비만, 상태 loaded), `pause/resume(id)`, `pauseAll/resumeAll`, `hardStop(id)`, `panic(id, seconds)`, `panicAll(seconds)`, `setLiveLevels(id, LevelMatrix)`, `setLiveRate(id, rate)`, `setDuckDb(id, db, seconds)`, `devamp(id, bySlice)`, `getPlayingCues()`에 상태(playing/paused/loaded/fadingOut/tail)·위치·유효 길이·반복 회차 포함, `setPatches(...)`, `getNumDeviceOutputs()`.
레벨 변경은 플레이어가 10 ms 램프로 따라가 지퍼 노이즈를 막는다. 장치는 0 in / M out(마이크 큐가 있을 때만 입력 열기).

### 2.3 Media Foundation 리더
`MediaFoundationAudioFormat`(AAC/M4A/MP4/ALAC/WMA, `IMFSourceReader` → PCM float). `.mp3`는 JUCE 내장 유지. `registerBasicFormats()` 뒤 추가 등록.

## 3. 컨트롤러 (src/app)
- `CueController`: GO/미리듣기/오디션, 큐 시작(아밍·건너뛰기·프리웨이트·자동 진행·자동 팔로우·페이드&정지 타인·덕·2차 트리거·그룹 모드·트랜스포트·wait·goto·devamp·페이드 큐·load), 패닉/하드 정지/일시정지/재개/리셋, 더블 GO 가드, 키업 가드, 핫키, 벽시계, 활성 큐 목록, 페이드 되돌리기, 열 때/닫을 때 큐.
- `Scheduler`: 1 ms `juce::Timer` + `Time::getMillisecondCounterHiRes()`로 예약 동작 실행(프리웨이트·포스트웨이트·자동 팔로우·wait·벽시계). 포스트웨이트 0의 자동 계속은 같은 GO 안에서 동기 시작(샘플 정확).
- `FadeRunner`: 실행 중 페이드 큐들을 100 Hz로 진행(레벨·속도·VST 파라미터). 페이드 전 상태를 기억해 되돌리기 제공.
- `BackupManager`: 저장 전 백업, 주기 백업, 회전(최근 20개/시간별/일별).

## 4. UI (src/ui)
- `WaveformView`: `AudioThumbnail` + 트림 핸들(회색 삼각) + 엔벨로프(노란 선·점) + 슬라이스(초록 마커·횟수) + 재생 위치 + 줌(버튼·Alt+휠) + 우클릭 메뉴 + 키(Shift+I/O, M, 화살표·Alt·Shift 조합).
- `CueInspector` → 종류별 탭: 기본 / 트리거 / (오디오) 시간·루프 / 레벨 / 트림 / 이펙트, (페이드) 레벨 / 커브 / 이펙트, (그룹) 모드, (대상 큐) 대상, (devamp) 설정.
- `CueListView`(기존 CueTable 대체): 트리 평탄화 표(상태·번호·이름·대상·프리웨이트·길이·포스트웨이트·진행), 다중 선택, 내부 드래그 이동, 파일 드롭, 접기, 빠른 편집 키(N Q O T E D W C), 색, 깃발, 빗금, 플레이헤드, 진행바.
- `CueCartView`(그리드), `ContainerTabs`(리스트/카트 탭), `ActiveCuesPanel`, `SidebarButtons`(리셋·전체 일시정지·전체 재개·패닉), `Footer`(편집/쇼 모드·큐 수·경고), `NotesField`.
- 재사용 컴포넌트: `LevelMatrixComponent`, `CurveEditor`.
- 다이얼로그: 워크스페이스 설정(일반·파일·표시·오디오·오디션·큐 템플릿), 패치 편집기, 재번호, 큐 속성 붙여넣기, 시간으로 로드, 찾기, 핫키 캡처, 경고 창, 파일 다시 찾기.
- 드래그앤드롭: `MainComponent`가 `FileDragAndDropTarget`(맨 뒤 추가), 목록은 위치 삽입, 인스펙터 파일칸은 교체.

## 5. 키 (큐랩 기본값을 윈도식으로)
Space GO · V 미리듣기 · Alt+Space 오디션 · Alt+V 오디션 미리듣기 · L 로드 · Ctrl+T 시간으로 로드 · S 선택 패닉(두 번=하드) · Esc 전체 패닉(두 번=하드) · F 선택 페이드 정지(GoCue 기존, 유지) · Shift+F 전체 페이드 · P 일시정지/재개 · [ 전체 일시정지 · ] 전체 재개 · N Q O T E D W C 빠른 편집 · Ctrl+Z / Ctrl+Y 실행 취소/다시 · Ctrl+R 재번호 · Ctrl+D 번호 삭제(기존 복제는 Ctrl+Shift+D로 이동) · Ctrl+F 찾기 · Ctrl+Shift+V 속성 붙여넣기 · Ctrl+I 인스펙터 · Ctrl+L 사이드바 · Ctrl+B 경고 · Shift+↑↓ 플레이헤드 이동 · = / - 시퀀스 이동 · Ctrl+Shift+[ / ] 편집/쇼 모드 · > < 그룹 펼침/접기 · Shift+I/O 트림 · M 슬라이스.

## 6. 단계 (각 단계 = 테스트 통과 + 빌드 + 스모크 + 릴리스 + 코덱스 리뷰)
1. v0.2.0 — 실행 취소, 파형·트리밍·엔벨로프·루프·미리듣기·우클릭(1~4·7·8), 드래그 전면, 2차 트리거·더블 GO·키업(33·34), 패닉/S/F 정리(39·40), 일시정지(41), 리셋(44), 자동 백업(62), 설정 창(일반·파일).
2. v0.3.0 — 기본 속성·번호·메모·색·깃발·아밍·빠른 편집(26·29~32), 웨이트·진행 모드·시퀀스(27·28), 스케줄러, 페이드&정지 타인(36), 덕(37), 벽시계(38), 로드·시간 로드(42), 활성 큐 패널(43), 쇼 모드(45), 플레이헤드 분리(46), 열/닫을 때 큐(48), 경고·다시 찾기(49), 상태 아이콘(50), 여러 리스트(55), 속성 붙여넣기(57), 다중 선택(58), 템플릿(59), 찾기(60), 행 크기(63), 핫키(35).
3. v0.4.0 — 매트릭스·겡·트림(9~12), 패치 편집기·출력 인서트·레벨 한계(13~15), 멀티채널·MF 포맷(16), 오디션(47).
4. v0.5.0 — 페이드 큐 전부(18~25), 커브 에디터 공유(20↔3).
5. v0.6.0 — 슬라이스(5), 속도·피치(6), devamp(53), 속도 페이드(22).
6. v0.7.0 — 그룹(51), 카트(52), 트랜스포트·goto·wait·memo·arm·target(54), 시퀀스 녹음(61).
7. v0.8.0 — 마이크 큐(17).
8. 최종 코덱스 전체 리뷰 → 수정 → 릴리스.
