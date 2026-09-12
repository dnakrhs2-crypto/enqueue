ship

`integrate-016`, `56bb89b..f71db08`의 8개 변경 파일과 필요한 호출 경로를 검토했습니다. **1차 지적 6건은 모두 닫힌 것으로 판단하며, 수정 범위에서 새 배포 차단 결함을 발견하지 못했습니다.**

1. **P1 안내 행 — 닫힘.** `hadNotice`를 텍스트 변경 전에 저장하고 최종 표시 문자열의 빈 상태와 비교하므로, 배너 없이 `ui.warning`만 나타나거나 사라져도 재배치됩니다. 타임라인 탭도 같은 조건을 적용합니다. [RecordView.cpp:108](C:/Users/claude/gocue-rec/recorder/src/ui/RecordView.cpp:108), [재배치 조건:130](C:/Users/claude/gocue-rec/recorder/src/ui/RecordView.cpp:130)  
   `refresh()` 끝의 `resized()`가 갱신된 `timelineBounds()`를 적용하고, `audioImporter`도 **현재 마커 버튼의 Y·높이**를 다시 읽어 배치합니다. 두 컴포넌트의 좌표계도 일치합니다. [MainComponent.cpp:125](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:125), [refresh 끝:267](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:267)  
   요청문에 적힌 “타임라인이 26px 이동·축소”는 실제 계산과 다릅니다. **960×640에서는 녹화 탭의 카메라 높이가 26px 줄어 하단 영역은 유지되고, 타임라인 탭에서는 카메라가 8px 줄고 하단 영역이 18px 이동·축소됩니다.** 안내 행 자체는 26px이며, 기존 가변 카메라 높이 계산에 따른 정상 결과입니다. 실제 테스트도 이 구조에 맞춰 작성됐습니다. [RecordView.cpp:154](C:/Users/claude/gocue-rec/recorder/src/ui/RecordView.cpp:154), [ShortcutExceptionTests.cpp:479](C:/Users/claude/gocue-rec/recorder/tests/ShortcutExceptionTests.cpp:479)

2. **P2 마커 이름 대비 — 닫힘.** 밝기 기준에 따라 흰색·노란색 마커에는 검정 글자, 어두운 마커에는 흰 글자를 사용합니다. 마커 채움과 글자는 구간 오버레이 다음에 그려지므로 대비가 유지됩니다. [TimelineView.cpp:596](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:596)

3. **P2 구간 강조 — 닫힘.** 그리기 순서는 **기존 구간 배경 → 클립 → 드래그 미리보기 → 녹화 미리보기 → 새 구간 오버레이 → 마커·라벨 → 스냅 가이드 → 재생헤드**입니다. 따라서 녹화 미리보기에는 선택 색조가 적용되고, 마커·스냅·재생헤드는 오버레이 위에 유지됩니다. [TimelineView.cpp:523](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:523), [오버레이 이후:574](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:574)  
   `paintX`의 화면 경계 ±8 클램프와 `reduceClipRegion(headerWidth, …)`의 교집합으로 **추가 오버레이가 헤더를 침범하지 않습니다.** [TimelineView.cpp:471](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:471), [클립 영역:577](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:577)  
   기존 스크럽 비교 테스트는 선택 구간이 없는 상태에서 시작하고 일반 룰러 스크럽도 구간을 비웁니다. 전후 모두 오버레이가 없으므로 새 변경과 충돌하지 않습니다. [TimelineView.cpp:626](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:626), [TimelineUxTests.cpp:296](C:/Users/claude/gocue-rec/recorder/tests/TimelineUxTests.cpp:296)

4. **P2 트림 한계 핸들 — 닫힘.** 왼쪽은 `sourceIn == 0`, 오른쪽은 자산 존재와 유효한 `sourceEnd`를 모두 확인합니다. 덧셈 오버플로는 `nullopt`를 반환하며, 단락 평가로 역참조하지 않고 오른쪽 핸들을 `accent`로 그립니다. 둥근 핸들 형상은 유지됩니다. [TimelineView.cpp:512](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:512), [TimelineInteraction.h:13](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineInteraction.h:13)

