작업 결과 — 2026-09-12, `fix-016-record`, 기준 `4c71826`

타임라인 재생헤드에서 일반 녹화를 배치하도록 수정했다. `Recorder`, `RecorderTests`, `RecorderProbe` Release 빌드 성공. 최종 전체 테스트 **707개 통과 / 0개 실패**, 헤드리스 내보내기 probe **PASS**. 실장치 녹화는 실행하지 않았다.

배치 정책: **타임라인 탭은 녹화 시작 시 재생헤드, 녹화 탭은 activeTimelineEnd()**. 녹화 탭에는 재생헤드가 보이지 않으므로 숨은 위치에서 덮어쓰는 사고를 막기 위해 기존 끝 배치를 유지한다. 별도 상태줄/배너는 추가하지 않았다.

덮어쓰기 정책: normal 테이크의 `[P, P+길이)`에서 **이번 녹화 대상 트랙의 활성 클립만** 정확한 샘플 단위로 트림·스플릿·제거한다. 다른 트랙의 시간과 클립, 비활성 더빙 버전은 보존한다. 정지 시 잘라내기·배치·녹화 마커·마이크 이름을 한 편집으로 반영하고, 실행취소 한 번으로 복원한다. 원본 미디어 등록은 실행취소 후에도 남는다.

빈 구간 정책: 클립 없는 시간은 재생·내보내기에서 검정 영상과 무음이다. 기존 재생의 **실제 인덱스 프레임 길이 미만인 클립 이음새 유지**와 **250ms IDR 프리롤**은 유지했다. 1프레임 이상 공백은 이전 화면을 지우며, 소스 자체의 누락 구간은 짧아도 이음새로 흡수하지 않는다.

수정 전 동작을 직접 확인한 결과:

- `TakeController::prepare()`가 `activeTimelineEnd()`를 예약하고, `RecorderDocument::placeNewTake()`와 `placeTake(Id)`도 normal 배치를 끝으로 다시 강제했다.
- `RecorderSession::scrub()`는 타임라인 끝을 상한으로 사용했고, `projectChanged()`는 커서를 0으로 초기화했다.
- `enterTimeline()`의 탭 플래그, `ui/TimelineView.cpp:362`의 재생헤드 포함 화면 범위, `ui/MainComponent.cpp:210`·`:212`의 `placementSample()` 기반 녹화 미리보기/커서 연결을 확인했다.
- `storage/RecoveryScanner.cpp`는 normal의 `Pstart=0`을 끝 배치로 바꾸는 보정이 있었다. `TakeStarted.Pstart` 필드와 실제 녹화 경로의 기록 전달은 이미 존재했다(`audio/RecorderAudioEngine.cpp:151`, `storage/RecordingJournal.cpp:190`). 필드/스키마 추가 없이 요청 위치가 이 경로를 통과하도록 연결했다.
- 재생 엔진의 긴 공백 선택과 DXGI clear는 이미 검정 처리를 했다. 다만 첫 디코딩 프레임 전에는 DXGI 장치가 없어, 기존 36px 안내 영역 밖 배경을 검정으로 보장하지 못했다. 영상 엔진 안에서 공백 안내 영역을 호스트 전체로 확장하고 검정으로 칠하도록 보완했다(`playback/VideoPlaybackEngine.cpp:537`, `:551`). 최종본은 검은 NV12(Y=16, UV=128)를 채운다(`export/FinalVideoExporter.cpp:393`).
- 기본 export range는 0부터 활성 타임라인 끝이다(`export/ExportJob.cpp:79`). 소재 exporter는 동일한 요청 range로 캠별 `FinalVideoExporter::run()`을 호출하고 MP4/WAV 공통 길이를 검사한다(`export/MaterialExporter.cpp:178`, `:186`, `:217`). 이 경로는 수정 없이 아래 테스트로 고정했다.

제품 코드 수정 목록(모든 경로는 이 폴더 기준):

