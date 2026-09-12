# Recorder 디자인 13 적용 결과

작업 트리: `C:\Users\claude\gocue-rec-play` / 브랜치 `design-13` / 기준 `5061168`.
실행기가 지정한 OUT 경로는 발견되지 않아 이 파일에 기록했다.

Recorder 전용 팔레트와 `juce::LookAndFeel_V4` 기반 룩앤필을 적용했다. 최종 `Recorder`, `RecorderTests` 빌드 성공, 전체 테스트 **758 passed / 0 failed, exit 0**, 실제 격리 메인 창 **1440×900 PrintWindow 캡처 및 창 닫기 exit 0**을 확인했다.

커밋·push·release.py 실행은 하지 않았다. ProductIdentity·버전·릴리스 노트·site·livemix 파일을 수정하지 않았다. 기존 `docs/design/`는 작업 시작부터 미추적 상태였고 수정하지 않았다. 프로세스 감시·kill 스크립트는 작성하지 않았다.

## 수정 목록 (file:line)

| 파일 | 변경 내용 |
| --- | --- |
| `recorder/src/ui/RecorderPalette.h:4` | `gocue::recorder::Palette` 신설. 기존 식별자 유지, 시안 토큰 및 음소거/솔로 색 분리 |
| `recorder/src/ui/RecorderLookAndFeel.h:2` | LiveMix include/팔레트 별칭 제거. Recorder 팔레트 직접 사용, V4 직접 상속 |
| `recorder/src/ui/RecorderLookAndFeel.h:8` | 시안의 CSS 글자 크기를 JUCE em 기준으로 해석하는 `recorderFont()` |
| `recorder/src/ui/RecorderLookAndFeel.cpp:10` | Cascadia Mono 우선, 없으면 Consolas. 이 환경에서는 Cascadia Mono 설치 확인 |
| `recorder/src/ui/RecorderLookAndFeel.cpp:15` | Windows 제목 표시줄에 어두운 배경·글자색 적용. 창 동작은 기존 native title bar 유지 |
| `recorder/src/ui/RecorderLookAndFeel.cpp:32` | 버튼·콤보·에디터·라벨·토글·슬라이더·팝업·스크롤바·툴팁·AlertWindow·ListBox·탭·창 색 설정 |
| `recorder/src/ui/RecorderLookAndFeel.cpp:101` | 그라데이션 없는 버튼, 1px 테두리, 4px 모서리, 비활성 배경/글자 45%, 연결 버튼 및 프로젝트 화살표 |
| `recorder/src/ui/RecorderLookAndFeel.cpp:166` | field 배경 에디터와 4px 테두리, 탭 panel2 배경 및 선택 탭 아래 2px accent |
| `recorder/src/ui/RecordView.cpp:13` | 캠 카드 4px 모서리·테두리·검정 영상 영역·굵은 머리줄·녹화 중 REC 배지 |
| `recorder/src/ui/RecordView.cpp:37` | 마이크 이름·입력 설명, 카드 및 미터 배경/테두리 |
| `recorder/src/ui/RecordView.cpp:56` | 프로젝트명 16px, 상태 12px 모노, 녹화 버튼 흰 글자, 단일 테두리 세그먼트 |
| `recorder/src/ui/RecordView.cpp:93` | 녹화 상태 색, 정지 버튼의 rec 테두리·글자·14% 배경 |
| `recorder/src/ui/RecordView.cpp:141` | 46px 상단바, 가져오기 버튼을 탭과 설정 사이에 배치, 빈 안내 행 접기, 타임라인 모드 캠 카드 최대 236px |
| `recorder/src/ui/RecordView.h:19` | 세그먼트 외곽선 paint 및 카메라의 표시용 녹화 상태 |
| `recorder/src/ui/RecorderTransportBar.cpp:5` | 모노 타임코드·파형 배율, 재생 색, panel2 배경과 하단 선, 파형→확대 순서 |
| `recorder/src/ui/RecorderTransportBar.h:13` | 트랜스포트 paint 선언 |
| `recorder/src/ui/TimelineView.cpp:40` | 11.5px 상태줄, 모노 숫자 입력, 270px 사이드바 및 34px 탭 |
| `recorder/src/ui/TimelineView.cpp:359` | 트랜스포트/툴바/상태줄 배경·경계, 화면 폭에 따른 툴바 스크롤 공간 |
| `recorder/src/ui/TimelineView.cpp:446` | 룰러 panel2·모노 10px, 트랙 배경, 118px 뒤 1px 세로 보조선(border 45%) |
| `recorder/src/ui/TimelineView.cpp:493` | 영상 세로 그라데이션·오디오 배경, 흰 제목, 링크 아이콘, 안쪽 2px 선택 링·4px 핸들 |
| `recorder/src/ui/TimelineView.cpp:564` | 녹화 중 클립 rec 22% 채움·2px 테두리, 마커 흰 굵은 글자, 정수 픽셀 2px 재생헤드·삼각형 |
| `recorder/src/ui/TimelineView.h:42` | 타임라인 paint 선언 |
| `recorder/src/ui/TrackHeader.cpp:5` | panel2 헤더, 13.5px 굵은 이름, 24px 음소거/솔로 및 지정 활성 색 |
| `recorder/src/ui/TrackHeader.h:12` | 트랙 헤더 paint 선언 |
| `recorder/src/ui/ClipInspector.cpp:6` | 제목·안내·필드 간격, field 배경을 사용하는 오른쪽 정렬 모노 숫자 |
| `recorder/src/ui/MarkerPanel.cpp:15` | 위치/색 입력 모노, 선택 배경, 목록 구분선, 숫자와 이름의 글꼴 구분 |
| `recorder/src/ui/MainComponent.cpp:74` | 하단 표시 라벨 4개 및 기존 버튼의 작은 글자 |
| `recorder/src/ui/MainComponent.cpp:112` | 높이 30px panel 하단바, 1px 상단 선, 둥근 정보 알약, 기존 버튼 배치 및 가져오기 진행 영역 |
| `recorder/src/ui/MainComponent.cpp:225` | `session.deviceInfo()`의 실제 장치명/샘플레이트/버퍼, `cameraCaption()` 및 녹화 상태 표시 |
| `recorder/src/ui/MainComponent.h:19` | 하단바 paint 및 라벨 멤버 |
| `recorder/src/ui/ProjectDialogs.cpp:31` | 설정 탭 새 팔레트·밑줄, 설정/새 프로젝트의 native 제목 표시줄 스타일 |
| `recorder/src/ui/ExportDialog.cpp:59` | 시간/샘플 표시 모노, 내보내기 창 제목 표시줄 스타일 |
| `recorder/src/Main.cpp:174` | 실제 메인 창의 native 제목 표시줄 스타일. 기존 `Palette::background` 생성자는 새 팔레트를 참조 |
| `recorder/tests/UiWiringTests.cpp:44` | 상단 가져오기 버튼 및 진행 패널 배치 검사 갱신 |
| `recorder/tests/UiWiringTests.cpp:696` | 설정 화면 검사에서 Recorder 룩앤필 사용 |
| `recorder/tests/TimelineUxTests.cpp:187` | 음소거/솔로 지정 색 및 영상 클립 그라데이션 검사 갱신 |
| `recorder/tests/ShortcutExceptionTests.cpp:130` | 기존 선택적 캡처에 Recorder 테마 적용, 장치 주입을 사용하는 설정 폼 캡처, 새 가져오기 위치 검사 |

