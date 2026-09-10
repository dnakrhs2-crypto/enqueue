# fix-015-snap 검증 결과

작업 트리: `C:\Users\claude\gocue-rec-storage`, 브랜치 `fix-015-snap`, 기준 `5cda42c` (배포된 Recorder 0.1.4). 실행기 OUT 경로가 환경/메시지에 전달되지 않아 사전에 안내한 이 파일을 사용했다. 커밋·push·release.py 실행 없음. ProductIdentity·버전·릴리스 노트·site 및 MainComponent·RecordView·AudioSettingsPanel·import 코어 수정 없음.

## 원인 직접 확인

기준 코드의 `Rows::updateDrag`는 자석 결과에 `dragTo(snapped.value, bypass)`를 호출했다. 두 번째 인자는 `exact`이며, `TimelineEditController::apply`는 `!exact`를 모델의 `frameSnap`으로 전달했다. `ClipEdits::move`는 영상 기준 클립의 목적지를 프로젝트 프레임 격자로 다시 반올림했다. Rows는 그 결과가 자석 목표와 다르면 안내선만 지웠다. 사용자 지적 1~5의 코드 경로를 모두 확인했다.

실제 마우스 콜백을 사용하는 실패 테스트를 먼저 빌드·실행했다. **60fps·48kHz, 앞 클립 끝 320640 → 자석 드래그 후 실제 시작 320800, 틈 +160샘플(3.333ms)**로 재현됐다. 수정 전 `timeline-ux`는 기존 19건 통과, 신규 재현 1건 실패였다. 근거: `out/fix-015-snap/baseline-build.log`, `baseline-timeline.log`.

제공된 실제 프로젝트를 읽어 `Fs=48000`, `fps=60/1`, 원본 테이크·클립 길이 `480480`샘플(600.6프레임)을 확인했다. 이 경로에는 실제 테이크가 **1개** 있었다. 두 독립 파일 경계 검증에는 복사본 안에서 H264와 PCM을 각각 한 번 더 복제하고 독립 asset/take/link ID로 배치했다. 서로 다른 녹화 두 건을 확보한 것으로 표현하지 않는다.

## 수정 위치와 동작

| 위치 | 변경 |
| --- | --- |
| `recorder/src/ui/TimelineView.cpp:710`, `:717` | 프로젝트 fps를 포함한 허용치 적용. 자석 목표가 있으면 `exact=true`; Alt/스냅 OFF도 기존처럼 exact. 자석이 없을 때만 프레임 격자 사용. 격자가 목표를 밀어낸 뒤 안내선만 지우던 코드를 제거했다. |
| `recorder/src/ui/TimelineInteraction.h:46`, `:77`, `:114` | 8→12픽셀, 시간 최소 범위, int64 포화 처리. 클립 경계와 마커/재생헤드 인덱스를 분리하여 같은 거리에서는 클립 경계를 선택한다. |
| `recorder/src/model/ClipEdits.cpp:30`, `:238`, `:350` | 같은 트랙의 활성 이웃과 맞닿는 앞/뒤 끝에 대해 엄격히 1프레임 미만의 틈·겹침을 흡수한다. 격자 반올림 전과 후에 확인하며, 반올림 전 이미 1프레임 이상 겹친 요청은 이 보정으로 승인하지 않는다. 이동은 기준 영상 클립에서 구한 델타 하나를 링크·버전 구성원과 take stack에 공유한다. 소스 핸들 및 다른 구성원의 충돌 검증은 유지한다. |
| `recorder/src/model/ClipEdits.h:26`, `:36`; `recorder/src/ui/TimelineView.logic.cpp:170` | 인터랙티브 편집만 `joinNeighbours`에 참여하도록 분리했다. 컨트롤러의 비 exact 이동/트림은 격자와 붙임을 켜고, 자석 exact·Alt·정확한 샘플 입력은 둘 다 끈다. 기존 모델 직접 호출의 기본 동작은 유지한다. |
| `recorder/src/ui/TimelineView.logic.h:54`; `recorder/src/ui/TimelineView.logic.cpp:289`; `recorder/src/ui/TimelineView.cpp:718`, `:768` | 성공한 미리보기에서 같은 트랙의 실제 맞닿음을 확인한다. 드래그 중과 확정 후 상태줄에 **이웃 클립에 붙임**을 표시한다. Alt/스냅 OFF에는 붙임 표시를 적용하지 않는다. |
| `recorder/src/playback/VideoPlaybackEngine.cpp:404`, `:410`, `:478`, `:493`, `:644`, `:779` | 짧은 클립 사이 틈의 표시 끝만 앞 클립에 포함시킨다. 틈의 디코드 좌표는 앞 클립 마지막 소스 샘플에 고정하여 콜드 seek에도 실제 직전 PTS를 준비한다. 원본 범위·들어오는 클립 위치/PTS는 변하지 않는다. 1프레임 이상, 다른 트랙, 동일 clip ID 내부 공백, 명시적인 소스 공백 조각은 보정 대상에서 제외한다. 캐시 프레임의 표시 범위 검사에도 같은 끝을 적용했다. |

