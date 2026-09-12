Recorder 컷 편집 안정성 검증 · 2026-09-10
========================================

대상은 `C:\Users\claude\gocue-rec-app`, 브랜치 `s1-stability`, 기준 커밋 `4f2131931f1f298debe014a01ae1943df114a050` (Recorder 0.1.2)이다. 이전 세션의 미커밋 변경을 직접 읽고 호출 경로·소유권·예외 경계를 검토했다. 아래 `file:line`은 최종 작업 파일 기준이다.

**CEO가 겪은 종료의 원인은 확정하지 못했다.** 덤프와 당시 편집 순서가 없다. 정상적인 마우스 조작으로 만들 수 있는 음수 이동 미리보기는 구체적인 예외 경로가 확인되므로 가장 유력하다. 극단적인 정수 경계, 비동기 예외 주입, 문서 교체 테스트의 성공을 당시 장애의 해결 증거로 확대하지 않는다.

원인 후보의 순위와 근거는 다음과 같다.

| 순위 | 후보 | 코드에서 확인한 실패 경로 | 당시 장애와의 연결 및 한계 |
| --- | --- | --- | --- |
| 1 | 거부된 음수 드래그의 고스트가 `Clip` 시간 범위 검사를 통과하지 못함 | `TimelineView.cpp:547`의 이동 좌표 → `TimelineView.logic.cpp:175`의 편집 거부 → `TimelineView.cpp:195`의 실패 미리보기 생성 → `TimelineView.scale.cpp:19`의 인덱스 생성 → `RecorderModel.cpp:67`에서 `std::overflow_error`. 기준 코드에서는 시작 시각을 보정하지 않고 길이만 최소 1로 만들었다. | 0초에서 시작하는 클립의 5초 지점을 잡아 1초로 끌면 목표 시작은 -4초다. 문서 편집을 거부해도 마우스 콜백 안의 고스트 생성에서 예외가 발생한다. 일반 컷 편집과 직접 연결된다. 실제 CEO 입력과의 일치는 미확인이다. |
| 2 | 표시 인덱스에는 있으나 현재 문서에는 없는 클립을 클릭 | `TimelineView.cpp:506`은 표시 인덱스를 조회하고 `TimelineView.cpp:529`는 별도 문서 스냅샷에서 조회한다. 기준 코드는 두 번째 조회의 null을 검사하지 않고 즉시 시작 시각을 읽었다. 문서 교체/삭제 후 화면 갱신 전 입력이면 null 역참조 경로다. | `MainComponent.cpp:41`은 문서 통지를 갱신 예약으로 바꾸고 `MainComponent.cpp:214`에서 화면을 갱신한다. 다만 보통의 편집은 `TimelineView.cpp:247`의 `finish()`가 바로 갱신하므로 모든 컷 편집에 이 틈이 생긴다고 할 수 없다. 테스트는 외부 문서 게시와 클릭 순서를 의도적으로 만든다. |
| 3 | `planWork`에 저장된 예외가 소유자 타이머/종료 경계로 전파 | `RecorderSession.cpp:282`의 `std::async` 시작 실패, 작업 내부 catch 이전의 `PreparedPlan` 할당 실패, 표준 예외가 아닌 실패 등이 가능하다. 기준 `tick()`은 `future::get()`을 직접 호출했다. 호출자는 `MainComponent.cpp:160`의 타이머와 `MainComponent.cpp:61`의 종료 대기다. | 일반 파일 인덱싱의 `std::exception`은 기존에도 작업 내부에서 `plan->error`로 변환했다. 따라서 단순 미디어 열기 오류가 곧바로 종료를 일으킨다는 주장은 근거가 없다. 예외 주입으로 회수와 종료 진행을 검증했지만 실제 발생 원인은 확인되지 않았다. |
| 4 | 교체 전 프로젝트/미디어의 파생 결과가 현재 화면에 게시 | 기준 `RecorderSession::tick()`은 `assetId` 존재만 확인했다. 같은 ID를 가진 프로젝트 복사본 재열기나 `mediaGeneration` 교체 후 옛 결과가 도착하면 잘못된 파형/썸네일이 게시될 수 있다. `RecorderSession.cpp:581`에서 결과의 세 가지 출처를 확인하도록 보강했다. | 잘못된 결과 게시의 결함이며 직접적인 크래시 증거는 없다. 기존 작업도 `takeItem`, `asset`, `folder`를 값으로 캡처한다. `ThumbnailCache.cpp:103`은 작업 예외를 잡고, `RecorderSession.h:125`의 워커는 결과 저장소보다 먼저 파괴/조인된다. “교체된 자산을 해제 후 참조했다”는 설명은 근거가 없어 채택하지 않는다. |

