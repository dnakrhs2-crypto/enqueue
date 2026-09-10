# 컷 이음 프리롤·프레임 유지 검증

2026-09-10, `fix-014-seam`, HEAD `92138fb`(integrate-014b). 파일 수정·빌드·헤드리스 검증만 수행했다.

## 확정 원인과 재현 범위

기준 HEAD `recorder/src/playback/VideoPlaybackEngine.cpp:621–641`은 한 카메라 작업자가 **현재 프레임 → 다음 클립 첫 프레임 → 현재 클립 다음 프레임** 순서로 디코드한다. 다음 클립 준비가 없었던 것은 아니다. 선택적 프리페치는 `lane.request`가 바뀔 때 취소되어, 60Hz 진행 요청보다 긴 IDR 프리픽스를 반복 취소하고 다시 시작한다. 현재 클립의 다음 프레임도 그 작업 뒤에서 기다린다. 기준 `:699–705`의 `lane.ready = ready`는 현재 프레임 게시 때 기존 다음 클립 프레임도 잠시 지운다. 준비 큐는 3개, LRU는 4개/32MiB/2 GOP였다.

80ms 프리픽스 지연 주입 시 동일 파일/두 파일/구간 삭제 당기기에서 경계 ±10프레임 중 정확한 프레임 부재가 각각 16/15/16개였다. 다음 클립 도착은 약 101/85/101ms 늦었다. 동일 파일도 다음 클립에 별도 디코더를 사용하므로 같은 취소 문제가 발생한다. 동일 파일 기준 프리픽스 시도 26회 중 취소 25회가 기록됐다.

**검정 제출의 확정 경로는 seek 세대 변경 후 디코드 대기다.** 기준 HEAD `VideoPlaybackEngine.cpp:1073`은 이전 세대 `displayed`를 버리고, `:1079`에서 검정으로 지운 뒤 `:1106`에서 Present한다. 스크럽 42회 중 검정 38개를 합성 제시 기록으로 재현했다. `TimelineTransport.cpp:260` 부근의 오디오 underrun 재준비도 seek 세대를 바꿀 수 있다. 이번 PCM 출력에서 underrun은 0회였다.

**동일 세대 일반 재생에서는 기준 코드도 직전 화면을 유지했다.** 따라서 이번 일반 이음의 기준 검정 제출은 0개이며, 프레임 지연이 재현됐다. 대표가 본 특정 GUI 실행의 검정을 직접 캡처한 것으로 확대 해석하지 않는다. 재현된 프리페치 취소/소실과 세대 전환 시 검정 제출 경로를 수정했다.

플레이백 D3D 구현은 `video/PreviewPresenter.cpp`가 아니라 [VideoPlaybackEngine.cpp](../../../recorder/src/playback/VideoPlaybackEngine.cpp)의 `PreviewPresenter::PlaybackView::run()`에 있다. `video/PreviewPresenter.cpp:252`는 라이브 입력 경로로 기존 업로드 슬롯을 유지한다. `ui/MainComponent.cpp:132`는 플레이백 트랙에 클립이 있으면 호스트를 유지한다. 이 두 파일은 수정하지 않았다.

실제로 빌드되는 컴파일러는 `recorder/CMakeLists.txt:291–292`의 `playback/RenderPlanCompiler.cpp`다. `model/RenderPlanCompiler.cpp`는 HEADER_FILE_ONLY다. 활성 컴파일러 `:42–44`는 sourceIn/시작/길이를 그대로 전달하며 붙어 있는 클립 사이에 양의 길이 gap을 만들지 않는다. 실제 ClipEdits를 거친 세 경우 모두 앞 끝=48000, 뒤 시작=48000, 뒤 sourceIn=110400이었다. 컴파일러와 TimelineTransport는 수정하지 않았다.

## 수정 위치

