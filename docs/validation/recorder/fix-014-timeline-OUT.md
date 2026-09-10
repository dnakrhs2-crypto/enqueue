# fix-014-timeline OUT

요청한 타임라인 P2·P3 수정, Recorder/RecorderTests 빌드, 전체 테스트, 실제 프로젝트 복사본의 스트레스 검증을 완료했다. 커밋은 하지 않았다.

작업 트리: `C:\Users\claude\gocue-rec-storage`, 브랜치: `fix-014-timeline`, 실제 작업 HEAD: `92138fb049643854cecd674daff870f47bf8d42c`. 이 HEAD는 `integrate-014`의 `3fd979bae3cc1a12ef273afd0527907e9a97f01a`와 추가 0.1.3 핫픽스를 포함한다. 별도 실행기 OUT 경로가 전달되지 않아 이 파일을 사용했다.

| 수정 | 파일:줄 및 동작 |
| --- | --- |
| 지연 드래그 대상 보존 | `recorder/src/ui/TimelineView.h:71`, `TimelineView.cpp:666`, `:682`, `:698`: 선택 처리 직후 snapshot 포인터, editRevision, 문서 선택, 직접 선택, 기준 순서의 대상 목록, 클립 ID와 양쪽 경계를 보존한다. 4px 임계값에서 같은 상태인지 확인하고, 변경됐으면 드래그를 취소한다. |
| 클릭 선택과 취소 정리 | `TimelineView.cpp:758`, `:770`: 종료·취소 때 보존 상태를 해제하고, 지연된 다중 선택 축소가 외부의 새 선택을 덮어쓰지 않게 한다. 잠긴 타임라인의 정상 클릭 선택도 유지한다. 기존 컨트롤러의 commit 검사와 `TimelineView.logic.cpp`는 변경하지 않았다. |
| 스냅 정수 안전성 | `recorder/src/ui/TimelineInteraction.h:11`, `:47`, `:88`; `TimelineView.cpp:710`: offset, 이동 경계, 최종 후보의 덧셈·뺄셈을 검사해 표현 불가능한 후보를 제외한다. 거리는 uint64 차이로 비교하고 `abs(INT64_MIN)`과 `tolerance + 1`을 제거했다. tolerance는 NaN·무한대·상한을 검사한 뒤 변환한다. 마우스와 가장자리 타이머가 같은 updateDrag 경로를 사용한다. |
| 큰 좌표의 페인트 | `TimelineView.cpp:454`, `:474`, `:488`, `:525`, `:578`, `:597`, `:623`: 파형·썸네일의 원본 좌표, 트림 한계, 녹화 끝 경계를 보호한다. 눈금 변환의 범위를 검사하고 화면 폭으로 눈금 개수를 제한한다. 래스터 좌표를 화면 주변으로 제한하고, 샘플 표시 영역이 없는 좁은 행은 건너뛴다. |
| 큰 시간의 문자열 변환 | `recorder/src/ui/UiState.cpp:91`: `sample * 1000` 대신 초와 나머지를 따로 계산하여 시간 표시의 오버플로를 막았다. |
| 실제 녹화 대상 및 첫 녹화 행 | `TimelineView.h:16`, `TimelineView.cpp:147`, `:380`, `:409`, `:596`: RecordingPreviewTargets로 실제 카메라 준비 상태와 무장된 논리 마이크 슬롯 1..8을 받는다. 트랙이 없는 활성 마이크는 비활성 헤더·빈 클립 목록의 표시 전용 행으로 추가하고, 실제 트랙이 게시되면 중복 없이 교체한다. 설정은 이름·스테레오 라벨에만 사용한다. |
| 사용자 지정 메뉴 안내 | `TimelineView.cpp:301`: 메뉴 생성 시 현재 RecorderShortcuts의 스플릿·마커 바인딩을 사용한다. Ctrl+Z/Ctrl+Shift+Z/Delete는 현재 설정에서 예약된 고정 키이므로 실제 라우팅과 같은 안내를 유지했다. |
| 자동화 호출 호환 | `recorder/src/ui/TimelineUxScenario.cpp:145`: 기존 합성 미리보기 자동화의 호출 한 줄을 새 입력 형식으로 바꿨다. |

다른 세션 영역의 수정은 **`recorder/src/ui/MainComponent.cpp:137` 및 `:138` 두 줄뿐**이다. 기존 한 줄 호출을 아래 두 줄로 교체했다. 키 라우팅이나 예외 경계에는 변경이 없다. `RecorderSession.cpp/.h`, `TakeController`, `MediaIndex`, `ExportDialog`는 수정하지 않았다.

```cpp
timelineView.setRecordingPreview(ui.live, take.placementSample(), session.elapsed(),
    {{session.cameraReady(0), session.cameraReady(1)}, ui.live ? session.audioEngine().armedMicrophones() : std::vector<unsigned>{}}, settings.get());
```

추가 회귀 테스트는 `recorder/tests/TimelineUxTests.cpp`의 **11건**이다. 실제 Rows에 juce::MouseEvent를 전달하며, 마이크 입력은 openSynthetic으로만 구성했다.