보조 처리를 RenderPlanCompiler 대신 재생 엔진에 적용한 이유: 현재 실제 앱의 `RecorderSession.cpp:340`~`:367`은 비디오를 원본 프로젝트의 활성 클립과 availableRanges에서 직접 구성한다. RenderPlanCompiler만 바꾸면 이 화면 재생 경로에 적용되지 않는다. 허용된 엔진 파일만 수정했으며, 컴파일된 오디오 계획·export 구간·프로젝트 저장 좌표는 바꾸지 않았다. 0.1.4의 독립 프리롤, source epoch 무효화, seek 중 직전 그림 유지 경로는 회귀 테스트로 확인했다.

## 허용치 근거

- 자석: **max(12픽셀에 해당하는 샘플, 1.5프레임에 해당하는 샘플, 20ms)**. 정수 샘플 계산에서는 한 프레임과 반 프레임을 각각 올려 더한다. 축소 화면에서는 픽셀 범위가 커지고, 확대 화면에서도 시간 최소 범위를 보장한다.
- 한 프레임의 붙임 범위에 격자의 최대 반 프레임 이동 여유를 더했다. 예를 들어 끝이 `320160`일 때 마우스 목적지 `321100`을 격자로만 반올림하면 `320800`이 되어 640샘플 틈이 생긴다. 최소 범위에 반 프레임 여유가 있어야 이 상황도 격자 처리 전에 자석이 잡고, 허용 범위 밖의 마우스가 나중에 격자 보정 때문에 갑자기 붙지 않는다.
- 48kHz에서 60fps 최소 **1200샘플=25ms**, 30fps **2400샘플=50ms**, 30000/1001fps **2403샘플**, 120fps는 **960샘플=20ms**. 600초/1000픽셀 화면은 픽셀 항이 **345600샘플**로 우세한다.
- 모델 붙임 자체는 자석 범위와 별개로 **엄격히 1프레임 미만**이다. `(Fs * fps.denominator - 1) / fps.numerator`로 최대 정수 거리를 계산하여 60fps·48kHz의 799샘플은 허용하고 800샘플은 이 보정으로 흡수하지 않는다. 큰 겹침의 기존 거절 정책은 유지한다.

## 추가 테스트와 전체 실행

추가 회귀는 `recorder/tests/TimelineUxTests.cpp:105`~`:270`에 있다. 기본 15건과 실제 미디어 환경변수로 켜지는 1건을 추가했다.

