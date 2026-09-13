# Tally 0.1.12 3차 리뷰(아스트라 read-only, 2026-09-13) 원문

fix first

`integrate-016`, HEAD `b95ccc3`의 미커밋 변경 18개 파일과 새 파일 3개, 2차 리뷰 원문을 검토했습니다. **P2 2건과 테스트 진단용 P3 1건이 남습니다.** 코드 수정·테스트 실행·OUT 파일 작성 없이 결과 전문을 아래에 남깁니다.

1. **[P2] 마커 교체 후 복원까지 실패하면 백업 위치를 사용자에게 알리지 않습니다.**  
   [MarkerPanel.cpp:139](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:139)는 `backup.moveFileTo(file)`의 결과를 무시합니다. `ReplaceFileW`가 1177을 반환하면 원본은 백업 이름으로 남을 수 있으며, 이어지는 이동도 실패하면 원래 경로는 없는 상태가 됩니다. 그런데 [144행](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:144)은 원래 경로의 저장 실패만 알립니다. 사용자는 복구본 존재와 무작위 백업 이름을 안내받지 못합니다. [Microsoft의 오류별 상태 설명](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)

   복원 성공 여부를 확인하고, 실패하면 **보존된 백업의 전체 경로와 복원 실패 사실**을 알려야 합니다. 현재 [TimelineUxTests.cpp:309](C:/Users/claude/gocue-rec/recorder/tests/TimelineUxTests.cpp:309)는 대상이 잠겨 교체 초기 단계에서 실패하는 경로입니다. 1177 이후 복원 실패 경로는 검증하지 않습니다. 참고로 Windows에서는 `.` 접두만으로 숨김 속성이 설정되지는 않지만, 복구 경로 안내가 없다는 문제는 같습니다.

2. **[P2] 250 ms 상한이 큰 오디오 버퍼의 시작 여유를 잘라 녹화 시작 실패를 유발할 수 있습니다.**  
   [TakeController.cpp:519](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:519)의 최외곽 `min(Fs/4, …)` 때문에 `4버퍼` 하한이 보장되지 않습니다. 실제 장치 경로는 [RecorderAudioEngine.cpp:577](C:/Users/claude/gocue-rec/recorder/src/audio/RecorderAudioEngine.cpp:577)에서 최대 16,384프레임을 허용합니다.

   예를 들어 **48 kHz·16,384프레임**이면 새 여유는 12,000샘플로 한 버퍼보다 짧습니다. 이전 계산은 `max(Fs/4, 2버퍼)`여서 32,768샘플이었습니다. 시작 요청 직후 다음 콜백이 저널 flush보다 먼저 도착하면 커서가 예약 시점을 넘어 [RecorderAudioEngine.cpp:178](C:/Users/claude/gocue-rec/recorder/src/audio/RecorderAudioEngine.cpp:178)의 `missedStart`로 중단될 수 있습니다.

   디스크 추정분의 상한과 버퍼 하한을 분리해야 합니다. 현재 범위 테스트는 기본 **8 kHz·80프레임**이고, commit 완료 후 콜백을 진행하므로 이 조건을 잡지 못합니다([TakeControllerTests.cpp:103](C:/Users/claude/gocue-rec/recorder/tests/TakeControllerTests.cpp:103), [TimelineRecordingTests.h:244](C:/Users/claude/gocue-rec/recorder/tests/TimelineRecordingTests.h:244)). 큰 버퍼 조건을 포함해 하한과 문서의 상한을 일치시켜야 합니다.

