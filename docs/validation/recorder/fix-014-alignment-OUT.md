# fix-014-alignment 실측·수정 보고

2026-09-10, 작업 트리 `C:\Users\claude\gocue-rec-asio`, 브랜치 `fix-014-alignment`. 실행기 OUT 경로가 별도로 전달되지 않아 사전에 알린 이 경로에 작성했다. 파일 수정·빌드·합성 테스트만 수행했다. 커밋/push/release.py, ProductIdentity/버전/릴리스 노트/site 수정은 하지 않았다. 캡처보드·카메라·ASIO 실장치는 열지 않았다.

## 결론과 확인 범위

정렬 차이를 재현하여 다섯 경로를 수정했다.

1. 녹화 CFR의 대기 제한이 카메라/디코더 전달 지연을 포함하지 않아 가까운 프레임이 도착하기 전에 이전 프레임을 선택했다. 실제 파일의 추적 CSV에서 격자 대비 선택 영상 시각은 평균 **-15.282ms**였다. 같은 위상·전달 지연을 합성한 실행에서 잘못 선택한 소스 프레임은 **59/60 → 0/60**으로 줄었다.
2. 재생 영상 요청에 고정 60Hz 한 주기를 더해 오디오·플레이헤드보다 **800샘플/+16.667ms** 앞섰다. 같은 audible cursor를 사용하도록 수정했다.
3. 타임라인/스크럽 썸네일 요청을 **0.5초 단위로 내림**하고 임의의 가까운 캐시 이미지를 표시했다. 실측 지점에서 재생과 **최대 29프레임/-483.333ms** 차이를 만들었다. 프레임 경계로 요청하고 요청 시각을 포함하는 캐시만 표시한다.
4. 썸네일 디코더가 요청 PTS 이후 프레임을 선택하여 **+1프레임/+16.667ms**, 마지막 프레임 구간에서는 빈 결과를 냈다. 디코딩 프레임의 샘플 구간으로 포함 여부를 판단한다.
5. 32kHz/60fps의 반올림 경계에서 RenderPlanCompiler와 VideoIndex의 프레임 번호가 달랐다. 샘플 533에서 이전 컴파일러는 프레임 0, 인덱스는 프레임 1이었다. 반올림된 PTS 샘플 경계를 공유하도록 수정했다. 제공 프로젝트의 48kHz/60fps에서는 이 문제가 발생하지 않았다.

제공 프로젝트 복사본의 헤드리스 실측에서 수정 후 영상 seek는 **16개 유효 지점 모두 프레임 오차 0**, 썸네일도 같은 16개 지점에서 **영상과 차이 0ms**, WAV는 **11개 지점 모두 독립 PCM24 바이트 읽기와 일치**했다. 합성 출력의 재생 요청/플레이헤드 차이도 측정 구간 전체 **0샘플**이다. 아래의 물리적 A/V 동기 및 기존 녹화 내용에 관한 한계는 별도다.

## 원본 보호와 재현 방법

원본:

`C:\Users\claude\AppData\Local\Temp\claude\C--Users-claude--local-bin\08ab1c51-2e72-4134-a6ce-9b201246a06f\scratchpad\rec_selftest\demo-f0ad256141554a16bf14d905d8a1d710`

