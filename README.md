# GoCue

Windows용 오디오 큐 플레이어. QLab의 오디오 기능을 단계적으로 옮기고 있다 (진행 계획: `docs/superpowers/plans/2026-09-02-gocue-qlab-features.md`).

- 출력 장치: ASIO / WASAPI (`juce::AudioDeviceManager`, 메뉴 `오디오 > 오디오 출력 설정`)
- 재생 포맷: WAV / AIFF / FLAC / MP3 / OGG
- 큐마다: 파형 위 **트림(시작/끝)**, **재생 횟수 / 무한 루프**, **속도**, **통합 페이드 엔벨로프**(파형 위에 점을 찍어 그리는 페이드), 정지 페이드(ms), 게인(dB, 재생 중에도 즉시 반영), 2차 트리거 규칙
- 여러 큐 동시 재생(믹서), 큐마다 독립 VST3 인서트 체인 + 마스터 버스 체인, 플러그인 에디터 창, 상태 저장/복원
- 일시정지/재개, 전체 페이드 정지(기본 2초), 더블 GO 방지, 실행 취소/다시 실행(200단계)
- **큐 리스트**(QLab식): 상태 아이콘 · 번호 · 이름 · 파일 · 프리웨이트 · 길이 · 포스트웨이트 · 진행 열, 큐 색/두 번째 색, 깃발, 아밍(비활성이면 건너뛰기), 자동 로드, 메모. 다중 선택·드래그 순서 변경·셀 바로 편집·우클릭 메뉴·행 크기
- **진행(시퀀스)**: 프리웨이트/포스트웨이트, 자동 계속 / 자동 팔로우, 플레이헤드(선택과 분리 또는 잠금). **트리거**: 큐 핫키, 시간(벽시계) 트리거, 시작 시 다른 큐 페이드 정지, 덕(-dB)
- 로드(L) / 시간으로 로드(Ctrl+T), 활성 큐 패널(Ctrl+L: 일시정지·스크럽·페이드 정지), 쇼 모드(편집 잠금), 경고 목록 + 없어진 파일 다시 찾기
- 큐 복사/잘라내기/붙여넣기(플러그인 체인 포함), 큐 속성 붙여넣기(카테고리 선택), 찾기(Ctrl+F), 새 큐 기본값(템플릿), 자동 번호 / 재번호
- 프로젝트 파일 `.gocue` (JSON v3, v1·v2 파일도 읽음), 자동 백업(`<이름>.gocue.backups`), 파일을 프로젝트 폴더로 복사 옵션
- 오디오 파일·폴더를 창 어디에나 끌어다 놓으면 큐로 추가(목록 위 = 그 자리에 삽입, 인스펙터 파일칸 = 파일 교체, `.gocue` = 열기)
- 단일 창 UI(한국어): 상단 GO / 중앙 큐 목록(재생 초록·일시정지 노랑·페이드 주황 + 진행바 + 남은 시간) / 하단 인스펙터 탭(기본 · 시간·루프 · 트리거 · 이펙트)
- Inno Setup 인스톨러 + WinSparkle 자동 업데이트 (GitHub Releases의 `appcast.xml`)

## 단축키

