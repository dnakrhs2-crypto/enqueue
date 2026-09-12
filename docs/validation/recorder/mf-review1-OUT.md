fix first

`integrate-016`, `8188f8b..57671da` 기준입니다. 제공하신 실측은 **새 Windows의 1024샘플 회귀가 해결됐다는 강한 근거**입니다. 다만 새 파서의 오류 처리와 보정 적용 범위는 배포 전에 수정하는 편이 맞습니다.

1. **[P2] MP4 박스 내부 읽기가 경계와 실패를 검사하지 않습니다.**
   [MediaFoundationAudioFormat.cpp:169](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:169), [183](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:183), [202](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:202)
   `hdlr`, `mdhd`, `elst`의 필드를 읽을 때 각 박스의 `end`를 확인하지 않습니다. `entry_count`가 실제 항목 수보다 크면 다음 박스까지 항목으로 해석할 수 있습니다. 또한 **양수 `media_time`까지 읽고 `media_rate`에서 EOF가 발생해도 양수 offset을 반환**합니다. JUCE의 EOF 반환값 0이 안전한 실패 처리를 대신하지 못합니다. 버전도 1 이외를 모두 0 형식으로 해석합니다.
   박스별 남은 길이, 지원 버전, 항목 크기·개수, 정확한 읽기 성공을 확인해야 합니다. 추가로 현재 JUCE의 [InputStream.cpp:249](C:/Users/claude/JUCE/modules/juce_core/streams/juce_InputStream.cpp:249)는 EOF 이전에 지속적으로 `read() == 0`이 되는 I/O 오류에서 `skipNextBytes()`가 진행하지 못합니다. 새 파서에서는 실패를 반환하는 읽기 방식이 적절합니다.

2. **[P2] 경계 검사 자체에 정수 overflow 가능성이 있습니다.**
   [MediaFoundationAudioFormat.cpp:114](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:114), [207](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:207), [410](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:410)
   작은 파일에서도 조작된 양수 `largesize`가 `INT64_MAX`에 가까우면 `at + size`가 signed overflow를 일으킵니다. 뒤의 `child.end <= at` 검사는 이를 방지하지 못합니다. version 1의 큰 `media_time`은 샘플 환산 후 `llround` 범위를 벗어날 수 있고, 큰 양수 offset은 seek 덧셈·100ns 환산에도 전달됩니다.
   크기는 먼저 `size <= limit - at`으로 확인하고, 시간 환산과 seek 연산도 표현 가능한 범위를 검사해야 합니다.

