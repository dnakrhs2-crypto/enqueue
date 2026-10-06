# 앱 버전 컴파일 범위 축소 — 검증 결과

2026-10-06, `C:\Users\claude\gocue-verdef`, 브랜치 `build-version-local`, 기준 `04f067a`.

구현과 빌드·측정을 완료했다. 전체 시험은 **290,990 passed / 1 failed**다. 실패는 기존 ASIO 시험의 가짜 드라이버 등록이며, 현재 실행 환경에서 필요한 레지스트리 쓰기 권한이 거부된다. Git 메타데이터 쓰기도 거부되어 **커밋은 생성하지 못했다**.

## 구현

- `src/app/AppVersion.h`는 `gocue::appVersionString()` 선언만 제공한다. `cmake/AppVersion.cmake`가 각 타깃의 `JUCE_VERSION`을 `cmake/AppVersion.cpp.in`에 적용한다. `configure_file(... @ONLY)`이 내용이 같으면 생성 파일의 수정 시각을 유지한다.
- 생성 소스는 Enqueue, LiveMix, EnqueueTests에 각각 하나씩 연결된다. Enqueue와 EnqueueTests는 `PROJECT_VERSION`, LiveMix는 `LIVEMIX_VERSION`을 사용한다. 버전이 들어간 생성 헤더는 없다.
- 두 앱의 `getApplicationVersion()`과 `ControlServer::Impl::start()`의 빈 `appVersion` 대체 경로를 함수로 연결했다. 세 타깃의 전체 컴파일 정의에서 `JUCE_APPLICATION_VERSION_STRING`을 제거했다.
- OBS 설치 시험 픽스처의 C 파일 1개와 RC 파일 4개를 `file(CONFIGURE)`로 생성한다. 기존 파일과 내용·수정 시각이 모두 같음을 확인했다.
- 버전 함수의 비어 있지 않은 값, `PROJECT_VERSION` 일치, 시험 exe의 버전 리소스 일치를 검사한다. 기대 버전은 실행 시 읽는 `AppVersion.expected`에 두어 시험 소스 재컴파일을 유발하지 않는다.
- 제어 서버는 빈 값 → 공백 한 글자 → 긴 명시값 → 한 글자 → 빈 값으로 같은 서버를 재시작하며 discovery의 `appVersion`과 `helloAck.server.version`을 검사한다. 명시값은 그대로 보존되고 빈 값만 타깃 버전으로 대체된다. 새 스위트는 첫 `expect()` 전에 `beginTest()`를 호출한다.

첫 버전 변경 측정에서 EnqueueTests의 생성 함수는 `0.15.2`인데 exe 리소스가 `0.15.1`에 남는 기존 갱신 누락을 확인했다. 시험 타깃에도 오래된 RC를 삭제하는 처리를 추가했고, 임시 버전에서 리소스 회귀 시험 4개 assertion이 모두 통과했다. 두 GUI 앱의 기존 RC 삭제 블록은 원문과 동일하게 유지했다.

## 빌드와 시험

PATH 앞에 `C:\Users\claude\tools\cmake\bin`을 추가하고 실행했다. PowerShell이 옵션을 분리하지 않도록 `-v:m`을 인용했다.

```powershell
cmake --preset local
cmake --build --preset local-release --target Enqueue EnqueueTests LiveMix -- '-m' '-v:m' '-nologo'
python C:\Users\claude\tools\claude_harness\run_tests_hidden_desktop.py --timeout 1200 --idle 180 C:\Users\claude\gocue-verdef\build\vs2022\tests\EnqueueTests_artefacts\Release\EnqueueTests.exe C:\Users\claude\gocue-verdef\build\version-local\tests-hidden.log
```

configure와 세 타깃 Release 빌드는 성공했다. 전체 시험은 원래 버전으로 복원한 최종 빌드에서 실행했다. 시험 실행 파일은 지정된 숨은 바탕화면 실행기로만 실행했고 앱을 직접 실행하지 않았다. 시험용 TEMP/TMP와 로그는 작업 폴더의 `build/version-local` 아래에 두었다.

| 시험 | 통과 assertion | 실패 assertion | 종료 코드 | 시간 |
|---|---:|---:|---:|---:|
| EnqueueTests 전체 기본 실행 | 290,990 | 1 | 1 | 183초 |
| 앤큐 patch+1 상태의 Application version 필터 실행 | 4 | 0 | 0 | 1초 미만 |

전체 실행의 유일한 실패는 `LiveMix ASIO channel count across rates`의 `the fake ASIO driver could not be registered for this process`다. `tests/LiveMixAsioChannelTests.cpp`는 변경하지 않았다. 이 시험은 `HKCU\Software\LiveMixFakeAsio_<pid>`에 임시 키를 만든다. 별도의 읽기 전용 접근 확인에서 `HKCU\Software`의 읽기 핸들은 열리지만 `KEY_CREATE_SUB_KEY` 및 시험의 정리용 쓰기 접근은 모두 **WinError 5 (Access denied)**로 거부됐다. 시험을 건너뛰거나 통과 처리하도록 코드를 바꾸지 않았다. 추가한 버전 시험과 제어 서버 시험은 모두 통과했다. 기존의 실제 장치 opt-in 시험은 기본 실행 정책을 유지한다.