5. **P3 탭 상태 — 닫힘.** 부모 비활성까지 반영하는 `isEnabled()`를 사용하고 글자·밑줄에 45% 알파를 적용합니다. 활성 상태에서만 hover `.06`, down `.10` 오버레이를 적용하며 down이 우선합니다. 로컬 JUCE 구현에서도 부모의 활성 변경이 자식 버튼의 repaint로 전달됨을 확인했습니다. [RecorderLookAndFeel.cpp:178](C:/Users/claude/gocue-rec/recorder/src/ui/RecorderLookAndFeel.cpp:178)

6. **P3 하단바 — 닫힘.** 문자열이 같은 라벨은 `setText/setTooltip`을 건너뜁니다. 상태 색 비교는 별도로 수행하므로 **첫 refresh에서 LookAndFeel 기본색이 목표 색과 달라도 적용됩니다.** `session.recording()` 변화 역시 별도로 추적하므로 시작·정지 시 부모가 그리는 pill 외곽선도 repaint됩니다. [MainComponent.cpp:234](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:234), [외곽선:119](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:119)  
   캐시는 축소 전 문자열 폭입니다. 창 크기 변경 때는 현재 가용 폭으로 `scale`과 라벨 bounds를 다시 계산하므로 `resized()`만 호출되어도 배치가 맞습니다. 초기 캐시도 stale로 시작합니다. [MainComponent.cpp:134](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:134), [MainComponent.h:111](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.h:111)

회귀 검출력은 테스트용 접근자를 유지하고 해당 동작만 수정 전으로 되돌리는 전제로 정적으로 판단했습니다.

| 회귀 검사 | 수정 전 코드에서 실패할 근거 |
|---|---|
| 안내 등장·해제, 두 탭 | 동일 bounds에서 안내 높이가 0px로 남아 26px 검사가 실패합니다. [ShortcutExceptionTests.cpp:477](C:/Users/claude/gocue-rec/recorder/tests/ShortcutExceptionTests.cpp:477) |
| 마커 대비 | 검사 영역은 라벨 내부이며, 기존 흰 글자는 흰색·노란색 채움과 밝기차 `.5`를 넘지 못합니다. [TimelineUxTests.cpp:291](C:/Users/claude/gocue-rec/recorder/tests/TimelineUxTests.cpp:291) |
| 구간 강조 | 기존 구현에서는 룰러가 변하지 않고 불투명 클립이 강조를 가려 두 픽셀 검사가 실패합니다. [TimelineUxTests.cpp:631](C:/Users/claude/gocue-rec/recorder/tests/TimelineUxTests.cpp:631) |
| 트림 핸들 | 기존 양쪽 `accent`는 전체 자산 클립의 노란색 검사에서 실패합니다. 후속 검사는 trim 후 좌우 구분도 확인합니다. [TimelineUxTests.cpp:319](C:/Users/claude/gocue-rec/recorder/tests/TimelineUxTests.cpp:319) |

경고 단독 전환, importer의 세로 이동, 오버플로 fallback, 탭 상태와 하단바 캐시에는 이번 diff의 직접 회귀 assertion이 없습니다. 해당 경로는 코드로 확인했습니다.

**UTF-8 및 diff 검사:** 변경 파일 8개 모두 엄격한 UTF-8 디코딩에 성공했고, 추가된 비ASCII 행은 0개입니다. 새 리터럴은 전부 ASCII입니다. `git diff --check 56bb89b HEAD`는 종료 코드 0이며 작업 트리는 깨끗합니다.

테스트는 실행하지 않았습니다. 49개 스위트·739 passed / 0 failed는 사용자 보고값입니다. 현재 작업공간에는 `tools/claude_harness`가 없어 1차 리뷰는 요청문 기준으로 대조했으며 로그 원문은 확인하지 못했습니다. 병행 장비 데모 결과는 이번 코드 리뷰 판정에 포함하지 않았습니다. 읽기 전용 환경이므로 OUT 파일은 저장하지 않았으며, 이 메시지가 결과 전문입니다.