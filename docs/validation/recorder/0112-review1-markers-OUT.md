# Tally 0.1.12 마커 작업 1차 리뷰(아스트라 read-only, 2026-09-13) 원문

fix first

**[P2] 덮어쓰기의 최종 교체 실패 시 기존 파일을 잃을 수 있습니다.** [MarkerPanel.cpp:126](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:126)의 `overwriteTargetFileWithTemporary()`는 Windows에서 백업 없이 `ReplaceFileW`를 호출합니다. 실제 연결된 JUCE 구현은 [juce_Files_windows.cpp:374](C:/Users/claude/JUCE/modules/juce_core/native/juce_Files_windows.cpp:374)입니다.

Microsoft 문서에 따르면 백업 없는 호출에서 `ERROR_UNABLE_TO_MOVE_REPLACEMENT(1176)`가 발생하면 기존 대상 파일이 이미 없어질 수 있습니다. 이후 JUCE의 재시도까지 실패하면 [juce_TemporaryFile.cpp:79](C:/Users/claude/JUCE/modules/juce_core/files/juce_TemporaryFile.cpp:79)의 소멸자가 남은 임시 파일도 삭제합니다. 따라서 실패 안내만 표시되고 기존 파일과 새 출력이 모두 사라지는 경로가 있습니다. 이는 실행 재현이 아닌 소스와 [ReplaceFileW의 명시된 실패 계약](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew#return-value)에 근거한 판정입니다.

배포 전 **기존 대상의 백업·복구가 가능한 교체 처리**가 필요합니다. 현재 [TimelineUxTests.cpp:301](C:/Users/claude/gocue-rec/recorder/tests/TimelineUxTests.cpp:301)은 잘못된 부모 경로에서의 열기 실패를 검증하므로, 이 교체 단계 실패는 검증하지 않습니다.

나머지 요청 항목에서는 배포를 막을 결함을 찾지 못했습니다.

- **파서 경계:** 빈 필드, 분·초의 초과 자릿수와 60 이상 값, 내부 공백·부호, 잘못된 소수를 거부합니다. 바깥 공백은 허용합니다. `paddedRight('0', 3)`은 `.5→500ms`, `.25→250ms`로 정확합니다. `millis≤999`, `Fs`가 32비트이므로 소수 계산은 `int64`에 들어가며, 누적·최종 곱셈 전 검사도 맞습니다. 실패 시 출력 인자를 변경하지 않습니다. [TimelineView.logic.cpp:338](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.logic.cpp:338)

- **이름 변경과 샘플 정밀도:** 표시 문자열을 그대로 두면 저장된 정확한 샘플을 보존합니다. 같은 밀리초 안의 다른 샘플은 정수 입력으로 지정할 수 있으므로 해당 규칙은 결함으로 보지 않습니다. [MarkerPanel.cpp:78](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:78)

- **내보내기 내용·비동기 처리:** 정렬, 초 내림, 시간 형식, 개행 치환, 마지막 CRLF와 `Fs==0` 처리는 설명과 일치합니다. 선택기 실행 전 스냅샷을 캡처하므로 프로젝트가 바뀌어도 원래 프로젝트를 내보냅니다. 결과 파일을 복사한 뒤 선택기를 삭제하며, JUCE도 콜백을 별도로 보존합니다. [MarkerExport.cpp:6](C:/Users/claude/gocue-rec/recorder/src/model/MarkerExport.cpp:6), [MarkerPanel.cpp:98](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:98), [juce_FileChooser.cpp:271](C:/Users/claude/JUCE/modules/juce_gui_basics/filebrowser/juce_FileChooser.cpp:271)

- **이름의 경계:** 개행 외 제어문자는 그대로 남고, 빈 이름은 `00:02 \r\n`처럼 빈 제목으로 출력됩니다. 일반 추가·편집 UI는 빈 이름을 방지합니다. 이번에 명시한 변환 규칙에서는 추가 차단 사유로 잡지 않았습니다. [MarkerExport.cpp:20](C:/Users/claude/gocue-rec/recorder/src/model/MarkerExport.cpp:20), [TimelineView.logic.cpp:286](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.logic.cpp:286), [ProjectDialogs.cpp:109](C:/Users/claude/gocue-rec/recorder/src/ui/ProjectDialogs.cpp:109)

- **추가 창 수명:** 커스텀 컴포넌트를 멤버로 소유하는 방식과 서로 다른 `SafePointer`의 실제 포인터 비교는 적절합니다. 모달 콜백 이후 삭제되며, 명시적 해제 시 JUCE가 자동 삭제를 해제하므로 중복 삭제 경로도 보이지 않습니다. [ProjectDialogs.cpp:15](C:/Users/claude/gocue-rec/recorder/src/ui/ProjectDialogs.cpp:15), [ProjectDialogs.cpp:105](C:/Users/claude/gocue-rec/recorder/src/ui/ProjectDialogs.cpp:105), [juce_ModalComponentManager.cpp:70](C:/Users/claude/JUCE/modules/juce_gui_basics/components/juce_ModalComponentManager.cpp:70), [juce_ModalComponentManager.cpp:207](C:/Users/claude/JUCE/modules/juce_gui_basics/components/juce_ModalComponentManager.cpp:207)

- **색·호출 호환·UTF-8:** 견본 선택은 저장된 이름·샘플로 편집을 한 번 호출하고 잠금을 따릅니다. 기존 호출은 기본 파랑을 유지하며 녹화 큐에도 색이 복사됩니다. 변경된 한글 리터럴의 UTF-8 처리, BOM 없는 출력, CMake 등록에도 문제를 찾지 못했습니다. [MarkerPanel.cpp:23](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:23), [RecorderSession.h:36](C:/Users/claude/gocue-rec/recorder/src/app/RecorderSession.h:36), [RecorderSession.cpp:519](C:/Users/claude/gocue-rec/recorder/src/app/RecorderSession.cpp:519), [MarkerPanel.cpp:123](C:/Users/claude/gocue-rec/recorder/src/ui/MarkerPanel.cpp:123)

검토 HEAD는 `b95ccc3`, 작업 트리는 깨끗하며 지정 diff의 `git diff --check`는 통과했습니다. 코드·파일 수정과 테스트 실행은 하지 않았습니다. 병행 중인 병합 트리 전체 테스트·실장비 결과는 판정에 포함하지 않았으며, OUT 파일 대신 이 메시지에 전문을 제공합니다.