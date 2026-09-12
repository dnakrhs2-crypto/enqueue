fix-014-keys 작업 결과 — 2026-09-10

작업 트리: `C:\Users\claude\gocue-rec-app`, 브랜치 `fix-014-keys`. 실제 HEAD는 `92138fb`이며 `integrate-014`와의 merge-base는 `3fd979b`이다. 실행기가 제공한 OUT 경로는 환경변수에서 발견되지 않았고 추가 응답도 없어, 작업 초기에 안내한 이 경로에 기록했다.

파일 수정·빌드·테스트만 수행했다. 커밋·push·release.py 실행은 없으며 ProductIdentity, 버전, 릴리스 노트, site, MediaIndex, TimelineView*, TimelineInteraction.h, RecordView.cpp는 수정하지 않았다. 요청 본문에 명시된 CameraSettingsPanel.cpp와 CrashHandler.*는 예외 경계 구현을 위해 수정했다.

수정 내용

| 위치 | 변경 내용 |
| --- | --- |
| `recorder/src/ui/MainComponent.cpp:158`, `MainComponent.h:21` | `routeShortcut`으로 키 처리를 통합했다. 메인·소유 최상위 창의 현재 포커스에 동일한 리스너를 설치하고, 텍스트 입력과 단축키 캡처 컨트롤은 제외한다. Stop은 버튼 활성 상태 대신 실제 녹화 상태를 확인한다. |
| `recorder/src/ui/ExportDialog.cpp:15`, `ExportDialog.h:20`, `ProjectDialogs.cpp:13` | 내보내기·설정·프로젝트 창에서 공통 라우터로 전달한다. 내보내기 창 소속 여부와 미리 듣기 상태를 공개하여 별도 최상위 창도 판단한다. |
| `recorder/src/ui/MainComponent.cpp:102`, `MainComponent.cpp:168` | 설정 표시 중 녹화 시작과 다른 전역 명령을 차단하고 녹화 정지를 허용한다. 내보내기 미리 듣기·준비 중에도 정지 외 전역 명령을 제한하여 F9와 Space가 경쟁 ASIO 출력을 시작하지 못하게 한다. 기존 내보내기 중단·완료 대기 흐름은 유지한다. |
| `recorder/src/ui/ProjectDialogs.cpp:71` | 기존 `showSettings()`의 `session.busy()` 검사는 유지했다. 정상 UI에서는 녹화·준비·마무리 중 설정을 열 수 없다. 테스트는 먼저 설정을 표시한 뒤 합성 녹화를 시작하여 설정 표시 + 녹화 상태를 의도적으로 만들었다. |
| `recorder/src/ui/MainComponent.cpp:204`, `MainComponent.cpp:215`, `MainComponent.cpp:224`, `ProjectDialogs.cpp:136` | 파일/설정 작업 시작과 future 회수를 예외 경계로 감쌌다. 파일 작업의 스냅샷·대상 경로·열기 여부를 별도로 보존하여 get 예외에도 실패한 체크포인트를 성공 처리하지 않는다. 실패 시 recovering/fileWork/pending 상태와 저장 후 콜백을 정리하고 배너를 표시한다. 저장 공간 조회의 시작/회수 예외도 처리한다. |
| `recorder/src/app/RecorderSession.cpp:208`, `RecorderSession.cpp:220`, `RecorderSession.cpp:511`, `RecorderSession.cpp:660` | deviceWork 시작/회수 실패 시 configuring과 준비 문구를 해제하고 부분 적용된 오디오 장치를 닫으며 실패 콜백·오류를 게시한다. 종료 작업 시작/회수 실패 시 남은 카메라 정리와 메시지 스레드의 오디오 종료를 수행한다. |
| `recorder/src/app/RecorderSession.cpp:424`, `RecorderSession.h:30` | `pause()`가 실패 Result를 반환한다. 실제 TimelineTransport 명령 큐 포화 예외를 잡고 오류를 보존하며 재생 클라이언트를 분리한다. TimelineTransport 파일은 수정하지 않았다. |
| `recorder/src/record/TakeController.cpp:437`, `TakeController.cpp:449`, `TakeController.cpp:812` | prepare 시작 실패는 구조 잠금 해제 + idle 복귀. 마무리/저장 시작 실패와 future 예외는 입력 분리, 오디오 drain, 영상 마무리, 저널 종료를 수행하고 partialFailure로 전환한다. 실패한 체크포인트와 부분 테이크를 문서에 반영하며 저장된 자료를 성공으로 표시하지 않는다. |
| `recorder/src/ui/CameraSettingsPanel.cpp:23`, `CameraSettingsPanel.cpp:54` | 카메라 열거 시작 실패와 표준/비표준 future 예외를 상태 문구로 표시한다. 실패 시 scanning이 남지 않는다. 테스트에서는 열거 함수를 주입하여 실제 카메라를 열거하거나 열지 않는다. |
| `recorder/CMakeLists.txt:165`, `recorder/src/Main.cpp:57`, `recorder/src/support/CrashHandler.cpp:51` | Recorder 및 테스트에 `JUCE_CATCH_UNHANDLED_EXCEPTIONS=1`을 적용했다. 앱 `unhandledException`은 공유 reporter를 호출한다. 기본 경로는 `ProductIdentity::settingsDirectory()/crash`, 파일명은 `Recorder-<ver>-<time>-<pid>-exception.txt`이며 time 부분에 밀리초와 일련번호를 넣어 연속 예외 파일을 구분한다. what, 파일, 행, 스택을 기록하고 reporter/알림 재진입도 막는다. 기록 실패를 성공으로 알리지 않는다. |
| `recorder/src/ui/MainComponent.cpp:90`, `MainComponent.cpp:127`, `recorder/src/Main.cpp:120` | 별도 예외 배너가 장치/상태 갱신에 가려지지 않게 했다. 녹화·캡처·마무리 중에는 “녹화를 정지하고 프로젝트를 저장하세요”를 붙인다. 초기화 중 기록된 예외도 메인 창 생성 후 표시한다. 표시된 비치명 예외 보고는 seen 처리하여 다음 실행에서 비정상 종료로 잘못 안내하지 않는다. |