`TimelineLayout`의 헤더 폭 210 / 룰러 높이 30 / 행 높이 72 및 기존 편집·단축키·히트 판정 코드는 유지했다. 타임라인과 사이드바 사이 공간도 기존 6px로 복원해 좌표 계산이 바뀌지 않게 했다. 파일·녹화·재생·내보내기 처리 로직은 수정하지 않았다.

## 팔레트 표

모든 색 식별자는 `gocue::recorder::Palette` 소속이다.

| 시안 토큰/역할 | 값 | 식별자 |
| --- | --- | --- |
| bg | `#1e1e1e` | `background` |
| panel | `#262626` | `card` |
| panel2 | `#2d2d2d` | `bar`, `card2` |
| field | `#232323` | `field`, `meterBg` |
| border | `#3a3a3a` | `line` |
| text | `#e6e6e6` | `text` |
| muted | `#9a9a9a` | `dimText` |
| accent / selRing | `#4a9df0` | `accent` |
| accentInk | `#ffffff` | `juce::Colours::white` |
| rec | `#e0443a` | `brand`, `danger`, `recording`, `meterRed` |
| play / wave | `#4ec27a` | `meterGreen` |
| warn | `#e2a93b` | `meterYellow` |
| sel | `#31445f` | `selection` |
| 영상 클립 위 | `#3b3560` | `clipVideoTop` |
| 영상 클립 아래 | `#332f5e` | `clipVideoBottom` |
| 오디오 클립 | `#243f33` | `clipAudio` |
| 음소거 켜짐 | `#e5342a` / 흰 글자 | `muteOn` |
| 솔로 켜짐 | `#f2c53d` / 검정 글자 | `soloOn` |
| radius | `4px` | `cardRadius`, `controlRadius` |

