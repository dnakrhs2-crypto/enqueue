ship

`f0af932..9da4c70`과 필요한 호출 경로에서 배포를 막을 신규 결함은 발견하지 못했습니다.

1. **미저장 판정과 종료 규칙이 일치합니다.** 다른 활동이 없다면 빈 기본 프로젝트와 저장 완료 프로젝트는 업데이트를 허용합니다. 파일이 있는 dirty 프로젝트는 타임라인 길이와 관계없이 차단하고, 파일이 없어도 dirty이며 `activeTimelineEnd() > 0`이면 차단합니다. 파일 없는 새 프로젝트에서 마커·이름만 변경한 상태는 기존 종료 규칙처럼 보호하지 않습니다. [RecorderDocument.h:69](C:/Users/claude/gocue-rec/recorder/src/app/RecorderDocument.h:69), [MainComponent.cpp:363](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:363)

2. **자동 업데이트에서도 미저장 미디어 보호가 유지됩니다.** 문서 변경 시 즉시 lifecycle을 발행하며, WinSparkle 콜백과 조용한 검사 모두 `canShutdown()`을 확인합니다. 실제 종료 요청 직전에도 다시 발행·검사합니다. 녹화·불러오기는 프로젝트 파일을 요구하므로 이번 변경으로 실제 미저장 미디어가 허용 대상으로 바뀌지 않습니다. [MainComponent.cpp:64](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:64), [Main.cpp:131](C:/Users/claude/gocue-rec/recorder/src/Main.cpp:131), [MainComponent.cpp:374](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:374), [MainComponent.cpp:484](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:484), [RecorderSession.cpp:116](C:/Users/claude/gocue-rec/recorder/src/app/RecorderSession.cpp:116), [MainComponent.cpp:169](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:169)

3. **exclusive 검사는 변경되지 않았습니다.** `begin()`의 검사 마스크에는 기존대로 `unsaved`가 없습니다. 녹화·내보내기 등의 시작 조건에 이번 수정이 영향을 주지 않습니다. [RecorderLifecycle.cpp:34](C:/Users/claude/gocue-rec/recorder/src/app/RecorderLifecycle.cpp:34)

4. **문구 조합의 UTF-8 경계에 문제가 없습니다.** 한글과 구분자를 `ko()`로 변환한 뒤 `juce::StringArray::joinIntoString()`으로 결합합니다. 활성 항목만 나열하고, unsaved 단독·복합·captureBusy·기본 문구 분기도 요청대로 구현되어 있습니다. 기존 `updateBusy` 문구도 유지됩니다. [RecorderLookAndFeel.h:7](C:/Users/claude/gocue-rec/recorder/src/ui/RecorderLookAndFeel.h:7), [MainComponent.cpp:458](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:458), [RecorderLifecycle.cpp:25](C:/Users/claude/gocue-rec/recorder/src/app/RecorderLifecycle.cpp:25)

5. **`git diff --check f0af932 9da4c70` 통과.** 코드·파일 수정과 테스트 실행은 하지 않았습니다.

신규 테스트는 말씀하신 **저장된 프로젝트 + 마커 편집** 시나리오입니다. WinSparkle은 테스트에서 비활성화되어 있어 실제 업데이트 연동은 이번 정적 검토 범위입니다. 제공하신 **749 passed / 0 failed**는 참고했으며, 지정 로그는 현재 작업공간에 없어 직접 확인하지 못했습니다. [ShortcutExceptionTests.cpp:497](C:/Users/claude/gocue-rec/recorder/tests/ShortcutExceptionTests.cpp:497), [ShortcutExceptionTests.cpp:14](C:/Users/claude/gocue-rec/recorder/tests/ShortcutExceptionTests.cpp:14)