| 키 | 동작 |
|---|---|
| `Space` | GO — 선택 큐 재생 후 다음 큐 선택. **일시정지된 큐가 있으면 그 큐를 재개** |
| `P` | 일시정지 / 재개 (선택 큐가 재생 중이 아니면 가장 최근 재생 큐) |
| `F` | 페이드아웃 정지 (큐의 정지 페이드 시간, 위와 같은 대상 규칙) |
| `Esc` | 전체 페이드 정지 — 프로젝트 설정의 시간(기본 2초) 동안 페이드아웃 후 정지. **0.5초 안에 두 번 = 즉시 정지**. 입력창을 편집하는 중에도 동작 |
| `V` | 미리듣기 — 선택 큐를 재생하되 선택(플레이헤드)은 그대로 (프리웨이트·시퀀스 무시) |
| `L` / `Ctrl+T` | 로드(GO 지연 0) / 시간으로 로드(초 또는 m:ss.mmm, 음수 = 끝에서부터) |
| `Ctrl+L` | 활성 큐 패널 보이기/숨기기 |
| `Ctrl+Shift+M` | 쇼 모드 ↔ 편집 모드 (푸터 버튼과 같음) |
| 큐 핫키 | 트리거 탭에서 지정한 키(F1~F12, 숫자 등)로 그 큐 GO. 입력창 편집 중에는 무시 |
| `Ctrl+Z` / `Ctrl+Y` | 실행 취소 / 다시 실행 |
| `Insert` | 큐 추가 (파일 선택) |
| `Delete` / `Ctrl+D` | 큐 삭제 / 큐 복제(플러그인 체인 포함) |
| `Ctrl+↑` / `Ctrl+↓` | 큐 순서 변경 (여러 개 선택 시 함께) |
| `Ctrl+A` / `Shift+클릭` / `Ctrl+클릭` | 모두 선택 / 범위 선택 / 추가 선택 |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | 큐 복사 / 잘라내기 / 선택 큐 아래에 붙여넣기 (플러그인 체인 포함, 핫키는 제외) |
| `Ctrl+Shift+V` | 큐 속성 붙여넣기 — 기본 · 시간 · 트리거 · 시간루프 · 레벨 · 이펙트 중 골라서 선택 큐들에 적용 |
| `Ctrl+F` / `F3` | 찾기(번호·이름·파일 이름·메모) / 다음 찾기 |
| `Ctrl+R` | 선택 큐 재번호 (시작 · 증가 · 접두 · 접미) |
| `N` `Q` `E` `W` `C` `O` `D` | 목록에서 바로 편집: 번호 / 이름 / 프리웨이트 / 포스트웨이트 / 진행 모드 순환 / 메모 / 길이(끝 트림). 셀 더블클릭도 같음 |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` | 새 프로젝트 / 열기 / 저장 / 다른 이름으로 저장 |
| `Ctrl+Shift+,` | 프로젝트 설정 (GO 간격, 전체 페이드 정지 시간, 백업 등) |
| `Ctrl+,` / `Ctrl+P` / `Ctrl+M` | 오디오 출력 설정 / VST3 플러그인 관리(스캔) / 마스터 버스 인서트 |

파형(시간·루프 탭)에서:

| 키 / 마우스 | 동작 |
|---|---|
| 아래쪽 회색 삼각 핸들 드래그 | 시작 / 끝 트림 (재생 중이면 즉시 반영, 끝이 재생 위치보다 앞이면 바로 끝남) |
| 클릭 | 커서 위치 지정 |
| `Shift+I` / `Shift+O` | 커서를 시작 / 끝으로 |
| 노란 엔벨로프 선 클릭 | 점 추가. 점 드래그 = 이동, `←`/`→` = 점 선택, `Alt+화살표` = 이동(`Shift` 더하면 미세), `Delete` = 점 삭제 |
| `Alt+휠` / `Ctrl+=` / `Ctrl+-` | 줌 (휠 = 스크롤) |
| 우클릭 | 외부 편집기로 열기 · 탐색기에서 보기 · 표시 채널 · 줌 |

재생 동작 규칙:
- 같은 큐에 다시 GO가 오면 큐의 **2차 트리거** 설정대로: 무시 / 전체 페이드 정지 시간으로 페이드 정지 / 정지 페이드로 정지 / 즉시 정지 / 즉시 정지 후 재시작(기본) / 이번 반복만 마치고 끝(루프 큐). 서로 다른 큐는 동시에 재생된다.
- 프로젝트 설정의 "GO 사이 최소 시간"을 켜면 그 안에 들어온 GO는 무시되고 GO 버튼에 빨간 테두리가 깜빡인다. "키를 뗀 뒤에만 다시 GO"는 키를 누르고 있는 동안의 반복을 막는다.
- 모든 정지·일시정지는 5 ms 램프로 클릭 노이즈를 막는다. 파일이 끝나거나 페이드아웃이 끝나면 체인의 플러그인들이 보고한 테일의 합(최대 10 s)만큼 체인을 더 돌린다. 즉시 정지는 테일을 건너뛴다.
- 일시정지 중에도 플러그인은 무음을 계속 처리한다(딜레이·리버브 시간 유지). 재개하면 멈춘 자리에서 이어진다.
- 엔벨로프는 트림 구간 기준이며 반복(루프)마다 다시 적용된다. "시작/끝에 잠금"을 켜면 구간을 바꿔도 모양이 따라 늘어나고, 끄면 초 단위로 고정된다. 엔벨로프 편집은 다음 재생부터 반영된다(트림·속도·게인은 즉시).
- 바이패스(`B`)된 플러그인도 계속 돌아가되 출력만 버린다. 플러그인 에디터에서 노브를 움직이면 프로젝트가 수정됨(`*`)으로 표시된다.
- 모노 파일은 양쪽 채널로, 3채널 이상은 앞 2채널만 출력한다. 출력은 장치의 첫 스테레오 페어(설정 창에서 선택).

## 프로젝트 설정 (파일 > 프로젝트 설정)

- 일반: GO 사이 최소 시간, 키를 뗀 뒤에만 다시 GO, 전체 페이드 정지 시간(Esc), 새 큐 자동 번호·증가, 플레이헤드를 선택에 잠금, 열 때/닫을 때 큐 시작(번호로 지정; 닫을 때 큐는 끝난 뒤 종료, 최대 2분), 큐 리스트 행 크기
- 큐 메뉴 "선택 큐를 새 큐 기본값으로": 이후 추가하는 큐가 그 큐의 설정(페이드·게인·색·웨이트·트리거·이펙트 체인 등)을 물려받는다(이름·번호·파일·핫키·시계 제외). 프로젝트에 저장됨
- 파일: 큐 추가 시 오디오 파일을 프로젝트 폴더 `audio/`로 복사, 자동 백업(간격 5~600초, 저장한 적 있는 프로젝트만), 저장 전 백업(1분 1회), 오래된 백업 정리(최근 1시간 20개 / 하루 동안 시간별 / 그 뒤 일별)
- 설정은 프로젝트 파일에 저장되며 실행 취소 대상이 아니다.

## 빌드

요구 사항: Visual Studio 2022 (C++ 데스크톱 워크로드 또는 Build Tools), CMake 3.22+, git.
JUCE 8.0.15는 CMake `FetchContent`가 GitHub에서 자동으로 받는다.

```bat
cmake --preset vs2022
cmake --build --preset vs2022-release
ctest --preset vs2022-release        :: 단위 테스트 (VST3가 설치돼 있으면 실물 플러그인 검사도 수행)
```

결과물: `build\vs2022\GoCue_artefacts\Release\GoCue.exe` (+ `WinSparkle.dll`)

### 별도로 받아야 하는 SDK (저장소에 포함하지 않음)

| 항목 | 받는 곳 | CMake 변수 | 없을 때 |
|---|---|---|---|
| ASIO SDK | https://www.steinberg.net/asiosdk | `ASIO_SDK_DIR` (`common/iasiodrv.h`가 있는 폴더) | 경고 후 WASAPI 전용 빌드 |
| WinSparkle 0.9.4 | https://github.com/vslavik/winsparkle/releases | `WINSPARKLE_DIR` (`include/winsparkle.h`, `x64/Release/WinSparkle.dll`) | 자동 업데이트 없이 빌드 |
| 로컬 JUCE 체크아웃(선택) | https://github.com/juce-framework/JUCE (태그 8.0.15) | `FETCHCONTENT_SOURCE_DIR_JUCE` | GitHub에서 자동 다운로드 |

`CMakeUserPresets.json`(git 무시됨)에 `local` 프리셋으로 적어 두면 `cmake --preset local` 한 번으로 끝난다:

```json
{ "version": 3,
  "configurePresets": [{ "name": "local", "inherits": "vs2022",
    "cacheVariables": {
      "ASIO_SDK_DIR": "C:/SDKs/ASIOSDK",
      "WINSPARKLE_DIR": "C:/SDKs/WinSparkle-0.9.4",
      "GOCUE_APPCAST_URL": "https://github.com/<owner>/<repo>/releases/latest/download/appcast.xml" } }],
  "buildPresets": [{ "name": "local-release", "configurePreset": "local", "configuration": "Release" }],
  "testPresets":  [{ "name": "local-release", "configurePreset": "local", "configuration": "Release" }] }