| file:line | 수정 내용 |
| --- | --- |
| `recorder/src/record/TakeController.h:62` | 요청 `placementSample`과 한 번의 배치 편집에 포함할 메타데이터 콜백 추가 |
| `recorder/src/record/TakeController.cpp:771`, `:800`, `:894` | 요청 위치 예약, 음수 요청 거부, 정지/최종화 대체 경로에서 동일 배치, 실제 배치 성공 후 ready 공개 |
| `recorder/src/app/RecorderSession.h:77`, `:95` | 배치 구성 함수와 장치 없는 테스트 접근점 |
| `recorder/src/app/RecorderSession.cpp:298`, `:445`, `:478`, `:502`, `:624`, `:681` | 탭별 배치, 끝 너머 재생 종료, 0~24시간 탐색, 프로젝트 변경 시 끝 커서, 배치 직후 커서 이동, 준비 완료 시 탐색 위치 보존 |
| `recorder/src/app/RecorderDocument.h:111`, `:145` | 녹화 배치의 메타데이터를 같은 편집으로 받는 인터페이스 |
| `recorder/src/app/RecorderDocument.cpp:350`, `:367`, `:375`, `:464` | 끝 강제 배치 제거, 대상 트랙 carve+배치의 원자적 공개, 원본 재삽입 시 sparse 마이크 경로 보존 |
| `recorder/src/model/ClipEdits.h:33`, `recorder/src/model/ClipEdits.cpp:200`, `:318` | 순수 `carveOut()` 추가: 링크 확장/시간 이동/프레임 스냅 없이 활성 대상만 편집, 결정적 fragment ID |
| `recorder/src/playback/TimelineTransport.h:43`, `recorder/src/playback/TimelineTransport.cpp:36`, `:101`, `:109`, `:295` | 끝 너머 seek/scrub, 미확인·지연 scrub 커서 유지, 끝 이후에는 빈 오디오 tail만 준비하고 정지 |
| `recorder/src/playback/VideoPlaybackEngine.cpp:537`, `:547` | 첫 클립 앞 공백에서도 DXGI 생성 전에 전체 호스트를 검정으로 표시. 기존 이음새 유지와 프리롤 조건은 보존 |
| `recorder/src/storage/RecoveryScanner.cpp:443`, `:577`, `:602`, `:610` | 0을 포함한 Pstart 그대로 복구, 같은 대상별 덮어쓰기, 신규 클립 수 직접 집계(덮어쓰기 후 순감소 시 unsigned 차감 방지) |

추가·수정 테스트 및 픽스처:

| file:line | 검증 내용 |
| --- | --- |
| `recorder/tests/RecorderProjectTests.cpp:305`, `:314`, `:332`, `:380` | 5초→15초 배치, 등록 테이크 재삽입, 앞/뒤/가운데/전체 덮어쓰기, 링크 상대 캠/마이크 보존, undo/redo 한 번, 단일 저널 delta 재현, 활성 스택만 편집. 기존 자동 끝 배치 픽스처는 요청 위치를 명시 |
| `recorder/tests/ClipEditTests.cpp:60` | 샘플 단위 순수 carve, 결정적 ID, 미대상 storage/비활성 버전 보존, 잘못된 범위/트랙/overflow의 원자적 거부 |
| `recorder/tests/TakeControllerTests.cpp:188`, `:213`, `:232`, `:423`, `:487` | synthetic 오디오+video double로 탭 정책, off-grid Pstart 저널, 배치 ready 직후 커서, 마커/이름 포함 undo, sparse 마이크 재삽입, 녹화 중 구조 보존, 연속 녹화의 명시적 끝 요청. 다른 세션이 바꿀 addMarker 시그니처에 새 테스트를 의존시키지 않음 |
| `recorder/tests/PlaybackTests.cpp:143`, `:172`, `:193` | 숨은 HWND의 실제 GDI paint가 첫 공백에서 전체 검정인지 확인(디코더/GPU 미사용), 끝 너머 재생의 무음/정지/커서, 지연 scrub, 양 캠의 1프레임 경계와 10초 공백 clear, 짧은 이음새 유지 |
| `recorder/tests/RecoveryTests.cpp:38`, `:51`, `:74` | 5초→15초 배치와 재실행 멱등성, TakeStopped 없는 미완료 테이크의 durable prefix, Pstart=0/24037 덮어쓰기, sparse 마이크 및 원본 해시 보존 |
| `recorder/tests/ExportRangeTests.cpp:45` | 앞/중간 공백을 포함하는 기본 range와 재생/내보내기 PCM 전체 일치, 공백은 정확한 0 |
| `recorder/tests/FinalExportTests.cpp:91` | 실제 테스트 MP4/AAC 재디코딩으로 전체 길이, 모든 영상 프레임의 검정/콘텐츠 구분, 긴 공백 AAC 무음 확인 |
| `recorder/tests/MaterialExportTests.cpp:117` | 캠 2개 MP4와 마이크 2개 WAV의 공통 길이, 캠별 blackFrames, AAC 무음 및 WAV 공백의 모든 샘플=0 |
| `recorder/tests/TimelineGapFixtures.h:10` | 위 export 테스트가 공유하는 0~5/15~20초 및 첫 클립 15~25초 배치 픽스처(신규 파일) |
| `recorder/tests/DualPlaybackTests.cpp:150` | 계획 교체 후 끝 너머 seek를 거부한다는 기존 가정을 새 규칙으로 변경 |
| `recorder/tests/UiWiringTests.cpp:110`, `:113`, `:486` | 기존 복구 테스트 2건의 두 번째 테이크 Pstart=13을 명시하는 최소 픽스처 수정. UI 구현 파일은 수정하지 않음 |
| `recorder/tools/LifecycleFixtures.h:102` | 기존 완료 테이크를 보존하는 수명/복구 테스트가 끝 위치를 명시하도록 1행 수정 |

