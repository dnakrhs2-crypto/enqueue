# 라운드 05 / 스파이크 3 OUT — RecorderDualProbe

구현 위치는 **`C:/Users/claude/gocue-rec-storage`, 브랜치 `recorder-storage`**, 시작 HEAD `09751bd`이다. 세션의 유일한 저장소 쓰기 허용 경로다. 요청한 `C:/Users/claude/gocue-rec`의 `recorder` 브랜치는 읽기만 했으며 진행 중인 미커밋 수정이 있었다. 경로 차이를 알리고 현재 트리 진행 여부를 비동기로 물은 뒤, 답변 없이 현재 쓰기 가능한 트리에서 진행한다는 가정을 알렸다. 다른 트리의 미커밋 코드는 옮기지 않았다. 현재 기준에는 요청 라운드 외에 07·11·13·16 병합도 포함되어 있다.

제품 UI 확대 전 **독립 진단 exe**와 테스트를 구현했다. 실장치 캡처/ASIO 및 프리뷰를 포함한 GPU 동시 60초 실측은 실행하지 않았다. 그 측정은 요청대로 Claude에게 남긴다. 아래 헤드룸은 허용된 별도 합성 encode-only 실험이다. 실물 P0 출시 gate는 미확인이다.

새 파일:

- `recorder/tools/DualCaptureProbe.cpp`: CLI, 두 독립 EncodePipeline, 두 child HWND를 둔 한 창의 D3D11 프리뷰, 합성/실장치 혼합, 8ch 오디오, 동시 시작·종료, CPU 경합, 실시간 부분 실패 표시, JSON, 두 NVENC 세션 헤드룸.
- `recorder/tools/FramePatternSource.h/.cpp`: 1080p NV12/MJPEG, 기본 cam1=NV12 60/1·cam2=MJPEG 30/1. 프레임 십진 숫자·색상/이동 패턴·magic16/camera8/frame32/checksum8 픽셀 코드. 원본 PTS와 예정 QPC의 DeviceTimestamp, 실제 생성 후 callback QPC를 분리한다. 대기 샘플 2개, 생성 중/디코드 중 슬롯은 각각 별도다. 늦으면 프레임 번호를 건너뛰고 횟수를 기록하며 빠른 몰아 넣기로 숨기지 않는다. 완료 MP4 전체 디코드와 독립 CFR oracle 포함.
- `recorder/tools/DualAudioLoad.h/.cpp`: 실제 RawAudioTap→NativePcmConverter→WavTrackWriter, 합성 8ch 48kHz PCM24 또는 실제 ASIO 입력 1~8, 무음 출력. ASIO가 8개의 물리 입력을 제공하지 않으면 UNAVAILABLE이다. 합성으로 자동 전환하거나 입력을 복제하지 않는다.
- `recorder/tools/DualProbeSupport.h`: writer별 실제 FileIoFaultAdapter 스톨, 원본 오디오 오류의 테이크 전체 중단 신호.
- `recorder/src/support/ThreadPriority.h`: worker 우선순위 범위와 복원, 실패 Win32 코드 보존.
- `recorder/tests/QueueIsolationTests.cpp`: `int runQueueIsolationTests()` 하나만 제공하는 11개 테스트.
- 이 OUT 문서.

기존 파일 수정:

- `CaptureTelemetry.h/.cpp`: 기본값을 유지하는 immutable `pipelineId`, reset 후 ID 보존, 캡처/프리뷰 우선순위 적용 오류 코드.
- `MfCameraCapture.cpp`, `PreviewPresenter.cpp`: 캡처/decode·present worker ABOVE_NORMAL. 콜백에 우선순위 API를 넣지 않았다.
- `EncodePipeline.h/.cpp`: 기존 생성자에 기본값 있는 writer fault 인수 추가, 캠별 live 실패 및 atomic 큐 점유 snapshot, encode/write NORMAL. 표면 overflow는 해당 캠 인코드를 부분 실패로 중단하고 미완료 MP4를 보존한다. 프리뷰/다른 캠/원본 오디오 경로는 독립이다.
- `WavTrackWriter.h/.cpp`: writer NORMAL, 적용 오류 보고, upstream raw loss/reset 때 정상 TakeFinalized를 남기지 않는 lock-free `requestAbort()`.
- `tests/TestMain.cpp`: `queue-isolation` 레지스트리 항목/선언. 다른 main이나 이름 변경 없음.
- `recorder/CMakeLists.txt`: 끝에만 RecorderDualFixtures/RecorderDualProbe 소스·링크·동일 SDK DLL 복사 및 `add_test(NAME RecorderQueueIsolation COMMAND RecorderTests --suite queue-isolation)` 추가.
- 계획서 라운드 05: 구현/측정 상태와 명령을 이 OUT에 연결.

