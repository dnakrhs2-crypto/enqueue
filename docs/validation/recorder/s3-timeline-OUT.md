# s3-timeline OUT

작업 폴더: `C:\Users\claude\gocue-rec-storage`, 브랜치 `s3-timeline`, 기준 `4f21319`.
실행기 OUT 경로가 전달되지 않아 이 파일을 사용했다.

**부분 완료.** UI/입력·단축키 구현과 테스트 완료. 더빙 연결과 두 카메라 실제 연속 표시 P0 인증이 남았다. 커밋은 Git 메타데이터 쓰기 제한으로 불가했다.

| 변경 | 파일:행 |
| --- | --- |
| 4px 드래그, 고스트, 커서, Esc, 가장자리 스크롤 | `recorder/src/ui/TimelineView.cpp:521`, `:570`, `:584`, `:613`, `:625` |
| 경계/마커/재생헤드 자석 스냅, 8px 임계값 | `recorder/src/ui/TimelineInteraction.h:11`, `:25` |
| 마우스 기준 줌·최소 reveal·녹화 임시 클립 | `recorder/src/ui/TimelineView.cpp:315`, `:323`, `:325` |
| 작은 카메라 영역·타임라인 녹화 레이아웃 | `recorder/src/ui/RecordView.cpp:112` |
| 전역 단축키, 텍스트 포커스 제외, 반복 억제, 33ms 위상 | `recorder/src/ui/MainComponent.cpp:140`, `:161`, `:248` |
| 설정 단축키 탭·장치 재연결 없이 적용 | `recorder/src/ui/ProjectDialogs.cpp:27`, `:86`; `ShortcutSettingsPanel.cpp:5` |
| 저장/로드·기본값·중복/예약 키 검사 | `recorder/src/app/RecorderShortcuts.h:7`; `RecorderSettings.cpp:37`, `:82`, `:105` |
| 회귀 테스트·검증 창 | `recorder/tests/TimelineUxTests.cpp:35`; `recorder/src/ui/TimelineUxScenario.cpp:25` |

기본 키: **F9 녹화 시작 / F10 녹화 정지 / Space 재생·정지 / S 스플릿 / M 마커**.

새 `UserSettings.shortcuts`의 XML 필드: `shortcutRecordStart`, `shortcutRecordStop`, `shortcutPlayStop`, `shortcutSplit`, `shortcutMarker`. JUCE KeyPress 텍스트 형식이며 기존 schemaVersion 1 유지, 누락 필드는 기본값. Escape/Tab/Return/Delete/Ctrl+Z/Ctrl+Shift+Z/Alt+F4 충돌 방지.

빌드 Recorder/RecorderTests 성공. 전체 **516 passed, 0 failed**, 새 timeline-ux **8/8**, ui-wiring **11/11**, dual-playback **6/6**. [로그](timeline-ux/tests.txt). 실제 미디어는 복사본으로 실행했고 캡처/ASIO를 열지 않았다. 화면과 분리한 실행에서 H.264 342프레임 디코딩, PCM 에너지 확인, 언더런 0회. 평균 페인트 간격 32.77ms, p95 43.58ms이며 GPU 무드랍 인증은 아니다.

커밋 해시: **없음**. `git add`가 `C:/Users/claude/gocue/.git/worktrees/gocue-rec-storage/index.lock` 생성 시 Permission denied로 실패했다. 이 세션은 작업 트리 파일만 쓸 수 있고 원래 Git 메타데이터는 읽기 전용이다. 보호된 경로의 권한을 우회하지 않았다. 작업 트리 변경 및 `build/timeline-ux/s3-timeline.patch`를 인계한다.

남은 일/위험:

- 더빙 버튼은 기준 커밋부터 미연결/비활성이다. 실제 더빙 UI·컨트롤러 연결은 미완료.
- 합성 두 카메라의 성공한 네이티브 Present 응답이 이 데스크톱에서 0회다. cam1 캡처는 검게 나타나며 **P0 통과로 인증하지 않는다**. 보고서는 PARTIAL.
- 실제 녹화 핫키, 두 캠의 연속 실시간 프리뷰, 정지 후 실제 클립 교체를 사용자 하드웨어에서 확인해야 한다.
- 10,000클립 가시 영역 페인트 검사는 통과했지만 대규모 연속 드래그 지연은 별도 측정 필요.
- s1 TimelineView 입력/페인트 부분, s2 RecorderSettings 새 필드·RecordView 레이아웃 병합 시 확인 필요. `showEditMenu()`와 요청한 금지 파일은 변경하지 않았다. push/release 실행 없음.

자세한 근거·스크린샷: [timeline-ux.md](timeline-ux.md).

현재 작업 트리에는 변경이 이미 있으므로 패치를 다시 적용하지 않는다. 쓰기 권한이 있는 세션에서 브랜치가 `s3-timeline`인지 확인한 뒤 커밋하면 된다. 패치는 기준 `4f21319`의 별도 복사본에 적용할 수 있다.
