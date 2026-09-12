fix first

검토 범위: `integrate-016`, `4f43e8c..6fb5e57` 및 필요한 호출 경로. **기존 4건 중 3건은 닫혔지만, 구버전 종료 경쟁 P2 1건은 미해소입니다.** 신규 P3 경계도 1건 있습니다.

1. **[P2·미해소] 구버전이 살아 있어도 설치와 자동 실행을 허용합니다.**  
   [installer/Recorder.iss:121](C:/Users/claude/gocue-rec/installer/Recorder.iss:121)의 이름 변경은 프로세스를 종료하지 않습니다. 이름 변경마저 실패해도 [111행](C:/Users/claude/gocue-rec/installer/Recorder.iss:111)의 `Result := ''`가 유지됩니다. 따라서 기존 `Recorder.exe` 잔존을 보장해서 막지 못하며, 설치가 완료되면 [93행](C:/Users/claude/gocue-rec/installer/Recorder.iss:93)의 `[Run]`은 구버전 생존 여부와 무관하게 Tally를 실행합니다.

   단일 실행 제한도 이 상황을 막지 못합니다. [recorder/src/Main.cpp:54](C:/Users/claude/gocue-rec/recorder/src/Main.cpp:54)는 표시 이름을 반환하고, [JUCE의 잠금 생성:97](C:/Users/claude/JUCE/modules/juce_events/messages/juce_ApplicationBase.cpp:97)은 그 이름을 사용하므로 `Recorder (가칭)`과 `Tally`의 잠금이 다릅니다. 새 앱은 [MainComponent.h:38](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.h:38)에서 저장된 장치를 다시 연결합니다. **구버전이 장치를 놓지 않은 채 남으면 장치 연결 충돌이 가능한 호출 경로입니다.** 실행 재현 결과라는 뜻은 아닙니다.

   타임아웃 때 설치를 중단하거나, 구버전의 실제 종료를 확인한 뒤 설치·재실행을 허용해야 닫힙니다. `.old`로 이름을 바꾼 사실을 종료 확인으로 사용할 수 없습니다. 비어 있지 않은 오류 반환으로 설치를 중단하는 것은 `PrepareToInstall`의 공식 계약입니다. [Inno 이벤트 문서](https://jrsoftware.org/ishelp/topic_scriptevents.htm)

2. **[P3·신규] 선언된 EXE 자체에 대한 심볼릭 링크 검사가 약해졌습니다.**  
   [tools/release.py:438](C:/Users/claude/gocue-rec/tools/release.py:438)에서 먼저 EXE를 resolve하고 그 부모를 순회합니다. 선언 경로의 `Tally.exe`가 **다른 디렉터리의 `Tally.exe`를 가리키는 링크**라면 원래 링크가 순회 대상에서 사라져 [446행](C:/Users/claude/gocue-rec/tools/release.py:446)의 거부 검사를 우회합니다. 같은 빌드 트리 안의 대상이면 [423행](C:/Users/claude/gocue-rec/tools/release.py:423)도 통과할 수 있습니다. 링크 거부 정책을 유지하려면 resolve 전 선언 경로도 검사해야 합니다. 현재 확인한 빌드의 `Tally.exe`는 일반 파일이므로 정상 패키지의 차단 사유로 보지는 않습니다.

각 1차 지적의 판정은 다음과 같습니다.

| 1차 지적 | 판정과 근거 |
|---|---|
| P1 옛 EXE 혼입 | **닫힘.** [release.py:451](C:/Users/claude/gocue-rec/tools/release.py:451)은 선언된 EXE만 복사하고, [validate_release.py:155](C:/Users/claude/gocue-rec/tools/recorder/validate_release.py:155)은 manifest에 포함된 추가 EXE까지 거부합니다. [새 회귀:188](C:/Users/claude/gocue-rec/tools/recorder/tests/test_release_recorder.py:188)은 manifest 재생성 후에도 거부되는 경로를 검증합니다. |
| P2 옛 바로가기 | **기본 설치 경로에서 닫힘.** [Recorder.iss:83](C:/Users/claude/gocue-rec/installer/Recorder.iss:83)의 삭제 대상은 0.1.7 기본 그룹·바탕화면 이름과 일치합니다. `PrivilegesRequired=lowest`에서는 auto 상수가 사용자 경로로 매핑됩니다. [Inno 상수 문서](https://jrsoftware.org/ishelp/topic_consts.htm) |
| P2 실행 중 삭제 경쟁 | **미해소.** 위 P2 참조. 재시도는 짧은 종료 지연을 흡수하지만, 실패 후 계속 진행하는 분기가 남습니다. |
| P2 옛 notes URL | **사이트 배포 경로에서 닫힘.** [notes.html:3](C:/Users/claude/gocue-rec/site/recorder/notes.html:3)의 refresh·canonical·수동 링크가 일치합니다. 목적지 HTML은 [release.py:315](C:/Users/claude/gocue-rec/tools/release.py:315)에서 생성하므로 저장소에 정적 `site/tally/notes.html`이 없는 것은 문제가 아닙니다. |

요청한 추가 확인 결과입니다.

- **정상 payload·Windows 경로 비교:** 필터는 `.exe`에만 적용되어 DLL 복사에 영향이 없습니다. Windows `Path` 비교는 대소문자를 구분하지 않습니다. 순회 중 발견한 링크는 EXE 필터보다 먼저 거부됩니다. 다만 선언 EXE 자체의 링크는 위 P3 예외입니다. [Python pathlib 문서](https://docs.python.org/3/library/pathlib.html#general-properties)
- **fixture 충돌 없음:** [candidate():74](C:/Users/claude/gocue-rec/tools/recorder/tests/test_release_recorder.py:74)는 identity에서 얻은 `Tally.exe`와 DLL들을 payload에 만들고, installer는 bundle 루트에 만듭니다. 새 검사 대상에 installer가 섞이지 않습니다.
- **Inno 문법·인코딩:** [Recorder.iss:106](C:/Users/claude/gocue-rec/installer/Recorder.iss:106)의 이벤트 시그니처와 `Sleep`·`RenameFile`·`DeleteFile`·`Log` 호출에서 6.5 호환성을 깨는 문법은 찾지 못했습니다. 실제 ISCC 컴파일은 수행하지 않았습니다. 파일 시작 바이트는 **`EF BB BF`**, UTF-8 BOM이 유지되어 있습니다. [이벤트 계약](https://jrsoftware.org/ishelp/topic_scriptevents.htm), [내장 함수 계약](https://jrsoftware.org/ishelp/topic_scriptfunctions.htm)
- **대기 시간:** [Recorder.iss:115](C:/Users/claude/gocue-rec/installer/Recorder.iss:115)은 마지막 실패 뒤에도 쉬므로 최대 **31 × 500ms = 15.5초에 파일 작업 시간이 더해집니다.** WinSparkle은 설치 완료를 기다리지 않고 실행 직후 종료를 요청하므로 이 대기 자체에서 순환 대기는 확인되지 않습니다. 문제는 대기 종료 후에도 종료 확인 없이 진행한다는 점입니다. [WinSparkle 0.9.4 실행 순서](https://raw.githubusercontent.com/vslavik/winsparkle/v0.9.4/src/ui.cpp)
- **사용자가 삭제한 바탕화면 바로가기:** [Recorder.iss:69](C:/Users/claude/gocue-rec/installer/Recorder.iss:69)는 파일 존재 여부가 아닌 `desktopicon` task를 따릅니다. 이전에 선택했던 사용자가 `.lnk`만 삭제했다면 업데이트에서 `Tally.lnk`가 생길 수 있습니다. 이는 이번 삭제 구문의 신규 결함이 아니라 기존 task 복원 동작입니다. 쿠팡 링크의 별도 보호 조건은 유지됩니다. [UsePreviousTasks 문서](https://jrsoftware.org/ishelp/topic_setup_useprevioustasks.htm)

`git diff --check 4f43e8c HEAD`는 통과했고 작업 트리는 깨끗합니다. 테스트·빌드·설치 실행과 파일 수정은 하지 않았습니다. Python 33 tests OK·skipped 2 및 C++ 739/0은 사용자 제공 결과로만 반영했으며, 병행 중인 ISCC 결과는 이 판정에 포함하지 않았습니다.

지정한 1차 OUT 파일은 현재 경로에 없어 요청 메시지의 지적 목록을 기준으로 검토했습니다. 읽기 전용 환경이므로 OUT 저장 대신 위에 리뷰 전문을 출력했습니다.