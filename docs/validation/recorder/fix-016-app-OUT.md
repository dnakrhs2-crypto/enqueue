# fix-016-app 작업 결과

작업 트리: `C:\Users\claude\gocue-rec-app`, 브랜치 `fix-016-app`, 기준 `4c71826`.
파일 수정·빌드·테스트만 수행했다. 커밋, push, release.py 실행, ProductIdentity/버전/릴리스 노트/site 수정은 하지 않았다.
병렬 세션 소스는 수정하지 않았다. `RecorderSession.cpp`의 변경은 기존 `addMarker()` 본문과 바로 옆의 이름/시각 오버로드뿐이다.

## 결과

- `Recorder`, `RecorderTests`, `RecorderProbe` 빌드 통과.
- 기준 바이너리 전체 재실행: **685 passed, 0 failed**, exit 0.
- 최종 수정본 전체 실행: **695 passed, 1 failed**, exit 1. 추가한 회귀 테스트는 11개이다.
- 수치는 출력된 각 suite의 `N passed, N failed` 합계이다. 별도 형식으로 출력되는 `edit-property`도 PASS이며 seed 909, 1,000회 실행이다.
- 남은 실패는 수정 범위 밖 `recorder/tests/TimelineUxTests.cpp:647`의 옛 기본 키 전제이다. F10을 recordStart로 지정하면 중복이라고 검사하지만, 새 기본 정지 키는 Space이므로 F10은 유효하다. 전체 통과로 보고하지 않는다.
- 실제 `Recorder.exe --test-root <build 아래 격리 폴더>`의 메인 창을 열고 제목 `Recorder (가칭)`을 확인했다. 정상 창 닫기로 종료했으며 exit 0이었다. 캡처보드·FlexASIO는 열지 않았다.

## 수정 목록 (file:line)

| 파일 | 변경 |
| --- | --- |
| `recorder/src/app/RecorderShortcuts.h:12` | recordStop 기본값을 spacebar로 변경 |
| `recorder/src/app/RecorderSettings.cpp:93` | 저장된 F10/spacebar 조합 마이그레이션 |
| `recorder/src/app/RecorderSettings.cpp:116` | recordStop/playStop 두 명령만 키 중복 허용 |
| `recorder/src/ui/ShortcutKeys.h:14` | recording 인수에 따라 공유 키 명령 결정 |
| `recorder/src/ui/ShortcutSettingsPanel.cpp:8` | 공유 키 안내 문구와 3줄 높이 확보 |
| `recorder/src/app/RecorderSession.h:36` | 이름·시각을 받는 addMarker 선언 |
| `recorder/src/app/RecorderSession.cpp:473` | addMarker 오버로드 구현, 기존 진입점 유지 |
| `recorder/src/ui/MainComponent.h:27` | 시작 처리 API, 마커 창 수명/프로젝트 ID와 시작 상태 보관 |
| `recorder/src/ui/MainComponent.cpp:37` | 녹화 화면 버튼과 타임라인 마커 훅 연결 |
| `recorder/src/ui/MainComponent.cpp:63` | 프로젝트 교체 시 마커 창 폐기 |
| `recorder/src/ui/MainComponent.cpp:106` | 마커 오른쪽 진행 패널과 가져오기 버튼 간 공간 계산 |
| `recorder/src/ui/MainComponent.cpp:192` | 프로젝트가 없을 때 새 프로젝트 메뉴 안내 배너 |
| `recorder/src/ui/MainComponent.cpp:227` | 상태별 Space 라우팅, 텍스트/마커 창 입력 보호, M 연결 |
| `recorder/src/ui/MainComponent.cpp:258` | 포커스 이동 중 실제 눌린 키의 반복 억제 유지 |
| `recorder/src/ui/MainComponent.cpp:305` | 종료 시 마커 창 취소 및 시작 재표시 차단 |
| `recorder/src/ui/MainComponent.cpp:369` | 최근 프로젝트 열기 실패 후 시작 창 표시, 이후 장치 연결 |
| `recorder/src/ui/ProjectDialogs.cpp:43` | 새 프로젝트 폼에 기존 프로젝트 열기 버튼, Enter/Esc 처리 |
| `recorder/src/ui/ProjectDialogs.cpp:78` | 비동기 마커 이름 창 |
| `recorder/src/ui/ProjectDialogs.cpp:112` | 최초 프로젝트 선택과 실패 후 표시 정책 |
| `recorder/src/ui/ProjectDialogs.cpp:170` | 창 중앙 배치, 이름 포커스, 최근 상위 폴더 기본값 |
| `recorder/src/ui/RecordView.cpp:105` | 오디오 불러오기 버튼을 컨트롤 행 오른쪽으로 이동 |
| `recorder/src/Main.cpp:104` | test-root 단독 GUI와 기존 왕복 테스트 분리 |
| `recorder/src/Main.cpp:122` | 격리 GUI의 자동 프로젝트 창/장치/업데이터 제외 |
| `recorder/src/Main.cpp:144` | 일반 시작 시 새 프로젝트 창 정책 연결 |
| `recorder/tests/ShortcutExceptionTests.cpp:248` | 앱 회귀 9개 추가, 기존 F10 정지 테스트를 Space로 변경 |
| `recorder/tests/RecorderProjectTests.cpp:403` | 설정 검증·마이그레이션 회귀 2개 추가 |
| `recorder/tests/UiWiringTests.cpp:44` | 앱 가져오기 버튼의 새 위치와 진행 패널 여유 폭 검사 |
| `recorder/tests/LifecycleTests.cpp:71` | 단독 lifecycle 실행도 NO_HARDWARE 옵션으로 실제 ASIO 생성 차단 |

