fix-014-finalize 작업 결과 — 2026-09-10

작업 트리: `C:\Users\claude\gocue-rec-app`, 브랜치 `fix-014-finalize`, HEAD 및 `integrate-014b`와의 merge-base는 `76157da`이다. 시작 시 작업 트리는 깨끗했다. 실행기 지정 OUT 경로를 환경에서 찾지 못해 경로를 문의했고, 별도 응답이 없어 안내한 이 파일에 기록했다.

파일 수정·빌드·테스트만 수행했다. 커밋·push·release.py 실행은 없으며 ProductIdentity·버전·릴리스 노트·site·CMakeLists.txt는 수정하지 않았다. 다른 세션 영역인 N0·배치 계산과 playback/VideoPlaybackEngine은 수정하지 않았다.

수정 내용

| 위치(최종 파일 기준) | 변경 내용 |
| --- | --- |
| `recorder/src/record/TakeController.cpp:455` | `failedWorker()`의 독립 자원 정리 후 공통 `finalizeAssets()`를 호출한다. 자산 게시 실패도 기존 실패 메시지에 추가한다. |
| `recorder/src/record/TakeController.cpp:684` | `confirmAudioAsset()`가 WAV 청크별 PCM24 헤더·채널 수·레이트·파일 크기를 확인한다. 실제 존재하는 청크의 확인된 구간만 보존하고 부족하거나 확인하지 못한 구간은 gaps로 만든다. |
| `recorder/src/record/TakeController.cpp:717` | 정상·실패 마무리가 같은 `finalizeAssets()`를 사용한다. WAV 가용 길이는 logicalLength, writtenSamplesPerMic, mediaDurableSamplesPerMic의 최솟값으로 제한한 뒤 청크 파일과 대조한다. 카메라는 최종 파일 경로를 게시하고, mux 보고가 있으면 finalized·completedFragments·videoPackets로 확인된 기록량을 제한한다. 미완료 `.recording.mp4`는 재생 가능한 범위로 게시하지 않는다. |
| `recorder/src/record/TakeController.cpp:757` | 확정된 각 자산의 mediaGeneration을 올린 뒤 기존 문서 registry 갱신 경로로 게시한다. 테이크가 아직 문서에 없으면 `placeRecordedTake()`의 coordinator 경로를 사용한다. take 상태와 오디오 피크도 정리 결과에 맞춰 갱신한다. |
| `recorder/src/record/TakeController.cpp:952` | 정상 finalizer 회수 뒤의 개별 자산 갱신을 공통 확정 함수 호출로 교체한다. 게시 실패를 성공한 체크포인트로 저장하지 않고 실패 정리 경로로 전달한다. |
| `recorder/tests/TestSupport.h:10`, `:41` | 예상 밖 JUCE 예외를 프로세스 공통 카운터에 누적한다. `Suite::test()`가 실행 전후 카운터 차이를 확인하여 해당 테스트도 실패시키며 비표준 C++ 예외도 집계한다. 의도한 예외는 횟수를 지정하는 `ExpectedUnhandledExceptions` 스코프로만 허용한다. |
| `recorder/tests/TestMain.cpp:19`, `:105`, `:207` | 전체 테스트 동안 살아 있는 테스트 JUCEApplication이 callback 예외를 기록한다. 스코프가 허용한 횟수 부족·초과와 관찰 콜백 예외도 실패로 남긴다. GUI 초기화는 기존 테스트별 수명을 유지하며, 프로세스 종료 시 누적 예외가 있으면 종료 코드 1을 반환한다. `:182`에서 진행 출력도 즉시 flush한다. |
| `recorder/tests/ShortcutExceptionTests.cpp:201`, `:377` | 일반 `Suite::test()` 안에서 callAsync·Timer 예외를 던지는 숨김 주입 suite 2개와 이를 실행하는 서브프로세스 회귀 테스트를 추가했다. 부모는 자식의 종료 코드 1, 실패 1건·통과 1건, 예상 밖 예외 1건 및 후속 callback 실행을 모두 확인한다. 주입 suite는 기본 전체 실행 목록에서 제외한다. |
| `recorder/tests/ShortcutExceptionTests.cpp:360`, `:388` | 기존 단일 테스트 전용 애플리케이션을 제거하고 명시적인 expected 스코프로 보고 테스트를 실행한다. 보고 실패 콜백에서는 경로·호출 횟수만 수집하고 assertion은 반환 후 수행한다. 재진입은 별도 임시 하위 디렉터리와 중첩 알림 호출 횟수로 검증하며, 다음 독립 호출이 다시 보고서를 만드는지도 확인한다. |
| `recorder/tests/TakeControllerTests.cpp:127`, `:199`, `:212` | 기존 finalizer 시작 실패·checkpoint 시작 실패·메타데이터 쓰기 예외 테스트에 최종 cam1.mp4 경로/존재/generation, 실제 WAV 인덱싱, project.recorder 저장·재로딩 후 경로/가용 범위/전체 메타데이터 일치 검증을 추가했다. |

TakeController의 정확한 변경 구간 (`git diff --unified=0`, 기준 `76157da`)

| 기준 파일 구간 | 최종 파일 구간 | 작업 |
| --- | --- | --- |
| 455행 | 455–456행 | 실패 정리 후 공통 확정 호출로 교체 |
| 682행 직후 | 684–764행 | WAV 확인 함수와 공통 자산 확정 함수 삽입 |
| 731–742행 | 최종 812행 직후에서 삭제 | 정상 worker 내부의 중복 자산/경로/generation 계산 제거 |
| 882–891행 | 952–953행 | 정상 worker 회수 후 공통 확정 함수 호출로 교체 |

N0·collectionOrigin·placement 계산은 위 변경에 포함되지 않는다. 정리 함수 옆의 신규 함수 삽입 때문에 이후 행 번호만 이동했다.

