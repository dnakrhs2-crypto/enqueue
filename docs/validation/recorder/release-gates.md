# Recorder 라운드 31 출시 게이트

기준: [설계 §14.3·§15·Claude 검토 반영](../../superpowers/specs/2026-09-09-recorder-design.md), [구현 계획 라운드 31](../../superpowers/plans/2026-09-09-recorder.md). **현재 출시 승인 보류.** 구현·합성 회귀의 통과와 승인 실물에서의 통과를 별도로 판정한다.

## 입력 증거와 결함 판정

`C:\Users\claude\tools\claude_harness\recorder_validation`의 `r*`와 `merged` 아래 JSON **757개를 모두 파싱했고 오류는 0개**다. 프로젝트·저널 메타데이터도 포함한 개수다. 폴더별 목록·JSON SHA-256은 이번 빌드의 `r31/input-evidence-index.json`에 남겼다. `r15/r23/r26/r27/r28` 이름의 별도 실측 폴더는 이 위치에서 찾지 못했으며, 계획서의 해당 라운드 기록과 `merged`의 관련 보고서를 함께 대조했다. 사용자 추가 관측인 r2527 수치는 전달받은 관측으로 구분한다.

| 증거·현상 | 원인과 이번 처리 | 검증 범위 |
|---|---|---|
| `r17/DEFECT-dubbing-epoch.md`, `dubbing-off.json`, `dubbing-on.json`: 수신 3,616/3,618프레임, 제출 0, epoch 오류 | `DubbingController`가 start의 ASIO/카메라 epoch를 매 프레임과 deadline에 강제했다. 준비된 ASIO/QPC로 첫 프레임을 O0에 앵커하고 이후 절대 MF PTS 차이를 사용한다. fit epoch 변화 자체로 중단하지 않는다. | 컨트롤러 합성 mic OFF/ON·두 레인, 시작 직후 master/camera epoch +1, CPU MPEG4/AAC mux·전수 decode. 실물 OFF/ON 60초는 Claude 재측정 전까지 **미확인 → 스파이크 6**. |
| 같은 더빙 기준음의 로컬 코드 대조 | 별도 renderer의 fade 분모·경계와 mute 처리가 공통 재생/export와 달랐다. 선택한 import lane을 공통 `TimelineAudioRenderer`로 렌더한다. | 공통 renderer와 샘플별 일치, 선택 lane mute, mono/stereo, 다른 lane 제외, 컷/gap, 기존 더빙·export 회귀. 실물 출력·AAC 청취/루프백은 스파이크 6. |
| export probe의 기존 out-dir·실패 잔여 충돌, 합성 소스 parent 누락 | 모든 export fixture root를 충돌 없는 새 형제 폴더로 만들고 원래 폴더는 보존한다. `syntheticCamera`가 encoder를 열기 전에 parent를 생성한다. | 빈 폴더·잔여 파일·반복 충돌 회귀와 audio-materials 실행. 최종 NVENC 3소스·합성 카메라 실행은 Claude. |
| `merged/r25/p60.json`: `Selected physical input is unavailable` | product dual-load에 `--inputs`를 추가했다. 기존에 전달되던 `--synthetic-audio`와 함께 사용 가능하다. 합성 native view도 실제 선택된 active→physical 순서로 공급한다. | 1-based·중복·빈 항목·범위·none 파서 회귀. 2캠 GPU 실행은 Claude. 기존 synthetic-audio 옵션을 새 기능으로 계산하지 않는다. |
| `r05`와 `merged/dual-preview.json`: `Generate source JPEG: Invalid argument` | 현재 병합본의 MJPEG 합성 warmup PTS 수정과 대조했다. 이번 라운드에서 CPU MJPEG decoder를 변경하지 않았다. | 기존 queue-isolation/mixed-fps 회귀. 혼합 2캠 60초 확인은 아래 명령. |
| `r11/r11b` seek 지연, `r11c`·`r1718` 후속 PASS | r11c의 cache hit 우회·GPU 완료 이벤트 수정이 현재 소스에 있다. 후속 보고의 warm/seek PASS를 보존하고 새로운 실측으로 계산하지 않는다. | 기존 playback-engine/seek-generation, 반복 취소 및 자원 회수. GPU 지연·텍스처·두 실물 프리뷰는 스파이크 5. |
| `r12/early-demo.json`, `r1718/demo.json`, `merged/demo.json`의 20회 PASS | 추가 재현 가능한 UI 손실/크래시를 이 증거에서 추출하지 못했다. | 전체 회귀의 ui-wiring/lifecycle. 2캠·긴 테이크 응답 시간은 미확인. |
| seed 시험의 리테이크 전환 실행 누락 | 첫 보강 실행에서는 version 선택 시도 75회 중 실제 전환 0회였다. 무작위 컷이 stack을 해체하기 전에 비활성 버전을 복원하는 첫 연산과 성공 횟수 검사를 추가했다. | seed 909, 1,000회; 각 연산의 시도/commit 수 및 undo/redo 중간 hash/selection 확인. |
| 10,000클립 검사에서 찾은 fade 메타데이터의 이차 시간 검색 | `RenderPlanCompiler`가 각 fade 경계마다 전체 프로젝트 클립을 순회했다. 해당 트랙의 정렬·비중첩 조건을 이용해 시작/끝을 각각 이진 탐색한다. | 10,000개 fade 필드의 144-sample 내부 경계·양 끝 보존 및 전체 컷/PCM 회귀. 복합 저장 시험 전체의 속도 향상을 입증한 결과로 해석하지 않는다. |

