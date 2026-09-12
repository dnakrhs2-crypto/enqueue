fix-016-timeline 작업 결과 · 2026-09-12

**최종 빌드 성공, 전체 695건 통과·0건 실패(종료 코드 0), cut-edit-stress 312회 PASS(종료 코드 0).**

기준은 `4c71826676e10c0bdcf110e71466a19b2bb24f6f`, 브랜치는 `fix-016-timeline`이다. 이전 세션의 미커밋 7개 파일 수정을 검토해 이어서 완성했다. 변경은 담당 소스·관련 테스트 12개 파일과 이 문서다. 다른 세션 영역, ProductIdentity/버전/릴리스 노트/site는 수정하지 않았다. 커밋·push·release.py 실행은 하지 않았다. 프로세스 감시·강제 종료용 스크립트 파일은 만들지 않았다.

수정 위치는 최종 파일 기준이다.

| 파일:줄 | 수정 내용 |
| --- | --- |
| `recorder/src/ui/TimelineView.cpp:10`, `:218` | 툴바와 편집/우클릭 메뉴에 같은 7개 액션 목록을 사용한다. ripple 프롬프트·순서 hover 미리보기·관련 힌트·폭 특례를 제거했다. 링크 묶음 드래그 고스트는 유지했다. |
| `recorder/src/ui/TimelineView.cpp:67`, `:456`; `recorder/src/ui/TimelineView.h:25` | 타임라인의 썸네일 공급 API·저장소·변환 캐시·progressive worker·스크럽 영상 상자를 제거했다. 영상 클립은 카드와 테이크 이름·상태를 그린다. |
| `recorder/src/ui/TimelineView.cpp:151`, `:527`; `recorder/src/ui/TimelineView.h:121` | 문서 갱신 시 마커를 정렬해 보관한다. 가시 영역에 색상 2px 세로선·삼각 깃발·14px 볼드 라벨을 그린다. 라벨은 최대 160px이며 다음 마커와 화면 경계 전에 자르고 말줄임한다. |
| `recorder/src/ui/TimelineView.cpp:292`, `:349`; `recorder/src/ui/TimelineView.h:32`, `:128` | 파형 세션 배율, L/R 레인 분리, 실제 마이크 채널 오프셋, 레인별 배율과 클리핑을 구현했다. |
| `recorder/src/ui/RecorderTransportBar.cpp:5`, `:15`, `:25`; `recorder/src/ui/RecorderTransportBar.h:12`, `:16` | 확대 조절 옆에 파형 − / 현재 배율 / +를 배치했다. 배율 경계에서 버튼을 비활성화하고 중복 dim 스냅 문구를 제거했다. |
| `recorder/src/ui/TimelineView.cpp:333` | 스냅 버튼 영역을 먼저 확보해 파형 조절 버튼과 겹치지 않도록 했다. |
| `recorder/src/ui/TrackHeader.cpp:6`, `:27`; `recorder/src/ui/TrackHeader.h:18` | 대상 버튼·헤더 트랙 선택을 제거했다. 음소거 활성 색은 빨강/흰 글자, 솔로는 노랑/검정 글자다. 실제 206px 헤더 안에 두 버튼을 각각 94px로 배치한다. |
| `recorder/src/ui/ClipInspector.cpp:32` | “링크 해제: 직접 클릭한 클립” 안내만 제거하고 “링크 · N개 함께 편집”은 유지했다. |
| `recorder/src/playback/ImportedAudioCache.cpp:117` | peakSnapshot에서 스테레오 채널 수와 채널별 extrema를 보존한다. 이 함수 밖의 생성·저장·읽기 경로는 수정하지 않았다. |
| `recorder/tests/StabilityTestAccess.h:83` | 스트레스의 제거된 버튼 참조를 갱신했다. 제거된 6개 액션은 `view.invoke`로 모델을 검증하고 남은 액션은 실제 버튼을 호출한다. 공용 헬퍼의 타임라인 부분만 수정했다. |
| `recorder/tests/TimelineUxTests.cpp:144`; `recorder/tests/AudioImportTests.cpp:86`; `recorder/tests/CutEditStabilityTests.cpp:14` | 아래 회귀 테스트를 추가·갱신했다. |

