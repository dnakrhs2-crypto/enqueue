# GoCue 설계 (2026-08-30)

Windows용 오디오 큐 플레이어. QLab 오디오 기능의 부분집합.
gom이 준 요구사항을 그대로 옮기고, 결정이 필요했던 부분은 "결정" 항목으로 적었다.

## 1. 스택
- JUCE 8.0.15 + CMake 3.22+, Visual Studio 2022(Build Tools 가능), C++17.
- JUCE는 `FetchContent`(태그 고정)로 받고, 로컬 체크아웃은 `FETCHCONTENT_SOURCE_DIR_JUCE`로 지정.
- ASIO SDK는 별도 다운로드 → `ASIO_SDK_DIR` → `JUCE_ASIO=1`. 없으면 WASAPI 전용 빌드(경고).
- 결정: 작업명 **GoCue**, 저장소 `C:\Users\claude\gocue`.

## 2. 오디오
- `AudioDeviceManager`(0 in / 2 out) — ASIO + WASAPI. 장치 상태는 앱 설정(XML)에 저장.
- 포맷: wav / aiff / flac / mp3(JUCE 내장 디코더) / ogg. `registerBasicFormats`.
- 신호 경로(큐 1개): 파일 → `AudioTransportSource`(디스크 read-ahead, 장치 SR로 리샘플) → `FadeEnvelope`(선형 진폭) → 큐 게인 → [큐 VST3 체인] → 믹스 버퍼.
- 믹스 → [마스터 VST3 체인] → 출력 ch1-2 (나머지 채널은 무음).
- 페이드 인/아웃은 ms 단위, 샘플 단위로 정확히 램프. 모든 정지는 5 ms 디클릭 램프.
- 결정: 큐 하나당 재생 인스턴스 1개. 같은 큐에 GO를 다시 누르면 처음부터 재시작(이전 인스턴스는 디클릭 정지). 서로 다른 큐는 동시 재생.
- 결정: 모노 파일은 양쪽 채널로, 3채널 이상은 앞 2채널만.
- 스레드: 메시지 스레드가 플레이어를 만들고(파일 열기 포함) `CriticalSection`으로 보호된 벡터에 추가. 오디오 콜백은 락을 잡고 렌더만 한다(파일 IO·할당 없음). 끝난 플레이어는 `AsyncUpdater`로 메시지 스레드에서 파괴.

## 3. VST3 (2단계)
- `AudioPluginFormatManager` + `KnownPluginList`, 스캔 결과는 앱 설정에 저장.
- 큐마다 독립 인서트 체인(`PluginChain`), 마스터 버스에 하나 더.
- 에디터 창(`DocumentWindow`) 열기, `getStateInformation`/`setStateInformation`을 base64로 프로젝트에 저장/복원.

## 4. 큐 리스트
- `Space` GO(선택 큐 재생 → 다음 큐 선택), `S` 정지, `Esc` 전체 정지, `F` 페이드아웃 정지, `Shift+F` 전체 페이드아웃.
- 결정: `S`/`F`는 선택 큐가 재생 중이면 그 큐, 아니면 **가장 최근에 시작한 재생 큐**에 적용(GO 직후 선택이 다음 큐로 넘어가는 문제 해결).
- 추가(`Insert`, 파일 선택) / 삭제(`Delete`) / 복제(`Ctrl+D`) / 순서 변경(`Ctrl+↑↓`), 파일 드래그앤드롭(놓은 위치에 삽입).
- 결정: 번호는 행 순서(1,2,3…)로 자동. 이름만 편집.

## 5. 프로젝트 파일 (`.gocue`, JSON)
```json
{ "app": "GoCue", "version": 1, "name": "...",
  "cues": [ { "id": "uuid", "name": "", "file": "절대경로", "fileRelative": "프로젝트 기준 상대경로",
              "fadeInMs": 0, "fadeOutMs": 0, "gainDb": 0.0, "durationSeconds": 0.0,
              "plugins": [ { "format": "VST3", "name": "", "fileOrIdentifier": "", "uniqueId": 0, "state": "base64", "bypassed": false } ] } ],
  "master": { "plugins": [] } }
```
- 누락 필드는 기본값, 모르는 필드는 무시, `version`이 더 크면 경고만 내고 읽는다.
- 파일 경로는 절대경로 우선, 없으면 프로젝트 폴더 기준 상대경로로 재시도, 그래도 없으면 `fileMissing` 표시.

## 6. UI (단일 창)
- 상단 `TransportBar`: 다음 큐 번호·이름·파일·메타 + 큰 GO 버튼 + 정지/페이드아웃/전체 정지 버튼 + 재생 중 개수/상태 메시지.
- 중앙 `CueTable`: 번호·이름·파일·페이드인·페이드아웃·길이. 재생 중 행은 초록(페이드아웃 중 주황) + 진행바, 선택(스탠바이) 행은 파란 테두리, 파일 없음은 빨강.
- 하단 `CueInspector`: 이름·파일(찾아보기)·페이드인/아웃·게인 슬라이더·플러그인 슬롯(2단계).
- 메뉴바: 파일 / 큐 / 재생 / 오디오.
- 한국어 UI, 기본 서체 Malgun Gothic.

## 7. 배포/업데이트 (4단계)
- Inno Setup 인스톨러, WinSparkle + GitHub Releases appcast 기반 자동 업데이트.

## 8. 테스트
- `tests/` 콘솔 타깃(JUCE UnitTest): FadeEnvelope 수학, CueList 조작/선택, ProjectSerializer 왕복·하위호환·상대경로, AudioEngine 오프라인 렌더(페이드/게인/EOF/믹스/재시작/정지).

## 9. 진행 순서
1. 오디오 재생 + 큐 리스트 + 프로젝트 파일 (이 문서 기준선)
2. VST3 (큐 체인 / 마스터 체인 / 에디터 / 상태 저장)
3. UI 마감 (플러그인 슬롯, 진행바 다듬기, 테이블 내 드래그 정렬 등)
4. Inno Setup + WinSparkle