HTML의 `.th-premiere .clip.vclip` 마지막 규칙은 시작색을 `#43407a`로 덮어쓴다. 이번 구현은 사용자에게 명시된 `#3b3560 → #332f5e`를 우선했다. 상단 프로젝트 바는 HTML `.topbar`의 `panel` 배경을 사용한다.

일반 컨트롤/설정창의 기존 글자 크기는 유지했다. 시안에서 지정한 10~18px 크기는 JUCE의 ascender+descender 높이와 CSS em 크기가 다른 점을 실제 캡처에서 확인한 뒤 `withPointHeight()`로 맞췄다. 기본 한글 글꼴은 Malgun Gothic, 숫자는 Cascadia Mono → Consolas이다.

## 실제 창 캡처와 비교

다음 명령의 경로를 **절대 경로로 확장**해 실행했다. 현재 CLI 파서가 상대 경로를 거부하므로, 프로젝트 메뉴 열기 우회는 필요하지 않았다.

```powershell
build\vs2022\recorder\Recorder_artefacts\Release\Recorder.exe `
  --test-root C:\Users\claude\gocue-rec-play\out\design13\settings `
  --open-project C:\Users\claude\gocue-rec-play\out\design13\demo-project\project.recorder
```

`RECORDER_TEST_NO_HARDWARE=1`도 지정했다. `--test-root` GUI의 `initialiseProject(..., false, false)` 경로를 사용해 장치 연결과 업데이터를 시작하지 않았다. 설정 GUI는 카메라 목록 조회 때 실제 장치를 열 수 있으므로 실제 앱에서는 열지 않았다.

PrintWindow(2), GetWindowRect 기준 **1440×900**. native 테두리를 제외한 클라이언트는 **1424×861**, 시작 오프셋 **(8,31)**이다. [캡처 도구](out/design13/capture_window.py)는 명시적으로 실행한 PID의 창을 한 번 열거해 캡처/입력/WM_CLOSE만 수행한다.

| 증거 | 경로 및 확인 내용 |
| --- | --- |
| 녹화 탭 | [shot-record.png](out/design13/shot-record.png) — 캠 카드 2개, 녹화 컨트롤, 마이크 스트립, 하단 정보 바 |
| 타임라인 탭 | [shot-timeline.png](out/design13/shot-timeline.png) — 캠1·마이크1의 실제 미디어 테이크 3개, 테이크003의 링크 클립 2개 선택, 마커1, 재생헤드, 음소거 활성 |
| 솔로 활성 보조 캡처 | [shot-timeline-solo.png](out/design13/shot-timeline-solo.png) — `#f2c53d`와 검정 글자 |
| 나란히 비교: 녹화 | [compare-record.png](out/design13/compare-record.png) |
| 나란히 비교: 타임라인 | [compare-timeline.png](out/design13/compare-timeline.png) |
| 실제 내보내기 창 | [shot-export.png](out/design13/shot-export.png) — 어두운 native 제목 표시줄, 탭 버튼·필드·숫자 |
| 실제 새 프로젝트 창 | [shot-new-project.png](out/design13/shot-new-project.png) — 새 팔레트, 입력칸 및 버튼. 생성하지 않고 닫음 |
| 실제 마커 이름 창 | [shot-marker.png](out/design13/shot-marker.png) — AlertWindow 배경/입력칸/선택/버튼 |
| 설정 컴포넌트 | [settings-form.png](out/design13/test-shots/settings-form.png) — 기존 테스트의 빈 카메라 목록 주입 사용. 실제 장치 연결 없이 캡처; 창 밖/투명 영역은 검게 보임 |
| 오디오 설정 페이지 | [500px 1쪽](out/design13/test-shots/audio/audio-500-page1.png), [684px 2쪽](out/design13/test-shots/audio/audio-684-page2.png) — 기존 페이지 배치 테스트에서 Recorder 룩앤필로 캡처 |

시안 PNG의 실제 크기는 **2880×1800**이다. [비교 도구](out/design13/compare.py)에서 시안만 1440×900 논리 크기로 축소하고, 실제 캡처는 그대로 나란히 배치했다. 두 비교 이미지를 직접 열어 확인했다.