현재 로컬 JUCE의 `juce_core.h:171`은 `JUCE_CATCH_UNHANDLED_EXCEPTIONS=0`이고 Recorder의 컴파일 정의에는 이를 켜는 설정이 없다. `juce_ApplicationBase.h:342`의 예외 매크로가 빈 정의가 되므로 위 예외를 받아 주는 앱 경계가 없다. 처리되지 않은 예외가 앱 종료로 이어질 수 있으나, 덤프 없이 `std::terminate`의 실제 호출이나 CEO의 Windows 종료 코드를 확정하지 않는다.

이전 세션에서 남긴 `out/stability-regression-before.log`에는 음수 move/trim 및 큰 move 미리보기 3건의 시간 범위 예외가 기록되어 있다. `out/stability-before-confirmed/20260910-123624-d8281ecd/process.json`에는 `negative-move`, phase 2 직전과 `0xC0000409`가, `out/stability-after/20260910-123835-35e3805d/report.json`에는 312회 PASS가 기록되어 있다. **이는 이전 세션의 참고 자료다.** 실행 바이너리와 현재 소스의 일치가 검증되지 않았고 해당 실행은 이번에 지정된 `demo-f0ad...` 프로젝트와도 다르다. 이번 세션의 앱 스트레스 통과나 CEO 장애 재현 결과로 계산하지 않았다.

수정별로 유지한 근거와 검증 범위는 다음과 같다.