단축키 허용 조건

| 상태 | 처리 |
| --- | --- |
| 소유하지 않은 창 | 라우팅하지 않음 |
| TextEditor 또는 그 자손에 포커스 | 전역 단축키 실행 안 함 |
| 단축키 캡처 버튼에 포커스 | 캡처 위젯이 키를 처리함; 다른 설정 컨트롤은 Stop 라우팅 가능 |
| 설정 창 표시 | Record Stop 허용, Record Start 및 다른 전역 명령 차단 |
| 내보내기 미리 듣기/준비 | Record Stop 외 전역 명령 차단; 녹화·타임라인 재생이 ASIO 출력을 가져가지 않음 |
| 일반 상태 | 기존 녹화·재생·편집 가능 조건과 키 반복 억제 적용 |

추가 테스트: 총 17개

- `recorder/tests/ShortcutExceptionTests.cpp:213` 이하 새 suite 13개: 실제 JUCE ComponentPeer/포커스 경로의 내보내기 F10, 설정 표시 상태의 메인/설정 F10 및 F9 무시, 텍스트/캡처/외부 창 제외, 미리 듣기 F9/Space 제한, 녹화 중 오류 배너 유지, 장치/파일/설정/카메라의 시작·future 예외, 종료 자원 정리, 실제 transport 큐 포화, 메시지 큐 표준·비표준 예외 보고와 후속 콜백 실행, 기록 실패·재진입.
- `recorder/tests/TakeControllerTests.cpp:117` 이하 4개: prepare 스레드 시작 실패 후 idle/잠금 복원 및 재녹화, finalizer 시작 실패, checkpoint 시작 실패, 실제 메타데이터 쓰기 실패가 future.get 예외로 회수된 뒤 잠금·저널 해제와 미저장 상태 보존.
- `recorder/tests/TestMain.cpp:18`에서 실제 UI 구현을 기존 테스트 번역 단위에 포함했다. CMake는 컴파일 정의만 바꾸고 별도 main/타깃을 추가하지 않았다. 합성 오디오 및 가짜 영상 스트림 생성자 주입과 작업 시작 실패 주입은 테스트용이며 기본 앱 동작에서는 사용하지 않는다.