제거한 UI는 영상 클립 필름스트립, 룰러 스크럽의 캠1/캠2 영상 상자, 구간 삭제하고 당기기(전 트랙/선택 오디오), 앞으로, 뒤로, 링크 해제, 선택 클립 링크, 순서 변경 hover 미리보기, ripple 충돌 창, 트랙 대상 버튼과 헤더 선택, 링크 해제 안내, 중복 스냅 상태 문구다.

남은 편집 UI는 편집 메뉴, 스플릿, 앞 트림, 뒤 트림, 삭제, 실행취소, 다시실행, 마커 추가, 구간 시작/끝 입력과 구간 선택이다. 구간 삭제는 시간을 보존한다. TimelineAction·컨트롤러·ClipEdits의 ripple/reorder/link/unlink 구현과 기존 모델 테스트, TimelineView.automation.cpp의 직접 호출은 유지했다. `std::function<void()> onAddMarkerRequested` 시그니처와 툴바/메뉴/마커 패널/M키 경로도 유지했다. 마커 이름 창은 fix-016-app 담당이다.

스테레오 녹음은 `originalFormat.channels == 2`와 해당 테이크에서 앞선 마이크 슬롯들의 실제 채널 수 합으로 L/R 위치를 구한다. L은 위, R은 아래에 그리고 중앙에 1px 구분선과 L/R 표시를 둔다. 불러온 오디오의 기존 `CachedImportedAudio.peaks`와 v1 파일은 이미 두 채널 min/max를 보존하므로 표시용 변환만 수정했다. 긴 파일은 최대 16,384 bins로 줄이면서 L/R 각각의 extrema를 보존한다. 모노·옛 1채널 요약은 단일 레인으로 그린다.

파형 배율은 TimelineView의 `waveScale`로 ×1 → ×2 → ×4 → ×8 → ×16만 사용한다. 각 레인의 min/max에 배율을 곱하고 표시 범위를 제한하며 그래픽 clip도 적용한다. 녹음·불러온 오디오 모두 적용하고 PCM·청취 게인·문서·undo·설정 파일은 바꾸지 않는다. 캐시 갱신에서는 유지하고 새 view는 ×1에서 시작한다.

담당 UI 파일의 한글·특수문자 리터럴을 확인했다. 새 `−`, `×`, 한글은 `ko()`를 사용하고 기존 확대 `−` 수정도 유지했다. UI `transport.pause` 참조는 없으며 재생 툴팁은 “재생 / 정지 · Space”다. 재생 엔진 자체의 pause는 유지했다.

회귀 테스트는 총 10건 추가했다(TimelineUxTests 8, AudioImportTests 1, CutEditStabilityTests 1). 기존 테스트는 삭제하지 않았다.

