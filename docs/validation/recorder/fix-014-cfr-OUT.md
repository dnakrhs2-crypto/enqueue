fix-014-cfr 작업 결과 — 2026-09-10

작업 트리 `C:\Users\claude\gocue-rec-asio`, 브랜치 `fix-014-cfr`, HEAD `820a97e`에서 P2 두 건을 수정했다. Recorder / RecorderTests / RecorderProbe 최종 빌드와 전체 **640 passed / 0 failed**, 미디어 복사본 alignment 프로브를 완료했다. 실행기 OUT 경로가 메시지·환경변수에 없어 안내한 이 경로에 기록한다.

수정 위치와 동작은 다음과 같다. 경로는 작업 트리 기준이다.

| 파일:행 | 수정 내용 |
|---|---|
| `recorder/src/record/VideoCfrScheduler.cpp:54`, `:88`, `:108`, `:137` | 테이크 전체 진단 최대값과 실제 대기 예산을 분리했다. 실제 예산에는 프레임 주기 기반 상한, 관측 만료, 명시적 초기화를 적용했다. 도착 시각 역행도 관측을 초기화한다. |
| `recorder/src/record/VideoCfrScheduler.h:55`, `:67`, `:76` | 초기화 API 및 고정 크기 관측 버킷을 추가했다. 출력 격자·후보·진단 카운터는 초기화 대상에서 제외했다. |
| `recorder/src/record/TakeController.cpp:293` | 유효 프레임의 source revision 재앵커 처리에서 `scheduler.resetDeliveryDelay()` 한 줄만 추가했다. 마무리 실패 정리 함수는 수정하지 않았다. |
| `recorder/src/playback/VideoPlaybackEngine.h:103` | `skip / picture / clear` 결정을 명시했다. epoch 무효화 또는 미제출 gap clear는 `clearPending`으로 보존하고, 성공적인 빈 화면/대체 화면 제출로 해소한다. |
| `recorder/src/playback/VideoPlaybackEngine.cpp:1219`, `:1226`, `:1261` | presenter가 공통 결정의 `shouldSubmit()`을 사용한다. epoch 무효화는 `gap == false`여도 ClearRenderTargetView와 Present로 이어지며, S_OK일 때만 제출 상태를 갱신한다. |
| `recorder/tests/CfrSchedulerTests.cpp:36`, `:80`, `:113`, `:122`, `:135` | 기존 정렬 회귀를 유지하고 스파이크·만료·재앵커·정상 지연 테스트 4건을 추가했다. |
| `recorder/tests/CutSeamChecks.h:106`, `:290`, `:319` | 컷/스크럽 측정도 공통 제출·건너뛰기 결정을 사용한다. 기존 epoch 검사를 실제 결정 순서까지 확장하고, 준비된 대체 프레임의 즉시 제출 테스트 1건을 추가했다. |
| `recorder/tests/PlaybackTests.cpp:589` | 실제 엔진 handoff와 지연 decoder를 통해 유효 클립 buffering → 빈 화면 제출 1회 → 새 PTS 제출을 검사하는 테스트 1건을 추가했다. |

대기 상한은 **기본 한 프레임 대기를 포함한 총량**이다. 단위는 100ns이며, 네이티브 주기를 `P = 10,000,000 × denominator / numerator`, `T = ceil(P)`라 두면 다음과 같다.

```text
총 대기 상한 W = max(ceil(1.5 × P), min(2 × T, 500,000))
추가 지연 한도 = W - T
실제 대기 = T + 최근 관측 지연의 최대값(추가 지연 한도 이하)
```

| 네이티브 입력 | 총 대기 상한 |
|---|---:|
| 60/1 fps | 33.3334ms |
| 60000/1001 fps | 33.3668ms |
| 30/1 fps | 50ms |
| 30000/1001 fps | 50.05ms |
| 24/1 fps | 62.5ms |