- `VideoPlaybackEngine.cpp:419`의 `prerollClipAt`: 다음 세그먼트 시작이 현재 샘플에서 250ms 이내이면 준비한다. 실제 빈 구간에서도 다음 시작을 준비하지만 gap 화면 정책을 유지한다.
- `VideoPlaybackEngine.cpp:638`의 `startPreroll`: 카메라당 독립 작업 하나가 다음 디코더를 열고 IDR 프리픽스를 포함해 첫 두 프레임을 준비한다. 진행 요청으로 취소하지 않는다. seek 세대/계획/미디어 epoch 변경과 대상 이탈은 취소한다. 경계에서 완료된 작업의 디코더와 warm DPB를 현재 작업자에게 넘긴다. 최초 현재 프레임은 다음 디코더를 기다리지 않는다.
- `VideoPlaybackEngine.cpp:607,629`의 `wantedNow/publishFrame`: 현재 2개 + 다음 2개를 최대 4개 준비 슬롯으로 유지해 게시 중 다음 프레임이 사라지지 않게 한다. LRU와 카메라당 32텍스처/256MiB 상한은 유지했다.
- `VideoPlaybackEngine.h:105`의 `PlaybackDisplayState`: 성공적으로 제출한 픽셀은 seek 세대가 바뀌어도 다음 정확한 프레임까지 유지한다. 원본 프레임의 세대는 바꾸지 않는다. 실제 gap/프로젝트 시작 전/미디어 epoch 폐기에서는 유지하지 않는다.
- `VideoPlaybackEngine.cpp:1225`: 첫 픽셀조차 없는 유효 영상 준비 상태에서는 빈 검정 Present를 제출하지 않는다. `:1251`은 제출 직전 세대/gap/파일 epoch를 재검사하고, `:1263`은 유지한 이전 그림을 새 seek의 성공 receipt로 기록하지 않는다.
- `VideoPlaybackEngine.cpp:468`의 기존 `mappedFrame` half-open 매핑을 유지했다. 첫 PTS는 `frameAt(sourceIn)`의 포함 프레임이다. sourceIn이 프레임 중간이면 다음 프레임으로 반올림하지 않고 타임라인 begin만 클립 시작에 맞춘다. 1601/49001 샘플의 비정렬 경계도 테스트했다.

## 측정 방법

[CutSeamChecks.h](../../../recorder/tests/CutSeamChecks.h)는 실제 split/remove/move 또는 rippleDeleteAll → RenderPlanCompiler → VideoPlaybackEngine → 생산 코드와 공유하는 PlaybackDisplayState → 합성 제시 기록을 실행한다. 48kHz/60Hz 커서, 경계 48000샘플(1초), 뒤 sourceIn 110400샘플(2.3초), 시작 커서 28800샘플(0.6초)다. 24프레임 전부터 진행하고 ±10프레임 총 21행을 기록한다. 출력 시점에 검사하며 매 프레임 디코드가 끝나기를 기다리지 않는다.

`blackSubmissions`는 합성 제시에서 빈 그림을 선택한 횟수다. 원본 영상 내용이 검정인지 픽셀을 검사한 값이나 모니터 광학 측정은 아니다. `missingExactFrames`는 포함 프레임 부재이며 직전 그림 유지와 구분한다. 경계 지연은 예정된 경계 시각부터 다음 클립의 첫 합성 제시까지의 실제 시간으로 Windows 스케줄링 오차를 포함한다. 수정 후 약 1ms는 프레임 누락이 아닌 제시 호출의 시간 오차다.

합성 입력은 프리픽스 80ms/순차 프레임 1ms의 취소 가능한 디코더다. 실제 입력은 복사본 H.264를 기본 D3D11VA 디코더로 읽고 복사본 WAV를 finalized journal로 연다. 두 파일은 복사본 내 `cam-second.mp4`를 추가 복제해 별도 파일/인덱스/디코더로 검증했다. 캡처보드·ASIO·실제 HWND/스왑체인은 열지 않았다. 기존 RecorderProbe 일반 playback 모드는 ASIO를 열기 때문에 실행하지 않았다.

편집 번호: **0=스플릿→중간 삭제→이동(동일 파일), 1=독립 파일 두 개 인접, 2=구간 삭제 후 전체 당기기**.

## 경계 전후 측정

| Input / edit | Black before / after | Missing before / after | Delay ms before / after | PTS / order errors after |
|---|---:|---:|---:|---:|
| synthetic / 0 | 0 / 0 | 16 / 0 | 100.969 / 0.640 | 0 / 0 |
| synthetic / 1 | 0 / 0 | 15 / 0 | 85.032 / 1.098 | 0 / 0 |
| synthetic / 2 | 0 / 0 | 16 / 0 | 100.533 / 0.888 | 0 / 0 |
| real / 0 | 0 / 0 | 0 / 0 | 0.769 / 0.723 | 0 / 0 |
| real / 1 | 0 / 0 | 0 / 0 | 0.610 / 0.673 | 0 / 0 |
| real / 2 | 0 / 0 | 0 / 0 | 1.271 / 1.226 | 0 / 0 |

양방향 스크럽 42회의 검정 제출은 **38 → 0**, 수정 후 직전 그림 유지 38회였다. 각 release에서 정확한 포함 PTS를 확인했다. 연속 재생의 세 경우는 수정 후 21행 모두 정확해 유지 프레임도 0개였다. 서로 다른 파일/뒤로 이동한 컷은 전역 source PTS 증가가 아니라 세그먼트 내부 순서와 타임라인 begin으로 판단한다.

실제 미디어 프리롤:

