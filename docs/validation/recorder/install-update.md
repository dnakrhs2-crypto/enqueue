# Recorder 라운드 33·34 — 설치·업데이트 검증

이 문서는 게시 전 검증 절차다. 실제 GitHub release, tag 생성/push, site push는 수행하지 않는다. Recorder는 ProductIdentity에 가칭 표시명, 독립 AppId, `recorder-v` tag prefix, `.recorder`, 전용 registry 경로를 둔다. 공개 repo/remote/URL은 UNCONFIRMED 또는 `.invalid`이며 `--repo`로 게시 가드를 우회할 수 없다.

## 로컬 패키징

필요한 도구는 CMake/CTest, MSVC x64, 고정 FFmpeg SDK와 부모 폴더의 원본 zip, JUCE/ASIO SDK, Inno Setup, WinSparkle이다. `CMakeUserPresets.json`은 수정하지 않는다. CMake/bin을 PATH에 추가하거나 `CMAKE`에 cmake.exe 절대 경로를 준다. WinSparkle 도구는 `--winsparkle-dir`/`WINSPARKLE_DIR`, 또는 현재 local preset의 SDK 경로로 찾는다. 포함된 별도 preset 파일이나 매크로 경로를 쓰는 환경은 `--winsparkle-dir`를 명시한다.

```powershell
# 개인 키는 저장소 밖에 보관. 실제 경로는 배포 담당이 지정한다.
$env:GOCUE_EDDSA_PRIVATE_KEY_FILE = '<실제 Recorder 서명 키 PEM 경로>'
python -B tools/release.py --app recorder --preset local --package-only
```

빌드 디렉터리는 `out/recorder-package-build/<preset>`이다. CMake가 생성한 `recorder-package-Release.json`의 실행 파일 경로를 읽으며, 기존 `build/`·추정한 artefacts 경로를 사용하지 않는다. ALL 타깃과 전체 등록 CTest를 실행하므로 병합 후 추가된 타깃·테스트도 포함한다. Python 배포 unittest도 실행한다. 테스트 실패 시 ISCC/서명으로 진행하지 않는다. `--skip-build`/`--skip-tests`/`--publish`/`--site-only`는 `--package-only`와 함께 사용할 수 없다.

사용할 키의 **공개키만** 추출해 Recorder 전용 `RECORDER_UPDATE_PUBLIC_KEY`로 빌드한다. Enqueue/LiveMix의 키·feed 설정을 재사용하거나 변경하지 않는다. 감사한 실행 파일 디렉터리의 런타임 파일·리소스와 고지를 staging에 복사하고, 잠긴 FFmpeg DLL과 PE 전이 의존성을 다시 검사한다. ISCC 이후 staging 변경도 거부한다. WinSparkle EdDSA는 **설치기 바이트의 서명**이며 Authenticode 코드 서명이나 XML 전체의 서명이 아니다.

출력은 충돌을 피하는 `out/release/recorder/0.1.0-<임의문자>/`이다. 그 안에 설치기, payload, manifest, lock/audit, appcast, latest.json, notes, 공개키, site 미리보기, validation.json을 만든다. `--source-bundle`이 있으면 source-manifest 구조·SHA를 검증한 소스 zip도 같은 폴더에 묶는다. 개인 키는 복사하지 않는다. 기존 Enqueue/LiveMix 사이트 파일은 미리보기에도 원문 그대로 복사한다. 기존 배포 모드는 미확정 Recorder 사이트를 조회·게시 대상에서 제외한다.

기술 검사를 통과한 후보를 만들면 `--package-only`는 0을 반환할 수 있지만, `validation.json.status=BLOCKED` 및 남은 gate를 출력한다. 이것은 출시 승인 표시가 아니다. 같은 릴리스의 소스·공개 URL·계약·라운드 28/34가 남아 있는 현재 후보는 게시할 수 없다. 후보 `latest.json.publishable`은 false이며 사이트의 다운로드/소스 버튼을 활성화하지 않는다.

## 설치·사용자 파일

설치기는 `PrivilegesRequired=lowest`, HKCU 연결, `%LOCALAPPDATA%/Programs/Recorder`를 사용한다. 설치기 진입 하한 `10.0.19045`는 후보 검증 범위이며 Windows 10/11 전체의 지원 보장이 아니다. 최소 OS·드라이버 조합은 클린 PC 결과로 확정한다.

프로젝트는 설치 폴더 밖 사용자가 지정한 폴더에 둔다. 설치·업데이트·제거에 프로젝트 폴더를 삭제하는 규칙이 없다. 설정 폴더도 제거하지 않는다. 쿠팡 선택 항목·고지·silent update에서 삭제된 바로가기를 다시 만들지 않는 정책은 기존 LiveMix 설치기를 따른다. 커뮤니티는 기존 사이트 링크를 보존하고 새 브라우저 자동 실행 항목은 추가하지 않았다. 검증 설치는 `/NORUN=1`을 사용할 수 있다. 실행 중 앱을 설치기가 강제 종료하지 않도록 `CloseApplications=no`다.

## 라운드 28에 연결할 API — 아직 호출부 없음

