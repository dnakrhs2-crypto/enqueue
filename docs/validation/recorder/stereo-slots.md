# 마이크 스테레오 슬롯

작업 기준: `s2-stereo`, `4f21319` (Recorder 0.1.2). 물리 장치를 열지 않고 합성 입력으로 검증한다.

## 설정과 표시

- `UserSettings::physicalInputs[i]`는 기존과 같이 0부터 시작하는 물리 입력이다. 스테레오에서는 왼쪽 입력이다.
- 새 `stereoSlots[8]`은 기본값이 모두 `false`다. `true`이면 오른쪽은 `physicalInputs[i] + 1`이다.
- XML PropertySet에는 `stereoSlot0`부터 `stereoSlot7`까지 저장한다. 키가 없는 기존 설정은 모노로 로드된다.
- `validate(deviceInputCount = 256)`는 모노·스테레오 전체 물리 입력의 중복, 음수/상한, 선택되지 않은 스테레오 슬롯, 장치 밖의 오른쪽 입력을 거부한다. 실제 장치 입력 수는 설정 UI 검증과 엔진 매핑 적용/장치 열기에서도 검사한다.
- AudioSettingsPanel의 기존 콤보에 `1 · 이름`과 `1+2 · 이름/이름 (스테레오)`를 함께 제공한다. 인접 쌍은 홀수 시작으로 제한하지 않는다. 다른 슬롯이 같은 입력을 사용하면 먼저 그 슬롯을 해제해야 한다.
- RecordView는 `물리 입력 1+2 (스테레오)`를 표시한다. **미터는 L/R 중 큰 값**이며 툴팁에 이를 명시한다. 입력 듣기는 L을 왼쪽 출력, R을 오른쪽 출력으로 보낸다. 명시적 모노 출력은 기존처럼 두 출력의 평균이다.
- 타임라인 마이크 트랙에 스테레오 표시를 추가한다. 파형은 L/R 양쪽의 최솟값·최댓값을 합친 외곽선이다.

## 녹음과 자산

- 논리 슬롯 수는 최대 8개, 물리/PCM 채널 수는 최대 16개다. RawAudioTap과 PeakCache의 저장 상한도 16으로 확장했다.
- `RecorderAudioEngine::setInputMap(map, stereoSlots)`는 두 설정을 한 번에 검증하고 적용한다. 기존 단일 인자 호출은 모노다. 녹음 중 매핑/채널 변경은 거부한다.
- `JournalDeviceMapping`에는 `rightActiveIndex`, `rightPhysicalIndex`를 추가했다. 모노에서는 둘 다 `-1`이다.
- `WavTrackWriter::Config::mics`는 슬롯 수다. `slotChannels`는 슬롯 순서의 1 또는 2이고, 생략하면 모노다. 생산자 블록은 슬롯 순서로 묶이며 스테레오 슬롯 내부는 L/R 인터리브다.
- 슬롯별 원본 경로는 `media/takes/<take>/audio/micNN/000001.wav`를 유지한다. 스테레오는 같은 파일에 2채널 PCM24 (`blockAlign=6`), 모노는 1채널 PCM24 (`blockAlign=3`)다. 모든 슬롯의 30초 경계와 프레임 수는 같다.
- 파형 캐시는 동일한 PCM 채널 순서를 사용한다. 타임라인은 앞 슬롯들의 채널 수를 합산해 각 자산의 시작 채널을 찾는다.
- 마이크 자산의 `originalFormat.channels`는 1 또는 2다. `Take.capture.physicalInputsRight`는 무장 슬롯 순서의 오른쪽 입력 목록이고 모노는 `-1`이다. 기존 프로젝트에서 이 필드가 없으면 모노다. 모델 검증은 자산 채널 수와 L/R 매핑의 일치도 검사한다.
- `take.json.microphones[]`에는 기존 `physicalIndex`와 함께 `leftPhysical`, `rightPhysical`, `channels`를 기록한다. 일반 녹음과 더빙 모두 같은 엔진/기록 경로를 사용한다.

## 저널과 복구

- `TakeStarted.pcm`은 기본 형식이다. 모든 슬롯이 스테레오면 `channels=2`, `blockAlign=6`; 혼합이면 모노 기본 형식을 사용한다.
- `TakeStarted.files[].pcm`은 각 WAV의 명시적 형식이다. 혼합 테이크에서 파일별 형식이 우선하며, 이 필드가 없는 기존 저널은 `TakeStarted.pcm`을 따른다.
- `Checkpoint.files[].blockAlign`도 파일별로 3 또는 6이다. 프레임 수/위치는 L/R을 합산한 샘플 개수가 아니라 오디오 프레임 수다.
- RecoveryScanner는 저널 형식, WAV 채널/정렬, 실제 파일 크기와 durable watermark를 함께 검사한다. 부분 프레임은 제외하고 복구 복사본을 만든다. 원본은 수정하지 않는다.
- 복구된 자산에 채널 수와 오른쪽 물리 입력을 복원하고, `micNN` 경로의 희소 슬롯 번호를 타임라인 트랙에 유지한다.
- 기존 모노 설정·프로젝트·저널·파형 캐시는 새 코드에서 읽을 수 있다. 스테레오 프로젝트를 이전 실행 파일에서 읽는 것은 지원하지 않는다.

## 재생과 내보내기

