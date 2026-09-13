# Tally 0.1.12 2차 리뷰(아스트라 read-only, 2026-09-13) 원문

fix first

**P2 2건이 남아 있으며, 1차 리뷰의 파일 손실 문제도 해결되지 않았습니다.** `integrate-016`, `b95ccc3` 기준 미커밋 변경을 읽었습니다. 검토 중 추가된 버전 변경과 검증 문서도 포함했습니다. 코드 수정·테스트 실행·OUT 파일 작성 없이 아래에 전문을 남깁니다.

1. **[P2] 마커 내보내기 fallback이 기존 파일을 삭제할 수 있습니다.**  
   [MarkerPanel.cpp:130](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:130)의 `copyFileTo()`는 내부적으로 **대상 삭제 → 복사** 순서입니다([juce_File.cpp:320](C:/Users/claude/JUCE/modules/juce_core/files/juce_File.cpp:320)). 교체 실패 후 대상 삭제는 성공하고 복사가 I/O 오류 등으로 실패하면 원본을 잃습니다. 임시 파일도 소멸자가 삭제하므로 복구본이 없어질 수 있습니다. 백업 없는 `ReplaceFileW`의 1176 오류 후 복사까지 실패하는 원래 문제도 남습니다. [Microsoft 문서](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)  
   [TimelineUxTests.cpp:309](C:/Users/claude/gocue-rec/recorder/tests/TimelineUxTests.cpp:309)는 대상 삭제 자체가 막히는 경우만 검증합니다. 교체 완료까지 원본의 복구 가능한 백업을 유지하고, **교체 실패와 후속 복사 실패가 겹치는 경로**를 검증해야 합니다.

2. **[P2] 저장 단축키 후보가 모두 점유되면 정상적인 옛 설정 전체를 로드하지 못합니다.**  
   [RecorderSettings.cpp:100](C:/Users/claude/gocue-rec/recorder/src/app/RecorderSettings.cpp:100)의 후보 루프에는 소진 처리가 없습니다. 예를 들어 옛 설정에서 녹화 시작=`Ctrl+S`, 스플릿=`Ctrl+Shift+S`, 마커=`Ctrl+Alt+S`는 유효한 조합입니다. 마이그레이션은 세 후보 모두 실패한 뒤 마지막 중복값을 남겨 전체 `s.validate()`를 실패시킵니다. 앱은 기본 설정 상태로 계속 실행하므로 저장해 둔 장치·카메라 설정까지 적용되지 않습니다([Main.cpp:118](C:/Users/claude/gocue-rec/recorder/src/Main.cpp:118)). 기존 설정을 보존하면서 반드시 사용 가능한 후보를 찾는 처리가 필요합니다.

3. **[P3] 버튼부터 녹화까지의 시간에서 동기 작업 시간이 빠집니다.**  
   [DemoAutomation.cpp:82](C:/Users/claude/gocue-rec/recorder/src/ui/DemoAutomation.cpp:82)는 `onClick()` 반환 **후** `recordQpc`를 기록합니다. 핸들러 내부의 준비 작업과 첫 테이크의 동기 체크포인트 저장([TakeController.cpp:1022](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:1022))이 측정에서 제외됩니다. `pressToRecordingMs`의 기준인 `prepareQpc`도 그 저장 뒤에 찍힙니다([TakeController.cpp:1041](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:1041)). 버튼 진입 시각을 먼저 기록해 전달해야 실제 체감 지연을 측정합니다.

4. **[P3] 크래시 보고용 테스트 이름의 수명이 보장되지 않습니다.**  
   [TestSupport.h:47](C:/Users/claude/gocue-rec/recorder/tests/TestSupport.h:47)은 이름의 포인터만 atomic으로 공개합니다. 실제로 임시 문자열 이름도 사용합니다([LifecycleTests.cpp:197](C:/Users/claude/gocue-rec/recorder/tests/LifecycleTests.cpp:197)). 다른 스레드의 크래시 보고기가 포인터를 읽은 뒤 덤프를 작성하는 동안 테스트가 끝나면, [TestCrashGuard.cpp:49](C:/Users/claude/gocue-rec/recorder/tests/TestCrashGuard.cpp:49)에서 해제된 이름을 읽을 수 있습니다. 보고가 끝날 때까지 수명이 유지되는 이름 저장소가 필요합니다.

요청하신 나머지 검토 결과는 다음과 같습니다.

- **UAF 수정은 타당합니다.** [ExportJob.cpp:160](C:/Users/claude/gocue-rec/recorder/src/export/ExportJob.cpp:160)의 지역 `json`은 동기 쓰기가 끝날 때까지 살아 있습니다. 남은 `.toUTF8()` 두 곳도 각각 지역 문자열([RecorderSerializer.cpp:231](C:/Users/claude/gocue-rec/recorder/src/model/RecorderSerializer.cpp:231)), 함수 호출 동안 유지되는 참조 인자([SafeFileWrite.cpp:8](C:/Users/claude/gocue-rec/src/model/SafeFileWrite.cpp:8))에 기반하므로 같은 수명 문제는 없습니다.