## 구현 방식

### 마커 창

`recordView.markerButton`, `timelineView.onAddMarkerRequested`, 전역 M은 모두 `MainComponent::promptMarker()`를 호출한다. 기존 타임라인의 툴바·우클릭·마커 패널은 이미 같은 훅으로 연결돼 있어 앱의 이름 창을 사용한다.

창을 만들기 전에 `session.recording() ? session.takeController().placementSample() + session.elapsed() : session.playhead()`를 값으로 캡처한다. 기본 이름은 현재 문서 마커 수 + 1인 `마커 N`이며 전체 선택/포커스를 준다. 확인/Enter는 캡처한 시각과 입력 이름을 저장한다. 공백뿐인 이름이면 기본 이름을 쓰며, 그 외 이름의 앞뒤 공백과 한글·특수문자는 그대로 보존한다. 취소/Esc는 추가하지 않는다.

`AlertWindow::enterModalState(true, ModalCallbackFunction, true)`를 사용한다. runModalLoop는 쓰지 않는다. SafePointer로 중복 창과 늦은 콜백을 차단하고, 프로젝트 ID 변경/교체 시작/종료/소멸 시 취소한다. 텍스트 포커스 경로와 열린 마커 창에서 전역 단축키를 거부한다. 녹화 중 확인된 마커는 기존 recordedMarkers에 넣고 기존 세션 tick이 테이크 배치 후 반영한다.

### 시작 창

일반 실행에서 명시적 openPath가 없고 최근 프로젝트도 없으면 새 프로젝트 폼을 즉시 표시한다. 최근 프로젝트 열기/복구/채택 후에도 프로젝트 파일이 없으면 폼을 표시한다. 폼은 비모달 DocumentWindow이므로 장치 연결을 기다리는 모달 루프가 없으며, 장치 연결 중에도 표시된다. 연결 중 생성 버튼을 누르면 폼 안에 대기 안내를 표시하고 폼을 유지한다.

폼은 메인 컴포넌트 중앙에 배치하고 이름 칸에 포커스를 준다. 최근 프로젝트 파일의 부모 디렉터리가 존재하면 폴더 칸에 넣고, 없으면 비운다. 기존 프로젝트 열기 버튼은 기존 chooseOpen 경로를 재사용한다. 취소하면 프로젝트 없이 진행하며 readyToRecord는 계속 false이고 배너는 `프로젝트 > 새 프로젝트`를 안내한다.

데모와 자동화는 Main.cpp의 기존 조기 반환 경로를 유지한다. `--test-root` 단독 실행은 격리 GUI 확인용으로 메인 창만 표시하며 자동 프로젝트 창·자동 장치 연결·업데이터·절전 복귀 장치 연결을 실행하지 않는다. `--test-root ... --new-project ...`는 기존처럼 창 없는 왕복 테스트이다. 이 구분으로 B2의 테스트 모드 자동 창 제외와 실제 메인 창 확인 요구를 함께 처리했다.

### Space와 설정

기본 recordStop/playStop 모두 spacebar이다. 두 키가 일치할 때 녹화 중이면 recordStop, 아니면 playStop을 반환한다. 사용자 지정한 서로 다른 키는 그대로 동작한다. heldShortcut은 반복 입력을 억제하고, 실제 키가 눌린 상태의 포커스 이동에서도 유지한다.

decode는 정확히 `recordStop == F10 && playStop == spacebar`인 옛 기본 조합만 바꾼다. F8/spacebar, F10/P, 사용자 지정 공유 키는 유지한다. encode의 기존 직렬화 경로는 변경된 설정값을 저장한다. 검증은 recordStop/playStop 사이의 중복만 허용하고 세 번째 명령의 중복은 거부한다. 기존 MainComponent의 툴팁 갱신을 유지해 `녹화 정지 · spacebar`, `재생 / 정지 · spacebar`를 표시한다.

### 배치와 글자

상단의 오디오 불러오기 버튼을 녹화 컨트롤 행 오른쪽으로 옮겨 프로젝트 이름 라벨에 148px를 돌려줬다. 진행/취소 패널은 마커 버튼 오른쪽과 오디오 불러오기 버튼 왼쪽 사이에 놓는다. 960/1180/1600px 창 너비에서 겹침 없이 400px 이상의 패널 공간을 테스트했다.