3. **[P3] 크래시 이름의 수명은 해결됐지만 비원자 버퍼의 데이터 경쟁이 남습니다.**  
   [TestSupport.h:16](C:/Users/claude/gocue-rec/recorder/tests/TestSupport.h:16)의 `snprintf` 쓰기와 [TestCrashGuard.cpp:26](C:/Users/claude/gocue-rec/recorder/tests/TestCrashGuard.cpp:26), [48행](C:/Users/claude/gocue-rec/recorder/tests/TestCrashGuard.cpp:48)의 읽기 사이에는 동기화가 없습니다. `reporting.exchange()`는 이름 작성자와 동기화하지 않습니다. 따라서 피해가 “찢어진 문자열”에만 한정된다고 보장할 수 없으며, C++에서는 미정의 동작입니다. 끝의 NUL도 이 데이터 경쟁을 해결하지 않습니다. [C++ 표준 초안](https://eel.is/c++draft/intro.races#17)

   또한 덤프 작성 전에는 포인터만 잡고 실제 문자열은 덤프 작성 후 읽으므로 다른 테스트 이름으로 바뀔 수 있습니다. 잠금 없이 읽을 수 있는 원자 저장소에서 **보고 진입 시 길이를 제한한 지역 복사본**을 만든 뒤 사용하는 방식이 필요합니다. 현재 단일 스레드 AV 주입 테스트는 이 경합을 검증하지 않습니다.

나머지 집중 검토 결과는 다음과 같습니다.

- **마커 교체의 기본 오류 분기는 타당합니다.** 1175와 백업을 지정한 1176은 원래 파일 이름을 유지하고, 1177은 원본이 백업 이름으로 남는 상태이므로 현재 복원 조건의 방향은 맞습니다. 다만 [MarkerPanel.cpp:134](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:134)는 백업 이름의 미존재 여부를 확인하지 않습니다. 충돌 상대가 삭제 공유를 막으면 안전하게 실패하지만, 단순히 열려 있다는 사실만으로 덮어쓰기를 막지는 못합니다. 기존 백업 이름도 교체될 수 있으므로 미존재 이름을 확보하는 처리가 필요합니다. [Microsoft 설명](https://learn.microsoft.com/en-us/dotnet/api/system.io.file.replace?view=net-10.0), [Windows 구현](https://github.com/dotnet/runtime/blob/main/src/libraries/System.Private.CoreLib/src/System/IO/FileSystem.Windows.cs#L79)  
  `NOMINMAX`는 [CMakeLists.txt:88](C:/Users/claude/gocue-rec/recorder/CMakeLists.txt:88)에서도 정의되므로 헤더 포함 순서 문제는 발견하지 못했습니다.

- **저장 단축키 마이그레이션 P2는 해결됐습니다.** 후보 6개는 중복 비교에서 수정자를 유지하므로 서로 다릅니다. `ctrl + shift + alt + S`는 ctrl→shift→alt 순서의 접두 제거를 통과하고, F12는 유효한 함수 키이며 예약 목록에 없습니다. 다른 명령 5개보다 후보가 많다는 보장도 성립합니다([RecorderSettings.cpp:103](C:/Users/claude/gocue-rec/recorder/src/app/RecorderSettings.cpp:103), [135행](C:/Users/claude/gocue-rec/recorder/src/app/RecorderSettings.cpp:135), [141행](C:/Users/claude/gocue-rec/recorder/src/app/RecorderSettings.cpp:141)).

- **측정 시작점 수정은 반영됐습니다.** 데모는 [DemoAutomation.cpp:82](C:/Users/claude/gocue-rec/recorder/src/ui/DemoAutomation.cpp:82)에서 클릭 전에 기록하고, [TakeController.cpp:984](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:984)의 진입 시각은 첫 체크포인트 저장도 포함합니다. 다만 take.json의 기준은 `prepare()` 진입이며, 종료 구간은 여전히 tick 관측 시각입니다. 버튼 전체 지연은 데모의 `recordButtonToRecordingMs`로 해석하는 것이 정확합니다.

- **디스크 측정은 실제 내구 쓰기를 포함합니다.** take.json 경로는 검증 쓰기와 `FlushFileBuffers`를 거치고, 저널 append도 `DurableFile::flushData()`에서 같은 API를 호출합니다([TakeController.cpp:52](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:52), [RecordingJournal.cpp:181](C:/Users/claude/gocue-rec/recorder/src/storage/RecordingJournal.cpp:181), [DurableFile.cpp:90](C:/Users/claude/gocue-rec/recorder/src/storage/DurableFile.cpp:90)). 4배는 여유를 늘리는 휴리스틱으로 타당하지만 HDD 지연 상한을 보장하는 계수는 아닙니다. **12 ms × 4 = 48 ms이므로 최소 100 ms 유지** 계산은 맞습니다. `<cmath>`도 직접 포함돼 있습니다.

- **나머지 변경에서 추가 문제는 발견하지 못했습니다.** 내보내기 JSON 수명 수정, listening 원점과 프리롤, Ctrl+S 라우팅은 유지됩니다([ExportJob.cpp:160](C:/Users/claude/gocue-rec/recorder/src/export/ExportJob.cpp:160), [TakeController.cpp:1075](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:1075), [391행](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:391), [MainComponent.cpp:296](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:296)). 빌드·패키징 변경에서도 추가 차단 문제는 발견하지 못했습니다.

릴리스 노트의 다음 후보·최소 0.10초·디스크 실측 문구는 반영됐습니다. 다만 위 큰 버퍼 문제를 수정하면 [릴리스 노트:8](C:/Users/claude/gocue-rec/docs/release-notes/recorder/0.1.12.html:8)의 0.25초 상한 설명도 맞춰야 합니다. [검증 문서:4](C:/Users/claude/gocue-rec/docs/validation/recorder/0.1.12.md:4)의 “복사로 착지” 설명은 현재 코드와 다릅니다.

검토 중 갱신된 기록에는 **수정 후 Release 전체 760/0, 실장비 데모 3/3 PASS**가 기재됐습니다. ASan 전체 760/0은 2차 수정 전 바이너리이고, 수정 후 변경 스위트는 아직 `PENDING_ASAN2`입니다([검증 문서:28](C:/Users/claude/gocue-rec/docs/validation/recorder/0.1.12.md:28), [30행](C:/Users/claude/gocue-rec/docs/validation/recorder/0.1.12.md:30)). 이는 제공된 검증 기록을 확인한 것이며, 이번 리뷰에서 테스트를 실행한 결과는 아닙니다.