3. **[P2] 빈 edit와 복수 edit를 단순 priming offset으로 처리합니다.**
   [MediaFoundationAudioFormat.cpp:193](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:193), [204](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:204)
   앞에 길이 `E`인 빈 edit가 있고 다음 edit의 시작이 `D`라면, 그 구간의 presentation time은 `raw − D + E`입니다. 현재 코드는 `E`를 버리고 `raw − D`만 적용합니다. 예를 들어 1초짜리 빈 edit라면 새 raw 경로에서 그 지연이 사라집니다. 첫 비어 있지 않은 항목에서 즉시 반환하므로 이후 편집 구간과 `media_rate`도 반영하지 않습니다. 빈 edit가 트랙 시작 지연을 나타낸다는 것은 [Apple 문서](https://developer.apple.com/documentation/quicktime-file-format/edit_atom)에도 명시돼 있습니다.
   이번 수정은 우선 **빈 edit 없는 단일 edit·정상 재생률**로 적용 범위를 제한하거나, 일반 edit의 시간 매핑까지 처리해야 합니다.

②의 프로브는 **구 Windows 호환성을 보장하는 판별법으로는 부족합니다**. [MediaFoundationAudioFormat.cpp:310](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:310)의 조건은 이미 편집 목록이 적용된 정상 출력 `0 → 1024`와 raw 출력 `0 → 1024`를 구별하지 못합니다. 전자라면 실제 오디오 첫 1024샘플을 추가로 버립니다. 첫 timestamp가 음수인지, 첫 버퍼가 부분적으로 잘렸는지도 검사하지 않습니다.

다만 **구 Windows의 실제 AAC 파일에서 그 오판이 발생했다는 재현 근거는 이번 검토로 확인하지 못했습니다**. 이를 재현된 회귀로 단정하지는 않습니다. 반대로 Microsoft API가 “편집 목록 적용 시 반드시 중복 timestamp를 반환한다”고 보장하지도 않습니다. 확장자만 검사하므로 [MF가 지원하는 PCM·MP3 등의 MP4 오디오](https://learn.microsoft.com/en-us/windows/win32/medfound/mpeg-4-file-source)까지 같은 판별에 들어간다는 점도 적용 범위를 넓힙니다.

총 전달 프레임 수는 보강 근거가 될 수 있지만, 단순히 `총 프레임 > lengthInSamples`로 판별하면 trailing padding 때문에 오판합니다. 단일 트랙의 정확한 presentation 길이, raw 길이, padding 범위를 함께 알아야 합니다. 판별 불능을 별도 상태로 두고 독립 근거가 있을 때만 시프트하는 방식이 필요합니다. 파일을 열 때마다 전부 디코드해 세는 방법은 큐 로딩 비용도 커집니다.

나머지 요청 항목은 다음과 같습니다.

- **① 정상 MP4 구조:** `mdat` 뒤의 `moov`는 데이터 본문을 읽지 않고 크기로 건너뛰므로 처리됩니다. 정상 largesize, 최상위 size 0, video-first 후 audio 트랙 탐색, mdhd/elst version 1의 필드 배치는 맞습니다. 정상 크기에서는 루프도 전진합니다. 여러 audio 트랙에서는 파일상 첫 `soun`을 택하며 MF가 선택한 트랙과 ID를 대조하지는 않습니다. [MediaFoundationAudioFormat.cpp:145](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:145)

- **② 2112 priming·HE-AAC:** 정상 범위에서 `media_time × 출력 sampleRate / mdhd timescale` 환산은 맞습니다. 2112라면 1024짜리 두 버퍼와 다음 버퍼의 64샘플을 버리는 구조라 프라이밍이 버퍼 크기의 배수일 필요도 없습니다. 다만 HE-AAC는 SBR로 출력 sampleRate가 두 배가 될 수 있어, 고정 preroll 2048을 항상 “두 AAC 프레임”이라고 볼 수는 없습니다. 이는 기존 상수의 한계이며 이번 LC 테스트가 HE-AAC seek 정확성까지 입증하지는 않습니다. [MediaFoundationAudioFormat.cpp:406](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:406), [Microsoft AAC Decoder](https://learn.microsoft.com/en-us/windows/win32/medfound/aac-decoder)

- **③ 프로브 상태·COM:** 생성자는 COM 초기화 뒤 동기 `ReadSample`을 호출하고, read-ahead 연결은 생성 완료 후입니다. 추가 경쟁 상태는 찾지 못했습니다. `seekTo()`는 pending 사용 길이·offset, lookahead 유효성, EOF, 위치, skip, expectedNext, 버퍼 수를 초기화하므로 벡터 데이터를 지우지 않아도 stale 출력은 없습니다. 두 번째 읽기가 false여도 상태는 초기화되지만 raw 판별은 false로 남습니다. **한 버퍼만 전달되는 파일은 정렬 보장이 없고**, 두 버퍼가 정상 전달되면 짧아도 판별됩니다. [MediaFoundationAudioFormat.cpp:233](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:233), [397](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:397), [CuePlayer.cpp:128](C:/Users/claude/gocue-rec/src/audio/CuePlayer.cpp:128)

- **④ 음수 위치·끝 처리:** 단일 연속 edit에서 음수 버퍼의 전체 폐기와 부분 skip은 맞습니다. `lengthInSamples`는 presentation 길이로 유지해야 하며 여기서 priming을 다시 빼면 안 됩니다. `clearSamplesBeyondAvailableLength()`도 그 길이 뒤를 제한합니다. `expectedNext >= 0` 때문에 음수 preroll 구간에는 ±2 보정이 적용되지 않지만 정상 정수 샘플 timestamp에서는 문제되지 않습니다. 참고로 ±2 로직은 작은 차이를 맞추는 보정이며 큰 불연속을 검출·거부하는 검사는 아닙니다. [MediaFoundationAudioFormat.cpp:323](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:323), [528](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:528), [535](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:535)

- **⑤ 재생·seek·루프·성능:** 관측된 구 Windows 중복 timestamp 경로는 재탐색 성공 시 기존 처리로 돌아갑니다. 관측된 새 raw 경로는 읽기에서 offset을 빼고 seek에서 더하므로 시작·seek·루프의 좌표가 일치합니다. 정상 파일 파싱은 박스 헤더 중심이고 추가 디코드도 두 출력 버퍼라 비용은 제한적입니다. 다만 큐 메타데이터 갱신도 매번 리더를 열어 대량 큐에서는 누적됩니다. 성능 수치는 측정하지 않았습니다. 공용 리더를 사용하는 제품 경로에는 같은 조건과 위험이 적용됩니다. [MediaFoundationAudioFormat.cpp:410](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:410), [482](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:482), [CueFileInfo.h:21](C:/Users/claude/gocue-rec/src/audio/CueFileInfo.h:21)

- **⑥ 정적 확인·자산:** `git diff --check 8188f8b HEAD`는 통과했습니다. 추가 코드의 괄호 앞 공백도 기존 JUCE 스타일과 맞습니다. 버전·아이콘·설치기 경로에서는 배포 차단 사항을 찾지 못했습니다.

코드 수정과 테스트 실행은 하지 않았습니다. 739/0·8553/0 및 RMS 수치는 제공하신 결과로 판단에 반영했습니다. 읽기 전용 제약으로 OUT 파일을 저장하지 않고 검토 전문을 여기에 출력했습니다.