현재 녹화의 `CameraSampleTimeMapper`도 r25 병합으로 엄격한 epoch 검사 경로다. r17 당시의 `TakeCameraMapper`가 지금도 그대로 있다는 가정은 사용하지 않았다. 새 `AnchoredCameraTimeMapper`는 더빙에만 연결했다. 동일한 음수 preroll stamp 재검사는 허용하고, 새 generation·PTS/QPC 역행·큰 timestamp jump·device timestamp 역행은 해당 레인을 중단한다. native ASIO reset/resync/sample/rate/buffer/xrun/latency 불연속 검사는 유지한다.

이 결정은 라운드 **04/17/25/30**의 더빙 시간 매핑 재계획이다. 장시간 드리프트 보정이 매 프레임 재적합되는 것으로 설명하지 않는다. 라운드 30의 루프백/광학·교정 offset·1시간/3시간 측정으로 MF PTS와 ASIO 사이 누적 오차를 판정해야 한다. 기준음 공통화는 라운드 **14/17/21**의 렌더 경로 재검증 대상이다.

큰 문서의 저널 성능은 라운드 **08/19/26**에서 추가 프로파일링·재계획한다. 최종 독립 실행의 10,000클립 복합 검사는 25.166초이며, 그중 durable reorder→undo→redo→worker shutdown 묶음이 19.108초다. 함수별 CPU/파일 I/O와 반복 전체 JSON 직렬화 비용을 분리한 뒤 캐시·entity 단위를 검토한다. 이번에는 저널 schema·검증 hash·durable flush를 생략하지 않았다. 원인별 시간이나 UI 응답 SLA 달성으로 이 합성 묶음 시간을 표시하지 않는다.

## §14.3 항목별 상태

| 출시 판정 항목 | 확인된 범위 | 남은 판정·담당 |
|---|---|---|
| 1. 승인 실물 1/2캠, 프로젝트 30/60 P0·cadence·원본 보존·분리/ASIO 오류 | 과거 GC311G2/FlexASIO 경로 보고와 합성 상태·원본 보존 회귀 | **미확인 → 스파이크 1·2·3, Claude/CEO 실물**. StreamCam+C920, USB/노출/저조도, 광학 지연, 스튜디오 8ch, 실제 분리/오류, 승인 포트, 1시간/3시간. 더빙 P0 수정의 실제 확인 전 출시 차단 유지. |
| 2. stop→clip ≤250ms·첫 재생 ≤2초, 긴 2캠에서도 동일 | 과거 1캠 20회 demo PASS, 현재 headless UI/seek 회귀·10,000클립 문서/조회 | **미확인 → 스파이크 5, Claude 실물**. 실제 2캠 창·양쪽 Present receipt, 30분/3시간 trailer·첫 재생, DPI/긴 한글 장치명. GrowingTakeReader는 장시간 trailer 실측 게이트에 따라 결정한다. |
| 3. 링크/독립 컷의 재생·소재·최종본 동일성, seed 1,000회·10,000클립 | 공통 오디오 샘플 oracle·export mask/gap·retake/undo/reorder/ripple·메타데이터 규모 회귀 | 단위·합성 범위 통과 후에도 **미확인 → 스파이크 5·7**. GPU 최종 3소스 반복, 실시간 청취/시각 정렬, NLE·실제 RF64·장시간 파일. |
| 4. 더빙 Pstart/O0·출력 보정·mic OFF/ON·리테이크·완성음 보존 | 원점 부호, 입력 지연 중복 적용 없음, epoch +1, 선택 음원 보존·버전 복원 합성 회귀 | **미확인 → 스파이크 6, Claude/CEO 스튜디오**. 실제 OFF/ON 60초, 교정 ±offset, DAC 루프백/광학, retake 반복, 1시간/3시간 드리프트. |
| 5. 완료 take/edit crash 보존·tail/gap·원본 불변·복구 멱등성 | r07 recovery20, merged edit-recovery PASS 및 전체 recovery/edit-journal/lifecycle 회귀, 가상 >4GiB·disk-full 산술 | **미확인 → 스파이크 4**. 100회 전체 crash matrix의 최종 후보 결과, 실물 동시 부하·긴 I/O, 정전·OS crash·장치/컨트롤러 cache. |
| 6. DLL/정확한 소스/고지·설치/업데이트/제거·외부 인계 | 다른 라운드의 [의존성](dependencies.md)·[설치/업데이트](install-update.md) 문서로 인계 | **미확인 → 스파이크 8, 배포 담당/CEO·클린 PC**. exact corresponding source/toolchain/SBOM·회사 계약·identity·실제 DLL 교체·서명 설치/업데이트/제거. NLE는 스파이크 7. 이 라운드는 배포 파일을 수정하지 않는다. |