| 수정 위치 | 방지하는 결함/수정 이유 | 검증 |
| --- | --- | --- |
| `recorder/src/ui/TimelineView.cpp:10`, `:17`, `:195`, `:547` | 실패 미리보기와 드래그 좌표 계산의 signed 덧셈/뺄셈을 포화 연산으로 바꾼다. 극단값에서 범위 검사 전에 발생하던 오버플로를 피한다. 일반적인 10초 영상에서 정수 상한에 도달한다는 가설은 채택하지 않는다. | `CutEditStabilityTests.cpp:33`, `:49`: `Sample` 상·하한의 move/trim, 고스트 존재, 실제 소프트웨어 paint, 거부된 commit과 문서/undo 보존. |
| `recorder/src/ui/TimelineView.cpp:169` | 표시용 고스트의 시작을 `[0, max-1]`, 길이를 `[1, max-start]`에 넣어 `Clip::timelineEnd()` 계약을 지킨다. 실제 편집 결과와 commit 거부 상태는 그대로 보존된다. 음수 구간 전체가 화면 밖이어도 경계에 최소 길이의 거부 표시를 남긴다. | `CutEditStabilityTests.cpp:15`, `:82`: 음수 move/trim의 고스트 생성·paint와 실제 `Rows` 마우스 down/drag/up 회귀. |
| `recorder/src/ui/TimelineView.cpp:321` | `llround()`에 범위 밖/무한대 값을 넘기지 않는다. 이전 미커밋 구현의 `max` 반환은 hit/paint의 `+1`에서 다시 넘칠 수 있어 `max-1`로 수정했다. NaN/음수는 0이다. 앱 전체의 모든 거대 시간값 연산을 보장하는 변경은 아니다. | `CutEditStabilityTests.cpp:70`: 큰 double, ±infinity, NaN, 한 칸 뒤 조회, 정상 좌표 왕복. |
| `recorder/src/ui/TimelineView.cpp:529` | 표시 hit의 ID를 현재 스냅샷에서 찾지 못하거나 비활성이면 드래그를 취소한다. null 역참조를 방지한다. | `CutEditStabilityTests.cpp:94`: 외부 삭제 후 화면을 갱신하지 않은 상태의 클릭. |
| `recorder/src/app/RecorderSession.cpp:334`, `:342`, `:473`, `:541` | `std::async` 시작 및 `future::get()`의 표준/비표준 예외를 오류 표시로 회수한다. 실패한 future를 비우고 null 결과를 검사하므로 일반 tick과 종료 대기가 계속 진행된다. | `CutEditStabilityTests.cpp:153`: `std::runtime_error` future와 비표준 예외 future 주입, busy 해제, 종료 commit 장벽 도달. 스레드 생성 실패 자체의 실물 재현은 하지 않았다. |
| `recorder/src/app/RecorderSession.cpp:337`, `:544`, `:554` | 모든 준비 실패에서 재생 자원을 해제하고 `wantPlay`, `pendingLatest`, 준비 안내를 정리한다. 기존의 정상적인 `plan->error` 경로도 `pendingLatest`가 남으면 `:572`에서 자동 재시도했으므로 함께 수정했다. | `CutEditStabilityTests.cpp:164`: 오류가 있는 최신 테이크 계획이 재시도 의도와 준비 안내를 남기지 않음. |
| `recorder/src/app/RecorderSession.cpp:409`, `:421`, `:426`, `:438`, `:448`, `:581`; `RecorderSession.h:115` | 프로젝트 교체 때 파생 요청 세대를 증가시키고 큐/완료 결과를 무효화한다. 결과에 요청 세대·프로젝트 ID·미디어 세대를 보존하여 게시 때 모두 검사한다. Peak 요청 키에도 자산/세대를 넣어 같은 take ID의 미디어 교체 후 새 작업이 가능하게 한다. | `CutEditStabilityTests.cpp:132`: 같은 ID 재열기, 미디어 세대 변경, 다른 프로젝트 결과를 거부하고 현재 결과만 게시. 실시간 경쟁 스케줄 전체를 재현한 테스트는 아니다. |
| `recorder/src/app/RecorderSession.h:72`; `recorder/src/ui/TimelineView.h:41`; `recorder/src/ui/MainComponent.h:31`; `recorder/tests/StabilityTestAccess.h:11` | 비공개 상태의 예외/완료 결과 주입 및 실제 UI 콜백 호출용 friend. `PreparedPlan` 정의를 헤더로 옮긴 것은 테스트의 promise 값 형식을 완성하기 위해서다. 별도의 런타임 결함 수정으로 계산하지 않는다. 메뉴는 버튼의 `onClick`을 호출하여 본선의 `showEditMenu(bool atMouse)` 서명과 결합하지 않는다. | 전체 컴파일 및 공유 회귀 테스트. `showEditMenu` 선언·정의는 수정하지 않았다. |
| `recorder/tests/CutEditStabilityTests.cpp:106`, `:118` | 기존의 드래그 스냅샷/선택 검사와 게시 재진입 차단을 검증한다. 새로 고친 결함으로 주장하지 않는다. | 중간 편집/선택 변경 시 stale commit 거부, notify 안의 undo/편집/문서 교체 거부. |
| `recorder/CMakeLists.txt:430`; `recorder/tests/TestMain.cpp:77` | 새 suite를 기존 단일 실행기 및 CTest에 등록한다. | `RecorderTests.exe` 전체 실행 및 `RecorderCutEditStability` 등록. |
| `recorder/src/ui/TimelineView.scenario.cpp:59`, `:69`; `recorder/tools/CutEditStress.cpp:21`, `:160`, `:211` | `--automation`에 `cut-edit-stress`를 등록한다. 타이머가 실 UI 편집 콜백을 실행하고 report를 쓴다. 잘못된 반복 수/편집할 수 없는 원본은 시작 오류로 처리하여 원본 복원의 무한 재귀를 막는다. 로그는 실행마다 비우고 쓰기 실패를 검사한다. report 쓰기 실패나 재생/paint 미관측을 PASS로 처리하지 않는다. | 앱 빌드 통과. 이번 세션의 앱 실행은 사용자 담당으로 미실행이다. |

빌드와 테스트는 다음 명령으로 수행했다. 이 셸은 CMake가 PATH에 없어 설치 절대 경로를 사용했다. 처음 요청된 `-m` 빌드는 `.tlog` 쓰기 권한 거부로 실패했고 새 출력 디렉터리에서도 같은 증상이 있었다. 기존 `build/vs2022`에서 `-m:1 -nr:false`로 실행하여 성공했다. 증상과 해결 명령을 기록하며 MSBuild 권한 오류의 내부 원인까지 확정하지 않는다. PowerShell에서는 `-v:m`을 따옴표로 감싸야 옵션 분리를 피할 수 있었다.

```powershell
Set-Location 'C:\Users\claude\gocue-rec-app'
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --preset local
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --build --preset local-release --target Recorder --target RecorderTests -- '-m:1' '-nr:false' '-v:m' '-nologo'
& '.\build\vs2022\recorder\RecorderTests_artefacts\Release\RecorderTests.exe'
```

