ship

검토 범위: `integrate-016`, `6fb5e57..d4f56c7` 및 필요한 호출 경로. **2차 P2·P3 모두 닫혔으며, 이번 수정에서 추가 결함은 발견하지 못했습니다.**

1. **P2 — 닫힘.** [Recorder.iss:114](C:/Users/claude/gocue-rec/installer/Recorder.iss:114)는 구 EXE가 없으면 통과하고, [116행](C:/Users/claude/gocue-rec/installer/Recorder.iss:116)은 삭제 성공 시 통과합니다. 계속 실패하면 [121행](C:/Users/claude/gocue-rec/installer/Recorder.iss:121)의 비어 있지 않은 오류를 반환합니다. `.old` 파킹과 실패 후 설치를 허용하던 분기가 제거됐습니다. Inno의 스크립트 `DeleteFile`은 Win32 `DeleteFile`을 직접 호출하므로, 실행 이미지의 삭제 실패를 기다리는 방식과 일치합니다. [Inno 구현](https://raw.githubusercontent.com/jrsoftware/issrc/main/Projects/Src/Setup.ScriptFunc.pas), [Windows 삭제 계약](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-deletefilew)

2. **WinSparkle 종료 흐름 — 정상 경로와 거부 경로 모두 타당합니다.** WinSparkle 0.9.4는 종료 가능 여부 확인 → Setup 실행 → 앱 종료 요청 순서이며, 설치 완료를 기다리지 않습니다. 따라서 이번 대기로 인한 순환 대기는 없습니다. [WinSparkle 실행 순서](https://raw.githubusercontent.com/vslavik/winsparkle/v0.9.4/src/ui.cpp)  
   0.1.7 태그와 동일한 [MainComponent.cpp:348](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:348)의 종료 경로는 설정 저장을 시작하고, [361행](C:/Users/claude/gocue-rec/recorder/src/ui/MainComponent.cpp:361)에서 저장·장치 해제 완료를 기다린 뒤 종료합니다. 이 종료가 재시도 시간 안에 끝나면 삭제와 설치가 진행됩니다. 이미 녹화 중이면 최초 종료 가능 검사에서 막히며, Setup 실행 직후 녹화를 시작한 경쟁 상황에서도 [RecorderUpdater.cpp:48](C:/Users/claude/gocue-rec/recorder/src/app/RecorderUpdater.cpp:48)의 재검사가 종료를 거부합니다. 이때 **설치 중단은 녹화를 보존하는 올바른 결과**입니다. 모든 장치에서 정상 종료가 30초 이내라는 성능 보장까지 정적 리뷰로 확인한 것은 아닙니다.

3. **`/SILENT` 오류 표시·부분 설치 방지 — 확인했습니다.** [release.py:171](C:/Users/claude/gocue-rec/tools/release.py:171)의 실제 인자는 `/SILENT /SP- /NORESTART`이며 `/VERYSILENT`·`/SUPPRESSMSGBOXES`는 없습니다. Inno는 이 준비 실패 문자열을 오류 메시지 상자로 표시하고, 확인 후 종료 코드 **7**로 중단합니다. [Inno silent 처리 구현](https://raw.githubusercontent.com/jrsoftware/issrc/main/Projects/Src/Setup.WizardForm.pas), [종료 코드](https://jrsoftware.org/ishelp/topic_setupexitcodes.htm)  
   해당 타임아웃 분기는 `[InstallDelete]`·`[Files]`·`[Icons]`·`[Registry]` 실행 전입니다. 삭제 시도도 모두 실패했으므로 구버전은 보존되고, 새 파일의 부분 설치나 [Recorder.iss:91](C:/Users/claude/gocue-rec/installer/Recorder.iss:91)의 Tally 재실행은 발생하지 않습니다. [설치 순서](https://jrsoftware.org/ishelp/topic_installorder.htm)

4. **PascalScript 문법·경계 — 문제없습니다.** [Recorder.iss:112](C:/Users/claude/gocue-rec/installer/Recorder.iss:112)의 `0 to 60`은 최대 **61회**입니다. 전부 실패하면 마지막 회차에도 쉬므로 총 `Sleep`은 **30.5초＋파일 작업 시간**, 마지막 삭제 시도는 대략 30초 시점입니다. `Exit`는 초기화된 빈 `Result`를 유지하며, [122행](C:/Users/claude/gocue-rec/installer/Recorder.iss:122)의 `+ #13#10 +` 연결도 유효합니다. 파일 시작 바이트 **`EF BB BF`**와 한글 리터럴의 정상 UTF-8 디코딩을 확인했습니다. [문자열 예제](https://jrsoftware.org/ishelp/topic_isxfunc_createinputdirpage.htm), [Unicode 지원](https://jrsoftware.org/ishelp/topic_unicode.htm)

5. **P3 — 닫힘. 정상 payload 선택도 유지됩니다.** [release.py:439](C:/Users/claude/gocue-rec/tools/release.py:439)는 선언 EXE 자체의 링크를 `resolve()` 전에 거부합니다. [442행](C:/Users/claude/gocue-rec/tools/release.py:442)의 `declared.parent`를 순회하면서 항목별 링크 검사도 유지합니다. [454행](C:/Users/claude/gocue-rec/tools/release.py:454)은 비교 양쪽을 resolve하고, 목적지 상대 경로는 순회 루트와 같은 `source`를 사용하므로 일관됩니다. 현재 빌드 디렉터리를 읽기 전용으로 비교했을 때 **수정 전후 선택 파일 9개가 동일했고, EXE는 `Tally.exe` 하나**였습니다.

`git diff --check 6fb5e57 HEAD`는 **통과**, `release.py` AST 구문 검사도 통과했습니다. 코드 수정·빌드·설치 실행은 하지 않았습니다. Python **33 tests OK / skipped 2**, C++ **739/0**은 사용자 제공 결과이며, 이번 커밋의 병행 드라이런 결과는 판정에 포함하지 않았습니다.

2차 리뷰는 `docs/validation/recorder/tally-review2-OUT.md`에서 확인했습니다. 읽기 전용이므로 OUT 저장 대신 리뷰 전문을 위에 출력했습니다.