검증 환경은 Windows 11, AMD Ryzen 5 5600X(12 logical CPUs), MSVC 19.44.35228.0, Windows SDK 10.0.26100.0, CMake 4.4.3, 공유 JUCE 8.0.15 및 고정 FFmpeg `n8.1.2-51-g7ba069f4f1-20260908`이다. `nvidia-smi --query-gpu=name,driver_version,pci.bus_id --format=csv,noheader`는 NVIDIA GeForce RTX 5060 Ti, **591.74**, `00000000:07:00.0`를 보고했다(종료 0). 이것은 시스템 GPU 열거이며 각 NVENC CPU-input context의 실제 adapter 선택을 추가로 증명하지 않는다. 그 선택은 기존 encoder JSON의 미확인 범위를 유지한다.

임시 빌드/증거 경로는 **`C:/Users/claude/AppData/Local/Temp/gocue-rec-r05-20260909`**이다. 정식 외부 evidence 디렉터리는 이 sandbox 쓰기 범위 밖이라 사용하지 않았다. 미디어는 저장소 밖 임시 폴더에 있으며 `build/`, `CMakeUserPresets.json`, 공유 JUCE, 기존 Enqueue/LiveMix 및 루트 CMake를 수정하지 않았다. `recorder/CMakeLists.txt` 원본 전체가 최종 파일의 prefix인지 검사하여 append-only를 확인했다. `git diff --check` 종료 0.

실행한 명령의 실질 인수는 다음과 같다. 실행 wrapper는 이전 라운드와 같이 Python subprocess에 `{k.upper(): v for k, v in os.environ.items()}`로 환경 변수 이름을 정규화했다(Path/PATH 충돌 회피). 로그는 임시 빌드 폴더의 `configure.log`, `build*.log`, `queue-isolation-*.log`, `ctest-*.log`, `cli-contracts.json`, `headroom60.log`다.

```powershell
$b = "$env:TEMP/gocue-rec-r05-20260909"
& C:/Users/claude/tools/cmake/bin/cmake.exe --preset local -B $b "-DLIVEMIX_BACKUP_ACCOUNT_FILE=$b/unused-backup.json"
& C:/Users/claude/tools/cmake/bin/cmake.exe --build $b --config Release --target RecorderDualProbe RecorderTests RecorderProbe Recorder --parallel 4
& "$b/recorder/RecorderTests_artefacts/Release/RecorderTests.exe" --suite queue-isolation
& C:/Users/claude/tools/cmake/bin/ctest.exe --test-dir $b -C Release --output-on-failure -R '^Recorder'
& "$b/recorder/RecorderDualProbe_artefacts/Release/RecorderDualProbe.exe" --headroom --project-fps 60 --seconds 60 --report "$b/r05/headroom60.json"
```

| 항목 | 결과 / 종료 코드 |
|---|---|
| Configure | 0 |
| 최초 빌드 | 1: 새 진단 exe의 `mmsystem.h` 누락. 추가 후 수정 빌드/최종 빌드 0 |
| 최초 QueueIsolation | 8 pass / 1 fail, 1: 테스트용 software MPEG4 packet duration=0. 기존 fixture와 같이 1 tick 보완 |
| 최종 QueueIsolation | **11 pass / 0 fail, 0** |
| 전체 Recorder CTest | **15/15 pass, 0**. 최종 로그 `ctest-final.log` |
| CLI 계약 검사 | wrapper 0, 9개 일치. help=0, 잘못된 길이/FPS/중복/옵션 충돌/누락 파일=1/FAIL, 존재하지 않는 ASIO index의 레지스트리 열거만 수행한 경로=2/UNAVAILABLE. 장치를 열지 않음 |
| 60초 합성 두 세션 헤드룸 | **0/PASS**, 아래 수치 |

