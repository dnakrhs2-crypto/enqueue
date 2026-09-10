# fix-015-gap 검증 결과

2026-09-10. 작업 트리 `C:\Users\claude\gocue-rec-storage`, 브랜치 `fix-015-gap`, 기준 `926d65bdb31bec3d0f58a1254fd0bb14e4fb736d` (`integrate-015`). 실행기 OUT 경로가 메시지와 환경 변수에 없어서 사전에 안내한 이 경로에 작성했다. 파일 수정·빌드·테스트만 수행했다. 커밋·push·release.py 실행 및 ProductIdentity·버전·릴리스 노트·site 수정은 없다. 병렬 세션을 사용하지 않았다.

**결과: P2의 꼬리·선두 소스 공백 불일치를 수정 전 실행으로 재현하고 수정 후 통과시켰다. 세 대상 빌드 성공, 전체 49개 suite 결과 합계 689 통과 / 0 실패, 정렬 프로브 비교 오차 0.**

## 원인 재확인과 수정 전 실패

`RecorderSession::preparePlayback()`은 원래 클립을 `availableRanges`와 실제 `VideoIndex::length`로 먼저 잘라 `mapping.gaps`를 비운 조각을 만들었다. 엔진의 `validatedClips()`는 받은 조각을 다시 처리하면서 `begin == 0`, `end == c.lengthSamples`만으로 원래 클립 경계 도달 여부를 만들었다. 세션에서 이미 제외한 결손 구간의 존재가 사라져, 서로 다른 클립 ID 사이의 1프레임 미만 소스 결손을 정상 틈처럼 메웠다.

새 테스트는 `StabilityTestAccess::prepareVideos()`에서 실제 `RecorderSession::preparePlayback()`을 호출하고 실제 비동기 `planWork` 완료를 기다려 `collectPreparedPlan()`의 영상 목록을 엔진에 전달한다. 인덱스 캐시와 오디오 장치만 합성 값으로 제공하며, 준비 결과나 `mapping.gaps`를 테스트가 만들어 엔진에 직접 넘기는 방식으로 두 필수 사례를 대체하지 않았다. 내보내기 판정은 실제 `ExportJob`의 컴파일된 span과 `FinalVideoExporter::mappingAt()`을 사용한다.

재생 제품 코드 3개 파일이 기준 커밋과 동일한 Git blob임을 `out/fix-015-gap/before-production-sources.json`에 기록했다. 최초 테스트 변경은 `before-test-changes.patch`, `before-PlaybackGapChecks.h`에 보존했다.

| 사례 | 수정 전 실제 관측 | 수정 후 |
| --- | --- | --- |
| 48kHz/60fps, 앞 클립 논리 길이 320640, 가용 범위 `[0,320000)`, 실제 인덱스 길이 320000, 다음 시작 320640 | 샘플 320000(출력 프레임 400)에서 `playbackGap=0`, `exportBlack=1`, `selectedPts=399`. 320001, 320639에서도 불일치 | 세 지점 모두 gap/clear, 내보내기와 일치 |
| 다음 클립 시작 320640, 선두 소스 gap `[0,640)`, 실제 첫 가용 조각 시작 321280 | 샘플 320640, 320800(출력 프레임 401), 321279에서 `playbackGap=0`, `exportBlack=1`, `selectedPts=400` | 세 지점 모두 gap/clear, 내보내기와 일치 |

수정 전 `RecorderTests.exe --suite timeline-ux`: **36 통과 / 2 실패, 종료 코드 1**. 새 필수 두 테스트가 실패했고 내부 1샘플 공백 및 정상 붙임 대조군은 통과했다. 실패 로그: `out/fix-015-gap/before-timeline.log`, 종료 코드 `before-timeline-exit.txt`. 빌드 로그 `before-build.log`, 종료 코드 0.

## 수정 위치

| 파일:행 | 변경 |
| --- | --- |
| `recorder/src/playback/VideoPlaybackEngine.h:33`, `:40` | `PlaybackVideoClip`에 `beginsAtClipStart`, `endsAtClipEnd` 추가. 전체 클립의 기본값은 true이고 절단한 조각은 해당 경계 값을 내려야 한다. |
| `recorder/src/app/RecorderSession.cpp:373` | 가용 범위와 실제 영상 길이를 적용한 뒤, `begin == clip.sourceIn`, `end == clip.sourceIn + clip.lengthSamples`로 원래 편집 클립의 양쪽 경계 도달 여부를 전달한다. |
| `recorder/src/playback/VideoPlaybackEngine.cpp:404`, `:458` | 엔진 내부의 별도 경계 필드를 제거하고 전달받은 필드를 사용한다. 명시적 gap 재분할에서는 기존 값과 새 조각 경계 조건을 AND하여 false가 true로 복원되지 않게 한다. `:482`의 기존 틈 메우기 조건이 이제 원래 경계를 판정한다. |
| `recorder/tests/StabilityTestAccess.h:54` | 실제 세션의 비동기 재생 준비를 실행하는 테스트 접근 함수 추가. 합성 오디오를 사용하며 실제 ASIO 장치를 열지 않는다. |
| `recorder/tests/PlaybackGapChecks.h:12`, `:80`, `:111` | 세션 준비 → 엔진 → 화면 선택/clear 판정과 실제 내보내기 mapping 비교, 정상 틈의 마지막 소스 좌표 고정 및 엔진 재분할 검증. |
| `recorder/tests/TimelineUxTests.cpp:260` | 회귀 테스트 7개 등록. 기존 직접 엔진 gap 테스트도 유지했다. |

