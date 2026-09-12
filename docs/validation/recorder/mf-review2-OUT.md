fix first

`integrate-016`, `57671da..032b382` 기준입니다. **1차 P2 중 박스 경계, 박스 크기, edit 처리 문제는 해소됐지만, seek 산술의 최종 범위 검사는 남아 있습니다.** AAC 적용 제한은 타당합니다.

1. **[P2 — 일부 미해결] offset 상한만으로 seek 전체 연산의 안전성을 보장하지 못합니다.**  
   [MediaFoundationAudioFormat.cpp:252](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:252), [455](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:455), [461](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:461)  
   `editListStart <= 1e9`는 보장하지만, `target`에는 하한만 있습니다. 따라서 `target > INT64_MAX - editListStart`이면 덧셈이 overflow합니다. 덧셈이 안전해도 100ns 변환 결과는 `LONGLONG` 범위를 넘을 수 있습니다. 예를 들어 48kHz에서 `target = 44,272,185,776,000,000`, offset이 `1e9`이면 변환 결과가 범위를 초과합니다. `lengthInSamples`도 MF duration에서 계산되므로 길이 제한만으로 이 경우를 배제하지 못합니다. [333](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:333)  
   **덧셈 전에 남은 정수 범위를 검사하고, 캐스트 전에 계산한 ticks가 유한하며 `[0, 2^63)`인지 검사해야 합니다.** `SetCurrentPosition`의 실패 검사보다 먼저 문제가 발생합니다. 이는 극단적인 duration/seek 입력에 대한 정적 반례이며, 정상 AAC에서 재현한 회귀라는 뜻은 아닙니다.

2. **경계·실패 검사와 박스 크기 overflow는 닫혔습니다.**  
   [MediaFoundationAudioFormat.cpp:95](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:95), [125](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:125), [136](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:136), [152](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:152)  
   실제 호출 경로에서 limit은 파일 또는 검증된 부모 박스의 끝입니다. 헤더 읽기 성공 후 덧셈하므로 8/16바이트 헤더 계산이 안전하고, 크기 검사로 `payload > end`를 거부합니다. 64비트 크기도 남은 길이와 비교한 뒤 signed로 변환합니다. 짧은 헤더와 필드 읽기는 실패하며, 자식 순회는 전진합니다.  
   `mdat → moov`는 크기로 건너뛰어 처리하고, `moov → mdat`도 정상입니다. `size == 0`은 현재 부모 끝까지 소비하므로 경계 침범이나 무한 루프는 없습니다. 다만 중첩 박스에서도 부모 끝으로 해석하는 것은 EOF 의미의 엄격한 형식 검증과는 다릅니다. 또한 이 함수는 필요한 메타데이터를 찾으면 반환하므로 **정상 `moov` 뒤의 잘린 `mdat`까지 검출하여 반드시 0을 반환하는 전체 파일 검증기는 아닙니다.** [Apple의 size 설명](https://developer.apple.com/documentation/quicktime-file-format/atoms)

3. **빈 edit·복수 edit·재생률 처리는 닫혔습니다.**  
   [MediaFoundationAudioFormat.cpp:204](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:204), [225](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:225), [230](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:230), [247](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:247)  
   mdhd/elst version 0/1, 정확히 한 항목, 음수가 아닌 `media_time`, rate 1.0만 인정합니다. 조작된 큰 `entry_count`로 반복하거나 다음 박스를 항목으로 읽지 않습니다. 마지막 rate가 잘려도 전체 항목 읽기가 실패합니다. 샘플 환산 결과의 범위 검사도 정수 캐스트 전에 있어, 종전의 큰 `media_time → llround` 문제는 해결됐습니다.

4. **길이를 지정한 ASCII 4cc 비교는 맞습니다.**  
   [MediaFoundationAudioFormat.cpp:141](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:141), [195](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:195)  
   실제 빌드가 사용하는 JUCE의 생성자는 최대 4바이트 또는 먼저 만난 NUL까지만 읽습니다. 따라서 `moov`, `trak`, `soun` 비교에 종단 NUL은 필요하지 않습니다. [juce_String.h:110](C:/Users/claude/JUCE/modules/juce_core/text/juce_String.h:110), [juce_String.cpp:330](C:/Users/claude/JUCE/modules/juce_core/text/juce_String.cpp:330)  
   단, 비ASCII 바이트가 들어간 임의의 4cc에는 Debug assertion이 발생할 수 있습니다. [juce_String.cpp:350](C:/Users/claude/JUCE/modules/juce_core/text/juce_String.cpp:350)

5. **AAC 판별의 호출 시점과 실패 처리는 맞습니다.**  
   [MediaFoundationAudioFormat.cpp:302](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:302), [349](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:349), [477](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:477)  
   `SetCurrentMediaType`은 Source Reader 출력 형식을 정하고, `GetNativeMediaType`은 기반 소스의 native 형식을 조회합니다. Float 출력 설정 뒤에도 원본 AAC 여부를 조회하는 용도로 맞습니다. 인덱스 0은 첫 native 형식이며 파일 소스는 통상 스트림당 하나를 제공합니다. 두 API 조회 실패와 비AAC subtype 모두 false로 처리됩니다. [Microsoft: GetNativeMediaType](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereader-getnativemediatype), [SetCurrentMediaType](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereader-setcurrentmediatype)

6. **프로브는 적용 범위가 좁아졌으며, 판별력 한계는 그대로입니다.**  
   [MediaFoundationAudioFormat.cpp:355](C:/Users/claude/gocue-rec/src/audio/MediaFoundationAudioFormat.cpp:355), [0.1.9.md:29](C:/Users/claude/gocue-rec/docs/validation/recorder/0.1.9.md:29)  
   이미 편집된 AAC 출력도 단조 timestamp라면 추가 시프트할 가능성은 남습니다. 이번 검토에서도 그 구 Windows 사례의 실재 근거를 확인하지 못했으므로, 이를 별도의 재현된 차단사항으로 추가하지 않습니다. HE-AAC preroll과 다중 트랙 한계도 문서에 기록돼 있습니다.  
   후속 보강 후보는 **알려진 PCM 패턴과 edit를 가진 짧은 AAC 기준 파일을 프로세스당 한 번 디코드해 실제 파형 위치를 비교하는 것**입니다. 전체 파일 디코드 카운트 없이 환경 동작을 독립적으로 확인할 수 있지만, 파일별 모든 동작을 보장하는 판별법으로 쓰려면 추가 검증이 필요합니다.

`git diff --check 57671da HEAD`는 통과했고, 추가 코드에서 별도의 JUCE 스타일 문제는 찾지 못했습니다. 제공하신 Recorder 739/0, Enqueue 8553/0, lag·seek 차이 0 및 RMS 결과는 정상 경로의 회귀 검증 근거로 반영했습니다. 조작·절단 파일과 극단 산술 입력은 테스트로 검증된 상태가 아닙니다.

코드 수정과 테스트 실행은 하지 않았습니다. 지정된 1차 OUT 대신 `docs/validation/recorder/mf-review1-OUT.md` 보관본을 읽었으며, 읽기 전용 제약에 따라 결과 전문을 여기에 출력했습니다.