# GoCue

Windows용 오디오 큐 플레이어. QLab의 오디오 기능 부분집합을 구현한다.

- 출력 장치: ASIO / WASAPI (`AudioDeviceManager`)
- 재생 포맷: WAV / AIFF / FLAC / MP3 / OGG (JUCE 내장 디코더)
- 큐별 페이드 인/아웃(ms), 페이드아웃 정지, 큐별 게인(dB), 여러 큐 동시 재생(믹서)
- 큐별 VST3 인서트 체인 + 마스터 버스 체인 (2단계)
- 프로젝트 파일 `.gocue` (JSON, `version` 필드, 누락 필드는 기본값)
- 단일 창 UI: 상단 GO / 중앙 큐 테이블 / 하단 인스펙터

## 단축키

| 키 | 동작 |
|---|---|
| `Space` | GO — 선택 큐 재생 후 다음 큐 선택 |
| `S` | 정지 (선택 큐가 재생 중이 아니면 가장 최근 재생 큐) |
| `F` | 페이드아웃 정지 (위와 같은 대상 규칙) |
| `Shift+F` | 전체 페이드아웃 정지 |
| `Esc` | 전체 정지 |
| `Insert` | 큐 추가 (파일 선택) / 파일을 테이블에 끌어다 놓아도 추가 |
| `Delete` | 큐 삭제 |
| `Ctrl+D` | 큐 복제 |
| `Ctrl+↑` / `Ctrl+↓` | 큐 순서 변경 |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` | 새 프로젝트 / 열기 / 저장 / 다른 이름으로 저장 |
| `Ctrl+,` | 오디오 출력 설정 |

## 빌드

요구 사항: Visual Studio 2022 (C++ 데스크톱 워크로드 또는 Build Tools), CMake 3.22+, git.

```bat
git clone <repo> gocue
cd gocue
cmake --preset vs2022            :: JUCE 8.0.15를 GitHub에서 자동으로 받는다
cmake --build --preset vs2022-release
ctest --preset vs2022-release    :: 단위 테스트
```

빌드 결과: `build\vs2022\GoCue_artefacts\Release\GoCue.exe`

### 로컬 JUCE / ASIO SDK 경로 지정

ASIO SDK는 재배포가 금지되어 저장소에 포함하지 않는다.
https://www.steinberg.net/asiosdk 에서 받아 압축을 풀고, 아래처럼 경로를 넘기면 `JUCE_ASIO=1`로 빌드된다.
경로가 없으면 경고와 함께 WASAPI 전용으로 빌드된다.

```bat
cmake --preset vs2022 -DASIO_SDK_DIR=C:/SDKs/ASIOSDK -DFETCHCONTENT_SOURCE_DIR_JUCE=C:/JUCE
```

같은 내용을 `CMakeUserPresets.json`(git 무시됨)에 `local` 프리셋으로 적어 두면 `cmake --preset local`로 쓸 수 있다.

## 구조

```
src/model    Cue / CueList / ProjectSerializer   — 데이터와 JSON
src/audio    AudioEngine / CuePlayer / FadeEnvelope — 장치, 믹서, 재생 인스턴스
src/app      ProjectDocument / AppSettings / Commands
src/ui       MainComponent / TransportBar / CueTable / CueInspector / AudioSettingsDialog
tests        JUCE UnitTest 기반 콘솔 테스트 (오프라인 렌더링으로 오디오 경로 검증)
```

신호 경로: 파일 → 리샘플(장치 SR) → 페이드 → 큐 게인 → [큐 VST3 체인] → 믹스 → [마스터 VST3 체인] → 출력 ch1-2