- 실제 Rows 드래그 및 양쪽 트림에서 비 프레임 경계 `320640`에 정확히 일치하고, 확정 상태 표시 확인.
- 자유 이동 `950123 → 950400`으로 기존 영상 격자 유지. 붙임 후 영상 `320640`, 지연된 마이크 `320677`로 **37샘플 상대 간격**과 undo/redo 유지.
- 마커·재생헤드 `900123` exact, Alt 및 스냅 OFF의 마우스 샘플 유지, 경계/마커 동거리 우선순위, 허용치 경계와 바깥, 격자 반올림으로 새로 생길 수 있는 틈 검증.
- 이동·양쪽 트림의 ±1/±799샘플 틈·겹침을 0으로 처리하고, 800샘플 겹침은 거절. 링크 델타 공유 확인.
- 기존 프로젝트의 1/160/799샘플 틈은 cold seek에서도 직전 PTS, 800/801샘플 틈은 실제 공백. 명시적 1샘플 asset gap과 다른 트랙은 공백 유지.
- 복사한 실제 두 파일을 각각 split → 가운데 삭제 → Rows 드래그로 붙인 후, 두 조각으로 된 두 번째 테이크 묶음을 첫 테이크 뒤에 붙였다. 경계 `418080`과 묶음 내 48000샘플 간격을 검사했다.

빌드: `cmake --preset local` → `cmake --build --preset local-release --target Recorder --target RecorderTests --target RecorderProbe -- -m -v:m -nologo` 성공. PATH에 cmake가 없어 `C:\Users\claude\tools\cmake\bin\cmake.exe`를 사용했다. Windows PowerShell이 `-v:m`을 분리해서 생긴 첫 MSB1016은 native 인자를 각각 따옴표로 전달해 해결했다. 디렉터리 충돌은 없었고 `-m:1 -nr:false` 전환은 필요 없었다. 최종 빌드 종료 코드 0, 로그 `out/fix-015-snap/final-build.log`.

**전체 RecorderTests 실행: 49개 suite 요약행, 666건 통과, 0건 실패, 종료 코드 0.** 마지막 행은 `RecorderTests: all suites passed`. 기준 647건 + 신규 기본 15건 + 신규 실미디어 1건 + 기존 CutSeamChecks 실미디어 옵션 3건 = 666건이다. `timeline-ux` 35/35, `cut-edit-stability` 12/12, cut-link-history 구성 23+8+10건, ripple/reorder/marker 구성 7+6+3건 통과. 별도 요약 JSON을 쓰는 `edit-property`도 seed **909**, **1000회**, committed 692 / rejected 163 / noops 145, undo 198회, 불변식·undo/redo 해시 PASS다. 로그 `out/fix-015-snap/all-tests.log`, 종료 코드 `all-tests-exit.txt`.

## 실측

| 경우 | 경계 / 틈 | 측정 프레임 | 검은 제출 | 빈 프레임 | 정확한 프레임 누락 | 잘못된 PTS / 순서 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 실제 복사본 split/delete/drag, 독립 파일끼리 붙임 | 418080 / **0샘플** | 경계 ±10, 21개 | **0** | **0** | **0** | **0 / 0** |
| 위 프로젝트에 과거 틈을 재현 | 418080 / 160샘플 | 경계 ±10, 21개 | **0** | **0** | **0** | **0 / 0** |

기존 `CutSeamChecks.h` 방식도 그대로 실행했다.

| 기존 실미디어 모드 | 프레임 | 검은 제출 / 누락 / 잘못된 PTS | 오디오 underrun / oracle 오류 | 경계 도착 지연 | 첫 프레임 준비 |
| --- | ---: | --- | --- | ---: | ---: |
| real-0: split-delete-move | 21 | 0 / 0 / 0 | 0 / 0 | 1.4533ms | 207.2539ms |
| real-1: 독립 파일 | 21 | 0 / 0 / 0 | 0 / 0 | 1.1292ms | 195.8492ms |
| real-2: ripple | 21 | 0 / 0 / 0 | 0 / 0 | 0.7519ms | 205.6007ms |