가장 가까운 미래 프레임은 격자보다 최대 반 프레임 뒤에 촬영될 수 있다. 정상 전달 지연을 한 프레임까지 허용하려면 최대 1.5프레임의 시간이 필요하다. 따라서 30fps 이상에서는 `min(2프레임, 50ms)`를 적용하고, 더 느린 입력에서는 그 정상 전달을 자르지 않도록 1.5프레임을 상한의 하한으로 둔다. 50ms를 일률 적용한 중간 구현은 확장한 정상 지연 테스트에서 실패했고, 최종 정책은 통과했다. 증거는 `build/fix-014-cfr-evidence/low-rate-before-tests.log`, `low-rate-after-tests.log`이다.

관측은 100ms 버킷 10개에 저장한다. 관측 시각을 버킷 시작으로 내리므로 각 피크는 관측 후 **0.9~1초 안에 만료**한다. `select()`에서도 나이를 검사하므로 입력이 끊겨도 만료하며, 정상 관측이 과거 스파이크의 수명을 갱신하지 않는다. 고정 메모리이며 시간 경과에 따라 큐가 늘지 않는다. 재앵커 직후 기존 예산은 0으로 돌아가고 새 epoch의 지연만 학습한다. `maximumDeliveryDelay100ns`는 진단을 위해 원래 값, 예를 들어 200ms를 계속 보존한다.

스파이크 200ms → 정상 전달 10ms → 입력 단절 테스트에서, 스파이크 유효 기간의 총 대기는 30fps 50ms / 60fps 33.3334ms에 끝났다. 만료 후 단절 대기는 각각 43.3334ms / 26.6667ms에 끝났고 진단 최대값은 200ms로 남았다. 대기 만료 이후에는 기존 선택·반복 및 알려진 captureLoss/encodeLoss 분류를 사용한다. 출력 PTS, N0, 캡처 시각, 최종 부분 실패 처리 경로는 유지했다.

추가 테스트는 총 **6건**이다. 정상 지연 검사는 24, 29.97, 30, 59.94, 60fps에서 지연 0 / 반 프레임 / 한 프레임의 15개 조합을 검사한다. 아직 도착하지 않은 후보까지 포함한 최근접 프레임 비교로 조기 선택을 검출한다. 기존 59.94→60 3시간 격자 검사도 통과했다. epoch 검사는 미제출·generation 변경 뒤 clear 재시도, 성공 후 추가 clear 건너뛰기, 대체 프레임 도착을 검사했다. 관측된 빈 화면 제출은 정확히 **1회**였다. 기존 3개 컷 시나리오는 검은 화면·정확한 프레임 누락·잘못된 PTS·PCM 오류·audio underrun 모두 0이었다. 스크럽 42회도 검은 화면 0회였고, 준비 중 이전 그림 유지가 38회 관측됐다.

빌드는 기존 local 구성을 사용했으며 첫 빌드에서 CMake 자동 재구성이 성공했다. 최초 `-m` 실행은 기존 빌드 산출물 `Recorder_rc_lib.lastbuildstate` 접근 오류 MSB3491로 중단됐다. 지정한 직렬 옵션으로 해결했고, 최종 명령은 다음과 같다. PATH에 cmake가 없어 로컬 실행 파일의 절대 경로를 사용했다.

```powershell
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --build --preset local-release --target Recorder --target RecorderTests --target RecorderProbe -- '-m:1' '-nr:false' '-v:m' '-nologo'
$env:RECORDER_CUT_SEAM_REPORT_DIR = 'C:\Users\claude\gocue-rec-asio\build\fix-014-cfr-evidence\seam'
& build\vs2022\recorder\RecorderTests_artefacts\Release\RecorderTests.exe
& build\vs2022\recorder\RecorderProbe_artefacts\Release\RecorderProbe.exe alignment --project build\fix-014-cfr-evidence\project-copy\project.recorder --report build\fix-014-cfr-evidence\alignment.json
```

최종 세 대상 빌드, 전체 테스트, alignment 프로브의 프로세스 종료 코드는 모두 **0**이다. 최종 테스트는 **640 passed / 0 failed**, CFR 13건 및 playback-engine 45건이 포함된다. 요청 기준 634건보다 6건 늘었다. suite 수는 실제 집계값을 기록한다: `--list` 등록 실행 단위는 **42개**, 묶음 suite와 중복 실행을 포함한 결과줄은 **49개**다. 요청에 적힌 48개와 차이가 있으며 TestMain은 수정하지 않았다. 작업 시작 때 남아 있던 이전 EXE는 48개 결과줄·563건이어서 현 소스의 기준선으로 간주하지 않았다. 최종 `git diff --check`도 종료 코드 0이다. 전체 컴파일 과정의 기존 C4324 경고는 남아 있다.