실측 복사본 및 로그: 작업 트리의 `build\alignment-evidence\`. 프로젝트는 `project-copy\project.recorder`. 복사본의 `project.writer.lock`만 삭제했다. 원본 lock은 남아 있으며, 복사본의 나머지 **11개 파일 SHA-256이 원본과 모두 일치**한다(`original-copy-sha256.json`). 프로브는 체크포인트와 미디어를 읽기만 한다.

FFmpeg SDK: `C:\Users\claude\SDKs\ffmpeg-lgpl\ffmpeg-n8.1-latest-win64-lgpl-shared-8.1\bin`. `ffprobe -show_streams -show_packets` 및 `-select_streams v:0 -show_frames`로 실제 MP4를 읽었다. 결과는 `ffprobe-packets.json`, `ffprobe-frames.json`이다.

추가한 실행 모드:

```powershell
& build\vs2022\recorder\RecorderProbe_artefacts\Release\RecorderProbe.exe alignment --project build\alignment-evidence\project-copy\project.recorder --report build\alignment-evidence\after.json
```

`tools/AlignmentProbe.cpp`는 실제 VideoPlaybackEngine의 decoder factory에 **FFmpeg 소프트웨어 디코더**를 넣는다. 요청 인덱스의 IDR로 seek한 뒤 실제 AVFrame PTS가 목표 패킷 PTS와 같은지 검사한다. GPU texture/Present는 만들지 않는다. 오디오는 TimelineAudioRenderer 출력과 WAV 파일의 `44 + sourceSample × channels × 3` 바이트 위치를 별도 읽어 비교한다. 재생 중 좌표는 TimelineTransport에 합성 BlockStamp/출력 콜백을 공급해 기록한다. `before.json`, `after.json`은 프레임 선택과 시각을 보고하는 측정 자료이며, GPU 화면 표시 지연의 측정 자료는 아니다.

## 녹화·파일 실측

| 항목 | 실측값 | 해석 |
|---|---:|---|
| take N0 / Nstop | 63,840 / 544,320 | ASIO master 좌표 |
| placementSample | 0 | 프로젝트 타임라인 배치 |
| logicalLength | 480,480 = 10.010000초 | Nstop - N0 |
| 영상·마이크 clip sourceIn / timelineStart / length | 0 / 0 / 480,480 | N0가 sourceIn에서 빠진 오류 없음 |
| 영상 stream time_base / start_pts | 1/15,360 / 0 | 첫 프레임 PTS는 N0가 아닌 0 |
| 영상 첫 / 마지막 PTS=DTS | 0 / 153,600 | 프레임 0 / 600, 각 duration=256 |
| 영상 패킷·전체 디코드 프레임 수 | 601 / 601 | FFprobe와 Mp4RecoveryIndex::decode 모두 일치 |
| 영상 인덱스 길이 | 480,800 = 10.016667초 | 마지막 CFR 프레임까지; 논리 길이보다 320샘플/6.667ms 김 |
| MP4 AAC 첫 DTS | -1,024 = -21.333ms | skip_samples=1,024, 다음 DTS=0 |
| AAC stream duration | 480,480 | 마지막 packet DTS=480,256, duration=224 |
| 복구 검증기의 AAC 디코드 샘플 수 | 481,280 | 마지막 AAC 프레임의 패딩 포함; 프로젝트 재생 길이로 쓰지 않음 |
| 마이크 WAV | PCM24 mono, 48kHz, 480,480샘플 | 앱 재생 원본; AAC 프라이밍과 무관 |
| 영상/마이크 availableRanges, gaps | [0,480480), 없음 | 파일 패딩을 타임라인에 추가하지 않음 |

601번째 영상 프레임 자체가 끝나는 시각은 480,800이지만, 재생 프레임의 타임라인 끝은 clip length로 잘려 **480,480**이다. 블록을 480,800으로 늘이거나 sourceIn에 N0를 더하는 수정은 하지 않았다.

## 타임라인 t → 소스 t′ 체인

`S=clip.timelineStartSample`, `I=clip.sourceIn`, `u=I+t-S`, `Fs=48000`, `F=800 samples/frame`. 구간은 모두 끝을 포함하지 않는다.

| 단계 | 실제 변환/동작 | 코드 위치 |
|---|---|---|
| ASIO 수집 | master N0부터 Nstop 직전까지 원본 PCM을 채택; WAV 위치는 N-N0 | `audio/RecorderAudioEngine.cpp:197`, `:202` |
| master 시계 | callback QPC ↔ ASIO samplePosition 회귀, epoch 검증 | `sync/ClockMapper.cpp:112` |
| 카메라 시계 | Nv=S(q-Lcam), mapped time=(Nv-N0)×10^7/Fs; N0 유지 | `sync/CameraClockMapper.cpp:186`, `:213` |
| CFR | k/FPS 격자에 가장 가까운 전달된 후보, 동률은 이전 후보; PTS=k | `record/VideoCfrScheduler.cpp:91`, `record/TakeController.cpp:334` |
| 전달 지연 보정 | availableTime-captureTime의 관측 최대값만큼 대기 제한 연장; PTS/카메라 시각은 이동하지 않음 | `record/VideoCfrScheduler.cpp:77`, `:98`; `record/TakeController.cpp:292`, `:302` |
| NVENC | frame PTS=k, duration=1, B-frame=0 | `record/NvencEncoder.cpp:124` |
| MP4 | 영상/AAC의 각 time_base를 mux time_base로 재배율; AAC priming 보존 | `record/Mp4TakeWriter.cpp:157`; `record/ReferenceMixWriter.cpp:49` |
| take 배치 | normal take는 activeTimelineEnd에 배치, sourceIn 기본값 0, 길이는 asset.logicalLength | `app/RecorderDocument.cpp:362`, `:385` |
| 샘플↔편집 프레임 | frameToSample/sampleToFrame는 절대 격자에서 최근접 반올림; 재생 프레임 포함 판정과 편집 스냅은 구분 | `model/RecorderModel.cpp:55`, `:60` |
| VideoIndex | sample=round((PTS-startPTS)×Fs×time_base), frameAt(u)는 그 반올림 경계의 포함 프레임 | `media/MediaIndex.cpp:51`, `:73`, `:110` |
| MP4 복구 | tfdt/trun PTS/DTS를 보존하여 remux; 전체 decode extent 확인. 고정 Nstop이 있으면 recovered extent를 논리 길이로 자름 | `storage/Mp4RecoveryIndex.cpp:225`, `:244`; `storage/RecoveryScanner.cpp:455`, `:518` |
| WAV 인덱스 | chunk.firstSample은 테이크 상대 좌표; 누락 구간은 무음, logicalLength 유지 | `media/MediaIndex.cpp:137`, `:144` |
| RenderPlanCompiler | span sourceIn+(t-span.start); camera는 반올림된 프레임 경계 포함, audio는 샘플 위치 | `playback/RenderPlanCompiler.cpp:44`, `:92` |
| 앱 재생 준비 | camera availableRanges와 clip/source 길이의 교집합으로 영상 배치; 마이크는 WAV | `app/RecorderSession.cpp:333`, `:338`, `:348` |
| 영상 seek/프레임 선택 | frameAt(u), 실제 AVFrame PTS 검사; 표시 범위를 [S,S+length)로 자름 | `playback/VideoPlaybackEngine.cpp:222`, `:411`, `:459` |
| 오디오 render | WAV read(span.sourceIn+t-span.timeline.start); gap은 0 | `playback/TimelineAudioRenderer.cpp:256` |
| audible cursor | 출력 위치+QPC 경과-출력 지연; 소프트웨어 선행 큐와 하드웨어 tail을 제외 | `playback/TimelineTransport.cpp:103` |
| 플레이헤드 | Session::playhead도 같은 audibleCursor. 영상 service의 고정 +1/60초 제거 | `app/RecorderSession.cpp:414`; `playback/TimelineTransport.cpp:299` |
| xFor/sampleFor | x=header+(t/Fs-viewStart)/viewSeconds×width; 역변환은 llround | `ui/TimelineView.cpp:336`, `:337` |
| 썸네일 | 동일한 u를 포함하는 프레임 경계로 요청, 디코딩된 [sample,endSample)로 조회 | `ui/TimelineView.cpp:111`, `:117`; `media/ThumbnailCache.cpp:76`, `:87`, `:155` |
| 스크럽 | Session은 [0,timelineEnd]로 clamp; transport는 drag ≤15Hz, release 즉시 새 seek generation; 영상은 위 frameAt(u) | `app/RecorderSession.cpp:405`; `playback/TimelineTransport.cpp:86` |

`src/model/RenderPlanCompiler.cpp`는 CMake에서 HEADER_FILE_ONLY이다. 실제 구현인 `src/playback/RenderPlanCompiler.cpp`만 수정했다.

## 실제 seek/썸네일/오디오 표

아래 PTS는 MP4의 1/15360 단위다. 영상 frame-start와 임의의 t가 최대 한 프레임 미만 다른 것은 포함 구간 내 위치이며, **프레임 선택 오차가 아니다**. 영상 프레임 오차는 모든 유효 행에서 0이었다. 오디오 위치는 u=t이며 별도 PCM 비교와 합성 고유 샘플 패턴 테스트로 검증했다.

| t (샘플) | t (ms) | 실제 영상 PTS / 소스 프레임 시작 | 수정 전 직접 썸네일 시작 | 수정 전 0.5초 UI 요청의 프레임 차이 | 수정 후 썸네일 시작 / 영상 차이 |
|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 / 0 | 0 | 0 | 0 / 0ms |
| 399 | 8.3125 | 0 / 0 | 800 | 0 | 0 / 0ms |
| 799 | 16.6458 | 0 / 0 | 800 | 0 | 0 / 0ms |
| 800 | 16.6667 | 256 / 800 | 800 | -1 | 800 / 0ms |
| 23,999 | 499.9792 | 7,424 / 23,200 | 24,000 | -29 | 23,200 / 0ms |
| 24,321 | 506.6875 | 7,680 / 24,000 | 24,800 | 0 | 24,000 / 0ms |
| 47,999 | 999.9792 | 15,104 / 47,200 | 48,000 | -29 | 47,200 / 0ms |
| 63,840 | 1,330 | 20,224 / 63,200 | 64,000 | -19 | 63,200 / 0ms |
| 240,363 | 5,007.5625 | 76,800 / 240,000 | 240,800 | 0 | 240,000 / 0ms |
| 479,679 | 9,993.3125 | 153,344 / 479,200 | 480,000 | -29 | 479,200 / 0ms |
| 480,479 | 10,009.9792 | 153,600 / 480,000 | 없음 | 0 | 480,000 / 0ms |
| 480,480 | 10,010 | gap | 해당 없음 | 해당 없음 | gap, 오디오 0 |

0.5초 열은 기존 UI 요청식을 재현한 값이다. 기존 `nearest()`가 이미 캐시된 다른 시각의 이미지를 선택하면 오차는 요청 이력에 따라 달라지므로, -29프레임을 전체 가능한 오차의 상한이라고 주장하지 않는다.

| 합성 출력의 audible t | 수정 전 영상 요청 | 수정 전 영상 시작 | 수정 후 요청 / 영상 시작 | 요청-오디오 오차 전→후 |
|---:|---:|---:|---:|---:|
| 384 | 1,184 | 800 | 384 / 0 | +16.667ms → 0ms |
| 864 | 1,664 | 1,600 | 864 / 800 | +16.667ms → 0ms |
| 1,344 | 2,144 | 1,600 | 1,344 / 800 | +16.667ms → 0ms |
| 1,824 | 2,624 | 2,400 | 1,824 / 1,600 | +16.667ms → 0ms |

이 출력은 48kHz, 480샘플 block, outputLatency=1536(32ms) 조건이다. 출력 지연을 제거한 것이 아니라 **이미 지연을 뺀 audible 좌표에 영상만 더하던 800샘플**을 제거했다.

## 녹화 내용의 시각과 입력/출력 보정

원본 `index/cam1-source-ids.csv`를 재배율한 `capture-grid.csv` 실측:

| 출력 프레임 | 소스 ID | CFR 격자(ms) | 기록된 mapped 시각(ms) | 차이(ms) |
|---:|---:|---:|---:|---:|
| 0 | 14 | 0 | 0 | 0* |
| 1 | 15 | 16.6667 | 1.5208 | -15.1459 |
| 2 | 16 | 33.3333 | 18.1875 | -15.1458 |
| 600 | 614 | 10,000 | 9,984.8541 | -15.1459 |

프레임 1~600의 평균/최소/최대 차이는 **-15.281561 / -15.375067 / -15.125033ms**. callbackQpc-MF PTS는 QPC=10MHz 조건에서 평균 **18.672014ms**, 범위 **18.4175~19.7477ms**였다. *첫 프레임의 0은 음수 preroll을 0으로 배치한 기록이므로 물리적 노출 시각이 N0와 같다는 증거가 아니다.

합성 CFR 입력은 위상 1.5208ms, 전달 지연 18.672ms를 사용한다. 기존 제한은 60개 중 59개에서 다음에 도착할 더 가까운 후보를 놓쳤다. 수정은 이 관측 지연만큼 대기를 늘려 0/60 오선택으로 만들었고, source ID/PTS 격자는 이동시키지 않는다. **0프레임 오차는 최근접 소스 프레임 oracle 기준이며, 카메라 위상 자체가 0ms가 되었다는 뜻이 아니다.** 카메라가 끊기면 관측 지연+한 native 주기 후 대기가 끝나고 stop drain은 즉시 진행한다.

보정 부호도 추적했다. 카메라는 `q-Lcam`이므로 양수 Lcam은 캡처 시각을 앞당긴다. 더빙은 `O0=masterBufferStart+projectOffset+reportedOutputLatency+outputResidual`, 입력 좌표는 `inputStamp.samplePosition-(reportedInputLatency+inputResidual)`이며 프로젝트 위치는 `Pstart+correctedInput-O0`다(`sync/ClockMapper.cpp:181`, `audio/RecorderAudioEngine.cpp:397`, `:698`). 이 세 경로의 부호를 바꾸지 않았다.

제공 normal take의 inputLatency=1056(22ms), outputLatency=1536(32ms), capture inputOffset/outputOffset/Lcam은 모두 0이다. normal 녹화의 WAV 좌표는 N-N0이며 더빙용 입력 보정 어댑터를 적용하지 않는다. 녹화된 신호에 공통 flash/click 기준이 없고 마이크 신호도 매우 작아, **이 22ms가 실제 영상·마이크 이벤트의 오차인지 확정하지 못했다.** 추측으로 WAV를 당기거나 N0를 변경하지 않았다.

## 변경 파일과 병렬 세션 경계

주요 수정: `media/ThumbnailCache.{h,cpp}`, `playback/TimelineTransport.cpp`, `playback/RenderPlanCompiler.cpp`, `model/RenderPlanCompiler.h`의 선언 주석, `record/VideoCfrScheduler.{h,cpp}`.

다른 세션 영역의 수정은 다음뿐이다.

- `src/record/TakeController.cpp:287`, `:292`, `:302`: `availableTime` 지역 변수 선언, 기존 프레임 소유권 해제 try/catch 안에서 `mapper->now(qpcNow())` 측정, 그 값을 CFR push에 전달. 예외 경계/상태 전이 로직은 수정하지 않았다.
- `src/ui/TimelineView.cpp:111`, `:115`, `:117`, `:139`: 프레임 경계 계산/요청, 포함 프레임 캐시 조회, 다른 시각의 legacy thumbnail fallback 차단. 이전 114행의 0.5초 step도 삭제했다. diff는 추가 4줄/삭제 4줄이다. 드래그/키 라우팅/레이아웃/RecordView는 수정하지 않았다.

`media/MediaIndex.*`, `playback/VideoPlaybackEngine.*`, `RecorderSession.*`, MainComponent/ExportDialog, TimelineInteraction/RecordView는 읽기만 했다.

회귀는 기존 `CfrSchedulerTests.cpp`, `PlaybackTests.cpp`, `SeekGenerationTests.cpp`, `DualPlaybackTests.cpp`에 추가했다. 측정 도구는 `tools/AlignmentProbe.cpp`, 진입점은 `tools/RecorderProbe.cpp` 두 줄, 빌드 연결은 `recorder/CMakeLists.txt`의 RecorderProbe source 한 줄이다.

## 빌드·검증

PATH에 cmake가 없어 `C:\Users\claude\tools\cmake\bin\cmake.exe`로 같은 preset을 실행했다. configure 성공. 최초 병렬 MSBuild의 MSB3191 디렉터리 접근 오류는 지정된 `-m:1 -nr:false`로 해결했다. PowerShell이 `-v:m`을 분리하지 않도록 각 인자를 인용했다. 측정 도구 연결 중 재configure 전 LNK2019, 실행 중 테스트 EXE와 재링크가 겹친 LNK1104도 각각 재configure/기존 프로세스 종료 후 재빌드하여 해결했다.

최종 실행 명령:

```powershell
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --preset local
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --build --preset local-release --target Recorder --target RecorderTests --target RecorderProbe -- '-m:1' '-nr:false' '-v:m' '-nologo'
& build\vs2022\recorder\RecorderTests_artefacts\Release\RecorderTests.exe
```

기준선 전체 실행은 **48개 suite 결과, 555 passed, 0 failed**였다(`baseline-tests.log`). 요청에 적힌 554와 1건 차이가 있어 관측값을 그대로 기록한다. 썸네일/영상 lead 회귀는 수정 전 각각 실패했고(`before-thumbnail-tests.log`, `before-playback-tests.log`), 32kHz 반올림 회귀도 수정 전 실패했다(`rounding-before-tests.log`).

최종 결과: **Recorder / RecorderTests / RecorderProbe 빌드 성공, 전체 48개 suite 결과 563 passed / 0 failed, 프로세스 exit 0.** 기준선 대비 회귀 8개를 추가했다. `final-configure.log`, `final-build.log`, `final-all-tests.log`에 결과를 보존했다. 기존의 C4324 등 컴파일 경고는 남아 있으며 최종 빌드 오류는 없다. `git diff --check`도 통과했다.

최종 바이너리의 추가 실측(`final-after.json`, `final-summary.json`)은 영상 16지점/썸네일 16지점/재생 중 36지점/PCM 11지점에서 모두 기대 좌표와 일치했다. 최종 미디어 디코드도 영상 601프레임, 영상 길이 480,800샘플이었다. 위 표의 원시 before/after 자료와 원본 대조 해시는 모두 `build/alignment-evidence`에 남겼다.

추가 회귀 8개는 전달 지연/카메라 단절 시 제한, 이동·trim·비영점 PTS origin과 고유 PCM 패턴, 출력 지연이 있는 재생 좌표, 실제 소프트웨어 영상의 썸네일 첫/마지막 구간, 캐시 이웃 프레임 차단, 48/44.1/32kHz 반올림, 실제 UI 컴포넌트의 zoom/scroll 후 x↔sample 왕복을 검사한다. INT64_MAX 부근 썸네일 경계도 확인한다. 기존 dual-playback 1000회 30/60fps seek, source-reanchor, recovery 및 전체 suite를 함께 실행한다.

## 남은 위험 / 실장치에서 확인할 부분

- 이미 기록된 MP4의 소스 프레임 선택은 이 수정으로 재작성되지 않는다. 제공 파일에 내포된 약 -15.28ms의 CFR 선택 위상은 남는다. 기존 프로젝트는 재생·플레이헤드·썸네일 좌표 수정의 혜택을 받지만, 기록되지 않은 프레임을 복원한 것은 아니다.
- 카메라의 물리적 노출, 마이크 입력 지연, DAC 출력, DXGI Present에서 실제 화면 광출력까지의 시간은 측정하지 않았다. 실장치 flash/click 왕복으로 최종 A/V 동기를 확인해야 한다. 고정 영상 lead 제거는 헤드리스 좌표 오차 수정이며 모니터 지연 캘리브레이션을 대체하지 않는다.
- 전달 지연 보정은 관측된 최대 지연을 기준으로 한다. 이후 더 큰 지연 급증·실제 프레임 유실, 시작 preroll·종료 drain은 여전히 가용 후보 중 프레임을 선택한다. 정상적인 지속 전달 조건의 최근접 선택 회귀와 유실 시 유한 대기를 검증했다.
- 정확한 썸네일이 준비되기 전에는 다른 시각의 이미지를 대신 보여주지 않아 잠시 빈 슬롯/준비 표시가 나타날 수 있다. 32개 작업/16MiB 캐시 제한과 녹화 중 파생 작업 중지는 유지한다.
- 대표가 본 모든 실기 프로젝트의 원인이 이 다섯 경로였다고 확정하지 않는다. 제공 복사본과 합성 조건에서 확인된 차이만 수정·보고했다.