| Edit | Lead ms | First ready ms | Prefix frames | Seek ms | Decode ms | Convert ms |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 250.0 | 128.803 | 18 | 0.2700 | 41.903 | 5.184 |
| 1 | 250.0 | 131.448 | 18 | 0.3035 | 45.859 | 3.696 |
| 2 | 250.0 | 132.835 | 18 | 0.3248 | 48.088 | 4.120 |

이번 파일에서 seek 자체는 30ms가 아니었다. 디코더 생성과 수십 ms의 프리픽스 처리를 합친 준비를 경계 전에 끝냈다. 수정 후 합성 프리픽스 취소는 0회다. 실제 세 경우 peakReady=4, peak BGRA=58,060,800 bytes(55.37MiB), decoder peak=2/카메라였다. DPB/드라이버 메모리는 BGRA 수치에서 제외한다.

동일 파일 80ms 주입의 21행 전체(PTS 단위는 인덱스 time base):

| Relative frame | Timeline sample | Source sample | Expected PTS | Before selected / shown | After selected / shown | Black before / after |
|---:|---:|---:|---:|---|---|---:|
| -10 | 40000 | 40000 | 50 | empty / 36 | 50 / 50 | 0 / 0 |
| -9 | 40800 | 40800 | 51 | empty / 36 | 51 / 51 | 0 / 0 |
| -8 | 41600 | 41600 | 52 | empty / 36 | 52 / 52 | 0 / 0 |
| -7 | 42400 | 42400 | 53 | empty / 36 | 53 / 53 | 0 / 0 |
| -6 | 43200 | 43200 | 54 | empty / 36 | 54 / 54 | 0 / 0 |
| -5 | 44000 | 44000 | 55 | empty / 36 | 55 / 55 | 0 / 0 |
| -4 | 44800 | 44800 | 56 | empty / 36 | 56 / 56 | 0 / 0 |
| -3 | 45600 | 45600 | 57 | empty / 36 | 57 / 57 | 0 / 0 |
| -2 | 46400 | 46400 | 58 | empty / 36 | 58 / 58 | 0 / 0 |
| -1 | 47200 | 47200 | 59 | empty / 36 | 59 / 59 | 0 / 0 |
| 0 | 48000 | 110400 | 138 | empty / 36 | 138 / 138 | 0 / 0 |
| 1 | 48800 | 111200 | 139 | empty / 36 | 139 / 139 | 0 / 0 |
| 2 | 49600 | 112000 | 140 | empty / 36 | 140 / 140 | 0 / 0 |
| 3 | 50400 | 112800 | 141 | empty / 36 | 141 / 141 | 0 / 0 |
| 4 | 51200 | 113600 | 142 | empty / 36 | 142 / 142 | 0 / 0 |
| 5 | 52000 | 114400 | 143 | empty / 36 | 143 / 143 | 0 / 0 |
| 6 | 52800 | 115200 | 144 | 144 / 144 | 144 / 144 | 0 / 0 |
| 7 | 53600 | 116000 | 145 | 145 / 145 | 145 / 145 | 0 / 0 |
| 8 | 54400 | 116800 | 146 | 146 / 146 | 146 / 146 | 0 / 0 |
| 9 | 55200 | 117600 | 147 | 147 / 147 | 147 / 147 | 0 / 0 |
| 10 | 56000 | 118400 | 148 | 148 / 148 | 148 / 148 | 0 / 0 |

모든 조건의 CSV와 엔진 측정은 [measurements.json](cut-seam-traces/measurements.json)과 같은 폴더에 저장했다. CSV의 source A/B는 다른 인덱스다. 스크럽의 [전](cut-seam-traces/before-scrub.csv)/[후](cut-seam-traces/after-scrub.csv)에는 generation과 빈 제출을 기록했다.

## 첫 준비 시간·오디오

정지 커서 0.6초에서 엔진 생성·prepare·seek와 PCM prefill을 시작한 뒤 첫 유효 프레임과 PCM이 준비되어 합성 제시를 시작할 수 있을 때까지 측정했다. 다음 클립의 프리롤 완료를 준비 조건에 넣지 않았다. 인덱싱은 별도 구간이다.

| Edit | First ready before ms | First ready after ms | Index before / after ms |
|---|---:|---:|---:|
| 0 | 339.824 | 302.727 | 138.584 / 159.046 |
| 1 | 261.433 | 249.936 | 126.317 / 123.955 |
| 2 | 276.598 | 263.413 | 124.128 / 131.743 |

First-ready mean: **292.618ms -> 272.026ms**.

이 측정 경로는 2초 이내이며 악화되지 않았다. 요청에 적힌 약 450ms의 GUI/실장치 전체 정지→재생과 동일한 측정값으로 주장하지 않는다. 디스크 캐시를 강제로 비우지 않았고 반복/부하에 따른 편차가 있다.