미디어는 아래 원본에서 작업 트리의 `build/fix-014-cfr-evidence/project-copy`로 복사하고, **복사본의 `project.writer.lock`만 삭제**했다.

```text
C:\Users\claude\AppData\Local\Temp\claude\C--Users-claude--local-bin\08ab1c51-2e72-4134-a6ce-9b201246a06f\scratchpad\rec_selftest\demo-f0ad256141554a16bf14d905d8a1d710
```

실행 후 원본 **12개 파일**과 복사본 **11개 파일**의 SHA-256을 다시 검사해 변경 0건을 확인했다. 원본 잠금 파일은 그대로 있고 복사본 잠금 파일은 없다.

| 최신 alignment 프로브 검사 | 결과 |
|---|---|
| 영상 seek | 유효 16지점 모두 프레임 오차 0, 실제 decoded PTS 및 compiled source frame 일치 |
| 썸네일 | 16지점 모두 영상과 차이 0, 요청 sample을 포함 |
| WAV PCM | 11지점 모두 원시 PCM24와 일치, mapping 오차 0 |
| 재생 중 좌표 | 36지점 모두 video request = audible sample, 영상이 audible sample을 포함 |
| 미디어 범위 | H.264 601프레임, video index 480,800 samples, 논리 길이·WAV 480,480 samples 유지 |

측정된 위상 1.5208ms와 전달 지연 18.672ms를 재현한 **합성 CFR 입력**에서는 전달 보정 없는 평균 source−grid가 **−15.1459ms**, 최종 보정에서는 **+1.5208ms**였다. 최근접 프레임 오선택은 **59/60 → 0/60**으로 유지했다. 이 값은 실제 측정 조건을 재현한 scheduler 결과이며, 새 하드웨어 캡처의 평균 오차 측정값은 아니다. 복사본 alignment 프로브는 이미 기록된 MP4의 재생 정렬을 별도로 검증한다.

검증 원자료는 모두 `build/fix-014-cfr-evidence/`에 있다: `final-build.log`, `final-tests.log`, `alignment.log`, `alignment.json`, `verification-summary.json`, `seam-summary.json`, `seam/*.csv`, `original-copy-sha256.json`, `binary-sha256.json`. `verify-results.ps1`은 전체 결과 수, 프로브 각 좌표/PCM, 원본·복사본 해시를 검사하며 최종 실행도 종료 코드 0이었다.

남은 위험은 다음과 같다.

- 미관측 지연 급증, 상한을 넘는 지연, 실제 프레임 손실, 시작 preroll 및 stop drain에는 도착 전 프레임의 최근접 선택을 보장하지 않는다. 의도적으로 유한 대기 후 기존 정책을 따른다.
- 100ms 버킷 때문에 만료 시점은 정확히 1초가 아니라 0.9~1초다. 정상 전달이 지속되면 새 관측이 예산을 유지한다.
- epoch clear는 실제 presenter와 공유하는 제출 결정 및 엔진 fence를 headless 테스트로 검증했다. 물리 모니터의 픽셀 소거, DXGI occlusion/busy, 캡처보드·FlexASIO를 이용한 실시간 A/V는 측정하지 않았다.
- 기존 MP4에 이미 기록된 CFR source 선택은 소급 변경되지 않는다. 이번 프로브의 오차 0은 재생/좌표 검증이며 물리 노출·마이크·DAC 지연의 교정을 의미하지 않는다.

커밋·push·release.py 실행, ProductIdentity·버전·릴리스 노트·site 변경은 하지 않았다. 다른 세션의 마무리 실패 정리, `tests/TestMain.cpp`, `tests/ShortcutExceptionTests.cpp`도 수정하지 않았다. 제품 소스 변경은 위 8개 파일에 한정된다.
