# 카메라 입력 모드 1080p 30/60fps 제한

작업 트리: `C:\Users\claude\gocue-rec-asio`, 브랜치 `fix-016-camera`, 기준 `4c71826`.

결과: 요청된 구현과 두 대상 빌드는 완료했다. 전체 테스트는 **695건 통과, 1건 실패**이며, 실패는 수정 범위 밖 기존 테스트의 5fps 허용 기대값이다. 전체 통과를 위해 필요한 한 줄 변경은 아래에 제시했으며 승인 없이 적용하지 않았다.

## 수정 목록

- `recorder/src/capture/CameraCatalog.h:18`, `recorder/src/capture/CameraCatalog.cpp:89`: `isStandardFrameRate(const Rational&) noexcept` 추가. 29.5~30.5 또는 59.5~60.5fps만 허용하며 분모 0은 거부.
- `recorder/src/capture/CameraCatalog.cpp:111`: `preferred1080pMode` 후보를 1080p 30/60fps로 제한. 기존 프로젝트 fps 도달·거리·MJPEG > NV12 > YUY2 점수 규칙 유지.
- `recorder/src/ui/CameraSettingsPanel.cpp:14`, `recorder/src/ui/CameraSettingsPanel.cpp:65`: 콤보 문구를 `1080p 30/60fps 입력 모드 선택`으로 변경하고 같은 필터 적용. 저장된 20fps 등은 기존 기본값 선택과 `replacedMode` 안내를 통해 교체.
- `recorder/src/ui/UiState.cpp:43`: `validateCameraSettings` 내부에서 저장 모드의 해상도·fps를 검사하고 `캠N 입력 모드를 30 또는 60fps로 선택하세요.`로 거부. 빈 선택·잘못된 문자열·연결되지 않은 장치·지원 목록에 없는 정상 fps 모드는 기존 장치/입력 모드 선택 오류 유지.
- `recorder/tests/CameraSlotTests.cpp:152`: 기존 `camera-slots` suite에 회귀 테스트 11개 추가. 허용 구간 경계와 바로 바깥, NTSC 분수, 저속 모드 제외, 기본값 우선순위, 양쪽 캠 검증 문구, 가상 장치를 사용한 실제 콤보·교체 안내·빈 상태를 검증.

`friendlyModeText`, `CameraCatalog::refresh`, 제품 식별자·버전·릴리스 노트·site는 변경하지 않았다. 기존 테스트 suite에 추가했으므로 `TestMain.cpp` 레지스트리 수정은 필요 없다. 커밋·push·release.py 실행 및 프로세스 감시·kill 스크립트 작성 없음.

## 검증

- 수정 전에 폴더에 있던 `RecorderTests.exe` 전체 실행: suite 요약 합계 **640건 통과, 0건 실패**, 종료 코드 0, `RecorderTests: all suites passed`. 현재 HEAD를 새로 빌드한 기준선은 아니며, 초기 실행 파일과 현재 소스의 기존 테스트 수 차이는 아래에 분리 기록했다.
- 별도 `EditPropertyTests`: seed 909, 1,000회, PASS.
- 집계는 기존 runner의 실행 횟수 기준으로 WAV chunk suite 중복 실행을 포함한다. 개별 `PASS` 줄 618개에 요약만 출력하는 ASIO 10개·PCM 12개를 더한 수다.
- `cmake --preset local`: 성공. 설치 경로 `C:\Users\claude\tools\cmake\bin\cmake.exe` 사용.
- Recorder / RecorderTests Release 빌드: **모두 성공**, 종료 코드 0. PowerShell에서 `-v:m`을 인용하고, 디렉터리 접근 오류 후 지시된 `-m:1 -nr:false`로 재시도했다. MSBuild에 전달되는 중복 `PATH`/`Path`는 해당 실행의 프로세스 환경에서만 정리했다.
- `RecorderTests.exe --suite camera-slots`: **25건 통과, 0건 실패**, 종료 코드 0. 기존 14개와 추가 11개 모두 통과.
- 수정 후 전체 `RecorderTests.exe`: suite 요약 합계 **695건 통과, 1건 실패**, 종료 코드 1, `RecorderTests: all suites FAILED`. `camera-slots`는 전체 실행에서도 25/25 통과. 기존 `LifecycleTests`는 17건 통과·1건 실패이며 나머지 suite는 통과했다.
- 수정 후 별도 `EditPropertyTests`: seed 909, 1,000회, PASS.
- `git diff --check`: 통과.
- 로그: `build/camera-baseline-tests.log`, `build/camera-configure.log`, `build/camera-build.log`, `build/camera-slot-tests.log`, `build/camera-all-tests.log`.

초기 실행 파일보다 현재 소스를 빌드한 실행 파일에서 기존 테스트 45건이 더 실행되었다. 해당 파일들의 HEAD 대비 diff가 없음을 확인했으며 이 세션에서 수정한 테스트는 `CameraSlotTests.cpp`의 11건뿐이다. 총 실행 수 차이는 기존 45건과 새 11건을 더한 56건이다.

| 기존 suite | 초기 실행 파일 | 현재 소스 빌드 후 | 차이 |
| --- | ---: | ---: | ---: |
| AudioImportTests | 10 | 11 | +1 |
| shortcut-exceptions | 13 | 15 | +2 |
| TakeControllerTests | 13 | 18 | +5 |
| timeline-ux | 19 | 41 | +22 |
| ui-wiring | 36 | 51 | +15 |

## 남은 위험 및 확인 사항

- 하드웨어는 열지 않았다. 실제 카메라 장치에서의 수동 확인은 수행하지 않았다.
- 지원되는 1080p 30/60fps 모드가 없는 장치는 콤보가 비고 선택/적용할 수 없다. 요구된 동작이며 저속 모드로 대체하지 않는다.
- 범위 밖의 `recorder/tests/LifecycleTests.cpp:134`는 아직 5fps 모드를 기본값으로 허용한다고 기대한다. 새 요구사항과 충돌하여 해당 한 줄의 기대값 `0`을 `-1`로 변경하는 예외 승인을 요청한 상태다. 승인 전에는 이 파일을 수정하지 않는다.
- 확인된 유일한 실패: `Camera input modes get a sensible default per project fps and friendly labels` / `Only a slow 1080p mode: still the best available`. 기존 기대값은 0이며, 구현은 요구사항대로 -1을 반환한다. 테스트 생략이나 실패 무시 없이 전체 실행 결과를 기록했다.

해당 기존 테스트에 필요한 변경안(아직 미적용):

```cpp
require(preferred1080pMode({nv5, hd720}, 30) == -1, "No standard 1080p 30/60 fps mode is available");
```