추가 테스트: 총 7건

- `TakeControllerTests.cpp:215`: 모노 정상 finalization, 스테레오 finalizer 시작 실패의 2건. 8,401샘플은 기록되었지만 마지막 WAV flush 실패로 durable watermark가 8,000인 상황을 실제 I/O fault adapter로 만든다. 저장 전후 가용 범위 8,000, gap 401 및 WAV 인덱싱을 확인했다.
- `TakeControllerTests.cpp:228`: 실제 WAV 파일을 97샘플로 자른 경우 metadata와 인덱스가 그 길이를 넘지 않는다.
- `TakeControllerTests.cpp:241`: 큐/인코더가 주장한 전체 길이를 사용하지 않고 mux의 완료 여부와 기록 packet 수로 카메라 범위를 제한한다.
- `TakeControllerTests.cpp:254`: WAV 청크가 사라지면 전체 gap과 청크 없는 무음 소스로 저장·재로딩·인덱싱된다.
- `ShortcutExceptionTests.cpp:377`: callAsync 및 Timer의 의도하지 않은 예외가 각각 테스트와 프로세스를 실패시키는 서브프로세스 2건이다. kill/PID 감시 스크립트는 만들지 않았다.

빌드·검증

`cmake`가 PATH에 없어 설치된 실행 파일을 지정했다. 최종 실행 명령은 다음과 같다.

```powershell
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --preset local
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --build --preset local-release --target Recorder --target RecorderTests -- '-m' '-v:m' '-nologo'
& './build/vs2022/recorder/RecorderTests_artefacts/Release/RecorderTests.exe'
```

- CMake 구성 성공, 최종 Recorder·RecorderTests 빌드 성공: 종료 코드 0. 이번에는 디렉터리 오류가 없어 `-m:1 -nr:false` 대안은 사용하지 않았다. 기존 C4324 정렬/패딩 경고는 남아 있다. 중간 컴파일의 juce::var 대입과 fingerprint 인수 타입 오류는 수정 후 재빌드했다.
- 집중 실행: TakeControllerTests 18/18, shortcut-exceptions 15/15, 각각 종료 코드 0.
- **최종 바이너리 전체 실행 2회 모두: 49개 suite 요약행, 625건 통과, 0건 실패, 종료 코드 0.** 초기 비정상 종료 이력 때문에 소스 변경 없이 동일 바이너리로 전체 검증을 한 번 더 반복했다. 사용자 기준선 49/618에서 추가 7건이며, 러너의 기존 복합 suite 및 WAV suite 재호출을 그대로 포함한 수다. `EditPropertyTests`의 별도 PASS 출력도 전체 실행 로그에 포함된다. 마지막 결과 확인 시각은 2026-09-10 15:32 KST이며 총 작업 시간은 약 32분이다.
- 집계는 모든 `N passed, M failed` 요약행을 합산했다. 일부 기존 suite는 끝에 세미콜론이 없으므로, 세미콜론을 요구한 최초 집계의 38/449는 전체 수가 아니었다.
- `git diff --check`: 종료 코드 0. 금지된 제품/릴리스 경로의 변경은 없다.

검증 로그: `build/fix-014-finalize-build.log`, `build/fix-014-finalize-take-tests.log`, `build/fix-014-finalize-shortcut-tests.log`, **`build/fix-014-finalize-final-tests.log`**, `build/fix-014-finalize-final-tests-stderr.log`, `build/fix-014-finalize-final-tests.exit.txt`, **`build/fix-014-finalize-repeat-tests.log`**, `build/fix-014-finalize-repeat-tests-stderr.log`, `build/fix-014-finalize-repeat-tests.exit.txt`.

초기 전체 실행 이력과 남은 위험

- 초기 구현에서 전체 GUI 초기화까지 유지한 실행은 출력 진행이 멈춘 뒤 **종료 코드 -1073741819 (0xC0000005)**로 끝났다. 이 실행은 성공 결과에 포함하지 않았다. 예외 애플리케이션만 전역 수명으로 남기고 GUI 초기화는 기존 suite별 수명으로 되돌린 최종 구현으로 재빌드·전체 2회 재실행하여 위 49/625 결과를 확인했으며, 두 실행에서 비정상 종료는 재현되지 않았다. 초기 비정상 종료의 네이티브 스택 원인까지 규명한 것은 아니다.
- 초기 실행은 강제 종료하지 않았다. 실행 중이던 이전 테스트 바이너리를 같은 빌드 출력 디렉터리의 `RecorderTests-initial-finalize.exe`로 보존하여 최종 바이너리를 빌드했고, 이후 초기 실행의 종료 코드도 회수했다. 초기 로그는 `build/fix-014-finalize-all-tests.log`와 `build/fix-014-finalize-all-tests-stderr.log`에 남아 있다.
- 캡처보드·FlexASIO·실제 카메라·NVENC는 열지 않았다. TakeController의 카메라 파일/fragment 보고 검증은 명시적인 비인코딩 테스트 더블이고, WAV는 실제 합성 PCM24 파일을 썼다. 제품 Recorder.exe의 하드웨어 시작은 실행하지 않았다.
- 최종 이름으로 확정되지 않은 MP4 fragment는 기존 recovery 입력으로 남고, 이 경로에서 복구용 재인코딩/재포장을 수행하지 않는다. 그 영상을 즉시 재생 가능한 범위로 과대 게시하지 않는다.
- 실패 시 동기 정리와 WAV 청크 헤더 확인에 따른 긴 녹화/느린 저장장치의 UI 지연은 별도 벤치마크하지 않았다. 다른 세션의 alignment·seam 수정과 합친 최종 통합 빌드는 이번 검증 범위에 포함되지 않는다.