## 보장 범위와 진단

제품 설명에 사용할 문구:

> 복구 검증의 보장 범위는 프로세스 crash와 검증된 파일 I/O·durable flush 조건으로 한정합니다. 완료 테이크·저장 완료 편집을 보존하고, 진행 중 녹화의 복구 가능 범위와 tail/gap을 보고합니다. 정전, OS crash, 저장장치·컨트롤러의 캐시/펌웨어 동작에 대한 보존은 아직 검증되지 않았습니다.

정상 I/O 조건의 tail ≤2초 목표를 정전이나 임의의 장기 stall 상한으로 확대하지 않는다. 복구 산출물의 유효 sample/frame·원본 hash·반복 실행의 추가 clip 수로 판정한다.

검토한 진단 경로는 `CaptureTelemetry::toJson/stampJson`, `RecoveryReport::toJson`, Take/Dubbing report·source-id CSV와 이번 hardening report다. QPC/PTS·counter·상태·파일 경로·장치 ID·hash를 기록하며, 원본 PCM/영상 payload·환경 변수·인증정보를 덤프하는 코드는 찾지 못했다. 복구용 미디어 사본과 합성 oracle 미디어는 명시적인 시험/복구 산출물이다. JSON 보고서에는 해당 미디어 bytes를 넣지 않는다. generic JSON writer가 임의 입력의 비밀을 자동 제거한다는 보장은 하지 않는다.

## 실행 결과와 재현

빌드와 원시 로그·합성 미디어 위치: `%TEMP%\gocue-rec-r31-build`. 외부 Claude 증거 폴더는 이번 workspace-write 세션의 쓰기 범위 밖이므로 읽기만 했다. 저장소에는 합성 미디어·빌드 산출물을 추가하지 않았다.

최종 확인: **2026-09-10 KST**, MSVC 19.44.35228 / Windows SDK 10.0.26100 / 공유 JUCE 8.0.15 / FFmpeg `n8.1.2-51-g7ba069f4f1-20260908`.

| 실행 | 결과·종료코드 | 근거 |
|---|---|---|
| configure | **0** | `configure.log` |
| Recorder·RecorderTests·RecorderProbe Release 빌드, 보조 probe 포함 | **0** | `build-release-final.log` |
| Enqueue·LiveMix·EnqueueTests Release 빌드 | **0** | `build-G.log` |
| RecorderTests 전체, seed 909/1,000 | **38/38 suite 통과, 0**, 80.000초 | `RecorderTests-all-final.log`, `r31/unit-run.json` |
| 단독 edit-property | **0**, commit 692 / reject 163 / no-op 145, undo 198단계; reorder 8 / ripple-all 50 / ripple-track 40 / retake-version 2회 commit | `edit-property-final.log` |
| 요청한 hardening 명령, 마지막 독립 실행 | **PASS, 0**, 전체 31.140초 | `r31/hardening.json`, `r31/probe-runs.json` |
| DubbingProbe 합성 epoch 회귀 | **PASS, 0**, epoch +1 후 60프레임 제출·60프레임 decode, 48,000 valid presentation samples | `r31/dubbing-epoch.json` |
| audio-materials probe, 기존 out-dir 재실행 | **PASS, 0**, 기존 keep 파일 보존 | `r31/export-audio.json`, `export-audio-final.log` |
| dual-load 옵션 전달/입력 거부, 장치 미개방 | 정상 help **0**, 중복 입력 **1** 예상값 일치 | `r31/cli-contracts.json` |
| G CTest | **8**, 8,542 통과 / 11 실패 | `G-ctest.log`: 기존 `Settings migration (GoCue -> Enqueue)`의 AppData 폴더 생성·파일 쓰기/읽기 실패. 제한된 AppData 쓰기 환경이며 기존 앱·테스트는 수정하지 않았다. Claude 일반 권한 환경에서 재실행 필요. |
| diff whitespace·CMake 원문 접두사 | **0 / append-only 확인** | `git diff --check`, 원문/현재 CMake 비교 |