| 테스트 위치 | 검증 |
| --- | --- |
| `TimelineUxTests.cpp:144` | 제거된 6개 액션 부재, 남은 툴바/메뉴 7개, 녹화 잠금 시 목록 유지, 마커 호스트 훅 호출. |
| `TimelineUxTests.cpp:162` | 실제 구간 입력·삭제 버튼의 시간 보존과 undo. |
| `TimelineUxTests.cpp:176` | 대상 부재, 음소거/솔로 활성 색·글자색·배치·동작, 헤더 클릭 후 선택 불변. |
| `TimelineUxTests.cpp:196` | 배율 전 단계·상하한·UTF-8 라벨·버튼 상태·스냅 배치·문서 불변. |
| `TimelineUxTests.cpp:216` | 앞선 스테레오 슬롯 뒤의 스테레오 마이크: live/loaded peak의 물리 L/R 오프셋과 배율을 paint 픽셀로 검증. |
| `TimelineUxTests.cpp:242` | imported L/R 파형과 ×2/×16 표시, 레인 간 침범·상자 밖 픽셀 부재, 옛 모노 요약 호환. |
| `TimelineUxTests.cpp:263` | 역순으로 추가한 마커 61개: 전 트랙 선·라벨·인접 간격·화면 이동·좁은 창의 소프트웨어 paint. |
| `TimelineUxTests.cpp:279` | 영상 카드 내부와 스크럽 전/중 픽셀 비교로 영상 미리보기 부재 확인. |
| `CutEditStabilityTests.cpp:14` | 스트레스 헬퍼의 제거된 액션 경로와 프로젝트 유효성, 남은 마커 버튼의 실제 훅 호출. |
| `AudioImportTests.cpp:86`, `:102` | 기존 오른쪽 단독 신호 테스트를 L/R 독립 extrema 검증으로 갱신. 모노/빈 캐시 호환 1건 추가. |
| `AudioImportTests.cpp:162`, `:185` | 실제 44.1/48/96kHz 모노/스테레오 생성과 v1 캐시 재사용 후 peakSnapshot 채널 데이터 보존 검증 추가. |

기준 트리의 UiWiringTests·TimelineUxScenario에는 `setThumbnails` 호출이 없었다. UiWiringTests의 독립 ThumbnailCache 큐 테스트와 `media/ThumbnailCache.*`는 유지했다.

| 최종 검증 | 실제 결과 / 근거 |
| --- | --- |
| 기준선 | 등록 suite 42개, 출력 요약행 49개, **685건 통과·0건 실패**. 이전 세션의 `out/fix-016-timeline/baseline-tests.log`를 집계했고 새 빌드 전 재실행 `baseline-confirm-tests.log`도 동일한 수와 종료 코드 0이었다. 묶음 suite와 중복 등록 때문에 등록 수와 요약행 수는 다르다. |
| Configure | `cmake --preset local` 성공. `out/fix-016-timeline/configure.log`. |
| 빌드 | Recorder·RecorderTests·RecorderProbe 모두 성공, 종료 코드 0. `out/fix-016-timeline/build-verified.log`. |
| 최종 전체 테스트 | 등록 suite 42개 / 요약행 49개 / **695건 통과·0건 실패 / 종료 코드 0**. `out/fix-016-timeline/all-tests.log`, `all-tests-result.json`. 마지막 writer-stall을 포함한 lifecycle 18건도 통과했다. 기준 대비 +10건이다. |
| 집중 테스트 | 타임라인 49/0, AudioImportTests 12/0 + ImportedClipTests 4/0. `timeline-ux-tests.log`, `audio-import-tests.log`. 최종 전체 실행에는 컷 안정성 13/0도 포함된다. |
| cut-edit-stress | **PASS, 312/312회, 종료 코드 0**. seed 910, 26종 동작 각각 12회. `out/fix-016-timeline/stress-final/report.json`, `stress-final/exit.json`. |

요청된 `-m -v:m -nologo` 빌드는 MSBuild의 `PATH`/`Path` 중복 오류(MSB6001)로 실패했다. `-m:1 -nr:false`만으로도 같았다. 빌드 자식 프로세스에 환경변수 키 대소문자를 통일해 전달하고 아래 명령을 실행해 성공했다. 시스템 환경이나 소스 빌드 설정은 변경하지 않았다. 실패 로그는 `build.log`, `build-serial.log`, 첫 성공 로그는 `build-clean-env.log`다.

```text
C:\Users\claude\tools\cmake\bin\cmake.exe --build --preset local-release --target Recorder --target RecorderTests --target RecorderProbe -- -m:1 -nr:false -v:m -nologo
```