```

`GOCUE_APPCAST_URL`이 비어 있으면 앱은 업데이트 확인을 하지 않는다(메뉴 항목 비활성).
`GOCUE_EDDSA_PUBLIC_KEY`(업데이트 서명 검증용 공개키)는 `CMakePresets.json`에 들어 있다.

GUI 스모크 테스트 도우미: `C:\Users\claude\tools\gocue_uitest.ps1` (launch / shot / click / drag / keys / hotkey / close). Ctrl+글자 단축키는 스캔코드를 넣는 `hotkey` 액션으로만 확인할 수 있다(`keys`의 SendKeys는 제어 문자로 들어가 JUCE 단축키에 안 잡힌다).

## VST3 플러그인

`오디오 > VST3 플러그인 관리`의 `Options > Scan for new or updated VST3 plug-ins`로 스캔한다.
기본 검색 경로는 `C:\Program Files\Common Files\VST3`와 `%LOCALAPPDATA%\Programs\Common\VST3`.
스캔 결과는 사용자 설정(`%APPDATA%\GoCue\GoCue.settings`)에 저장된다.
인스펙터 이펙트 탭의 `+ 플러그인`으로 큐 체인에 추가하고, 슬롯의 `편집`(또는 더블클릭)으로 에디터 창을 연다. `B`는 바이패스. 슬롯 추가/삭제/바이패스는 실행 취소된다(플러그인 안의 노브 값은 제외).
프로젝트를 저장하면 각 플러그인의 상태(`getStateInformation`)가 base64로 `.gocue`에 들어간다.
플러그인이 없는 PC에서 열면 슬롯이 `[없음]`으로 남고 다시 저장해도 상태는 보존된다.

## 릴리스 (인스톨러 + 자동 업데이트)

한 번만 준비:
1. `winsparkle-tool generate-key --file eddsa_priv.pem` — 개인키는 저장소 밖에 보관하고 백업한다. 잃어버리면 기존 사용자에게 업데이트를 보낼 수 없다.
   공개키(`winsparkle-tool public-key -f eddsa_priv.pem`)를 `CMakePresets.json`의 `GOCUE_EDDSA_PUBLIC_KEY`에 넣는다.
2. GitHub 저장소(공개 — WinSparkle이 로그인 없이 받아야 한다)를 만들고 `GOCUE_APPCAST_URL`을 `https://github.com/<owner>/<repo>/releases/latest/download/appcast.xml`로 빌드한다. (현재 `CMakePresets.json`에 `dnakrhs2-crypto/gocue`로 고정)
3. Inno Setup 6.3+ 설치 (`ISCC.exe`). 한국어 언어 파일(`Languages\Korean.isl`) 포함.