최종 바이너리의 전체 실행 결과는 **47개 suite 요약행, 520건 통과, 0건 실패, 실행기 종료 코드 0**이다. 기준 46개/508건에 `cut-edit-stability` 12건을 추가했다. `out/stability-20260910-tests-final.log`의 마지막 줄은 `RecorderTests: all suites passed`다. 빌드 로그는 `out/stability-20260910-delivery-build.log`에 있다. CTest 등록 검사도 `RecorderCutEditStability` 1/1 통과했고 로그는 `out/stability-20260910-ctest.log`다. 카메라/ASIO 장치를 여는 앱 실행은 수행하지 않았다.

스트레스는 `Recorder.exe --automation <json>`만 제공한다. 이전 미커밋 파일 `tools/recorder/cut_edit_stress.ps1`, `tools/recorder/run_stability_process.py`는 삭제했다. 외부 프로세스 실행·감시·강제 종료·PID/종료 코드 관리 스크립트는 추가하지 않았다.

지정 원본 `C:\Users\claude\AppData\Local\Temp\claude\C--Users-claude--local-bin\08ab1c51-2e72-4134-a6ce-9b201246a06f\scratchpad\rec_selftest\demo-f0ad256141554a16bf14d905d8a1d710`를 다음 두 디렉터리에 각각 복사했다. 복사본의 `project.writer.lock`을 삭제했으며 나머지 11개 파일은 원본과 SHA-256이 같다. 복사 전후 원본 12개 파일의 SHA-256 일치를 확인했다. 증거는 `out/stability-2026-09-10/source-manifest.json`이다. 기존 원본의 lock과 내용은 보존했다.

| 준비된 실행 | JSON | 설정 |
| --- | --- | --- |
| 혼합 편집 | `out/stability-2026-09-10/mixed/config.json` | seed 910, 312회, 26종 동작을 섞은 묶음 12회 |
| 음수 이동 집중 | `out/stability-2026-09-10/negative-drag/config.json` | seed 910, 26회, `reproduceNegativeDrag=true` |

혼합 편집 JSON 예시는 다음과 같다. `project`는 폴더가 아닌 복사본의 checkpoint 파일이며, `project`와 `report`는 절대 경로다.

```json
{
  "schemaVersion": 1,
  "scenario": "cut-edit-stress",
  "project": "C:/Users/claude/gocue-rec-app/out/stability-2026-09-10/mixed/project/project.recorder",
  "report": "C:/Users/claude/gocue-rec-app/out/stability-2026-09-10/mixed/report.json",
  "seed": 910,
  "iterations": 312,
  "reproduceNegativeDrag": false
}
```

사용자가 실행할 명령은 다음과 같다. 실행과 종료 코드 확인은 사용자가 담당한다.

```powershell
& 'C:\Users\claude\gocue-rec-app\build\vs2022\recorder\Recorder_artefacts\Release\Recorder.exe' --automation 'C:\Users\claude\gocue-rec-app\out\stability-2026-09-10\mixed\config.json'
& 'C:\Users\claude\gocue-rec-app\build\vs2022\recorder\Recorder_artefacts\Release\Recorder.exe' --automation 'C:\Users\claude\gocue-rec-app\out\stability-2026-09-10\negative-drag\config.json'
```

시나리오는 `MainComponent`와 실제 미디어 재생을 사용한다. 100ms 타이머의 네 단계에서 선택/재생 준비, 입력 시작, 드래그, 입력 확정/검증을 수행하므로 312회는 최소 약 125초다. `StabilityTestAccess::openAudio`와 `StabilityAudioClock`은 480-frame 가상 출력 콜백을 제공하며 물리 출력으로 소리를 내지 않는다. `Main.cpp:59`의 자동화 분기가 일반 장치 연결 초기화 전에 진입하고, 시나리오는 카메라/ASIO 연결 함수를 호출하지 않는다.

동작은 음수 이동, scrub, 선택/복수 선택, split 키/버튼, trim in/out, move/Alt/snap-off, 삭제, ripple, earlier/later, marker, undo/redo, 편집 버튼/컨텍스트 메뉴, unlink, zoom/scroll, 최근 테이크 재생, 탭 전환, 드래그 취소다. 편집 때문에 충분히 긴 활성 클립이 없어지면 최초 스냅샷을 복원하여 계속한다. 26개 동작의 순서는 seed로 재현되지만 비동기 재생과 UI 타이머의 실제 타이밍은 동일하게 보장되지 않는다.