단위 테스트는 GPU NVENC·실제 카메라/ASIO를 열지 않는다. 실제 NV12/MJPEG production decoder의 WARP 픽셀 검증, 동일 60 CFR 프레임 수를 가진 정상/번호 중복 MP4의 전체 software decode, 카메라 bank/캡처 큐/mailbox 격리, 실시간 합성 소스 worker 진행, RawAudioTap 및 WAV queue overflow의 테이크 중단, 8개 mono WAV 48,000개 샘플/채널 전수 일치, **실제 WAV 파일 owner에 2초 스톨을 주입한 4초 합성 오디오의 192,000 샘플/채널 보존과 4초 큐 상한**을 검사했다. MP4 oracle 테스트의 인코더는 장치 없는 software MPEG4 fixture이며 NVENC 승인으로 표기하지 않는다. C4324(기존 정렬 queue padding)·MSB8029(임시 빌드 경로) 및 기존 FFmpeg empty_moov/AAC priming 경고가 관측되었다.

헤드룸 결과(`r05/headroom60.json`):

| 항목 | 값 |
|---|---:|
| 공통 벽시계, 두 drain 포함 | 60.0055896초 |
| cam1 | 10,239 frames / 170.6341037fps |
| cam2 | 10,238 frames / 170.6174386fps |
| 합산 | **341.2515423fps** |
| 초기 목표 | 156fps, 충족 |
| 120fps 대비 비율 | 2.84376285 |
| process CPU, 100%=logical CPU 1개 | 13.0977381% |
| 두 worker priority 오류 | 각각 0 |

두 독립 NVENC P5 세션을 동시에 준비한 뒤 공통 시작 장벽에서 최대 속도로 제출했다. 캠마다 미리 만든 변화하는 NV12 패턴 bank를 재사용하고 packet은 버렸다. 캡처·디코드·프리뷰·CFR·8ch·AAC·MP4·디스크가 없는 **콘텐츠 의존 encode-only 수치**다. 빌드/CTest를 끝낸 뒤 실행했으며 이 실험의 `--cpu-contention`은 0이었다. 2캠+8ch 전체 gate나 1시간/3시간 보장으로 확대하지 않는다. 이후 oracle의 고유 ID 집계 변경은 헤드룸 코드/패턴/프로파일을 바꾸지 않았으므로 60초 측정을 반복하지 않았다.

설계/기존 계획과 달라진 결정 및 범위:

1. 기존 계획의 `RecorderProbe dual-load` 자리 대신 요청한 **RecorderDualProbe 독립 exe**다. 기본 측정은 60초. 기존 RecorderProbe 진입점과 제품 UI를 변경하지 않았다.
2. 합성 기본 native 입력은 프로젝트 FPS와 독립된 60+30이다. `--cam1-format nv12|mjpeg`, `--cam2-format nv12|mjpeg`로 비교 가능하다. MJPEG는 진단 소스의 FFmpeg CPU 생성/디코드 부하이며 실제 카메라의 MF 내부 decode 비용을 재현한다고 주장하지 않는다. 실장치 MJPEG는 기존 MF→NV12 기본 경로를 사용한다.
3. `captureLoss`는 **MP4 전체 디코드에서 요구되는 고유 native ID가 실제로 사라진 수**다. 30→60의 두 반복 출력이 사라져도 같은 native ID는 한 번 센다. 다른 위치 선택/손상 픽셀/다른 캠/PTS 오류는 별도 항목으로 실패시킨다. 60→30에서 의도적으로 생략한 native ID는 MP4로 검사할 수 없어 coverage에 명시한다. 센서/USB와 encode 중 어느 곳에서 없어졌는지 이 수만으로 단정하지 않는다. 실장치에는 독립 번호 패턴이 없으므로 `captureLoss=null`, oracle=UNAVAILABLE이다.
4. live encode/preview 풀은 캠별 기존 독립 객체이며 NVENC 포함 60fps=15/30fps=8 표면, packet 3초 개수·byte cap을 유지한다. WAV writer의 4초 큐 앞에는 **최대 1초 native 변환 staging**을 별도로 준비하고 보고한다(기존 ASIO probe의 4초 raw 큐를 중복 적용하지 않음). 큐 timeline은 약 1초마다 최근 128개, high-water는 전체 측정이다.
5. 원본 변환 worker/합성 오디오 producer=HIGHEST, capture/preview=ABOVE_NORMAL, encode/write= NORMAL, 사후 픽셀 검사=BELOW_NORMAL. ASIO driver 소유 callback 우선순위는 보존하고 프로세스 realtime class를 사용하지 않는다. OS가 우선순위 적용을 거부하면 오류 코드를 남긴다. 파형/후처리 제품 worker는 이번 진단 exe에서 만들지 않는다.
6. `--stall-ms`는 시작 후 `min(5, seconds/2)`초에 cam1 MP4·cam2 MP4·WAV owner 각각의 다음 실제 append I/O를 한 번 지연한다. startup header 쓰기는 지연하지 않는다. encode 표면/packet과 WAV 큐의 회복은 독립이다. 긴 스톨이나 지속 경합으로 큐가 넘치면 캠 부분 실패 또는 원본 오디오 전체 중단 정책을 따른다.
7. 부하 스파이크의 합성 CFR는 명목 PTS+공통 QPC 원점, 실장치 CFR는 기존 MF PTS adapter다. ASIO ClockMapper는 관측용이며 보정된 ASIO master A/V 정렬을 새로 통합하지 않았다. 기존 ReferenceMixWriter의 MP4 AAC는 **합성 참조 오디오**, 8ch 원본의 실제 다운믹스가 아니다. raw WAV·clock·synthetic reference를 JSON에 구분한다.

