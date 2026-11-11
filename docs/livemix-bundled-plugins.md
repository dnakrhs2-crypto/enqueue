# LiveMix 기본 플러그인

- 든 폴더: `LiveMix.exe` 옆의 `VST3`. 설치·업데이트는 이 폴더를 먼저 지우며, 기본 체크 항목이 켜져 있을 때만 새 구성품 전체를 복사한다. 항목을 끄고 다시 설치하면 폴더가 없어지고, 제거 시에도 삭제한다.
- 도장: `VST3/bundled_plugins.json`의 `id`이며, 마지막 등록 값은 설정의 `bundledPluginsStamp`에 저장한다. 읽을 수 없으면 빈 문자열로 취급한다.
- 맞추기: 도장이 달라지면 든 항목과 그 문제 기록을 지우고 전체를 다시 스캔한다. 같으면 목록에도 문제 기록에도 없는 묶음만 스캔한다. 없어진 묶음은 늘 목록에서 제거한다. 폴더가 없으면 그 아래 목록 항목과 blacklist만 지우고, 지운 것이 있을 때만 목록을 저장하며 도장과 스캔 기록은 그대로 둔다. 안전 모드에서는 맞추기를 하지 않는다.
- 두 군데 규칙: 형식·uniqueId·이름이 같으면 메뉴·관리 목록과 세션 복원에서 든 사본을 우선한다. 앤큐는 이 폴더를 지정하지 않아 기존 동작을 유지한다. 목록 초기화도 든 항목은 남긴다.

`syncBundledPlugins`는 앱 시작 시 세션을 열기 전과 `--register-bundled-plugins`에서 같은 워커 경로를 쓴다. 묶음당 제한은 20초이며 관리 창의 60분 제한은 그대로다. 창 없는 명령은 설정과 목록만 열고 저장한 뒤, 모든 묶음이 등록되었거나 폴더가 없으면 0, 빠진 묶음이 있으면 1로 끝난다. 실제 스캔이 있을 때만 설정 폴더의 `logs/plugin-scan.log`에 스캔한 묶음별 결과와 전체 시간 한 줄을 남긴다. 폴더를 지웠다가 같은 도장의 구성품을 다시 설치해도, 남은 blacklist 없이 빠진 묶음을 전부 다시 등록한다. 시험 전용 `LIVEMIX_SETTINGS_DIR`로 실행 파일 시험의 설정을 격리할 수 있다.

플러그인을 바꿔 배포할 때:

1. `plugin_studio`에서 플러그인 11종과 `installer_manifest.json`을 빌드한다.
2. LiveMix 저장소에서 아래 명령으로 구성품과 잠금 파일을 함께 갱신한다. 원본은 읽기만 하고, 전체 파일의 크기·SHA-256·추가 파일 검사를 통과한 뒤에만 옆 폴더에 복사하여 교체한다. 원본 목록의 `source_list_sha256`을 이어받는다.
3. 변경한 `installer/gplugins.lock.json`을 검토·커밋한다.
4. 평소 LiveMix 배포 도구로 포장·배포한다. 다른 구성품 위치는 `--gplugins-dir` 또는 `LIVEMIX_GPLUGINS_DIR`로 지정한다.

```powershell
python tools/gplugins_stage.py --manifest C:/Users/claude/plugin_studio/build/installer/installer_manifest.json --out C:/Users/claude/SDKs/gocue_release/gplugins --replace --lock installer/gplugins.lock.json
```

배포 도구는 구성품 목록과 잠금 파일의 id·버전·묶음·파일 수·총 크기가 다르거나, 파일 누락·크기 변경·SHA-256 변경·목록 밖 파일이 있으면 멈춘다. 설치기 컴파일도 `GPluginsDir` 또는 그 안의 `bundled_plugins.json`이 없으면 실패한다.

0.18.x에서 `/SILENT /SP- /NORESTART /AUTOUPDATE=1`로 업데이트할 때 새 `gplugins`는 켜진다. [UsePreviousTasks](https://jrsoftware.org/ishelp/topic_setup_useprevioustasks.htm)는 기본값이 yes이고, [Inno Setup 원본의 CreateTaskButtons](https://github.com/jrsoftware/issrc/blob/main/Projects/Src/Setup.WizardForm.pas#L1422)는 먼저 기본 체크 상태를 만든 뒤 이전 Selected/Deselected Tasks에 있는 항목만 덮어쓴다. 새 항목은 양쪽에 없으므로 기본 체크가 유지된다. [SILENT](https://jrsoftware.org/ishelp/topic_setupcmdline.htm)는 작업 선택을 바꾸지 않는다. 2026-11-11 실물 확인(0.19.0 시험판 설치 파일): 0.18.3 위에 이 인자로 덮어 설치하면 `gplugins`가 켜진 채 처리되어 파일 31개가 구성품과 같고 등록 명령이 0으로 끝나며, 항목을 끄고 설치한 뒤의 업데이트는 끈 선택을 유지한다.

바깥 사본의 경로로 저장한 세션(다른 PC에서 저장한 세션 포함)은 든 사본으로 올라간다. 올라온 경로가 저장된 경로와 달라 변경으로 취급되므로, 옮겨진 플러그인과 마찬가지로 세션을 떠날 때의 저장에서 든 사본의 경로가 기록된다(파일이 있는 세션은 묻지 않고 저장된다). 2026-11-11 깔린 0.19.0에서 확인: 이 PC에 없는 바깥 경로 11개를 적은 세션이 평소 시작으로 모두 든 사본에서 올라왔다.