빈 구간 export 확인값(48kHz, 30fps):

| 배치 | 기본 출력 range | 영상 프레임 | 검정 프레임(각 캠) | PCM 샘플(각 파일) |
| --- | --- | ---: | ---: | ---: |
| 0~5초, 15~20초 | 0~20초 | 600 | 300 | 960,000 |
| 첫 클립 15~25초 | 0~25초 | 750 | 450 | 1,200,000 |

WAV/렌더 PCM은 공백의 모든 샘플이 정확히 0이다. AAC는 코덱 경계 영향을 제외한 공백 내부(경계에서 512샘플 이상)에서 양 채널 절댓값 0.003 미만을 확인했다. 테스트 MP4는 소프트웨어 MPEG4 영상과 제품 AAC/MP4 writer·검증기를 사용했다. 제품 D3D11VA/NVENC 하드웨어 실행 결과로 해석하면 안 된다. off-grid export 끝은 기존 정책대로 프레임 경계까지 확장하고 공통 무음 tail을 둔다.

빌드·실행 결과:

- `cmake --preset local` 성공. 이 실행 환경에는 cmake가 PATH에 없어 `C:/Users/claude/tools/cmake/bin/cmake.exe`를 사용했다.
- `cmake --build --preset local-release --target Recorder --target RecorderTests --target RecorderProbe -- -m:1 -nr:false -v:m -nologo` 성공. 최초 병렬 빌드의 tlog 접근 오류 후 지정된 단일 노드 옵션을 사용했다. MSBuild의 중복 `PATH`/`Path` 오류는 Python `subprocess`에 정규화한 자식 환경을 전달해 해결했다. 최종 로그: `build/record-gap-paint-build.log`(세 타깃 모두 성공). 앞선 빌드 로그: `build/record-final-build.log`, `build/record-test-seam-build.log`.
- 기준선: **685 passed, 0 failed**. 최종: **707 passed, 0 failed**, `RecorderTests: all suites passed`, 종료 코드 0. 로그: `build/record-baseline-tests.log`, `build/record-final-tests.log`. 전체 49개 결과 요약의 합계이며 기존 중복 suite 실행을 포함한다. 개별 PASS 행만 세면 663/685개다(AsioStampTests 10개와 NativePcmTests 12개는 개별 PASS 행을 출력하지 않는다).
- 최종 전체 실행 명령은 `RecorderTests.exe --suite all`이다. 같은 바이너리의 직전 무인자 실행 1회는 출력 없이 종료 코드 `0xffffffff`로 끝났으며 원인은 확인되지 않았다. 이후 전체 재실행은 위의 707개 통과/종료 코드 0이다.
- 새 회귀 테스트 22개. 기존 DubbingPlacement/DubbingFailure/TakeStack/RetakeRecovery/Journal/Lifecycle를 포함한 전체 실행 통과.
- `RecorderProbe.exe export --fixture audio-cuts-gaps-rf64 --mode audio-materials --out-dir build/record-probe-output --report build/record-probe.json` **1회 PASS**, 종료 코드 0. 4개 시나리오에서 합계 **3,747,799**개 fade 바깥 샘플의 독립 원본 비교 일치, 원본 SHA-256 불변. 보고서: `build/record-probe.json`, 로그: `build/record-probe.log`. RF64는 가상 4GiB 초과 길이의 **80바이트 헤더만** 검증했으며 대용량 PCM 전체 쓰기 테스트는 아니다.

남은 확인 사항:

- 캡처보드·FlexASIO는 열지 않았다. 실제 60초 녹화, 두 캠/마이크 동시 입력, NVENC 출력 MP4의 화질·길이, DXGI 화면에서 긴 공백이 실제 검정으로 보이는지는 사용자 실장치 검증이 필요하다.
- 복구는 durable 저널/체크포인트, 미완료 writer, stop 없는 prefix, 반복 스캔으로 검증했다. 실제 녹화 중 전원 차단·물리 저장장치 장애와 실장치 MP4 복구를 이번 세션에서 재현하지 않았다.
- Pstart=0을 끝으로 추정하는 보정을 제거했다. 기존 저널도 기록된 Pstart를 그대로 따른다. 과거 기록 자체에 잘못된 위치가 들어 있었다면 자동으로 의도한 끝 위치를 추정하지 않는다.
- 비활성 더빙 버전은 보존한다. 새 normal 녹화와 겹치는 과거 버전의 활성화는 기존 `TakeStackEdits::impact()`의 트랙 겹침 검증 대상이다. 해당 전환의 추가 UI 시나리오는 실행하지 않았다.

`RecorderSession::addMarker` 함수 본문과 선언, `ui/*` 구현, ProductIdentity/버전/릴리스 노트/site는 수정하지 않았다. 커밋·push·release.py 실행과 프로세스 감시/kill 스크립트 작성은 하지 않았다. 커밋은 사용자 작업으로 남긴다.