hardening의 취소 seek와 WAV export는 각각 warmup 이후와 worker join 후의 handle/thread가 263→263/1→1이었고, 소유 decoder/source/frame·미완료 export 산출물 잔류 0 및 원본 PCM hash 불변을 확인했다. GPU texture 잔류는 미측정이다. 3시간 packet 인덱스의 마지막 가상 offset은 **46,762,233,856**, 가상 RF64 dataBytes는 **12,884,902,299**다. 대용량 검사의 실제 파일은 32-byte 경계 fixture와 80-byte RF64 header다.

규모 검사의 세부 시간은 생성·검증·JSON 왕복 0.934초, adopt/checkpoint 1.968초, visible index+1,000조회 4.186ms, durable reorder/undo/redo/shutdown 19.108초, checkpoint 두 번 재열기 3.134초다. 합성 기능 검사는 통과했으나 **큰 문서 저장 응답 시간 게이트는 추가 작업 대상**이다. 첫 전체 실행에서 새 MP4 fixture의 `.partial` 경로 계약 오류를 검출해 수정했다. 초기 빌드의 새 코드 컴파일 오류도 수정했으며 최종 결과와 구별해 `build-recorder*.log`에 보존했다. 기존 C4324·MSB8029 경고는 남는다.

```powershell
$b = Join-Path $env:TEMP 'gocue-rec-r31-build'
$cmake = 'C:\Users\claude\tools\cmake\bin\cmake.exe'
$ctest = 'C:\Users\claude\tools\cmake\bin\ctest.exe'
$normalized = 'import os,subprocess,sys; sys.exit(subprocess.call(sys.argv[1:],env={k.upper():v for k,v in os.environ.items()}))'
python -c $normalized $cmake --preset local -B $b '-DLIVEMIX_BACKUP_ACCOUNT_FILE:FILEPATH='
python -c $normalized $cmake --build $b --config Release --target Recorder RecorderTests RecorderProbe Enqueue LiveMix EnqueueTests --parallel 4 -- /nr:false
$recTests = "$b\recorder\RecorderTests_artefacts\Release\RecorderTests.exe"
$recProbe = "$b\recorder\RecorderProbe_artefacts\Release\RecorderProbe.exe"
$recApp = "$b\recorder\Recorder_artefacts\Release\Recorder.exe"
& $recTests --seed 909 --iterations 1000
& $recTests --suite edit-property --seed 909 --iterations 1000
& $recProbe hardening --clip-count 10000 --large-files --seed 909 --iterations 1000 --project-dir "$b\r31\project" --report "$b\r31\hardening.json"
& $recProbe dubbing --synthetic-epoch-regression --project-dir "$b\r31\epoch" --report "$b\r31\dubbing-epoch.json"
New-Item -ItemType Directory -Force -Path "$b\tests\assets" | Out-Null
Copy-Item -LiteralPath 'tests/assets/tone440.wma','tests/assets/sweep_ref.wav','tests/assets/sweep.m4a' -Destination "$b\tests\assets"
python -c $normalized $ctest --test-dir $b -C Release -R '^EnqueueUnitTests$' --output-on-failure
```

`hardening`은 3시간 source **메타데이터**를 가진 10,000클립, 실제 checkpoint/편집 저널, 가상 offset·RF64 header, 648,000 packet 인덱스, seed 편집, 20회 worker 생명주기마다 1,000 seek, 실제 WAV export 취소 20회를 실행한다. resource 수는 warmup 후/worker join 후의 프로세스 handle/thread와 소유 decoder/source/frame 해제를 확인한다. **GPU 텍스처는 UNAVAILABLE**로 보고한다. 실제 3시간 미디어·4GiB 파일·GPU 자원 회수·장시간 성능을 이 결과로 통과 처리하지 않는다.

