# GoCue

Windows용 오디오 큐 플레이어. QLab의 오디오 기능 부분집합을 구현한다.

- 출력 장치: ASIO / WASAPI (`juce::AudioDeviceManager`, 메뉴 `오디오 > 오디오 출력 설정`)
- 재생 포맷: WAV / AIFF / FLAC / MP3 / OGG
- 큐별 페이드 인/아웃(ms), 페이드아웃 정지, 큐별 게인(dB), 여러 큐 동시 재생(믹서)
- 큐마다 독립 VST3 인서트 체인 + 마스터 버스 체인, 플러그인 에디터 창, 상태 저장/복원
- 프로젝트 파일 `.gocue` (JSON, `version` 필드, 누락 필드는 기본값으로 읽는 하위 호환)
- 단일 창 UI: 상단 GO / 중앙 큐 테이블(재생 중 색상 + 진행바 + 남은 시간) / 하단 인스펙터
- Inno Setup 인스톨러 + WinSparkle 자동 업데이트 (GitHub Releases의 `appcast.xml`)

## 단축키

| 키 | 동작 |
|---|---|
| `Space` | GO — 선택 큐 재생 후 다음 큐 선택 |
| `S` | 정지 (선택 큐가 재생 중이 아니면 가장 최근 재생 큐) |
| `F` | 페이드아웃 정지 (위와 같은 대상 규칙) |
| `Shift+F` | 전체 페이드아웃 정지 |
| `Esc` | 전체 정지 |
| `Insert` | 큐 추가 (파일 선택). 오디오 파일을 테이블에 끌어다 놓아도 추가된다 |
| `Delete` / `Ctrl+D` | 큐 삭제 / 큐 복제(플러그인 체인 포함) |
| `Ctrl+↑` / `Ctrl+↓` | 큐 순서 변경 |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` | 새 프로젝트 / 열기 / 저장 / 다른 이름으로 저장 |
| `Ctrl+,` / `Ctrl+P` / `Ctrl+M` | 오디오 출력 설정 / VST3 플러그인 관리(스캔) / 마스터 버스 인서트 |

재생 동작 규칙:
- 같은 큐에 GO를 다시 누르면 처음부터 재시작(이전 인스턴스는 5 ms 디클릭 정지). 서로 다른 큐는 동시에 재생된다.
- 모든 정지는 5 ms 램프로 클릭 노이즈를 막는다. 파일이 끝나거나 페이드아웃이 끝나면 체인의 플러그인들이 보고한 테일의 합(직렬 연결이므로, 최대 10 s)만큼 체인을 더 돌린다. `S`/`Esc`의 즉시 정지는 테일을 건너뛴다.
- `Esc`는 인스펙터의 입력창을 편집하는 중에도 동작한다(편집 취소 + 전체 정지).
- 바이패스(`B`)된 플러그인도 계속 돌아가되 출력만 버린다(딜레이·리버브가 다시 켤 때 옛 소리를 뱉지 않도록).
- 모노 파일은 양쪽 채널로, 3채널 이상은 앞 2채널만 출력한다. 출력은 장치의 첫 스테레오 페어(설정 창에서 선택).
- 플러그인 에디터에서 노브를 움직이면 프로젝트가 수정됨(`*`)으로 표시되어 저장을 묻는다.

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

## VST3 플러그인

`오디오 > VST3 플러그인 관리`의 `Options > Scan for new or updated VST3 plug-ins`로 스캔한다.
기본 검색 경로는 `C:\Program Files\Common Files\VST3`와 `%LOCALAPPDATA%\Programs\Common\VST3`.
스캔 결과는 사용자 설정(`%APPDATA%\GoCue\GoCue.settings`)에 저장된다.
인스펙터의 `+ 플러그인`으로 큐 체인에 추가하고, 슬롯의 `편집`(또는 더블클릭)으로 에디터 창을 연다. `B`는 바이패스.
프로젝트를 저장하면 각 플러그인의 상태(`getStateInformation`)가 base64로 `.gocue`에 들어간다.
플러그인이 없는 PC에서 열면 슬롯이 `[없음]`으로 남고 다시 저장해도 상태는 보존된다.

## 릴리스 (인스톨러 + 자동 업데이트)

한 번만 준비:
1. `winsparkle-tool generate-key --file eddsa_priv.pem` — 개인키는 저장소 밖에 보관하고 백업한다. 잃어버리면 기존 사용자에게 업데이트를 보낼 수 없다.
   공개키(`winsparkle-tool public-key -f eddsa_priv.pem`)를 `CMakePresets.json`의 `GOCUE_EDDSA_PUBLIC_KEY`에 넣는다.
2. GitHub 저장소를 만들고 `GOCUE_APPCAST_URL`을 `https://github.com/<owner>/<repo>/releases/latest/download/appcast.xml`로 빌드한다.
3. Inno Setup 6.3+ 설치 (`ISCC.exe`). 한국어 언어 파일(`Languages\Korean.isl`) 포함.

