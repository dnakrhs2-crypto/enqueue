ship

`af06876..68aeb2b`와 필요한 호출 경로에서 배포를 막을 결함은 발견하지 못했습니다. 줄 번호는 `68aeb2b` 기준이며, 읽기 전용 결과 전문을 여기에 출력합니다.

1. **직렬화·체크섬·저널 정합성은 맞습니다.** `track()`은 `hidden=true`만 기록하고, `toJson()`과 편집 델타가 같은 직렬화를 사용합니다. 읽기는 체크섬을 **정규화 전에** 검증하고, 불리언 타입 검사 후 명시적 `false`만 제거하여 canonical 비교를 수행합니다. 배열 가드와 내부 객체 접근도 앞선 파싱 검증으로 보호됩니다. 저널은 트랙 엔티티 전체를 교체하므로 보이기에서 `hidden` 키가 사라지는 것도 반영됩니다. 코드상 저장→재열기→후속 저널 적용의 해시 불일치 원인은 없습니다.
   근거: [RecorderSerializer.cpp:77](C:/Users/claude/gocue-rec/recorder/src/model/RecorderSerializer.cpp:77), [RecorderSerializer.cpp:182](C:/Users/claude/gocue-rec/recorder/src/model/RecorderSerializer.cpp:182), [RecorderSerializer.cpp:262](C:/Users/claude/gocue-rec/recorder/src/model/RecorderSerializer.cpp:262), [RecorderSerializer.cpp:282](C:/Users/claude/gocue-rec/recorder/src/model/RecorderSerializer.cpp:282), [EditJournal.cpp:51](C:/Users/claude/gocue-rec/recorder/src/storage/EditJournal.cpp:51), [EditJournal.cpp:104](C:/Users/claude/gocue-rec/recorder/src/storage/EditJournal.cpp:104).

2. **`onlyTrackVisibilityChanged`는 보수적으로 판별합니다.** `media`는 `shared_ptr<const MediaRegistry>`이므로 `!=`는 저장 포인터의 동일성 비교입니다. 숨김 값을 되돌린 뒤 전체 `EditState`를 비교하여 클립·트랙 순서·mute/solo 등의 동반 변경도 검출합니다. 비용은 전체 편집 데이터 크기에 선형이며 JSON 두 벌의 생성 비용이 있지만, 실제 숨김 차이가 있는 편집에만 발생하고 프레임마다 실행되지 않습니다. 대규모 프로젝트에서의 지연은 측정하지 않았습니다.
   근거: [RecorderModel.h:146](C:/Users/claude/gocue-rec/recorder/src/model/RecorderModel.h:146), [TimelineView.cpp:38](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:38), [TimelineView.cpp:248](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:248).

3. **`revealTrack`의 참조 수명과 잠금 처리는 안전합니다.** ID 복사와 MainComponent의 스냅샷 유지가 편집 후 참조 무효화를 막습니다. 다만 `edits.playhead()`는 마지막 UI 동기화 값이므로, 바로 앞 `session.scrub()`의 최신 위치와 같다는 보장은 없습니다. 현재 불러오기는 항상 새 기본 표시 트랙을 추가하므로 해당 호출에서 숨김 해제 분기가 실행되지 않아, 이번 변경의 실제 재생헤드 회귀로 판단하지 않았습니다. 향후 기존 숨김 트랙에 불러오기를 지원한다면 이 순서를 맞춰야 합니다.
   근거: [TimelineView.cpp:404](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:404), [MainComponent.cpp:27](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:27), [AudioImport.cpp:465](C:/Users/claude/gocue-rec/recorder/src/media/AudioImport.cpp:465).

4. **헤더 이벤트와 비동기 콜백에서 신규 수명 문제는 보이지 않습니다.** 이름 라벨은 기존처럼 이벤트를 통과시키고, 버튼은 자기 이벤트 경로를 유지합니다. 추가한 `mouseDown`은 일반 클릭을 처리하지 않습니다. 메뉴 실행 시 `SafePointer`와 스냅샷을 확인하며, `onEdit` 복사 후 호출하고 이후 헤더 멤버에 접근하지 않아 `refresh()` 중 헤더가 파괴되는 순서도 안전합니다.
   근거: [TrackHeader.cpp:7](C:/Users/claude/gocue-rec/recorder/src/ui/TrackHeader.cpp:7), [TrackHeader.cpp:44](C:/Users/claude/gocue-rec/recorder/src/ui/TrackHeader.cpp:44), [TrackHeader.cpp:54](C:/Users/claude/gocue-rec/recorder/src/ui/TrackHeader.cpp:54), [TimelineView.cpp:127](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:127).

5. **메뉴 ID·재검증·일괄 편집은 정상입니다.** 기존 `editActions`의 최대 메뉴 ID는 14로, 1000/1001+와 충돌하지 않습니다. 모두 보이기→개별 숨김 트랙→기존 액션 목록 검사 순서도 맞습니다. 문서·선택 변경은 거부하고, 잠금은 컨트롤러에서도 재검사합니다. 모두 보이기는 `performEdit()` 한 번이며, 숨긴 마이크의 placeholder 재생성도 프로젝트 트랙 검사로 차단됩니다.
   근거: [TimelineView.logic.h:7](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.logic.h:7), [TimelineView.cpp:10](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:10), [TimelineView.cpp:303](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:303), [TimelineView.logic.cpp:270](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.logic.cpp:270), [TimelineView.cpp:112](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:112).

6. **숨긴 솔로 트랙은 비차단 UX 개선 사항입니다.** 솔로가 계속 믹스에 작용하는 것은 표시 전용 설계와 일치합니다. 현재 안내는 다시 보이는 경로를 제공하지만, 다른 트랙이 들리지 않는 이유까지 설명하지는 않습니다. 후속 개선으로 숨긴 목록에 솔로 표시를 붙이거나 “숨겨도 재생·솔로 유지” 안내를 권합니다.
   근거: [RenderPlanCompiler.cpp:28](C:/Users/claude/gocue-rec/recorder/src/model/RenderPlanCompiler.cpp:28), [TimelineView.cpp:236](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:236), [TimelineView.cpp:285](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.cpp:285).

7. **새 한글 리터럴은 모두 `ko()` 또는 `fromUTF8()`을 거칩니다.** `git diff --check af06876 68aeb2b`는 무출력·종료 코드 0으로 통과했습니다.
   근거: [RecorderLookAndFeel.h:7](C:/Users/claude/gocue-rec/recorder/src/ui/RecorderLookAndFeel.h:7), [TimelineView.logic.cpp:40](C:/Users/claude/gocue-rec/recorder/src/ui/TimelineView.logic.cpp:40), [TrackHeader.cpp:41](C:/Users/claude/gocue-rec/recorder/src/ui/TrackHeader.cpp:41).

테스트는 실행하지 않았습니다. **748 passed / 0 failed는 사용자 제공 결과**로 반영했으며, 지정 로그는 현재 워크스페이스에서 찾지 못했습니다. 병행 중인 실장비 데모 결과는 이 판정에 포함하지 않았습니다.