빌드·검증

`cmake`가 PATH에 없어 `C:\Users\claude\tools\cmake\bin\cmake.exe`를 사용했다.

```powershell
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --preset local
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --build --preset local-release --target Recorder --target RecorderTests -- '-m:1' '-nr:false' '-v:m' '-nologo'
& './build/vs2022/recorder/RecorderTests_artefacts/Release/RecorderTests.exe'
```

처음 요청된 병렬 빌드에서는 MSB3491 중간 tlog 파일 접근 오류가 발생하여 지정된 직렬 빌드 대안을 사용했다. PowerShell에서 `-v:m`이 분리되지 않도록 인수를 따옴표로 전달했다. 빌드에는 기존 C4324 정렬/패딩 경고가 있다.

최종 검증 결과 (2026-09-10 14:42 KST): Recorder·RecorderTests 빌드 성공(종료 코드 0). 마지막 ASIO 미리 듣기 게이트 보완을 포함한 전체 RecorderTests는 **49 suite 요약행, 572건 통과, 0건 실패, 종료 코드 0**이다. 새 shortcut-exceptions suite 13/13, TakeControllerTests 13/13(기존 9 + 신규 4)이 포함된다. 별도 EditPropertyTests도 seed=909, iterations=1000에서 PASS이며 committed=692, rejected=163, noops=145이다. `git diff --check` 및 금지된 소스/제품 식별자 경로의 무변경 확인도 통과했다. 총 작업 시간은 약 41분이다.

결과 수의 기준: `N passed, M failed` 요약행을 합산한다. 사용자 제시 기준 48 suite/554건과 현재 HEAD 사이에는 이 작업 전부터 `EditJournalTests`의 provisional rate/marker 회귀 테스트 1개가 더 있다(`git diff integrate-014 HEAD -- recorder/tests/EditJournalTests.cpp`로 확인). 이 세션의 신규 테스트는 17개이며, 원래 복합 suite와 WAV 재호출은 러너 구성대로 유지했다. 별도의 기준선 재빌드는 수행하지 않았다.

검증 로그: `build/fix-014-keys-build.log`, `build/fix-014-keys-focused.log`, `build/fix-014-keys-take.log`, `build/fix-014-keys-all-tests.log`, `build/fix-014-keys-all-tests-stderr.log`, `build/fix-014-keys-all-tests.exit.txt`.

남은 위험·검증 한계

- 캡처보드·FlexASIO·실제 카메라·NVENC는 열지 않았다. 실제 드라이버의 종료 지연과 OS/장치 조합별 포커스 동작은 이번 검증에 포함되지 않는다.
- 메시지 큐 테스트는 장치를 시작하지 않는 JUCE 테스트 애플리케이션에서 실제 큐 → unhandledException → 공유 CrashHandler 경로를 검증했다. 제품 Recorder.exe의 일반 시작은 실행하지 않았다. 배너와 녹화 안내는 실제 MainComponent로 별도 검증했다. 보고 파일은 격리된 임시 디렉터리에서 내용/존재/서로 다른 이름을 확인하고 정리했다.
- 최후 예외 경계는 임의 콜백의 모델 변경을 되돌리지 않는다. 국소 작업 경계에서 복원한 상태 외의 예기치 않은 불일치는 남을 수 있어 녹화 정지·저장 안내를 유지한다. 접근 위반이나 std::terminate는 기존 치명 크래시 경로이다.
- 스레드 생성 실패 시 자원 정리의 동기 대체 경로는 잠시 UI를 지연시킬 수 있다. 보고서의 파일/행은 JUCE가 예외를 잡은 위치이고 스택은 보고 시점의 스택이다.