릴리스마다:
```bat
:: CMakeLists.txt의 project(GoCue VERSION x.y.z)를 올린 뒤
python tools\release.py --repo <owner>/<repo> --key C:\keys\eddsa_priv.pem --notes notes.html --publish
```
`release.py`는 Release 빌드 → 테스트 → `installer\output\GoCue-Setup-x.y.z.exe` → `winsparkle-tool sign` → `appcast.xml` 생성 → `gh release create vx.y.z`(설치 파일 + appcast 업로드)까지 수행한다.
버전은 오직 `CMakeLists.txt`의 `project(GoCue VERSION x.y.z)`에서 읽으며, CI에서는 태그 `vx.y.z`와 다르면 중단한다(appcast가 404를 가리키는 사고 방지).
앱은 시작 시(하루 1회) 조용히 appcast를 확인하고, 새 버전이 있으면 WinSparkle 창으로 안내한 뒤 설치 파일을 받아 실행한다. 저장하지 않은 변경이 있으면 앱을 닫지 않는다.

`.github/workflows/release.yml`은 `v*` 태그를 푸시하면 같은 과정을 GitHub Actions에서 수행한다(저장소 secret `GOCUE_EDDSA_PRIVATE_KEY` 필요). 이 워크플로는 아직 실제 저장소에서 돌려 보지 않았다.

인스톨러는 기본적으로 사용자별 설치(`%LOCALAPPDATA%\Programs\GoCue`, UAC 없음)이며 대화상자에서 전체 사용자 설치를 고를 수 있다. `.gocue` 파일 연결을 등록한다.

## 구조

```
src/model    Cue / CueList / ProjectSerializer            데이터와 JSON
src/audio    AudioEngine / CuePlayer / FadeEnvelope       장치, 믹서, 재생 인스턴스
             PluginHost / PluginChain                      VST3 스캔·인스턴스, 인서트 체인
src/app      ProjectDocument / AppSettings / Updater / Commands
src/ui       MainComponent / TransportBar / CueTable / CueInspector
             PluginChainComponent / PluginWindows / PluginDialogs / AudioSettingsDialog
tests        JUCE UnitTest 콘솔 (오프라인 렌더로 오디오 경로 검증, 스텁 플러그인, 설치된 실물 VST3 검사)
installer    GoCue.iss (Inno Setup)
tools        release.py (릴리스 파이프라인), make_icon.py (아이콘)
```

신호 경로: 파일 → 리샘플(장치 SR) → 페이드 → 큐 게인 → 큐 VST3 체인 → 믹스 → 마스터 VST3 체인 → 출력 ch1-2

## 프로젝트 파일 형식 (`.gocue`)

```json
{ "app": "GoCue", "version": 1, "name": "show",
  "cues": [ { "id": "uuid", "name": "Intro", "file": "C:/audio/intro.wav", "fileRelative": "audio/intro.wav",
              "fadeInMs": 250, "fadeOutMs": 1500, "gainDb": -3.0, "durationSeconds": 12.3,
              "plugins": [ { "format": "VST3", "name": "3 Band EQ", "fileOrIdentifier": "C:/.../3BandEQ.vst3",
                             "uniqueId": 1662645128, "description": "<PLUGIN .../>", "state": "base64", "bypassed": false } ] } ],
  "master": { "plugins": [] } }
```
절대 경로가 없으면 프로젝트 폴더 기준 `fileRelative`로 다시 찾는다. 모르는 필드는 무시하고, 더 높은 `version`은 경고만 낸 뒤 읽는다.
