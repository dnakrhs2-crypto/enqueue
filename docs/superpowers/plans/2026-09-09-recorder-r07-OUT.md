# 라운드 07 / 스파이크 4B OUT — 2026-09-09

RecoveryScanner·RecorderCrashHarness를 구현하고 직접 빌드·테스트했다. **최종 Recorder CTest 8/8, 신규 복구 23개·대용량/오류 9개, 최종 강제종료 하네스 5/5 PASS**다. 100회는 실행하지 않았다. **정전은 미확인 → 스파이크 4**다.

실제 편집·빌드 대상은 이 세션의 쓰기 허용 트리 `C:\Users\claude\gocue-rec-storage`, 브랜치 `recorder-storage`, 시작 HEAD `796e4e7`이다. 요청에 기재된 `gocue-rec`로 이동하거나 다른 트리를 수정하지 않았다. 라운드 06·08 병합 이력을 확인했다. 기존 앱·공유 JUCE·기존 `build/`·루트 CMake·`CMakeUserPresets.json`은 변경하지 않았다. `recorder/CMakeLists.txt`는 기존 내용이 그대로 접두사로 남는 append-only 검증도 통과했다.

## 만든 파일과 연결

| 신규 파일 | 역할 |
|---|---|
| `recorder/src/storage/RecoveryScanner.h/.cpp` | 프로젝트/기존 take writer lock, checksum·schema 검증 checkpoint 선택, 편집 commit 재생, manifest 대조, WAV·MP4 복구, 가용 범위/gap, orphan 보고, 복구 commit 및 문서 adopt |
| `recorder/src/storage/RecoverySupport.h/.cpp` | Win32 writer lock, 새 파일 durable 쓰기, SHA-256, 경로 검사, CRC/sequence/UUID/commit을 가진 스트리밍 메타데이터 저널 |
| `recorder/src/storage/Mp4RecoveryIndex.h/.cpp` | 초기 serialized codec 정보, 실제 tfhd/tfdt/trun·mdat 패킷 위치/CRC, libavformat stream-copy, 전체 decode·프레임 수 검증, 초기 헤더가 남은 이전 fMP4의 인덱스 재구성 |
| `recorder/tools/CrashHarness.cpp` | `RecorderCrashHarness` 부모/전용 `--child`, named event handshake, 해당 자식 핸들만 `TerminateProcess`, 복구 전후 해시·범위·재실행 검사 |
| `recorder/tools/CrashFixtures.h/.cpp` | 테스트/하네스 전용 1080p30 CPU OpenH264+AAC, 좌표별 PCM24 oracle, 완료 take/저장 편집 fixture, 가상 I/O 어댑터 |
| `recorder/tests/RecoveryTests.cpp` | 복구 23개 테스트. 진입점 `runRecoveryTests()` 하나 |
| `recorder/tests/LargeFileTests.cpp` | 대용량/오류 전파 9개 테스트. 진입점 `runLargeFileTests()` 하나 |

수정한 연결 파일: `storage/{DurableFile,RecordingJournal}.h/.cpp`, `record/{WavTrackWriter,Mp4TakeWriter}.h/.cpp`, `model/RecorderModel.cpp`, `tests/TestMain.cpp`, `recorder/CMakeLists.txt`, 계획서·설계서의 라운드 07 기록. 테스트 main은 기존 `tests/TestMain.cpp` 하나이며 main 이름 변경이나 추가 테스트 main은 없다. CTest `RecorderRecovery`는 `RecorderTests --suite recovery-idempotence`, `RecorderLargeFile`은 `RecorderTests --suite large-files`를 실행한다.

## 저장·복구 계약