소스 좌표 clamp(`VideoPlaybackEngine.cpp:409`), 250ms IDR 프리롤(`VideoPlaybackEngine.h:163`), 이전 화면 유지 및 epoch 무효화 clear(`VideoPlaybackEngine.h:123`)의 구현은 변경하지 않았다. RenderPlanCompiler와 FinalVideoExporter도 변경하지 않았다.

## 추가 테스트 및 보존 동작

1. 실제 세션 준비의 복구 꼬리 공백: 23개 샘플 지점, 내보내기 불일치 0, 가용 영상 누락 0. 프레임 400의 검정 판정 포함.
2. 실제 세션 준비의 다음 클립 선두 공백: 23개 지점, 불일치 0, 가용 영상 누락 0. 프레임 401의 검정 판정 포함.
3. 실제 세션 준비의 내부 1샘플 카메라 공백: 가용 조각 3개, 23개 지점, 불일치 0, 해당 지점 clear 유지.
4. 실제 세션 준비의 정상 붙임: 경계 320640, 23개 지점에서 검정 0 / 빈 프레임 0.
5. 실제 세션 준비의 트림된 정상 클립: `sourceIn=800`, 틈 799샘플, 25개 지점에서 검정 0 / 빈 프레임 0. cold seek도 앞 클립의 마지막 소스 샘플에 해당하는 PTS에 고정된다.
6. 원래 끝까지 도달하지 못한 조각을 엔진에서 명시적 gap으로 추가 분할해도 끝 경계 자격이 복원되지 않는다.
7. 원래 시작에 도달하지 못한 조각을 추가 분할해도 시작 경계 자격이 복원되지 않는다.

수정 후 선택 실행: `timeline-ux` **41 통과 / 0 실패, 종료 코드 0**. 로그 `out/fix-015-gap/after-timeline.log`, `after-timeline-stderr.log`, `after-timeline-exit.txt`.

전체 실행에 포함된 기존 테스트에서 IDR prefix decode 계획, epoch 교체 시 검정 1회 후 새 프레임 표시, 두 카메라 프리롤의 epoch 취소, forward/reverse scrub 중 이전 그림 유지가 모두 통과했다. 발췌 `out/fix-015-gap/preserved-behavior.log`. 기존 1/160/799샘플 정상 틈의 PTS 유지와 800/801샘플 실제 틈 판정도 통과했다.

실제 H.264 복사본을 사용한 기존 선택 테스트 4개도 실행했다. split/delete/move, 독립 파일 붙임, ripple 삭제의 각 21프레임과 실제 테이크 경계 418080의 정상 붙임/160샘플 틈 각 21프레임, **총 5개 측정 조건 105프레임에서 검정·빈/정확 프레임 누락·PTS 오류·순서 오류 모두 0**. 모든 엔진 telemetry의 프리롤은 250ms였다. `out/fix-015-gap/seams-summary.json`, `seams/real-*.json`, 같은 이름의 CSV에 보존했다.

## 빌드와 전체 테스트

PATH에 cmake가 없어 설치된 실행 파일의 절대 경로를 사용했다. PowerShell이 MSBuild 인자를 분리하지 않도록 각각 따옴표로 전달했다.

```powershell
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --preset local
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --build --preset local-release --target Recorder --target RecorderTests --target RecorderProbe -- '-m' '-v:m' '-nologo'
$env:RECORDER_TEST_NO_HARDWARE = '1'
$env:RECORDER_SNAP_PROJECT = "$PWD\out\fix-015-gap\project-copy\project.recorder"
$env:RECORDER_CUT_SEAM_MEDIA = "$PWD\out\fix-015-gap\project-copy\media\takes\8b6841be-582a-4b00-87bb-606af0ab79e2"
$env:RECORDER_CUT_SEAM_REPORT_DIR = "$PWD\out\fix-015-gap\seams"
& .\build\vs2022\recorder\RecorderTests_artefacts\Release\RecorderTests.exe
```