## 버전 변경 재빌드

비교 기준은 제공된 `C:\Users\claude\tools\claude_harness\speed_audit_1006\version_rebuild_before.json`의 완료된 측정값이다. 각 제품의 patch를 1 올려 지정 타깃을 빌드한 후 원래 값으로 복원하고 다시 빌드했다. C/C++ 컴파일 로그의 파일 수를 실제 `.obj` 수정 시각 변화와 대조했다.

| 변경과 빌드 타깃 | 변경 전 C/C++ 파일 수 | 변경 후 C/C++ 파일 수 | 변경 후 RC 리소스 수 |
|---|---:|---:|---:|
| Enqueue `0.15.1 → 0.15.2`, Enqueue + EnqueueTests | 333 | **2** | 2 |
| Enqueue 원복, Enqueue + EnqueueTests | 333 | **2** | 2 |
| LiveMix `0.13.2 → 0.13.3`, LiveMix | 103 | **1** | 1 |
| LiveMix 원복, LiveMix | 103 | **1** | 1 |
| 변경 없는 configure 후 세 타깃 빌드 | 4 | **0** | 0 |

앤큐 변경 때 바뀐 객체는 `Enqueue.dir/Release/AppVersion.obj`와 `tests/EnqueueTests.dir/Release/AppVersion.obj`뿐이다. 라이브믹스 변경 때는 `LiveMix.dir/Release/AppVersion.obj` 하나뿐이다. JUCE 모듈, 일반 앱·시험 소스, OBS 설치 픽스처는 재컴파일되지 않았다. 표의 C/C++ 수는 RC를 제외하며, 버전 갱신에 필요한 RC 재컴파일은 별도 열에 표시했다.

마지막 configure에서 생성 소스 3개, OBS 픽스처 5개, 기대 버전 데이터 1개 등 **9개 파일 모두 내용과 수정 시각이 유지**됐다. 이어진 빌드에서 `.obj`와 `.res` 변경은 각각 0개다.

## 실행 파일의 ProductVersion

실행 없이 PowerShell `(Get-Item -LiteralPath <exe>).VersionInfo.ProductVersion`으로 읽었다.

| 시점 | Enqueue.exe | EnqueueTests.exe | LiveMix.exe |
|---|---|---|---|
| Enqueue patch+1 | 0.15.2 | 0.15.2 | 0.13.2 |
| LiveMix patch+1, Enqueue 원복 후 | 0.15.1 | 0.15.1 | 0.13.3 |
| 최종 원복 | **0.15.1** | **0.15.1** | **0.13.2** |

`project(... VERSION ...)`, 두 `juce_add_gui_app(... VERSION ...)`, `LIVEMIX_VERSION` 및 기존 두 GUI RC 갱신 블록을 유지했다. 임시 버전 변경은 모두 원복됐으며 CMakeLists.txt의 측정 전후 바이트 일치도 확인했다.

전체 소스 검색에서 남은 `JUCE_APPLICATION_VERSION_STRING` 타깃 정의는 **`recorder/CMakeLists.txt:179` 한 곳**이다. 요청대로 탤리/Recorder는 수정하지 않았다. `tools/release.py::read_version()`의 두 버전 추출 경로와 설치기의 `/DAppVersion` 사용을 확인했으며, 릴리스 스크립트·설치기에는 변경이 없다.

## 결과 파일과 커밋 제한

- [전체 시험 로그](../../build/version-local/tests-hidden.log)
- [버전 변경 재빌드 측정 JSON](../../build/version-local/version_rebuild_after.json)
- [임시 버전 시험 로그](../../build/version-local/bumped-version-test-output.log)
- [레지스트리 접근 확인](../../build/version-local/registry-access.json)
- [측정 스크립트](../../build/version-local/measure.py)

빌드 로그와 측정 자료는 gitignore 대상인 `build/version-local`에 보관했다. 최종 측정 JSON에는 각 단계의 컴파일 파일, 바뀐 객체·리소스, exe 버전, 수정 시각·내용 해시가 들어 있다. `git diff --check`는 통과했다.

한 커밋의 예정 메시지는 `Build: app version in one generated file per target`이다. 그러나 `git add`가 아래 오류로 실패하여 스테이징과 커밋을 진행할 수 없었다.

```text
fatal: Unable to create 'C:/Users/claude/gocue/.git/worktrees/gocue-verdef/index.lock': Permission denied
```

이 워크트리의 `.git`은 작업 폴더 밖에 있는 위 메타데이터 경로를 가리킨다. 현재 실행 환경은 그 경로에 쓰기 권한이 없다. 소스 변경과 이 OUT은 작업 폴더에 남아 있으며, 브랜치는 `build-version-local`, HEAD는 `04f067a` 그대로다. main 병합은 하지 않았다.