- 프로젝트 lock과 라운드 06의 `takes.writer.lock`을 함께 잡은 상태에서 복구/commit/문서 adopt를 수행한다. checkpoint primary/.bak의 유효 revision을 비교한다. 같은 revision의 primary/.bak은 primary 우선이다. 이는 라운드 08의 finalization/registry 변경이 편집 revision 증가 없이 저장되는 계약을 따른다.
- 편집 레코드는 `EditDelta`와 결과 프로젝트 snapshot을 함께 검증한다. CRC/commit/sequence 또는 entity delta/revision/hash 검증 실패 뒤의 다른 segment도 적용하지 않는다. 중복 transaction을 무시한다. 손상된 원본 저널에 append/절단/수정하지 않는다.
- 복구 checkpoint와 결과는 `recovery/<attempt-uuid>/`에 새로 쓴다. 출력 미디어를 flush한 뒤 새 checkpoint의 SHA-256·출력 파일 hash/길이·복구 take ID를 `commit.log`에 flush한다. 다음 실행은 검증된 commit만 선택한다. commit 전 중단은 새 attempt에서 다시 복구하고, commit 후 문서 연결 전 중단은 이미 완료된 세대를 재사용한다.
- 완료 take는 manifest checksum/schema·등록 경로·파일 길이를 대조한다. 완료 manifest만 남은 take의 등록은 기존 정상 파일을 사용한다. 편집 저널/registry에 이미 있는 take는 클립을 다시 만들지 않으며, 삭제·undo한 배치를 자동으로 되살리지 않는다.
- WAV는 저널의 fmt/data offset, block alignment, durable sample 수, `min(실제 길이, durable bytes)`를 비교한다. RIFF/data 길이 필드는 복구본에서만 다시 쓴다. PCM 완전 샘플까지만 복사하며 정상 마이크/캠을 짧은 트랙에 맞춰 자르지 않는다. 검증은 모든 복구 PCM 샘플을 원래 좌표의 oracle과 비교한다.
- MP4는 미디어 flush 뒤 완료 fragment의 moof와 패킷 CRC를 인덱스에 commit한다. 재실행 시 보존된 moof의 trun과 실제 mdat byte 범위를 대조하고, 손상 fragment 및 그 뒤는 제외한다. 새 일반 MP4로 stream-copy한 뒤 모든 stream을 끝까지 decode하고, 영상 decode frame 수와 검증된 packet 수가 같아야 연결한다. 원본 hybrid 헤더가 최종 moov 작성 중 변경된 경우도 최종 하네스에서 통과했다.
- 처음 구현한 인덱스가 인코더의 Annex B extradata를 저장하여 AVCC payload remux decode에 실패했다. 원본 MP4의 독립 FFmpeg decode는 종료 코드 0이었다. 인덱스가 실제 초기 MP4 헤더의 codec 정보를 저장하도록 수정한 뒤 복구 테스트와 최종 하네스가 통과했다.
- 복구 메시지와 JSON에 take별 논리 범위, 자산별 available/gap, 마지막 저장 편집 revision을 기록한다. 5.0초 캠1·WAV와 3.8초 캠2 fixture에서 **`캠2 마지막 1.2초 없음`**을 확인했다. TakeStopped가 없는 경우 최종 수집 종료점은 알려지지 않았으므로 `captureStopKnown=false`다. 하네스의 tail 손실은 별도 자식 공급 sample oracle과 비교한다.
- 등록되지 않은 media 파일과 commit되지 않은 recovery attempt를 보고하며 자동 삭제하지 않는다. 출력 disk-full은 복구 transaction 실패로 전파한다. 이를 짧은 캠으로 처리해 commit하지 않는다.

## 빌드·테스트 명령과 종료 코드

