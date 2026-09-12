fix first

1. **P1 — 새 오류 메시지가 숨겨질 수 있음.** [RecordView.cpp:151](C:/Users/claude/gocue-rec-play/recorder/src/ui/RecordView.cpp:151)  
   안내가 비어 있을 때 행 높이를 0으로 만들지만, 이후 `update()`의 재배치 조건은 마이크 수·탭 변경뿐입니다(`:127`). `MainComponent::resized()`가 같은 bounds를 다시 지정해도 JUCE는 자식의 `resized()`를 호출하지 않습니다. 따라서 창 크기·탭이 그대로면 새 저장 실패나 녹화 오류가 표시되지 않을 수 있습니다. **수정:** 안내의 빈 상태가 바뀌면 `RecordView::resized()`를 호출하고 새 `timelineBounds()`를 반영하세요. 동일 크기의 `MainComponent`에서 빈 안내→오류→빈 안내를 검증해야 합니다. 기존 테스트는 `main.banner` 문자열만 확인합니다([UiWiringTests.cpp:39](C:/Users/claude/gocue-rec-play/recorder/tests/UiWiringTests.cpp:39)).

2. **P2 — 밝은 사용자 지정 마커의 이름이 사라짐.** [TimelineView.cpp:591](C:/Users/claude/gocue-rec-play/recorder/src/ui/TimelineView.cpp:591)  
   배경 밝기에 따라 검정/흰색을 고르던 분기를 제거하고 흰색으로 고정했습니다. 허용되는 마커 색 `#ffffff`에서는 이름이 배경과 같아집니다. 기존 프로젝트에도 적용됩니다. **수정:** 기존 밝기 기반 전경색 선택을 복원하고, 흰색·노란색·어두운 마커의 이름 대비를 검사하세요. 현재 마커 픽셀 테스트는 파란 배경과 안내선만 확인합니다.

3. **P2 — 삭제할 선택 구간의 눈금 강조가 없어짐.** [TimelineView.cpp:476](C:/Users/claude/gocue-rec-play/recorder/src/ui/TimelineView.cpp:476)  
   선택 구간 채우기가 룰러 아래에서 시작하도록 바뀌었습니다. 이후 불투명한 클립을 그리므로 클립 내부의 강조도 가려져, 트랙이 채워진 화면에서는 행 사이 여백 등에만 표시가 남습니다. 몸통 드래그 후 `Delete`로 제거될 범위를 읽기 어려워집니다. **수정:** 최소한 기존 룰러의 반투명 구간 강조를 복원하세요. 구간 선택 전후의 룰러 픽셀을 검사해야 합니다. [TimelineUxTests.cpp:598](C:/Users/claude/gocue-rec-play/recorder/tests/TimelineUxTests.cpp:598)은 선택 값과 삭제 결과만 검사합니다.

4. **P2 — 트림의 원본 시작·끝 도달 표시가 제거됨.** [TimelineView.cpp:511](C:/Users/claude/gocue-rec-play/recorder/src/ui/TimelineView.cpp:511)  
   `sourceIn == 0`, `sourceIn + lengthSamples == asset.logicalLength`에 따른 노란 핸들 표시가 모두 파란색으로 대체됐습니다. 트림 제한은 유지되지만, 드래그 전에 확장 가능한 방향을 구분하던 정보가 없어졌습니다. **수정:** 새 핸들 모양을 유지하면서 기존 경계 조건과 `Palette::meterYellow` 분기를 복원하세요.

5. **P3 — 탭의 비활성·hover 표시 누락.** [RecorderLookAndFeel.cpp:178](C:/Users/claude/gocue-rec-play/recorder/src/ui/RecorderLookAndFeel.cpp:178)  
   `drawTabButton()`이 활성 여부와 두 상태 인자를 무시합니다. 기존 JUCE V3 경로의 비활성·hover 구분이 사라져, 부모가 비활성화된 탭도 동일하게 보입니다. **수정:** 활성 여부에 따른 투명도와 hover/down 표시를 적용하세요. 입력 차단 자체는 유지됩니다.

6. **P3 — 정적인 하단바도 매 UI 갱신마다 다시 그림.** [MainComponent.cpp:233](C:/Users/claude/gocue-rec-play/recorder/src/ui/MainComponent.cpp:233)  
   `Label::setText()`는 동일 문자열의 repaint를 생략하지만, 별도의 `repaint(footer)`는 항상 실행됩니다. `refresh()` 끝의 `resized()`에서도 네 문자열 폭을 매번 다시 계산합니다(`:136`). **수정:** 텍스트·녹화 상태·창 크기가 바뀔 때만 하단 배치와 repaint를 갱신하세요. 성능 병목이라는 실측 근거는 없으며 배포 차단 항목은 아닙니다.

