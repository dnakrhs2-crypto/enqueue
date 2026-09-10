fix-015-import 작업 결과 — 2026-09-10

작업 트리: `C:\Users\claude\gocue-rec-import`, 브랜치 `fix-015-import`, 시작 커밋 `5cda42c` (Recorder 0.1.4). 시작 시 변경 파일 없음. 실행기 OUT 경로가 메시지·환경변수에 없어 질의 때 안내한 이 경로에 기록했다.

메인 창에서 완성 오디오를 불러와 독립 `importAudio` 트랙에 배치하고 재생하는 경로를 연결했다. 시작 시 `AudioImportPanel`은 비활성 더빙 화면에서만 생성됐고, `RecorderSession::preparePlayback`은 가져온 오디오를 만나면 "타임라인 재생 연결은 준비 중" 예외를 던졌다. 재열기 후 임포트 파형을 로드하는 경로도 없었다. 재생을 완성하기 위해 `RecorderSession.cpp/.h`의 임포트 연동부를 함께 수정했다.

버튼은 RecordView 상단 줄의 **더빙과 설정 사이**에 있는 **오디오 불러오기**다. 버튼을 누르면 기존 파일 선택기 → `AudioImportPanel::importFile` → 원본 복사·검증·PCM 캐시 → 문서 반영으로 이어진다. 저장 위치가 없는 새 문서에서는 프로젝트 메뉴에서 저장 폴더를 먼저 선택하도록 안내한다.

배치 규칙은 **요청 순간을 기준으로 타임라인 모드에서는 재생헤드, 녹화 화면에서는 타임라인 끝**이다. 파일 선택기를 연 뒤 또는 변환 중 재생헤드가 바뀌어도 배치 위치를 다시 계산하지 않는다. 요청 위치를 캡처한 뒤 재생을 일시 정지한다. 완료하면 타임라인을 열어 새 클립을 선택하고 해당 트랙·위치로 이동한다. 재생 버튼으로 듣는다. 상태줄에는 단계·백분율과 배치 위치(초)를 함께 표시하고, 아래 조작 줄 오른쪽에는 진행률과 취소 버튼을 표시한다.

메인 창의 `FileDragAndDropTarget`도 같은 임포트 경로를 사용한다. WAV/WAVE, MP3, M4A, AAC와 기존 코어가 제공하던 M4B/MP4/WMA 확장자를 받는다. 한 번에 한 파일씩 처리하며 여러 파일을 동시에 놓으면 한국어로 안내한다. 미지원 형식·손상 파일·취소·녹화/마무리 중 요청도 한국어로 알린다.

프로젝트와 원본 Fs가 다르면 기존 HighQualityResampler 규칙으로 **프로젝트 Fs의 float32 재생 PCM**을 만든다. 완료 시 `44100 → 48000 Hz로 재생용 변환됨 · 원본 보존` 또는 `48000 Hz · 샘플레이트 변환 없음`처럼 알린다. 원본 바이트와 원본 형식 메타데이터는 보존한다. 변환/검증 실패 시 실패 사유를 표시하고 문서에 반영하지 않는다.

파일 선택·임포트 중 lifecycle `fileWork`를 유지한다. 녹화·마무리 중에는 버튼과 임포트 요청을 막고, 임포트 중에는 프로젝트 변경·녹화 시작·설정 변경·업데이트 종료를 차단한다. 앱 종료 요청은 선택기를 취소하고 워커 취소 완료를 기다린 뒤 종료 콜백으로 진행한다. 직접 컴포넌트가 파괴되는 경우에도 문서가 살아 있는 동안 워커를 취소·join한다. 재생 캐시 준비와 파형 로드 작업에도 프로젝트 교체/종료 시 취소와 세대 확인을 적용했다.

수정 위치:

| 파일:줄 | 변경 내용 |
| --- | --- |
| `recorder/src/ui/RecordView.h:22`, `recorder/src/ui/RecordView.cpp:43`, `:112` | 상단 오디오 불러오기 버튼 생성·배치. RecordView의 나머지 영역은 수정하지 않음. |
| `recorder/src/ui/MainComponent.h:20`, `:63` | 메인 파일 드롭 대상, 임포트 소유·상태. |
| `recorder/src/ui/MainComponent.cpp:19`, `:23`, `:131`, `:149` | 실제 버튼/드롭 경로, 고정 배치 위치, 파형 반영·클립 선택·타임라인 이동. |
| `recorder/src/ui/MainComponent.cpp:80`, `:122`, `:169`, `:294`, `:306`, `:396` | 녹화/마무리/파일 작업 게이트, 취소·종료·fileWork 상태. |
| `recorder/src/ui/AudioImportPanel.h:16`, `recorder/src/ui/AudioImportPanel.cpp:14`, `:25`, `:44`, `:63` | 파일 선택 명령 공개, 간결한 진행 UI, 한국어 상태 콜백, 실패/취소/예외 수집, 워커 join. |
| `recorder/src/ui/ProjectDialogs.cpp:73`, `:122`, `:139`, `:165` | 임포트 중 설정·프로젝트 교체·저장 진입 차단. |
| `recorder/src/app/RecorderSession.h:83`, `:113`, `:125`, `recorder/src/app/RecorderSession.cpp:345`, `:387`, `:660` | 가져온 오디오 PCM 캐시를 기존 TimelineAudioRenderer/SharedOutput 재생 경로에 연결. |
| `recorder/src/app/RecorderSession.cpp:485`, `:497`, `:711` | 재열기 파형·캐시 복원, 프로젝트 교체/종료 취소. |
| `recorder/src/playback/ImportedAudioCache.h:36`, `recorder/src/playback/ImportedAudioCache.cpp:117` | L/R 채널을 포함한 화면용 파형, 최대 bin 수를 지키는 압축. |
| `recorder/src/media/AudioImport.h:45` | 호출자가 고정한 배치 위치라는 주석 정리. |
| `recorder/tests/UiWiringTests.cpp:240`, `:311`, `:347`, `:370` | 실제 메인 명령 경로 회귀 7건. |
| `recorder/tests/AudioImportTests.cpp:86` | 오른쪽 채널만 있는 신호·긴 파형 끝·bin 상한 회귀 1건. |