마이크 카드가 보이도록 격리 설정의 `physicalInputs`만 `[0]`으로 지정했다. ASIO 장치 이름은 빈 값이며 장치를 연결하지 않았다. 변경 전 격리 설정은 [settings-before-mic.xml](out/design13/settings-before-mic.xml)에 보관했다. 데모 사본에는 GUI로 `마커 1`(226445 sample)을 추가했고, 음소거/솔로 표시를 확인했다. 최종 사본은 마이크 음소거 켜짐·솔로 꺼짐이다. 원본 미디어는 수정하지 않았다.

### 색 픽셀 확인

[pixel-audit.json](out/design13/pixel-audit.json)의 **13/13 지점 일치**. 이는 지정된 평면 색 위치의 검사이며 전체 화면 픽셀 일치라는 뜻은 아니다.

| 실제 캡처 부위 | 관측값 |
| --- | --- |
| 배경 / 트랙 행 / 트랜스포트 | `#1e1e1e` / `#262626` / `#2d2d2d` |
| 숫자 필드 / 툴바 아래 경계 | `#232323` / `#3a3a3a` |
| 스냅 버튼 / 선택 클립 링 | 각각 `#4a9df0` |
| 오디오 클립 / 파형 | `#243f33` / `#4ec27a` |
| 재생헤드 | `#e0443a` |
| 음소거 / 솔로 활성 | `#e5342a` / `#f2c53d` |
| 하단바 | `#262626` |

### 시안 대비 남은 차이

| 단위 | 시안 | 실제 캡처 / 남은 차이 |
| --- | --- | --- |
| 영상 클립 색 | HTML 최종 규칙 `#43407a → #332f5e` | 명시 지시에 따라 `#3b3560 → #332f5e`; 위쪽이 더 어둡다 |
| 창 영역 | 1440×900 가상 창, 제목 표시줄 34px | 1440×900 native 창, 클라이언트 1424×861. OS 테두리/여백과 기존 ON 아이콘·창 제목이 남는다 |
| 상단 상태/간격 | 안내 없는 상태줄 | 제공 데모를 열면 기존 복구 안내가 표시되어 26px 안내 행이 추가된다. 안내 문구/복구 동작은 수정하지 않았다 |
| 캠 카드 내용 | 연결된 카메라 장치명·영상·해상도/시간 오버레이 | 장치 없는 격리 실행이므로 검정 영역과 연결 안내, 기존 `cameraCaption()` 문구. 실제 영상·장치 정보로 채운 캡처는 아니다 |
| 녹화 탭 구성 | 제공 시안은 타임라인 탭 | 기존 녹화 탭의 큰 캠 카드와 마이크 스트립 구성을 유지. 시안과 같은 내용 밀도로 만들지 않았다 |
| 트랙/클립 수 | 캠2·추가 마이크·가져온 오디오·빈 구간 포함 | 데모에는 캠1/마이크1 두 트랙, 각각 테이크 3개. 추가 트랙/빈 구간을 만들지 않았다 |
| 트랙 간격 | 시안 헤더 200px, 행 약 62px, 룰러 28px | 기존 좌표 기준 210px / 72px / 30px 유지. 사이드바 270px, 기존 6px 옆 공간 유지 |
| 파형 | 크게 보이는 스테레오/모노 예시 | 제공 데모에서는 낮은 수평선으로 보인다. 색은 실제 픽셀로 확인했고 파형 계산은 변경하지 않았다 |
| 제목·버튼·문구 | 짧은 시각, 버튼 장식 아이콘/키캡, 상시 보조 설명 | 기존 전체 시각 문자열은 말줄임표 처리. 기존 버튼 문구/툴팁/명령 구성 유지; 시안의 모든 장식 아이콘·키캡·보조 문장을 새로 넣지는 않았다 |
| 글꼴/그림자 | CSS Noto Sans KR 우선, 카드 그림자 | 요청한 Malgun Gothic 사용으로 글자 폭·두께가 다르다. 카드 그림자 및 CSS 래스터 경계는 동일하지 않다 |
| 하단 정보 | 연결 장치, 마이크 무장 수, 인코드/CPU/GPU 텍스트 | 실제 세션의 장치 없음/캠 캡션/상태 표시와 기존 앱 정보·업데이트·조건부 재시도 버튼. 성능 수치는 만들지 않았다 |
| 녹화 중 상태 | REC 배지·녹화 중 클립/활성 정지 상태 | 실제 하드웨어 녹화 캡처는 하지 않았다. 기존 합성 녹화 UI/레이아웃 테스트가 통과했다 |