담당 파일의 한글·특수문자 리터럴을 확인했고, 직접 JUCE String으로 전달하는 UI 리터럴은 ko()/fromUTF8 경로를 사용한다. 기존 String 연결은 유지했다. 앱 정보는 `RecorderUpdater::aboutText()` → `ProductIdentity::displayName()`이며 회귀 테스트로 확인했다. WinSparkle은 `RecorderUpdater.cpp:75`에서 displayName().toWideCharPointer()로 넘긴다. 해당 파일과 ProductIdentity는 수정하지 않았고 WinSparkle 실행 자체는 테스트하지 않았다.

## 검증과 증거

- 구성: `cmake --preset local` 통과. 셸 PATH에 cmake가 없어 `C:\Users\claude\tools\cmake\bin\cmake.exe`로 실행했다.
- 빌드: 지정한 Recorder/RecorderTests/RecorderProbe 타깃 모두 통과. PowerShell에서는 `-v:m` 등을 따옴표로 전달했다. 최초 tlog 접근 오류에서 `-m:1 -nr:false`를 사용했고, 중복 PATH/Path 환경 항목을 현재 빌드 프로세스에서 정리한 뒤 최종 빌드는 `-m -nr:false -v:m -nologo`로 통과했다.
- 모든 전체 테스트에 `RECORDER_TEST_NO_HARDWARE=1` 사용. 테스트용 ASIO owner가 실제 드라이버 생성 전에 차단한다.
- 기준선 최초 전체 실행은 PASS 행 92개 후 자료 내보내기 테스트 중 접근 위반 `0xC0000005`로 종료했다. 기준 바이너리를 보존해 전체 재실행했고 **685/0, exit 0**을 확인했다. 첫 오류는 해결했다고 주장하지 않는다.
- 최종 전체 실행은 **695/1, exit 1**. shortcut-exceptions **24/0**, project-roundtrip **36/0**, ui-wiring **51/0**, lifecycle **18/0**. 신규 앱/설정 회귀는 모두 통과했다.
- 수정본 첫 전체 실행의 옛 버튼 위치 실패 4건은 앱 영역 UiWiringTests를 새 배치에 맞춰 수정했고 최종 전체 실행에서 통과했다.
- `git diff --check` 통과. RecorderSession.cpp의 diff는 addMarker 한 곳만 확인했다.
- 실제 앱 격리 실행의 창 제목은 `Recorder (가칭)`. PrintWindow로 렌더링 캡처 후 이미지로 확인했다. 정상 창 닫기와 exit 0 확인. 일반 화면 복사 API는 데스크톱 핸들 오류로 실패했다.
- 실제 `--test-root ... --new-project ...` 실행도 exit 0, `roundtrip.json`의 status PASS / byteIdentical true를 확인했다.

로그:

- [기준 빌드](build/fix-016-app-baseline-build.log)
- [기준 첫 전체 실행](build/fix-016-app-baseline-tests.log)
- [기준 전체 재실행](build/fix-016-app-baseline-retry-tests.log)
- [최종 빌드](build/fix-016-app-build.log)
- [최종 전체 테스트](build/fix-016-app-final-tests.log)
- [앱 회귀](build/fix-016-app-shortcut-tests.log)
- [설정 회귀](build/fix-016-app-settings-tests.log)
- [실제 메인 창](build/fix-016-app-ui/actual-main-print.png)
- [마커 이름 창](build/fix-016-app-ui/marker-prompt.png)
- [새 프로젝트 폼](build/fix-016-app-ui/new-project-form.png)

## 병렬 세션 인계 / 남은 위험

1. **fix-016-timeline: `recorder/src/ui/TimelineView.cpp:346` fallback 연결 필요.** 공유 함수는 `shortcutCommand(shortcuts, key, origin, bool recording)`을 지원한다. `!onGlobalKey` 경로에서 네 번째 인수에 녹화 상태를 전달하고 recordStop/playStop 명령을 해당 동작에 연결해야 한다. 현재 원본 fallback은 split/marker만 처리한다. 이 파일은 사용자 지정 수정 금지 영역이므로 건드리지 않았다. 실제 MainComponent에 연결된 타임라인은 onGlobalKey를 통해 상태별 Space 라우팅을 사용한다.
2. **fix-016-timeline: `recorder/tests/TimelineUxTests.cpp:647`의 중복 키 fixture 한 줄 필요.** `legacy.setValue("shortcutRecordStart", "F10")`를 `legacy.setValue("shortcutRecordStart", "spacebar")`로 바꾸면 새 기본값에 대한 중복 검사가 된다. 현재 전체 실행의 유일한 실패이다. 파일 범위 예외 승인을 요청했지만 답변이 없어 수정하지 않았다.
3. 기준선 첫 자료 내보내기 접근 위반은 재실행과 수정본 전체 실행에서 재현되지 않았다. 이번 앱 수정으로 해결된 것으로 해석하면 안 된다.
4. 실물 캡처/ASIO 녹화와 일반 실행의 실제 장치 연결은 사용자 지시에 따라 실행하지 않았다. 녹화 중 마커·Space 검증은 합성 오디오/비디오를 사용했다. 새 프로젝트 창의 실제 장치 연결 동시성은 진행 중인 합성 설정 future로 확인했다.