## Claude 반복 실측 명령

새 exe와 같은 빌드의 DLL·보조 probe를 함께 사용한다. `$devices`는 승인 장치를 선택한 devices.json, `$audio`는 최소 61초의 기준음, `$source`는 검증된 60초 MP4, `$asioDeviceIndex`는 현재 열거값이다. 아래 변수 경로는 기존 증거를 읽는 예시이며 실제 장치·음원 선택을 확인한다. 개발 재시험은 기본 **60초**, 1시간/3시간은 출시/CEO 게이트다.

```powershell
$ev = 'C:\Users\claude\tools\claude_harness\recorder_validation'
$r31 = "$ev\r31"
$devices = "$ev\r01\devices.json"
$source = "$ev\r02\encode60\cam1.mp4"
# 필수 P0: mic OFF/ON 각각 60초, Pstart는 sample 단위의 off-grid 값.
& $recProbe dubbing --devices $devices --asio-device $asioDeviceIndex --audio-file $audio --pstart 137 --mic-modes off,on --seconds 60 --inputs 1 --outputs 1:2 --buffer-size 480 --test-offsets --project-dir "$r31\dub60" --report "$r31\dubbing60.json"
# cam1 실캡처 + cam2 합성 MJPEG, synthetic audio를 보고서에 명시.
& $recProbe dual-load --devices $devices --cam2 synthetic --cam2-format mjpeg --synthetic-audio --inputs 1,2,3,4,5,6,7,8 --project-fps 60 --seconds 60 --report "$r31\dual60-synthetic-audio.json"
# FlexASIO의 실제 사용 가능한 한 입력으로 비교. 프로젝트 30도 별도 보고서로 반복.
& $recProbe dual-load --devices $devices --cam2 synthetic --cam2-format mjpeg --asio-device $asioDeviceIndex --inputs 1 --project-fps 30 --seconds 60 --report "$r31\dual30-physical-input1.json"
# out-dir가 이미 존재하는 경우, 같은 명령을 report 이름만 바꿔 다시 실행.
New-Item -ItemType Directory -Force -Path "$r31\final-existing" | Out-Null
& $recProbe export --fixture edited-one-camera --mode final --source-mp4 $source --audio-cases all --seconds 60 --out-dir "$r31\final-existing" --report "$r31\final-three-sources.json"
# source-mp4 없이 합성 소스를 생성해 parent 생성 회귀를 확인.
& $recProbe export --fixture edited-one-camera --mode final --audio-cases all --seconds 60 --out-dir "$r31\final-synthetic" --report "$r31\final-synthetic.json"
# 영상 export cancellation/실패 정리와 원본·완료 출력 hash.
& $recProbe export --fixture two-camera-independent-audio --mode both --seconds 60 --fault-cases --out-dir "$r31\export-faults" --report "$r31\export-faults.json"
& $recProbe playback --media-dir "$ev\r02\encode60" --seek-storm 1000 --sample-rate 48000 --buffer-size 480 --asio-device $asioDeviceIndex --asio-outputs 1:2 --report "$r31\seek-storm.json"
& $recProbe demo --app $recApp --devices $devices --asio-device $asioDeviceIndex --two-cameras --iterations 20 --report "$r31\dual-demo.json"
```

더빙은 framesSubmitted·videoPackets·decoded frames·availableSamples가 요청 길이와 맞는지, mic ON 원본 샘플 수·hash, OFF WAV 미생성, 기준음 시작·O0·offset 부호·실물 루프백을 함께 확인한다. 단순 종료코드만으로 P0 통과를 기록하지 않는다. 이번 라운드에서 CPU MJPEG·제품 queue/present·native output callback은 변경하지 않았다. 향후 이 경로를 수정하면 관련 capture/dual-load/playback/output **60초 재시험을 반복**한다. 이번 기준음 worker·더빙 매핑 변경에는 위 OFF/ON 재측정이 필요하다.

GPU seek/export 반복 종료 후 handle·thread·D3D live object/texture 잔류 0은 Claude가 별도 계측해야 한다. 위 프로브의 정상 종료나 CPU resource PASS만으로 GPU 누수 0을 선언하지 않는다. 실제 2캠 선택이 없는 `dual-demo`는 미확인으로 남긴다. 스튜디오 정전·승인 실물·클린 PC·외부 NLE·CEO identity/계약 확인은 코드 시험 완료 후에도 남는 출시 항목이다.