릴리스마다:
```bat
:: CMakeLists.txt의 project(GoCue VERSION x.y.z)를 올린 뒤
python tools\release.py --repo <owner>/<repo> --key C:\keys\eddsa_priv.pem --notes notes.html --publish
```
`release.py`는 Release 빌드 → 테스트 → `installer\output\GoCue-Setup-x.y.z.exe` → `winsparkle-tool sign` → `appcast.xml` 생성 → `gh release create vx.y.z`(설치 파일 + appcast 업로드)까지 수행한다.
버전은 오직 `CMakeLists.txt`의 `project(GoCue VERSION x.y.z)`에서 읽으며, CI에서는 태그 `vx.y.z`와 다르면 중단한다(appcast가 404를 가리키는 사고 방지).
앱은 시작 시(하루 1회) 조용히 appcast를 확인하고, 새 버전이 있으면 WinSparkle 창으로 안내한 뒤 설치 파일을 받아 실행한다. 저장하지 않은 변경이 있으면 앱을 닫지 않는다.
appcast의 `sparkle:installerArguments="/SILENT /SP- /NORESTART"` 덕분에 업데이트 설치는 진행 표시만 보이고 클릭 없이 끝난다(기본 사용자별 설치 기준).

현재 저장소: https://github.com/dnakrhs2-crypto/gocue (릴리스: https://github.com/dnakrhs2-crypto/gocue/releases)

`.github/workflows/release.yml`은 `v*` 태그를 푸시하면 같은 과정을 GitHub Actions에서 수행한다(저장소 secret `GOCUE_EDDSA_PRIVATE_KEY` 필요). 이 워크플로는 아직 실제 저장소에서 돌려 보지 않았다.