1–4는 배포 전 수정 대상입니다. 나머지 확인 결과는 다음과 같습니다.

- **레이아웃:** 하단바를 포함한 계산상 `MainComponent` 내용 영역 960×640에서 타임라인 높이는 안내 없음 290px/있음 272px이고, 기존은 268px입니다. 3840×2160에서는 각각 1750/1724px입니다. 정적 계산에서 겹침은 찾지 못했습니다.
- **가져오기·툴바:** 최소 폭에서 가져오기 버튼 148px, 진행 영역 606px로 겹침 없음; 툴바 실제 콘텐츠 폭 1334px와 분기 기준이 일치하고, 스크롤바 공간도 확보됩니다.
- **히트 판정:** `TimelineLayout` 210/30/72, 제목 띠·몸통 경계, 가장자리 ±7px, 드래그 임계값 4px는 유지됩니다. 헤더 크기와 음소거/솔로 버튼 높이는 실제로 바뀌었지만 명령 연결은 동일합니다.
- **탭·단축키·자동화:** 세그먼트와 사이드바 탭에 새 클릭 공백은 발견하지 못했습니다. 단축키 라우팅과 `--self-test-record`/`--automation` 명령 경로는 유지됩니다.
- **기존 글자 크기:** 팝업 19.2, 메뉴 18, 콤보 17.4, 토글 최대 18 및 메뉴바 높이 30은 기존 `pt()` 적용값과 같습니다. 체크박스 그림은 약 20→14 논리 px로 작아졌지만 클릭 사각형은 유지됩니다.
- **버튼 상태:** 일반 버튼은 비활성 45%·hover·down을 처리합니다. 일반 버튼의 별도 키보드 포커스 표시 부재는 기존에도 있었습니다.
- **폰트·DPI:** Cascadia Mono 미설치 시 Consolas를 지정하는 분기가 실제 존재하며, 이 환경에는 두 글꼴이 설치돼 있습니다. `withPointHeight()`는 논리 픽셀의 em 크기이므로 별도 DPI 중복 적용은 없습니다. 미설치 환경·다른 DPI의 실제 렌더링은 미검증입니다. [JUCE 설명](https://docs.juce.com/master/classjuce_1_1Font.html)
- **문자열 폭:** `GlyphArrangement::getStringWidth()`는 shaping 후 폭을 측정하며, 현재 사용처의 여백을 고려하면 교체로 인한 잘림 결함은 발견하지 못했습니다.
- **DWM:** `BOOL`/색상 값의 전달 크기와 HWND 사용이 맞습니다. API는 실패를 HRESULT로 반환하고 코드가 이를 무시하므로, 미지원 속성 때문에 실행을 중단하는 경로는 없습니다. [Microsoft 문서](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmsetwindowattribute)
- **UTF-8:** 변경된 소스 18개가 UTF-8로 유효하며, 새 비ASCII 리터럴은 `ko()` 경로입니다. 기존 문맥에 남은 리터럴을 신규 결함으로 계산하지 않았습니다.
- **그리기 성능 — 추측:** 4K 논리 폭의 메인 화면에서 추가 보조선은 27개이고, 그라데이션은 기존 가시 클립 선별 이후에 적용돼 추가 부담은 제한적으로 보입니다. 33ms 이내라는 실측 보장은 할 수 없습니다.
- **테스트 변경:** 음소거/솔로 색과 그라데이션 기대값 변경 자체는 타당하고 기존 스크럽 비교도 유지됩니다. 다만 일반 UI 테스트에는 Recorder 테마 적용이 빠져 있으며, 최소 크기 테스트는 하단바 없는 `RecordView` 단독이어서 새 메인 창 전체를 검증하지 않습니다.
- **증거:** 기존 `tests-all.log`를 재집계해 **758 passed / 0 failed**를 확인했고 시안·실제 캡처를 열어 대조했습니다. 이번 리뷰에서 빌드·테스트·하드웨어 실행은 재수행하지 않았습니다. `git diff --check` 통과, 작업 트리 변경 없음.

현재 세션의 파일시스템 권한이 읽기 전용이므로 **OUT 파일에는 저장하지 못했습니다. 위 내용이 OUT 원문입니다.**