CSV/JSON: `out/fix-015-snap/seams/real-snap-joined.*`, `real-snap-legacy-gap.*`, `real-0.*`, `real-1.*`, `real-2.*`. 전체 표는 `out/fix-015-snap/seam-summary.json`. 합성 80ms IDR 프리롤 3모드도 검은 제출·프레임 누락·오디오 오류 0, 순/역 스크럽 42요청의 검은 제출 0이다.

H264 디코드는 실제 FFmpeg D3D11VA와 GPU 텍스처를 사용했다. 위 수치는 기존 CutSeamChecks와 같은 **합성 successful-present sink의 표시 결정/PTS 측정**이며, HWND Present 성공률이나 화면 픽셀의 검은색 비율을 측정했다는 뜻은 아니다. 실제 MainComponent 창/재생/paint는 아래 스트레스 자동화로 별도 확인한다. 캡처보드와 FlexASIO 장치는 열지 않았다.

## 스트레스 및 산출물

**최종 바이너리 `--automation cut-edit-stress`: 312/312회 PASS, 종료 코드 0.** 설정은 seed 910, 26종 동작 각 12회, `reproduceNegativeDrag=false`. project/report는 모두 `out/fix-015-snap/stress/config.json`의 절대 경로 복사본이다. 합성 오디오 출력 시계와 실제 MainComponent/미디어/마우스·키·버튼 콜백을 사용했다. audioCallbacks **12580**, playbackReadyOperations **255**, playingOperations **150**, rowPaintCount **4222**, multipleSelectionOperations **48**을 관측했다. 결과는 `stress/report.json`, `stress/exit.txt`, 동작별 기록은 `stress/operations.jsonl`, 마지막 창 캡처는 `stress/final.png`다. 최종 허용치 반영 전 첫 312회 실행도 PASS였으며 `stress/initial-report.json`으로 구분해 보존했다.

원본 디렉터리의 **12개 파일**에 대해 복사 전/검증 후 경로·길이·SHA-256을 비교하여 **변경 0개**를 확인했다. 원본 lock은 유지하고 복사본의 `project.writer.lock`만 삭제했다. `source-hashes-before.json`, `source-hashes-after.json`, `source-integrity.json`에 근거가 있다. 편집 결과 복사본은 `out/fix-015-snap/real-project/snap-joined.recorder`, 실행 파일 SHA-256은 `out/fix-015-snap/binaries.json`에 있다.

기계 판독 요약은 `out/fix-015-snap/validation-summary.json`. 마지막 코드 변경 뒤 Recorder·RecorderTests·RecorderProbe를 모두 빌드한 다음 전체 테스트와 최종 스트레스를 실행했다. 이후 변경은 이 OUT 기록뿐이다. 작업/검증 시간은 약 **32분**(2026-09-10 16:41~17:13 KST)이다.

## 남은 제한

- 재생 보조의 한 프레임 판정은 엔진이 받은 실제 비디오 인덱스의 마지막 포함 프레임 길이 기준이다. 제공된 프로젝트와 현재 녹화 경로는 project-CFR 60fps다. 소스와 프로젝트 fps가 임의로 다르게 만들어진 외부 프로젝트에 대해 동일한 프로젝트 프레임 임계값이라고 주장하지 않는다.
- 보조는 비디오 표시만 메운다. 기존 프로젝트의 저장된 틈이나 오디오 무음, export 구간을 재작성하지 않는다. 새 인터랙티브 붙임에서는 샘플 위치 자체가 정확히 붙는다.
- 서로 모순되는 링크 배치나 공통 소스 핸들 부족은 여전히 편집 거절이다. 구성원을 개별 이동하여 상대 위치를 바꾸지 않는다.
- 실제 테이크 입력은 한 녹화의 독립 파일 복제 두 건이다. 서로 다른 카메라/녹화 조건의 두 원본 및 실제 모니터 픽셀까지 검증한 것은 아니다.