인스톨러는 기본적으로 사용자별 설치(`%LOCALAPPDATA%\Programs\GoCue`, UAC 없음)이며 대화상자에서 전체 사용자 설치를 고를 수 있다. `.gocue` 파일 연결을 등록한다.

## 구조

```
src/model    Cue / Envelope / CueList(선택·플레이헤드) / CueNumbering / CuePropertyPaste / ProjectSerializer / WorkspaceSettings
src/audio    AudioEngine / CuePlayer / RegionLoopSource / FadeEnvelope         장치, 믹서, 재생 인스턴스
             PluginHost / PluginChain                                          VST3 스캔·인스턴스, 인서트 체인
src/app      ProjectDocument / ProjectHistory(실행 취소) / CueController(GO·시퀀스·핫키·벽시계·덕·패닉 규칙) / Scheduler
             BackupManager / AppSettings / Updater / Commands
src/ui       MainComponent / TransportBar / CueTable / CueInspector(탭) / TimeLoopsPanel / WaveformView / ActiveCuesPanel / FooterBar
             WorkspaceSettingsDialog / PastePropertiesDialog / PluginChainComponent / PluginWindows / PluginDialogs / AudioSettingsDialog
tests        JUCE UnitTest 콘솔 (오프라인 렌더로 오디오 경로 검증, 컨트롤러 규칙, 직렬화, 백업, 실물 VST3 검사)
installer    GoCue.iss (Inno Setup)
tools        release.py (릴리스 파이프라인), make_icon.py (아이콘)
```

신호 경로: 파일 → 구간/루프/엔벨로프(`RegionLoopSource`, 파일 시간축) → 디스크 선독 → 리샘플(장치 SR × 속도) → 정지 페이드·일시정지 게이트 → 큐 게인 → 큐 VST3 체인 → 믹스 → 마스터 VST3 체인 → 출력 ch1-2

## 프로젝트 파일 형식 (`.gocue`, v3)

```json
{ "app": "GoCue", "version": 3, "name": "show",
  "cues": [ { "id": "uuid", "number": "1.5", "name": "Intro", "notes": "", "file": "C:/audio/intro.wav", "fileRelative": "audio/intro.wav",
              "color": 3, "secondColor": 0, "useSecondColor": false, "flagged": false, "armed": true, "skipIfDisarmed": false, "autoLoad": false,
              "preWait": 0, "postWait": 0, "continueMode": "none", "hotkey": "", "wallClock": { "enabled": false },
              "fadeStopOthers": { "enabled": false }, "duck": { "enabled": false },
              "fadeOutMs": 1500, "gainDb": -3.0, "durationSeconds": 12.3,
              "audio": { "start": 0.5, "end": -1, "playCount": 1, "infiniteLoop": false, "rate": 1.0, "preservePitch": false,
                         "envelope": { "enabled": true, "linear": false, "lockToTrim": true, "points": [[0, 0], [0.1, 1], [1, 0]] } },
              "secondTrigger": "hardStopRestart",
              "plugins": [ { "format": "VST3", "name": "3 Band EQ", "fileOrIdentifier": "C:/.../3BandEQ.vst3",
                             "uniqueId": 1662645128, "description": "<PLUGIN .../>", "state": "base64", "bypassed": false } ] } ],
  "master": { "plugins": [] },
  "settings": { "doubleGoSeconds": 0, "requireKeyUp": false, "panicSeconds": 2, "autoNumber": true, "numberIncrement": 1,
                "lockPlayheadToSelection": true, "startOnOpen": false, "startOnCloseCue": "", "rowSize": 1,
                "hasCueTemplate": false, "cueTemplate": { "...": "큐 객체" },
                "autoBackup": true, "backupIntervalSeconds": 60, "...": "..." } }
```
- `end: -1`은 파일 끝. 엔벨로프 점의 x는 `lockToTrim`이면 구간의 0~1 비율, 아니면 구간 시작 기준 초.
- v1 파일의 `fadeInMs`는 읽을 때 엔벨로프(0,0)→(fadeIn,1)로 바뀐다. `fadeOutMs`는 그대로 정지 페이드다.
- 절대 경로가 없으면 프로젝트 폴더 기준 `fileRelative`로 다시 찾는다. 모르는 필드는 무시하고, 더 높은 `version`은 경고만 낸 뒤 읽는다.