| 테스트 위치 | 추가 수 | 검증 |
| --- | ---: | --- |
| `:103` | 3 | mouseDown → undo/선택 교체/삭제 → 4px mouseDrag → mouseUp. 각각 이동·앞 트림·뒤 트림을 검사하고 문서·선택·undo 깊이가 유지되는지 확인. |
| `:125` | 1 | 선택 교체 후 지연 클릭 축소 방지, 잠긴 타임라인의 정상 클릭 선택. |
| `:150`, `:172` | 2 | INT64_MIN/MAX의 checked 연산·거리·후보 제외, 최대/음수 tolerance, NaN/무한대 변환, 큰 시간 문자열. |
| `:183` | 1 | 유효한 MAX-1 끝점 프로젝트에서 실제 updateDrag, 파형·썸네일 좌표·트림 가이드 페인트 경로, zoomToFit, 샘플 영역 1px와 영역 없는 행. 미리보기로 문서가 바뀌지 않음. |
| `:252`, `:268`, `:288` | 3 | 빈 프로젝트 첫 마이크 행, 새 스테레오 논리 슬롯 한 행과 실제 트랙 교체, 설정상 켜져 있지만 준비되지 않은 캠2 제외. |
| `:297` | 1 | 사용자 지정 Ctrl+Shift+X/F8 및 기본값 복원 후 메뉴 안내, 예약된 실행취소 키 유지. |

최종 빌드와 테스트 결과:

- `cmake --preset local`: 성공, 종료 코드 0. CMake가 PATH에 없어 `C:\Users\claude\tools\cmake\bin\cmake.exe`를 사용했다.
- `cmake --build --preset local-release --target Recorder --target RecorderTests -- '-m:1' '-nr:false' '-v:m' '-nologo'`: 두 타깃 성공, 종료 코드 0. 처음 병렬 빌드의 중간 파일 접근 오류 후 요청한 직렬 옵션으로 전환했다.
- 최종 RecorderTests.exe 전체: **48개 suite 요약행, 566건 통과, 실패 0건, 종료 코드 0**. 마지막 줄은 `RecorderTests: all suites passed`다.
- `timeline-ux`: **19/19**, `cut-edit-stability`: **12/12**. 별도 최종 timeline-ux 실행도 19/19, 종료 코드 0이다.
- 전달받은 554건과의 차이: 실제 HEAD에는 `recorder/tests/EditJournalTests.cpp:168`의 원자적 시간 기준 재생 회귀 테스트 1건이 이미 추가되어 있다(`git diff integrate-014..HEAD -- recorder/tests`로 확인). **554 + 기존 1 + 이번 11 = 566**이다.
- `git diff --check`: 통과. ProductIdentity, 버전, 릴리스 노트, site 변경 및 push/release.py 실행은 없다.

최종 스트레스는 `--automation`의 `cut-edit-stress`, seed **910**, **312회**로 **PASS**, 앱 종료 코드 **0**이다. 26개 동작을 각각 12회 완료했고 세션 오류는 0건이다. 합성 오디오 콜백 **12,580회**, 재생 준비가 된 상태에서 완료한 동작 **245개**, 재생 중 완료한 동작 **150개**, Rows 페인트 **4,102회**, 다중 선택 상태의 동작 **48개**를 관측했다. 중간 빌드에서도 312회 PASS를 확인했으며, 마지막 두 경계 보완 후 최종 바이너리로 새 복사본에서 다시 실행한 수치를 위에 기록했다.

사용자가 지정한 원본은 열기 전에 `out/fix-014-timeline/stress-final/project`로 복사하고, 복사본의 `project.writer.lock`만 삭제했다. 원본 프로젝트와 미디어 6개, 총 7개 파일의 SHA-256이 작업 전후 동일하고 원본 lock도 그대로다. 실제 캡처보드·FlexASIO는 열지 않았다.

검증 파일은 작업 트리의 `out/fix-014-timeline/`에 있다.

- `configure.log`, `build-final.log`: 설정 및 최종 빌드 로그.
- `timeline-ux-final.log`, `tests-final.log`, `tests-final-exit-code.txt`: 최종 테스트 로그와 종료 코드.
- `validation-summary.json`, `binary-hashes.json`: 기계 판독용 결과 집계와 검증 바이너리 해시.
- `stress-final/` 안의 `config.json`, `report.json`, `operations.jsonl`, `exit-code.txt`, `final.png`: 최종 스트레스 설정·결과·입력 기록·종료 코드·화면.
- `stress/source-manifest.json`: 원본 7개 파일의 작업 전 해시.

남은 검증 범위와 병합 주의점:

- 실제 장치를 열지 않았으므로, 첫 녹화·스테레오·캠2 실패는 합성 오디오의 실제 armed 목록과 준비 상태 입력을 이용한 표시 행 검증이다. 캡처보드 연결 실패나 FlexASIO 실녹화를 재현했다는 의미는 아니다.
- 극단 좌표 테스트는 정수 안전성과 updateDrag/paint 완료를 확인한다. double 기반 화면 좌표가 INT64_MAX 부근에서 샘플 하나까지 구별된다고 보장하지 않는다.
- 다른 fix-014 세션의 변경을 합친 상태는 이 작업 트리에서 검증하지 않았다. MainComponent의 위 두 줄은 키 세션과 병합할 때 확인해야 한다.

커밋은 사용자가 수행한다. push와 release.py는 실행하지 않았으며, 변경 파일과 검증 결과만 작업 트리에 보존했다.
