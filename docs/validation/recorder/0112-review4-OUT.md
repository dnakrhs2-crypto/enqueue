# Tally 0.1.12 4차 리뷰(아스트라 read-only, 2026-09-13) 원문 — ship

ship

`integrate-016`, HEAD `b95ccc3`의 변경 파일 20개와 새 파일 3개, 2·3차 리뷰 전문을 검토했습니다. **3차의 P2 2건과 P3 1건은 해결됐으며, 추가 배포 차단 결함은 발견하지 못했습니다.**

1. **마커 내보내기 착지 — 해결.**  
   기존 백업 이름을 피하고, 복원까지 실패하면 백업 전체 경로를 반환합니다. 그 `Result`는 상태줄과 툴팁까지 전달됩니다. [MarkerPanel.cpp:126](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:126), [135행](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:135), [154행](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:154), [TimelineView.cpp:238](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:238)

   번호가 붙어도 반환된 `File`을 그대로 사용하므로 이름 처리 문제는 없습니다. 로컬 JUCE는 이 접두에서 `_backup(2).txt` 형태를 만듭니다. `(swap ? swap : replaceFile)`도 같은 `SwapFunction` 타입 중 유효한 함수를 선택하므로 타당합니다. [juce_File.cpp:621](C:/Users/claude/JUCE/modules/juce_core/files/juce_File.cpp:621), [MarkerPanel.cpp:132](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:132)

   실패 시 새 임시 내용을 삭제하는 정책도 합당합니다. 이전 파일은 백업에 보존하고 실패를 알리며, 마커 텍스트는 프로젝트에서 다시 생성할 수 있습니다. 새 내용을 대상에 놓고 성공 처리하는 대안은 필수 수정이 아닙니다. 1177형 분기는 [Microsoft의 오류별 파일 상태](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)와 일치하며, 공유 없는 핸들로 복원을 막는 테스트도 실제 실패 조건을 검증합니다. [MarkerPanel.cpp:143](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:143), [TimelineUxTests.cpp:339](C:/Users/claude/gocue-rec/recorder/tests/TimelineUxTests.cpp:339)

2. **시작 여유 — 해결.**  
   2버퍼 하한이 디스크 추정분의 상한 밖에 있어, 48 kHz·16,384프레임은 이전처럼 **32,768샘플**을 확보합니다. 한 버퍼보다 짧아지던 3차 문제는 없어졌습니다. 4→2버퍼 자체는 기존 출시 하한으로 돌아가는 것이며, 콜백 단위를 무시하는 변경으로 보지 않습니다. 다섯 기대값과 listening 원점 식도 코드에 맞습니다. [TakeController.cpp:520](C:/Users/claude/gocue-rec/recorder/src/record/TakeController.cpp:520), [TakeControllerTests.cpp:539](C:/Users/claude/gocue-rec/recorder/tests/TakeControllerTests.cpp:539), [TimelineRecordingTests.h:242](C:/Users/claude/gocue-rec/recorder/tests/TimelineRecordingTests.h:242)

3. **크래시 보고 이름 — 해결.**  
   공유 저장소의 읽기·쓰기가 모두 바이트별 atomic이고, 복사본은 255바이트 뒤에 NUL을 강제합니다. 보고기는 덤프 작성 전에 복사한 지역 버퍼만 사용하므로 데이터 경쟁과 이름 수명 문제가 해소됐습니다. 경합 중 이름이나 UTF-8이 일부 섞일 수 있지만, 이 경로에서 범위 밖 읽기로 이어지지는 않습니다. [TestSupport.h:17](C:/Users/claude/gocue-rec/recorder/tests/TestSupport.h:17), [24행](C:/Users/claude/gocue-rec/recorder/tests/TestSupport.h:24), [TestCrashGuard.cpp:26](C:/Users/claude/gocue-rec/recorder/tests/TestCrashGuard.cpp:26)

   `{}` 초기화와 정적 저장 기간도 C++17에서 미초기화 읽기 문제가 되지 않습니다. 현재 MSVC x64의 `atomic<char>`가 잠금 없이 동작하는 구현도 확인했습니다. [C++17 초기화 규칙](https://timsong-cpp.github.io/cppwp/n4659/dcl.init#8), [정수 atomic 규정](https://timsong-cpp.github.io/cppwp/n4659/atomics.types.int#2)

4. **문서 설명 — 반영 완료.**  
   “복사로 착지”가 현재 `landExport` 동작으로 바뀌었고, 릴리스 노트에도 큰 버퍼의 예외가 반영됐습니다. [검증 문서:4](C:/Users/claude/gocue-rec/docs/validation/recorder/0.1.12.md:4), [릴리스 노트:8](C:/Users/claude/gocue-rec/docs/release-notes/recorder/0.1.12.html:8)

나머지 미커밋 변경에서도 추가 회귀는 발견하지 못했습니다.

**비차단 문서 보완:** [검증 문서:44](C:/Users/claude/gocue-rec/docs/validation/recorder/0.1.12.md:44)의 전체 **762**는 `PASS` 출력 행 수입니다. 개별 PASS를 출력하지 않는 AsioStamp 10건과 NativePcm 12건을 포함하면 스위트 요약 합계는 **784 passed / 0 failed**입니다. 집계 기준을 명시하면 정확합니다. [Release 로그:362](C:/Users/claude/tools/claude_harness/tests_0112_final5_all.log:362)

기존 로그에서 Release `all suites passed`, ASan 대상 스위트 **134/0·검출 0**을 확인했습니다. 최종 실장비 데모 보고서도 현재 **2/2 PASS**, 버튼→녹화 약 **781/226 ms**로 기록돼 있습니다. [Release 로그:969](C:/Users/claude/tools/claude_harness/tests_0112_final5_all.log:969), [검증 문서:44](C:/Users/claude/gocue-rec/docs/validation/recorder/0.1.12.md:44), [데모 보고서:3](C:/Users/claude/tools/claude_harness/recorder_validation/v016/demo-0112-final2-record.json:3)

코드 수정·빌드·테스트 실행은 하지 않았습니다. 읽기 전용 환경이므로 OUT 파일 대신 이 메시지에 결과 전문을 남깁니다.