증거/빌드 루트는 `C:\Users\claude\AppData\Local\Temp\gocue-rec-storage-r07\`다. 지정 영구 증거 폴더 `C:\Users\claude\tools\claude_harness\recorder_validation\`는 현재 권한 프로필의 쓰기 허용 범위 밖이므로 이번 세션에서 작성하지 않았다. 미디어·빌드 결과는 저장소 안에 만들지 않았다.

아래 명령은 `C:\Users\claude\gocue-rec-storage`에서 실행했다. MSBuild의 `Path`/`PATH` 중복을 피하도록 해당 자식 프로세스 환경 이름만 정규화한다.

```powershell
$recR07 = Join-Path $env:TEMP 'gocue-rec-storage-r07'
$recCmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$recCtest = Join-Path (Split-Path $recCmake) 'ctest.exe'
python -c "import os, subprocess, sys; sys.exit(subprocess.call(sys.argv[1:], env={k.upper(): v for k, v in os.environ.items()}))" $recCmake -S . -B "$recR07\build" -G 'Visual Studio 17 2022' -A x64 '-DFETCHCONTENT_SOURCE_DIR_JUCE=C:/Users/claude/JUCE' '-DASIO_SDK_DIR=C:/Users/claude/SDKs/ASIOSDK' '-DWINSPARKLE_DIR=C:/Users/claude/tools/winsparkle/WinSparkle-0.9.4'
python -c "import os, subprocess, sys; sys.exit(subprocess.call(sys.argv[1:], env={k.upper(): v for k, v in os.environ.items()}))" $recCmake --build "$recR07\build" --config Release --target RecorderTests RecorderCrashHarness RecorderDurabilityProbe RecorderProbe Recorder --parallel 4
& $recCtest --test-dir "$recR07\build" -C Release --output-on-failure -R '^Recorder'
& "$recR07\build\recorder\RecorderCrashHarness_artefacts\Release\RecorderCrashHarness.exe" --iterations 5 --crash-cases all --include-large-files --report "$recR07\r07\recovery.json"
```

| 검증 | 종료 코드·결과 | 로그 |
|---|---|---|
| Configure | **0** | `configure.log` |
| 최종 5개 target build | **0** | `build-final.log` |
| Recorder CTest | **0, 8/8**, 8.41초 | `ctest-final.log`, `build/Testing/Temporary/LastTest.log` |
| 복구 / 대용량 신규 테스트 | **23/23, 9/9**, 위 CTest에 포함 | `LastTest.log` |
| 최종 5회 하네스 + 가상 I/O | **0, 5/5**, 31.075초 | `harness-final.log`, `r07/recovery.json` |
| `git diff --check`, CMake 접두사 대조 | **0 / PASS** | 명령 출력 |

초기 core build는 JUCE `var[String]` overload 모호성으로 종료 코드 **1**이었으며 `Identifier/getProperty`로 수정했다. 초기 복구 테스트는 위 Annex B/AVCC 문제로 **14 PASS / 1 FAIL, 종료 코드 1**, 수정 후 **15/15**였다. 추가 경계 테스트를 넣은 최종 결과는 **23/23**이다. 최종 보고 전 초기 하네스도 별도로 **5/5** 통과했고, 코드 보완 뒤 위 최종 **5/5**를 재확인했다. 합계 10개의 자식 강제종료를 수행했으며 100회 시험은 실행하지 않았다. 초기 로그/보고서도 보존했다. 빌드에는 임시 출력 경로 MSB8029, 기존 cache-line 정렬 C4324, 새 저널 길이 비교의 C4018 경고가 남아 있으며 오류는 없다.

## 최종 5회 하네스 결과

2026-09-09 **17:30:04.890–17:30:35.965 +09:00**. 모든 회차가 실제 자식 `TerminateProcess` 종료 코드 `0xDEAD`를 확인했다. H.264 1920×1080 30fps + AAC reference + 48kHz PCM24 mono WAV 8개를 사용했다. 정상 I/O tail 목표의 분모는 자식이 공급했다고 기록한 샘플 수다.

| 단계 | 공급 길이 | 검증된 영상 | 최대 캠/WAV tail 손실 | 결과 |
|---|---:|---:|---:|---|
| `fragment-write` | 5.000초 | 120 frames / 4.000초 | **1.000초** | PASS |
| `wav-header-write` | 5.000초 | 120 frames / 4.000초 | **1.000초** | PASS |
| `chunk-replace` | 5.033초 | 150 frames / 5.000초 | **0.033초** | PASS |
| `final-moov-write` | 6.000초 | 180 frames / 6.000초 | **0초** | PASS |
| `checkpoint-replace` | 6.000초 | 180 frames / 6.000초 | **0초** | PASS |

모든 회차에서 완료 take 006와 저장된 marker/edit 보존, 원본 SHA-256 불변, WAV 전 샘플 oracle 일치, recovery commit 후 문서 연결, 두 번째 실행의 추가 take/clip **0**, 프로젝트 내용·파일 hash 변화 **0**을 확인했다. `r07/recovery.json`에 모든 원본의 before/after hash, 자산별 sample 범위, decode frame/sample 수, 단계/PID, 복구 결과와 media 폴더가 기록되어 있다.

`--include-large-files`는 가상 관찰 offset **4,294,967,312**, 실제 파일 **32 bytes**, 가짜 `ERROR_DISK_FULL (Win32 112)`를 확인했다. 실제 4GiB 파일 생성이나 디스크 채우기는 하지 않았다.

9개 named case 중 나머지 `take-before-commit`, `take-after-commit`, `recovery-before-commit`, `recovery-after-commit`의 **자식 강제종료**는 이번 5회 목록에 포함되지 않았다. 해당 journal/recovery commit 경계의 예외 주입·재실행 단위 테스트는 통과했다. 100회 `all` 실행은 9개 case를 순환한다. 60초 단위로 나누려면 `--iterations 5 --case-offset 5`처럼 다음 case offset과 새 report 경로를 주면 된다. 기존 보고서는 덮어쓰지 않는다.

## 미확인과 설계 결정

- **100회 프로세스 강제종료 gate는 미확인**, Claude가 아래 명령으로 실행한다. 실물 ASIO/카메라·NVENC 부하·디스크 stall 조건의 tail 상한·장시간 마무리 시간은 이 synthetic 결과로 인증하지 않는다.
- **정전/장치 cache 보존은 미확인 → 스파이크 4**. RF64/실제 >4GiB 파일과 내보내기는 미확인 → 스파이크 4·7/라운드 20. 가상 offset/가짜 disk-full 통과와 구별한다.
- AAC는 모든 packet을 decode했으나 복구된 AAC의 priming/padding·WAV 대비 gapless 동기 정확도는 인증하지 않았다. `audioSamples`는 실제 decoded 수이며 영상/WAV 유효 길이와 같다고 표시하지 않는다.
- 라운드 11의 GrowingTakeReader 실측 gate는 유지한다. 이번 명시 요청을 충족하기 위해 **크래시 복구 전용** packet index를 추가했다. 초기 헤더를 읽는 시점은 단일 mux owner가 초기 flush 후 정지한 상태다. 녹화 중 concurrent playback reader는 구현하지 않았다. 원본 헤더와 인덱스가 모두 사라져 복구 후보를 검증할 수 없는 캠은 진단과 gap으로 남긴다.
- 복구 세대의 commit을 원본 저널에 append하는 대신 `recovery/<attempt-uuid>/commit.log`에 둔다. 손상 tail과 원본 checkpoint도 그대로 보존하기 위한 결정이다. worker API `appendEdit`와 하네스의 `run(..., &document)` 연결을 검증했다. 제품 GUI 시작 경로와 비동기 편집 journal worker 연결은 후속 라운드에 남는다.
- 모델은 `recovery/<UUID>/...` 미디어 경로와 캠이 등록되지 못한 라운드 06 원본의 **부분 WAV take**를 수용한다. 정상 take의 캠1 계약을 이 예외로 대체하지 않는다.
- 입력/출력 클록과 normal/dub 배치를 구별하도록 TakeStarted에 호환 가능한 `placementMode`를 추가했다. 이전 저널에 이 필드가 없고 출력 클록을 썼다면 불명확함을 경고하고 `Pstart`를 보존한다.
- 30초 WAV 청크를 실제 5초 녹화 안에서 교체 시험하려고 **fault adapter가 있을 때만** `testChunkFrames=5×Fs`를 허용했다. 제품 기본값은 30초다. checkpoint crash 지점은 durable backup 교체 후 primary `MoveFileExW` 직전이다. kernel rename 내부나 정전 원자성을 시험했다는 뜻이 아니다.

Claude가 실행할 100회 명령:

```powershell
& "$env:TEMP\gocue-rec-storage-r07\build\recorder\RecorderCrashHarness_artefacts\Release\RecorderCrashHarness.exe" --iterations 100 --crash-cases all --include-large-files --report "C:\Users\claude\tools\claude_harness\recorder_validation\r07\recovery.json"
```