실제 테스트 프로세스는 같은 환경/인자로 `Start-Process -WindowStyle Hidden`을 통해 실행하고 stdout/stderr를 별도 파일에 수집했다. 기존 `RECORDER_TEST_NO_HARDWARE=1` 경로가 합성 ASIO owner를 유지하여 뒤쪽 lifecycle suite의 실제 장치 생성을 차단했음을 전체 로그 649행에서 확인했다. 캡처보드·FlexASIO는 열지 않았다.

- configure 및 Recorder / RecorderTests / RecorderProbe 빌드 모두 종료 코드 0. 디렉터리 오류가 없어 `-m:1 -nr:false` 재시도는 필요하지 않았다. 기존 C4324 경고 등은 있으나 빌드 오류는 없다.
- 전체 **49개 suite 결과, 689 통과 / 0 실패, 종료 코드 0**. 기준 678 + 새 회귀 7 + 기존 실제 미디어 선택 테스트 4 = 689. 전체 실행의 `timeline-ux`는 실제 미디어 선택 테스트를 포함해 42/42다.
- 별도 JSON 형식으로 결과를 내는 edit-property는 seed 909, 1000회, invariant 및 undo/redo hash 검증 PASS.
- 마지막 출력 `RecorderTests: all suites passed`. `git diff --check` 종료 코드 0.

로그: `out/fix-015-gap/configure.log`, `final-build.log`, `all-tests.log`, `all-tests-stderr.log`; 각 종료 코드 파일과 `all-tests-summary.json`. Git 검사 로그는 `diff-check.log`, `diff-check-exit.txt`.

## 실제 미디어 복사와 정렬 프로브

사용자가 지정한 원본:

`C:\Users\claude\AppData\Local\Temp\claude\C--Users-claude--local-bin\08ab1c51-2e72-4134-a6ce-9b201246a06f\scratchpad\rec_selftest\demo-f0ad256141554a16bf14d905d8a1d710\`

`out/fix-015-gap/project-copy/`로 전체 복사한 뒤 **복사본의 `project.writer.lock`만 삭제**했다. 기존 선택 테스트에 필요한 `cam-second.mp4`와 `000001-second.wav`는 복사본 내부에서 추가 복제했다. 선택 테스트가 만든 `snap-joined.recorder`도 복사본 내부의 새 파일이다. 검증 종료 후 원본 12개 파일의 SHA-256/수정 시각이 모두 최초와 같고 원본 lock이 남아 있음을 확인했다. 복사한 기존 파일 11개도 원본과 계속 일치했다. 근거 `original-copy-sha256.json`, `original-copy-after.json`, `copy-summary.json`, `test-media.json`.

```powershell
& .\build\vs2022\recorder\RecorderProbe_artefacts\Release\RecorderProbe.exe alignment --project "$PWD\out\fix-015-gap\project-copy\project.recorder" --report "$PWD\out\fix-015-gap\alignment.json"
```

종료 코드 **0**. 48kHz/60fps, 실제 MP4 601프레임 디코드, 인덱스/디코드 영상 길이 480800샘플, 프로젝트 클립 길이 480480샘플.

| 검사 | 실제 비교 수 | 결과 |
| --- | ---: | --- |
| seek 영상 / 디코딩 PTS / 컴파일된 소스 프레임 | 16 | 오차 0 |
| 썸네일 및 UI 프레임 요청 좌표 | 16 | 영상과 오차 0 |
| 배타적 클립 끝 | 1 | gap 유지 |
| 재생 중 영상 요청 대 가청 커서 | 36 | 0ms, 모든 선택 프레임이 해당 커서를 포함 |
| PCM 원본 바이트 대 재생 출력 | 11 | 불일치 0, 매핑 오차 0ms |

원시 결과 `out/fix-015-gap/alignment.json`, 집계 `alignment-summary.json`, 실행 로그 `alignment.log`, `alignment-stderr.log`, 종료 코드 `alignment-exit.txt`. 프레임 시작과 프레임 내부 요청 샘플의 차이는 정상적인 포함 프레임 선택이며 정렬 오차로 계산하지 않았다.

## 남은 검증 범위

두 결손 사례는 **실제 세션 준비 경로**를 거치되 인덱스/디코더와 오디오 장치는 합성 객체로 제어했다. 내보내기의 실제 렌더 계획 및 `FinalVideoExporter::mappingAt()` 검정 판정을 비교했으며, 해당 결손 프로젝트를 NVENC로 인코딩한 최종 픽셀이나 실제 HWND의 DXGI Present 결과를 측정한 것은 아니다. 실제 미디어 선택 테스트는 제품 D3D11VA 디코더와 합성 표시 수신기를, 정렬 프로브는 실제 소프트웨어 H.264 디코드와 합성 오디오 출력을 사용했다. 캡처보드·FlexASIO 실장치 검증은 요청대로 수행하지 않았다.