각 report 옆에 `operations.jsonl`과 `final.png`를 쓴다. report의 `coverage`는 시도한 동작 수이며, `effectiveEdits`는 문서 revision이 실제 증가한 동작 수다. 다른 선택/배치 상태에서는 일부 명령이 거부되거나 no-op일 수 있다. `audioCallbacks`, `playbackReadyOperations`, `playingOperations`, `rowPaintCount`도 확인한다. PASS는 반복 중 문서 검증과 세션 오류 확인, 재생 준비/paint 관측을 통과했다는 뜻이다. 종료 저장과 자원 해제는 뒤에 이루어지므로 사용자는 report와 실제 프로세스 종료 코드를 함께 확인해야 한다. 코드상 정상 완료는 0, 검증 실패/창 닫기는 1, 시작/보고서 쓰기 오류는 2이며 처리되지 않은 예외에는 이 코드 규약이 적용되지 않는다. UI 예외를 삼키지 않으므로 report가 없을 때는 마지막으로 flush된 operation/phase가 진단 자료다. JSON 자체가 잘못된 경우에는 시나리오 옆 `automation-error.txt`를 확인한다.

다른 seed로 다시 실행할 때는 원본에서 새 실행 디렉터리로 복사하고 그 복사본의 `project.writer.lock`을 삭제한 뒤 JSON의 경로를 변경한다. 위 두 복사본도 실행 종료 시 편집 checkpoint가 저장될 수 있으므로 초기 상태 비교에는 매번 새 복사본이 필요하다. 기록 원본 파일은 앱 스트레스 입력으로 직접 지정하지 않는다.

남은 위험은 CEO 장애 원인의 미확정, 이번 최종 앱의 실제 스트레스/종료 미검증, 실제 카메라·ASIO·GPU 환경의 재현 미실시다. 정수 상한 근처의 ruler/waveform 등 다른 시간 연산 전체는 감사하지 않았다. 파생 세대 검사는 메모리의 늦은 결과를 거부하는 기능이며 디스크의 오래된 `.peaks.json` 내용을 새로 검증하는 기능은 아니다. 메모리 고갈이나 native/SEH 장애를 모두 회수한다고 보장하지 않는다. 본선 0.1.3의 sample-rate 정책, 카메라 기본값, CrashHandler 및 `s2-stereo`/`s3-timeline` 병합 후 별도 통합 검증이 필요하다.

이 작업은 `ProductIdentity`, 버전, 릴리스 노트, site, `showEditMenu` 구현을 수정하지 않았고 `CrashHandler.*`를 추가하지 않았다. push와 `release.py`도 실행하지 않았다.

**커밋은 환경 권한 때문에 생성하지 못했다.** 실제 `git add`가 `C:/Users/claude/gocue/.git/worktrees/gocue-rec-app/index.lock: Permission denied`로 실패했다. 현재 HEAD는 기준 `4f2131931f1f298debe014a01ae1943df114a050`이며 새 커밋 해시는 없다. 승인 승격이 불가능한 세션의 Git 메타데이터 쓰기 제한을 우회하지 않았다. 권한이 있는 실행기에서 나눌 의미 단위는 다음과 같다.

1. `fix(recorder): guard cut-edit previews and async playback completions` — `RecorderSession.*`, `TimelineView.cpp/.h`, friend 선언, `CutEditStabilityTests.cpp`, `StabilityTestAccess.h`, `TestMain.cpp`, CMake의 suite 등록.
2. `test(recorder): add in-app cut-edit stress automation` — `CutEditStress.cpp`, `TimelineView.scenario.cpp`, CMake의 `target_sources(Recorder PRIVATE tools/CutEditStress.cpp)`.
3. `docs(recorder): record cut-edit stability evidence and validation` — 이 문서.

실행기가 지정한 OUT 경로가 환경/메시지에 전달되지 않아 요약은 `out/stability-2026-09-10/OUT.md`에 남긴다. Git에 추적되지 않는 새 테스트/도구 파일도 작업 트리에 모두 남아 있다. 전달용 전체 diff는 `out/stability-2026-09-10/changes.patch`, 검증 수치와 바이너리 SHA-256은 같은 디렉터리의 `validation-summary.json`에 보관한다. 패치는 커밋이 아니며 현재 작업 트리에는 이미 반영된 내용이다.