- WavSource/MediaIndex와 PCM24 읽기를 1~2채널로 확장했다. 기존 TimelineAudioRenderer의 스테레오 레인과 컷/트림/페이드 처리를 사용한다.
- 두 소재 내보내기 진입점 모두 스테레오 마이크를 `micNN.wav` **하나**로 출력한다. `-L/-R`로 분리하지 않는다. 모노 마이크는 기존 모노 WAV다. 기존 가져온 오디오 소재 정책은 유지한다.
- WavExportWriter는 1~2채널 PCM24와 RIFF/RF64를 지원한다. RF64의 샘플 수는 채널 수와 무관한 프레임 수이며 데이터 바이트 수는 `frames * channels * 3`이다.
- 소재 manifest의 스테레오 항목은 `channels=2`, `sourceChannel=-1`이다. `-1`은 L/R 전체 인터리브를 뜻한다.
- 최종 내보내기의 개별 마이크 선택과 마이크 믹스는 L/R을 유지한다. 믹스 분모는 물리 채널 수가 아닌 선택 슬롯 수다. 일반 녹음의 AAC 참조 믹스도 같은 슬롯 평균을 사용한다. 더빙의 참조 AAC는 기존처럼 선택한 참조 오디오다.

## 보정 키

`CalibrationKey::inputMapping`에 `[논리 슬롯 번호, L, R]` 삼중항을 무장 슬롯 순서로 저장한다. 슬롯 번호는 1부터, 물리 입력은 0부터, 모노 R은 `-1`이다. 보정 키가 같아야 잔여 지연을 적용한다. 입력 매핑이 없던 기존 보정 프로파일은 로드할 수 있지만, 현재 마이크 매핑이 있는 키와는 일치하지 않는다. 새 프로파일로 확인하지 않은 입력 매핑에 과거 보정값을 적용하지 않는다.

## 검증 결과

2026-09-10, Windows Release 빌드에서 확인했다.

- `Recorder`, `RecorderTests` 빌드 성공. `cmake`는 PATH에 없어 로컬 설치의 절대 경로를 사용했다. 병렬 MSBuild의 디렉터리 접근 오류를 피하려고 단일 워커와 node reuse 비활성화 옵션을 사용했다.
- 전체 테스트: **522 passed, 0 failed** (기준선 508건). 실행기 최종 출력: `RecorderTests: all suites passed`.
- 편집 property 테스트: seed **909**, **1,000** iterations, 불변식 및 undo/redo 해시 검사 PASS.
- 스테레오 추가 검증: 8슬롯/16물리채널의 PCM 바이트 비교, 모노 혼합과 30초 청크 경계, WAV별 저널/체크포인트, L/R 최대 미터와 모니터 라우팅, 혼합 참조 믹스 RMS, 16채널 피크 저장/로드, 설정·프로젝트 왕복, 슬롯 중복/범위 검증, 보정 키 불일치, 더빙 자산, 컷편집·청크 경계 재생, 두 소재 내보내기 경로, 스테레오 RF64 가상 헤더, 최종 소스 선택/믹스 및 AAC 디코드 비교.
- 복구 검증: 모노·스테레오 혼합의 원본 바이트/채널/매핑 보존 및 반복 실행 멱등성, 오른쪽 샘플 끝이 잘린 경우 1프레임 전체를 제외하고 tail gap 생성.
- 첫 전체 실행의 실패 1건은 기존 RawAudioTap 테스트의 8채널 상한 가정이었다. 16채널 허용/17채널 거부로 갱신한 뒤 최종 전체 실행이 통과했다.
- 실제 ASIO·캡처보드·NVENC를 열지 않았다. 최종 MP4 검사는 기존 CPU MPEG4 영상 테스트 어댑터와 제품 오디오 렌더/AAC/mux/디코드 검증 경로를 사용했다. 실장치 녹음 및 GPU 최종 영상 내보내기는 미실시다.

실행 명령(PowerShell):

```powershell
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --preset local
& 'C:\Users\claude\tools\cmake\bin\cmake.exe' --build --preset local-release --target Recorder --target RecorderTests -- '-m:1' '-nr:false' '-v:m' '-nologo'
& '.\build\vs2022\recorder\RecorderTests_artefacts\Release\RecorderTests.exe'
```

로그: `build/stereo-build-final.log`, `build/stereo-tests-final.log`.

## 병합 시 참고

- `RecorderSession.cpp`: `configure()`의 두 `setInputMap` 호출에 `settings.stereoSlots` 인자만 추가했다. `record()`에는 보정 키 입력 매핑 인자를 추가했다. 샘플레이트 정책이나 `tick()`은 변경하지 않았다.
- `TimelineAudioRenderer.cpp`: WAV 소스의 채널 수·바이트 정렬·PCM24 읽기 부분만 확장했다. 재생 워커/편집 동작은 변경하지 않았다.
- `TimelineView.cpp`: 트랙 표시와 파형 채널 선택/합산만 변경했다. `showEditMenu`는 변경하지 않았다.
- `RecordView.cpp`: 마이크 카드의 물리 입력 표시와 미터 설명만 변경했다.
- `ProductIdentity.h`, 버전, 릴리스 노트, `tools/release.py`, `site`, MainComponent, CameraSettingsPanel, CrashHandler는 변경하지 않았다. push 및 release.py를 실행하지 않았다.