같은 타임라인 샘플로 PCM 큐를 800샘플씩 소비했다. 합성 입력은 독립 PRBS 원본 oracle, 실제 입력은 복사 WAV의 수동 source 점프 oracle과 비교했다. 모든 조건에서 PCM 오차 0, underrun 0이다. `audio/MicroFade.cpp:7,47`의 앞뒤 144샘플(3ms)과 경계 2개 0샘플을 그대로 유지한다. 이 2샘플은 기존 fade의 0점이며 추가 무음 블록이 아니다. fade 밖 bit-exact 및 경계 0점으로 검증했고 물리 청취는 하지 않았다. 오디오 생산 파일은 수정하지 않았다.

## 빌드·테스트·보존

- `cmake --preset local`과 Recorder/RecorderTests/RecorderProbe Release 빌드 성공. PATH에 cmake가 없어 `C:/Users/claude/tools/cmake/bin/cmake.exe`를 사용했다.
- PowerShell 인수는 `'-v:m'`로 인용했다. `-m`의 MSB3491(lastbuildstate 접근 거부)은 안내된 `'-m:1' '-nr:false'`로 해결했다. 로그: `build/seam-validation/final-build.log`.
- 작업 시작 기존 바이너리: **48 suite/554 통과/0 실패**. 최종 단독 전체: **48 suite/563 통과/0 실패**, 연속 두 번 확인했다. 로그: `final-all.log`, `confirm-all.log`.
- 신규 seam 검사는 8건이다. 총 차이 9건 중 1건은 HEAD에 이미 있던 `EditJournalTests.cpp:168`의 `Provisional rate adoption rescales markers...`가 재빌드에 포함된 것이다(21 → 22건). 그 파일은 수정하지 않았다.
- 실제 미디어 옵션 playback-engine: **43 통과/0 실패**(정규 40+실제 미디어 3). 로그: `after-final.log`. `dual-playback`, `seek-generation`, 큐 등록명 `audio-prefetch-underrun`, `audio-cut-render`는 전체 실행에 포함됐다.
- 신규 검사는 세 편집의 ±10프레임/PCM, 양방향 스크럽, startup/gap/epoch, 비정렬 PTS, 두 카메라 프리롤 취소·handoff, gap 중 다음 시작 프리롤이다. 기존 suite에 등록했다.
- **중간에 전체 suite와 실제 미디어 suite를 동시에 실행한 1회가 `0xC0000005`로 종료됐다.** stdout 버퍼링으로 위치를 특정하지 못했고 충돌 모듈/원인은 미확인이다. `full-run-access-violation.log`와 `full-run-access-violation-result.txt`에 보존했다. `RECORDER_CUT_SEAM_FLUSH_TEST_OUTPUT=1`로 즉시 출력하는 단독 전체 재실행 두 번은 통과했다. 이 종료를 해결된 것으로 간주하지 않는다.
- 지정 프로젝트를 `build/seam-validation/real-project`로 복사한 후 **복사본의** project.writer.lock만 삭제했다. 나머지 원본 11개 파일은 복사본과 SHA-256이 모두 같고 원본 lock은 존재한다. MP4 해시: `896A9C54364AED1933697478140FD3C56EA354E38BFE97A3D68410064A0733FF`.
- 커밋/git 메타데이터 쓰기/push/release.py를 실행하지 않았다. ProductIdentity·버전·site·릴리스 노트를 수정하지 않았다. 사용자 `0.1.4.html`의 시작/종료 해시: `7FC0AF8ECB6823DB92AF7FF1343C8671E587037D30B263B6EBA58BBF238785D2`.
- 다른 세션 영역 수정 **0개**. 생산 파일은 VideoPlaybackEngine.cpp/.h만 수정했다. 테스트는 PlaybackTests.cpp와 새 CutSeamChecks.h다.

## 남은 위험

250ms를 넘는 디코더 준비/초장 GOP/저장장치 정체/GPU 포화, 재생 시작 직후 가까운 경계에서는 직전 프레임 유지가 길어질 수 있다. 오디오 시간을 늦추지 않으므로 일부 영상 프레임을 건너뛸 수 있다. 모든 하드웨어에서 지연 0을 보장하지 않는다.

실제 D3D11VA decode/GPU 완료 이벤트는 사용했지만 실제 DXGI Present·모니터 출력·ASIO DAC는 측정하지 않았다. 4K/혼합 GPU 장시간 스트레스와 대표가 본 특정 GUI 실행은 미검증이다. 위 병렬 전체 실행의 접근 위반 1회도 원인이 확인되지 않은 통합 위험이다. 프리롤은 카메라당 작업 스레드 1개를 추가 사용한다.