미확인: 실제 두 카메라 또는 GC311G2+synthetic 60초, 두 프리뷰 창 영역의 지연/최소화·복원/오류 표시 실측, MP4 writer 2초 스톨에서의 두 NVENC+프리뷰 동시 회복, CPU 경합 N별 전체 부하, FlexASIO/스튜디오 8개 실입력, 실제 장치별 native matrix/USB/노출/색/원본 무누락, 광학 P0, 물리 A/V 보정, 1시간/3시간 출시 gate. **이 항목들은 미확인 → 스파이크 3**이다. 실장치가 8입력을 제공하지 않는 개발 PC는 아래처럼 `--synthetic-audio`를 명시한다. 8입력 인터페이스 측정 시 그 옵션을 빼고 `--asio-device <열거 index>`를 지정한다.

Claude 실행 명령 3줄(1=synthetic 60초, 2=실장치 cam1+synthetic cam2 60초, 3=2초 writer 스톨). 두 번째 줄의 devices 파일은 이전에 실제 선택한 `r01/devices.json`이다. 결과 미디어는 각 report 옆 `dual-<UUID>`에 보존되므로 반복 실행해도 기존 take를 덮어쓰지 않는다. 같은 SDK DLL 7개가 있는 exe 폴더를 함께 사용한다.

```powershell
& "$env:TEMP/gocue-rec-r05-20260909/recorder/RecorderDualProbe_artefacts/Release/RecorderDualProbe.exe" --synthetic --synthetic-audio --project-fps 60 --seconds 60 --report C:/Users/claude/tools/claude_harness/recorder_validation/r05/synthetic60.json
& "$env:TEMP/gocue-rec-r05-20260909/recorder/RecorderDualProbe_artefacts/Release/RecorderDualProbe.exe" --devices C:/Users/claude/tools/claude_harness/recorder_validation/r01/devices.json --cam2 synthetic --synthetic-audio --project-fps 60 --seconds 60 --report C:/Users/claude/tools/claude_harness/recorder_validation/r05/mixed60.json
& "$env:TEMP/gocue-rec-r05-20260909/recorder/RecorderDualProbe_artefacts/Release/RecorderDualProbe.exe" --synthetic --synthetic-audio --project-fps 60 --seconds 60 --stall-ms 2000 --report C:/Users/claude/tools/claude_harness/recorder_validation/r05/stall60.json
```

프로젝트 30 검사는 `--project-fps 30`, CPU 경합 비교는 별도 report에 `--cpu-contention N`을 추가한다. 실측 결과를 본 뒤 라운드 25/29에서 동일 조건과 실제 StreamCam+C920으로 다시 판정한다.
