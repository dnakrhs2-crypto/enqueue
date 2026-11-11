# 이 저장소에서 일할 때 (아스트라용 규칙)

- 커밋하지 않는다 — 검토·커밋·판올림·배포는 클로드가 한다(샌드박스는 공용 .git 에 쓰지 못한다). 변경은 작업 트리에 두고 보고서에 커밋 메시지 초안을 적는다. 판 번호·배포 노트·사이트는 지시문이 시키지 않았으면 건드리지 않는다.
- 지금 작업 폴더 밖의 다른 작업 폴더(`C:\Users\claude\gocue`, `gocue-*`)는 건드리지 않는다.
- 빌드: `cmake --build --preset local-release --target <대상…> -- -m -v:m -nologo`. 프리셋 `local`은 `CMakeUserPresets.json`(git 에 없음)에 있다 — 없으면 `C:\Users\claude\gocue\CMakeUserPresets.json`을 복사한다.
- 시험 실행 파일은 창이 사용자 화면에 뜨지 않게 반드시 이 실행기로 돌린다: `python C:/Users/claude/tools/claude_harness/run_tests_hidden_desktop.py [--timeout 초] [--idle 초] <시험 exe> <기록 파일> [이름 거르개]` (시험 exe 예: `build/vs2022/tests/EnqueueTests_artefacts/Release/EnqueueTests.exe`).
- DVW 작업 폴더(`gocue-dvw*`)에서는 빌드·시험을 `python C:\Users\claude\tools\build_server\rb.py <이 작업 폴더> -- <명령>`으로만 돌린다(rb.py 가 DVW 전용 JUCE 고정본과 프리셋을 맞춘다 — 위의 프리셋 복사와 직접 cmake 는 쓰지 않는다).
- 설치 파일(설치기)은 이 PC 에서 실행하지 않는다(컴파일까지만 — 실제 설치·덮어 설치·제거 시험은 클로드가 한다).
- 화면(모양·배치·색·글자)은 클로드가 따로 만든다 — 지시문이 시키지 않았으면 화면 모양과 화면 글자를 바꾸지 않는다.
- 앱 설정·녹음 폴더 같은 사용자 자료를 시험에 쓰지 않는다(버리는 임시 폴더만). 프로세스는 자기가 띄운 것만 번호(PID)로 끈다.