## 빌드 및 테스트

`cmake`가 PATH에 없어 `C:\Users\claude\tools\cmake\bin\cmake.exe`를 사용했다.

```powershell
cmake --preset local
cmake --build --preset local-release --target Recorder --target RecorderTests -- -m:1 -nr:false -v:m -nologo
$env:RECORDER_TEST_NO_HARDWARE = '1'
build\vs2022\recorder\RecorderTests_artefacts\Release\RecorderTests.exe --suite all
```

- 구성: 성공. [configure.log](out/design13/configure.log)
- 최종 빌드: 두 타깃 성공, exit 0. [build.log](out/design13/build.log)
- 최초 `-m -v:m -nologo` 실행은 상속 환경의 `PATH`/`Path` 중복으로 MSBuild가 CL.exe를 시작하지 못했다. 환경 키를 빌드 자식 프로세스에서만 대문자로 정규화하고 `-m:1 -nr:false`로 재실행했다. 시스템 환경은 수정하지 않았다. [초기 로그](out/design13/build-initial.log)
- 컴파일 중 확인한 JUCE의 제거된 문자열 폭 API는 `GlyphArrangement::getStringWidth`로 교체했고, 테스트의 직접 LiveMix 룩앤필 사용도 교체했다.
- 첫 전체 실행: **755 passed / 3 failed**. 옛 가져오기 위치 검사, 영상 스크럽 픽셀 검사, 링크 이동 검사였다. [첫 전체 로그](out/design13/tests-all-first.log)
- 옛 위치 검사를 새 상단 배치로 맞추고, 기존 타임라인 폭과 정수 픽셀 재생헤드를 복원한 뒤 전체 **758/0**을 확인했다.
- 마지막 시각 보정 후 최종 전체 실행: **758 passed / 0 failed**, 49개 `N passed, N failed` 요약 행 합계, `RecorderTests: all suites passed`, **exit 0**. [최종 전체 로그](out/design13/tests-all.log), [집계 JSON](out/design13/test-summary.json), [종료 코드](out/design13/tests-exit.txt)
- 별도 JSON 형식의 `EditPropertyTests`도 **PASS**, seed 909, 1,000회, 불변식 및 undo/redo 해시 검증 통과. 이 1,000회를 위 758에 더하지 않았다.
- 주요 UI 결과: `shortcut-exceptions` **24/0**, `timeline-ux` **51/0**. 파형/마커 픽셀, 음소거·솔로 대비, 스크럽, 링크 이동, 기존 여러 창 크기 레이아웃 검사를 포함한다.
- `git diff --check` 통과. livemix/site/ProductIdentity/릴리스 노트 경로의 diff 없음.
- 최종 실제 앱: WM_CLOSE로 정상 창 닫기, 저장/정리 완료 후 **exit 0**. [app-exit.txt](out/design13/app-exit.txt)
- 종료 후 데모 폴더 아래 생성된 빈 `project.writer.lock`, `journal/takes.writer.lock` 두 파일만 확인해 제거했다. 재귀 삭제·프로세스 강제 종료는 하지 않았다.

## 남은 위험 / 확인 범위

1. 캡처보드·FlexASIO를 열지 않았다. 실제 장치 녹화, 실제 영상 프리뷰와 REC 배지의 동시 표시, 장치 연결 시 긴 하단 장치명은 하드웨어 환경에서 확인하지 않았다. 긴 하단 내용은 말줄임표와 툴팁으로 표시한다.
2. 제공 데모에서 복구 안내가 계속 표시되는 원인은 이번 디자인 작업에서 수정하지 않았다. 미디어/복구 로직을 바꿔 안내를 숨기지 않았다.
3. 실제 스크린샷은 이 Windows 환경의 1440×900 native 창에 한정된다. 다른 OS 버전에서 DWM 색 속성이 지원되지 않으면 OS 제목 표시줄이 남을 수 있다. 다른 DPI와 폰트 대체의 실제 창 캡처는 수행하지 않았다.
4. 설정 스크린샷은 빈 카메라 목록을 주입한 기존 테스트 컴포넌트이며 실물 설정창의 장치 연결 검증으로 해석하면 안 된다. 실제 새 프로젝트·마커·내보내기 창은 별도로 PrintWindow로 확인했다.

최종 소스 파일과 이 OUT만 커밋 검토 대상이다. `out/design13/`은 실행·비교 증거 및 격리 데모 자료이며, 기존 `docs/design/`의 커밋 여부는 사용자가 결정한다.
