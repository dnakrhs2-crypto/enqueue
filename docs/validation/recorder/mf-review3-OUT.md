ship

`integrate-016`, `032b382..837f5cc`의 코드 변경 기준입니다. **2차의 남은 P2는 해소됐으며, 이번 수정에서 배포 차단 결함은 발견하지 못했습니다.**

1. **① 덧셈 overflow 검사는 유효합니다.** `offset`은 `[0, 1e9]`이고 `target >= 0`이므로 `INT64_MAX - offset` 자체가 안전합니다. 조건 연산자는 `inRange`일 때만 덧셈을 평가하며, 이후 `-2048`도 최솟값이 `-2048`이라 안전합니다. 근거: [MediaFoundationAudioFormat.cpp:253](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:253), [459](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:459), [460](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:460).

2. **② 실패 처리는 기존과 같습니다. 다만 “다시 seek를 시도하지 않는다”는 설명은 정확하지 않습니다.** `position = sample`, `seekFailed = true`, `skipUntil = -1`은 유지됩니다. 다음 읽기가 seek 분기에 도달하면 `|| seekFailed` 때문에 같은 위치에서도 `seekTo`를 다시 호출합니다. 범위 초과가 계속되면 `SetCurrentPosition`은 호출하지 않고, 버퍼를 무음으로 지운 뒤 `false`를 반환합니다. 재시도 조건은 `032b382`에도 동일합니다. 근거: [MediaFoundationAudioFormat.cpp:476](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:476), [383](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:383), [386](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:386).

3. **③ ticks 검사와 캐스트는 안전합니다.** `9.2e18 = 9,200,000,000,000,000,000`은 binary64로 정확히 표현되며, `LONGLONG_MAX = 9,223,372,036,854,775,807`보다 작습니다. 유한성·비음수·엄격한 상한 검사를 통과해야 캐스트하므로, 소수부를 버린 결과가 표현 범위 안에 있고 변환 동작이 정의됩니다. 상한 부근의 일부 표현 가능한 값까지 거부하는 보수적 제한입니다. 근거: [MediaFoundationAudioFormat.cpp:462](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:462), [471](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:471), [Windows 형식 정의](https://learn.microsoft.com/en-us/windows/win32/winprog/windows-data-types), [C++ 변환 규칙](https://eel.is/c++draft/conv.fpint).

4. **④ 정상 seek 산술은 동일합니다.** 범위 안에서는 기존과 똑같이 `(target + offset) - 2048`을 계산하고 0으로 하한을 제한합니다. 따라서 sample 0은 `max(0, offset - 2048)`, 22050은 `20002 + offset`이며, 정상 파일 끝 근처도 같은 식입니다. double 곱셈·나눗셈 순서와 정수 변환도 유지됩니다. 근거: [MediaFoundationAudioFormat.cpp:452](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:452), [460](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:460), [471](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:471).

5. **⑤ 대상 코드의 `diff --check`는 통과했습니다.** `git diff --check 032b382 HEAD -- src/audio/MediaFoundationAudioFormat.cpp`는 종료 코드 0입니다. `<limits>` 추가, 공백, 중괄호 배치에서 JUCE 스타일 문제는 찾지 못했습니다. 전체 `git diff --check 032b382 HEAD`는 함께 추가된 리뷰 문서의 후행 공백 때문에 종료 코드 1입니다. 이는 대상 코드의 차단사항으로 보지 않습니다. 근거: [MediaFoundationAudioFormat.cpp:17](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:17), [mf-review1-OUT.md:5](C:/Users/claude/gocue-rec/docs/validation/recorder/mf-review1-OUT.md:5), [mf-review2-OUT.md:5](C:/Users/claude/gocue-rec/docs/validation/recorder/mf-review2-OUT.md:5).

사용자 제공 결과인 audio-import M4A RMS `0.00614471`, Recorder `739 passed / 0 failed`, Enqueue `8553 passed / 0 failed`, MF lag `0`·seek max diff `0`을 정상 경로의 회귀 검증 근거로 반영했습니다. 극단 seek의 안전성은 단위 테스트가 아닌 위 정적 분석으로 판단했습니다.

코드 수정과 테스트 실행은 하지 않았습니다. 읽기 전용 제약으로 OUT 파일에 저장하지 않고 결과 전문을 출력했습니다.