- **listening 원점 계산과 프리롤 처리는 유지됩니다.** 기본 `submit`은 `currentSample + lead`이고 출력 보정을 다시 더하면 N0가 됩니다. 과거 시각 검사는 그대로입니다([TakeController.cpp:1072](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:1072)). 영상은 N0 이전 프레임을 음수 시간으로 매핑하고, 마지막 프리롤을 CFR의 0 시점 후보로 넘깁니다([CameraClockMapper.cpp:217](C:/Users/claude/gocue-rec/recorder/src/sync/CameraClockMapper.cpp:217), [TakeController.cpp:390](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:390)).

- **100ms의 HDD 안정성은 확인되지 않았습니다.** 저널은 실제 `FlushFileBuffers`를 기다리며, 완료가 늦으면 `missedStart`로 중단합니다([RecordingJournal.cpp:181](C:/Users/claude/gocue-rec/recorder/src/storage/RecordingJournal.cpp:181), [RecorderAudioEngine.cpp:178](C:/Users/claude/gocue-rec/recorder/src/audio/RecorderAudioEngine.cpp:178)). listening은 출력 제출 시점까지 commit이 완료돼야 합니다([TakeController.cpp:102](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:102)). 작은 버퍼에서는 기존에 통과하던 약 100~250ms 지연이 새 시작 실패 구간이 됩니다. NVMe 실측만으로 범용 디스크에 충분한 여유라고 판정할 수는 없습니다.

- **새 준비 시간 필드의 데이터 경쟁은 발견하지 못했습니다.** 준비 manifest는 같은 워커에서 작성하고, 메인 스레드의 완료 처리와 이후 저장 워커는 `work.get()`을 거칩니다([TakeController.cpp:787](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:787), [TakeController.cpp:1131](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:1131)). 다만 `prepareDoneQpc`는 실제 워커 완료 시각이 아닌 메인 tick의 완료 관측 시각이어서, tick 지연이 `prepareWorkMs`에 포함됩니다.

- **Ctrl+S 라우팅·필드 유무 검사·키 비교는 타당합니다.** 명시적 중복은 계속 거절되고 TextEditor에서는 라우팅 전에 제외됩니다([MainComponent.cpp:273](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:273)). 700px 창에서는 패널 내부 가용 높이가 약 525px로 필요한 482px보다 큽니다([ProjectDialogs.cpp:45](C:/Users/claude/gocue-rec/recorder/src/ui/ProjectDialogs.cpp:45), [ShortcutSettingsPanel.cpp:38](C:/Users/claude/gocue-rec/recorder/src/ui/ShortcutSettingsPanel.cpp:38)). 화면 크기에 따라 창이 약 657px 미만으로 줄면 아래 초기화 버튼은 잘릴 수 있습니다.

- **크래시 가드는 일반 AV 종료 경로에 맞지만, 힙·잠금에 무관한 보고기는 아닙니다.** `reporting`은 중복 진입을 막고, 자기 프로세스의 `TerminateProcess` 뒤 `abort()`는 방어적 종결 처리로 타당합니다([TestCrashGuard.cpp:25](C:/Users/claude/gocue-rec/recorder/tests/TestCrashGuard.cpp:25), [TestCrashGuard.cpp:68](C:/Users/claude/gocue-rec/recorder/tests/TestCrashGuard.cpp:68)). 하지만 CRT 출력과 동일 프로세스의 `MiniDumpWriteDump`를 사용하므로 손상된 프로세스에서의 교착까지 방지하지는 않습니다. [Microsoft 문서](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump) 고정 버퍼 크기와 별개로 최종 덤프 경로의 `MAX_PATH` 초과도 처리하지 않습니다. 스위트 목록은 실행 전에 모두 검증합니다([TestMain.cpp:230](C:/Users/claude/gocue-rec/recorder/tests/TestMain.cpp:230)).

- **빌드·패키징에서 추가 배포 차단 문제는 발견하지 못했습니다.** 생성된 Release 설정에서 앱 링크 디버그 정보는 꺼져 있고 RecorderTests만 켜져 있습니다. PDB 경로는 빌드 트리별로 분리되며, 패키징은 `.pdb`를 명시적으로 제외합니다([CMakeLists.txt:75](C:/Users/claude/gocue-rec/recorder/CMakeLists.txt:75), [CMakeLists.txt:104](C:/Users/claude/gocue-rec/recorder/CMakeLists.txt:104), [release.py:447](C:/Users/claude/gocue-rec/tools/release.py:447)).

릴리스 노트는 [0.1.12.html:7](C:/Users/claude/gocue-rec/docs/release-notes/recorder/0.1.12.html:7)의 “저장은 Ctrl+Shift+S가 됩니다”를 다음 후보도 고려하는 표현으로 바꾸는 것이 정확합니다. [8행](C:/Users/claude/gocue-rec/docs/release-notes/recorder/0.1.12.html:8)의 0.10초도 **최소 여유**이며 큰 버퍼에서는 더 길어집니다. 성능 수치는 위 측정 누락을 반영해야 합니다.

읽은 검증 문서에는 Release 전체 **760 PASS / 0 FAIL**, ASan 재실행은 **PENDING**으로 기록돼 있습니다([검증 기록:28](C:/Users/claude/gocue-rec/docs/validation/recorder/0.1.12.md:28)). 테스트 통과와 별개로 위 P2 두 경로는 수정이 필요합니다.