추가 테스트 8건:

- 실제 WAV를 임시 폴더에 생성하는 4조합: 모노/스테레오 × 44.1/48 kHz, 프로젝트 48 kHz. 실제 `recordView.importButton.onClick`을 호출한다. OS 파일 선택기만 fixture 파일을 반환하도록 대체하고 `chooseFile` 이후 임포트 워커·문서·UI 콜백은 제품 코드를 실행한다.
- 각 조합에서 독립 트랙/클립·변환 길이·한국어 Fs 결과·lifecycle 게이트를 확인한다. 실제 TimelineView를 소프트웨어 이미지에 그려 파형 열이 그려졌는지 확인한다. 실제 MainComponent의 재생 버튼을 눌러 TimelineAudioRenderer → TimelineTransport → SharedOutput → RecorderAudioEngine의 합성 출력에 도달한 PCM의 에너지와 모노 복제/스테레오 채널 구분을 검사한다.
- 각 조합을 저장한 뒤 파생 PCM 캐시 파일을 제거하고 재열어, 원본에서 캐시와 파형을 복원하고 다시 출력되는지 확인한다. 소재 내보내기에서 `includeImports=true`로 실제 WAV를 생성해 비무음 PCM을 읽는다. 최종 내보내기는 완성 오디오 선택 검증·소재 ID 포함·공유 렌더러의 비무음 PCM을 확인한다. 외부 원본 SHA-256도 대조한다.
- 드롭 회귀 1건: 녹화 화면의 끝 배치, 타임라인의 비정렬 재생헤드 12347 샘플 고정, 임포트 중 커서 변경, 실제 TimelineView 스플릿·이동·삭제·undo/redo·저장/재열기. 남긴 구간은 재생되고 삭제한 구간은 무음인지 PCM으로 검사한다.
- 실패 회귀 1건: 미지원 확장자·복수 파일 드롭 안내, 녹화/마무리 게이트, 손상 WAV 롤백과 한국어 실패 메시지, 게이트 해제.
- 종료 회귀 1건: 사용자 취소·앱 종료 요청·직접 파괴 3경로에서 늦은 문서 반영과 임포트 원본 복사 디렉터리 잔류가 없고 외부 WAV가 보존되는지 확인한다.
- 파형 회귀 1건: 오른쪽 채널만 있는 끝 신호를 보존하고 최대 16384 bin 및 전체 길이 범위를 유지하는지 확인한다.

빌드·테스트 결과:

- `cmake --preset local` 성공. PATH에 cmake가 없어 설치된 VS BuildTools CMake 실행 파일의 절대 경로로 실행했다.
- `cmake --build --preset local-release --target Recorder --target RecorderTests -- -m -v:m -nologo`는 병렬 MSBuild의 빌드 디렉터리 접근 오류로 실패했다. 요청한 대로 `-m:1 -nr:false -v:m -nologo`로 재시도했다. PowerShell에서는 콜론 인수를 따옴표로 전달했다.
- 추가 테스트의 JUCE/Windows 헤더 포함 순서 오류 수정 후 Recorder·RecorderTests **모두 최종 빌드 성공, 종료 코드 0**.
- `--suite ui-wiring`: **43 통과 / 0 실패**.
- `--suite audio-import`: **AudioImportTests 11 + ImportedClipTests 4 통과 / 0 실패**.
- `RecorderTests.exe` 전체: **49개 suite 결과 합계 655 통과 / 0 실패, 종료 코드 0**. 기준 647건에서 8건 추가. 기존 edit-property 1000회도 PASS. 마지막 출력 `RecorderTests: all suites passed` 확인.
- `git diff --check` 통과.
- 최종 빌드 로그: `build/fix-015-import-build-final.log`.
- 테스트 로그: `build/fix-015-import-ui-tests.log`, `build/fix-015-import-audio-tests.log`, `build/fix-015-import-all-tests.log`.
- 실행 파일: `build/vs2022/recorder/Recorder_artefacts/Release/Recorder.exe`.

남은 검증 범위·사용 조건:

- 캡처보드·실제 FlexASIO/스피커는 열지 않았다. 위 PCM 검사는 제품 출력 경로에 합성 장치를 연결한 자동 검증이며 실물 청취 결과가 아니다.
- 새 회귀에서 실제 NVENC 최종 MP4 생성은 실행하지 않았다. 최종 MP4에는 기존 완성 오디오 파일 선택이 필요하고, 소재 WAV에는 기존 가져온 오디오 포함 옵션을 켜야 한다. 오디오만 있는 프로젝트의 최종 영상 생성에는 기존 캠 트랙 조건이 적용된다.
- 네이티브 파일 선택기 조작과 Explorer의 OS 드롭 전달은 수동 창으로 확인하지 않았다. 실제 메인 핸들러와 컴포넌트 배치·파형 그리기는 자동 검증했다. 복수 파일 일괄 임포트는 제공하지 않는다.
- 원본 데모 프로젝트에 접근·수정하지 않았다. ProductIdentity·버전·릴리스 노트·site, TimelineView*·TimelineInteraction.h·ClipEdits·AudioSettingsPanel·VideoPlaybackEngine 변경 없음. 커밋·푸시·release.py 실행 없음.