`RecorderUpdater.cpp`는 Recorder 타깃에 컴파일·링크된다. `initialise(Callbacks)`는 ProductIdentity의 appcast/registry를 WinSparkle에 전달한다. 공개 값·키가 없으면 초기화하지 않는다. `canShutdown`의 빈 callback은 요청대로 기본 허용이며, 자동 timer는 꺼져 있다. 종료 요청은 message thread로 전달하고 직전에 조건을 재확인한다. `shutdown()`은 callback 참조 대상 파괴 전에 호출한다.

**이 트리에는 라운드 28이 없고 Main.cpp/UI 파일은 이번 쓰기 범위 밖이다.** 따라서 Main의 initialise/shutdown, 메뉴의 업데이트 확인·앱 정보 호출은 아직 없다. 라운드 28에서 다음을 연결한다.

- 녹화·더빙·마무리·export·복구·미저장 revision·파일 작업을 반영한 thread-safe snapshot을 `Callbacks.canShutdown`에 제공한다. WinSparkle worker에서 UI/문서를 직접 읽지 않는다.
- `requestShutdown`을 기존 저장/종료 경로로 연결하고, Main 초기화/종료에 updater 수명을 붙인다. 작업 도중 수동·자동 업데이트가 종료하지 않는지 시험한다.
- 메뉴에 `checkForUpdatesWithUI()`와 `showAboutDialog()`를 붙인다. idle 시점에서만 `checkQuietly()`를 호출한다.

앱 정보 API는 제품 버전, 실제 `av_version_info()`, LGPL 고지, 동일 릴리스 소스 열기를 제공한다. 현재는 소스가 준비 중으로 표시된다. callback 훅/정보창 구현과 실제 사용자 메뉴 연결 완료를 구별한다.

## 클린 PC 오프라인 검사

별도 경로의 WinSparkle 도구와 배포 담당에게 받은 신뢰할 수 있는 공개키를 준비한다. bundle 안의 공개키만 사용하면 자기 일관성만 확인하므로 gate는 BLOCKED다.

```powershell
$env:WINSPARKLE_DIR = '<WinSparkle SDK 경로>'
$env:RECORDER_EDDSA_PUBLIC_KEY = '<별도 경로로 확인한 Recorder base64 공개키>'
python -B tools/recorder/validate_release.py --bundle '<package-only가 출력한 폴더>' --report out/r34/release.json
```

validator는 실행하지 않고 manifest 파일/크기/SHA, 필수 고지, 잠긴 DLL, x64 PE와 일반/지연 import의 전이, appcast/latest/identity 일치, 설치기의 실제 EdDSA 서명을 검사한다. 종료코드 0=기술 검사 및 기록된 gate 통과, 1=무효한 묶음, 2=기술 검사 통과·출시 gate 미해소다. 설치기 내부 파일 추출/설치 실행, 런타임 `LoadLibrary`, 계약의 실질적 적용이나 source rebuild의 완전성은 이 도구가 검증하지 않는다.

클린 PC에서는 SDK가 PATH에 없는 상태의 설치·실행, 일반 녹화/더빙/export/복구/미저장 중 업데이트 거부, signed 시험 feed의 실제 업데이트, 재실행·프로젝트 재열기, 제거 후 프로젝트/설정 보존, DLL 교체와 allocator 경계를 직접 확인한다. 최소 OS·ASIO·NVIDIA 드라이버·장치·검증 시간과 결과를 기록한다. 실제 공개 feed 사용/게시는 별도 출시 작업이다.

## 이 세션 실행 결과

| 실행 | 종료코드 / 결과 |
|---|---|
| `python -B -m unittest discover -s tools/recorder/tests` (`WINSPARKLE_DIR` 설정) | 0, 30개 통과. 실제 WinSparkle로 임시 테스트 키 서명 성공·설치기 변조 거부·전체 후보 CLI 검증(BLOCKED/2) 포함 |
| `cmake --preset local -B out/r33-build` | 0. 초기 PATH 미등록·MSBuild Path/PATH 중복을 해결한 실행 |
| `cmake --build out/r33-build --config Release --parallel 4 -- /nr:false` | 0, ALL 빌드. 다른 트리 MSBuild node 재사용을 끄고 실행 |
| `ctest --test-dir out/r33-build -C Release --output-on-failure --no-tests=error` | 8. Recorder 21/21 통과. 공유 EnqueueUnitTests는 8,542 검사 통과·11 실패 |
| 공유 실패의 위치 | 기존 SettingsMigrationTests의 AppData 하위 임시 폴더 생성·파일 쓰기/복사. 이 세션은 해당 작업 공간 밖 쓰기가 제한됨. 코드 변경 없이 전체 G를 권한 있는 Claude 세션에서 재실행해야 함 |
| `stage_recorder` → ProductIdentity.iss 생성 → 실제 ISCC | 0. `out/r33-installer-final-pr3r21h_/Recorder-Setup-0.1.0.exe` 생성. 서명 없는 컴파일 smoke이며 최종 배포 묶음 아님 |
| RecorderUpdater 최종 재빌드 | 0. 정보창의 버튼 반환값 수정 포함 |
| 최종 `--package-only` | 배포 키를 사용하는 Claude 실행으로 남김. 테스트를 생략하거나 공유 실패를 통과 처리하지 않음 |

원본 로그는 `out/r33-configure.log`, `out/r33-build.log`, `out/r33-ctest.log`, `out/r33-recorder-rebuild.log`이다. 게시·클린 PC·실제 업데이트·계약·exact source 확보는 수행하지 않았다.