최종 전체 테스트는 위 빌드의 `build/vs2022/recorder/RecorderTests_artefacts/Release/RecorderTests.exe`를 인수 없이 실행했다. 자식 프로세스의 표준입력을 DEVNULL로 두고 환경변수 키를 정규화했으며 이 세션의 다른 테스트와 동시에 실행하지 않았다. 빌드 과정의 기존 C4324 정렬 경고와 담당 범위 밖 ProjectDialogs의 C4996 경고는 관련 코드를 수정하지 않았다.

스트레스는 검증 문서에 명시된 `demo-f0ad256141554a16bf14d905d8a1d710` 원본 전체를 `out/fix-016-timeline/stress-final/project`에 새로 복사해 실행했다. 복사 전후 12개 파일 SHA-256을 비교하고 복사본의 `project.writer.lock`만 제거했다. 원본 해시 목록은 `stress/source-manifest.json`이며 실행 종료 후에도 12개 모두 같았다. `stress-final/config.json`의 project/report는 절대 경로다.

최종 스트레스에서 오디오 콜백 13,069회, 재생 준비 상태 257회, 재생 중 상태 150회, row paint 4,145회를 관측했다. `stress-final/final.png`를 열어 영상 카드·마커 선/라벨·파형 조절·대상 없는 헤더 배치를 확인했다. 원본 프로젝트의 녹음은 모노이므로 스테레오 표시의 증거는 별도 live/imported 픽셀 회귀 테스트다. 스트레스의 ripple/earlier/later/unlink 항목은 모델 직접 호출이며 UI 재노출이 아니다.

실패·중단된 시도도 보존했다. 이 시도들을 최종 PASS로 계산하지 않았다.

| 시도 | 결과와 처리 |
| --- | --- |
| 첫 전체 실행 | material export 항목에서 진행하지 않아 Ctrl+C로 중단. `all-tests-interrupted.log`. 해당 suite 단독 재실행은 17/0·종료 코드 0(`materials-recheck.log`). 정체 원인은 확정하지 못했다. |
| 두 번째 전체 실행 | 694건 통과·1건 실패, 종료 코드 1. 마지막 writer-stall의 “1초 안에 지연 감지” 조건 실패. 스트레스 시작과 마지막 suite가 잠시 겹쳤으나 원인으로 단정하지 않는다. `all-tests-failed-lifecycle.log`, `all-tests-failed-lifecycle-result.json`. 구현·시간 기준은 변경하지 않았다. |
| 세 번째 단독 전체 실행 | 요약행 4개·84건 통과 후 오류 출력 없이 종료 코드 -1. `all-tests-exit-minus-one.log`, `all-tests-exit-minus-one-result.json`. 원인은 확정하지 못했다. 이후 최종 실행에서 695/0·종료 코드 0을 확인했다. |
| 첫 스트레스 | 제거된 버튼을 찾던 공용 헬퍼로 iteration 1의 earlier 단계에서 예외 반복. 정상 창 닫기로 CANCELLED·종료 코드 1을 남겼다(`stress/report.json`, `stress/exit.json`). 헬퍼 수정·회귀 추가 후 새 복사본에서 최종 312회 PASS를 확인했다. |

남은 위험은 전체 검증 과정의 정체·중도 종료·1초 조건 실패 원인을 확정하지 못했다는 점이다. 실행 조건 변경이나 최종 한 차례 통과가 그 원인을 해결했다고 단정하지 않는다. 물리 캡처보드·FlexASIO·실제 청취는 요청에 따라 사용하지 않았으며 다른 세 세션을 합친 통합 트리는 아직 검증하지 않았다. 밀집 마커는 공간 부족 시 라벨을 생략하되 선·깃발을 유지하고 전체 이름은 기존 패널에서 확인한다. 큰 배율에서 파형 그림은 레인 높이에서 포화된다.

검증 집계는 `out/fix-016-timeline/verification-summary.json`, 빌드·스트레스·전체 테스트 사이에 변하지 않은 소스 12개 해시는 `verified-source-sha256.json`에 있다. `git diff --check`도 통과했다.
