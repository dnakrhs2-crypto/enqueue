작업 결과 — 2026-09-12, `fix-016-record`, 기준 HEAD `2c0f97b`

타임라인 탭의 일반 녹화에 타임라인 오디오 청취와 입출력 지연 보정을 연결했다. 녹화 시작 시점의 가청 믹스를 P부터 ASIO에 출력하며, 오디오 끝 이후에도 무음으로 녹화를 계속하고 Stop에서 함께 끝낸다. 녹화 탭과 P 이후 가청 구간이 없는 경우는 기존 일반 녹화 경로를 유지한다. 실장치는 열지 않았다.

선택한 구현과 근거

- **B의 일반 TakeController 수명주기를 유지하면서 A의 동기화 요소를 재사용했다.** DubbingController 전체로 전환하지 않고, 공통 타임라인 렌더러를 쓰는 오디오 공급자, `outputOriginSample`, 기존 native 입력 좌표 보정, `AnchoredCameraTimeMapper`를 사용한다. 일반 MP4의 마이크 AAC, 원본 WAV, normal 테이크 저널·복구·배치·덮어쓰기·실행취소가 그대로 이어진다. 들려주는 타임라인 믹스를 새 마이크 파일이나 MP4 AAC에 합성하지 않는다.
- 기존 TimelineTransport는 영상 준비/표시, 탐색 세대, 타임라인 끝 정지, `setDubbingLocked()`에 따른 재생 명령 차단이 결합돼 있다. 녹화 중에는 기존처럼 `clearPlayback()`으로 이 재생 인스턴스를 해제하고 라이브 카메라를 표시한다. 새 녹화용 `IAudioOutputClient`를 같은 RecorderAudioEngine의 `playbackClient`에 연결한다. 별도 ASIO 장치나 별도 장치 클록을 열지 않는다.
- PCM은 기존 `PlaybackProviderPump`와 `PlaybackBlockQueue`로 최소 250ms 미리 준비한다. ASIO 콜백에서는 준비된 PCM을 지정한 제출 샘플부터 소비한다. 디스크 읽기, 렌더링, 메모리 할당, 객체 해제는 콜백 밖에서 수행한다. Stop·실패·종료 시 출력 콜백 분리 장벽을 통과한 뒤 공급자/큐를 해제한다.
- 청취 대상은 녹화 시작 때 고정한 프로젝트의 **활성 가청 오디오 span**이다. import와 기존 mic, mute/solo, 소스의 availableRanges/gaps, 마이크로페이드, 빈 트랙을 포함한 기존 믹스 게인을 공통 렌더러 규칙으로 처리한다. P가 공백이어도 뒤에 가청 span이 있으면 청취를 시작한다. 가청 span이 없으면 공급자 자체를 만들지 않는다. PCM 값이 전부 0인지 스캔하는 판정은 아니다.
- 타임라인 배치는 여전히 P, 녹화 탭은 `activeTimelineEnd()`다. 지연을 이유로 배치 P를 음수로 이동하거나 sourceIn을 바꾸지 않는다. 무장된 마이크를 모두 녹음하며 sparse 논리 번호와 스테레오 채널을 유지한다. 테이크 스택을 만들지 않는다. 녹화 중 구조 잠금과 정지 시 일반 덮어쓰기를 유지한다.
- 출력 시작 때 클록 추정용 관측 세대만 바뀌는 경우는 기존 더빙 회귀 사례에 있다. 청취 녹화 카메라는 준비된 ASIO/QPC 원점과 MF PTS 차이를 사용하는 기존 AnchoredCameraTimeMapper를 사용한다. native ASIO 리셋/샘플 불연속/레이트·버퍼·지연 변경/언더런은 계속 실패로 처리한다. 청취 없는 일반 녹화의 기존 클록 세대 검사는 유지했다.

정렬 계산식 — 모두 프로젝트 Fs의 샘플 단위

| 기호 | 의미 |
| --- | --- |
| P | 녹화 버튼을 누를 때 예약한 타임라인 재생헤드 |
| B, C | ASIO 블록 길이, 예약 시점의 `audio.currentSample()` |
| Lin, Lout | `DeviceInfo.inputLatency`, `DeviceInfo.outputLatency` |
| Rin, Rout | 전체 장치/카메라/입력/출력 키가 일치하는 프로필의 잔차; 일치하는 프로필이 없으면 0 |
| I = Lin + Rin | 입력 좌표에서 한 번 빼는 보정량 |
| O = Lout + Rout | 출력 제출 시각에 한 번 더하는 보정량 |
| S0 | ASIO에 타임라인 P 샘플을 제출하는 위치 |
| O0 = S0 + O | 연기자가 P 샘플을 듣는 것으로 모델링한 원점 |

자동 시작은 `S0 = C + max(Fs/4, 2B)`를 예약한다. `outputOriginSample(P, Lout, {S0, P, B, epoch}, Rout)`로 O0를 계산하고 검증한다. `TakeController::start(N0)`에 명시적 N0를 주면 청취 모드에서는 그 값이 O0이며 S0는 `N0 - O`다. 두 카메라에 일치한 프로필이 각각 있으면 공통 입출력 잔차가 같은지도 검사한다. 음수인 유효 입출력 지연은 거부한다.

입력 native 샘플 좌표가 N이면 입력 어댑터에서 **`Ncorrected = N - I`**로 바꾼다. WAV에 처음 저장되는 native 샘플은 `O0 + I`이고, 파일 상대 샘플 0으로 기록한다. 타임라인에서 마이크 샘플 좌표는 **`Pmic = P + (N - I) - O0`**다. 이 뒤의 변환·WAV·AAC·배치에서는 입력 지연을 다시 빼지 않는다.

카메라는 프로필의 `cameraResidualLatency100ns`를 타임스탬프에서 빼고 준비된 ASIO/QPC 모델로 첫 영상 샘플 Nv를 구한다. **`Pvideo = P + Nv - O0`**로 원점을 정하고 이후 MF PTS 차이를 사용한다. 카메라에는 Lin을 빼지 않는다. 카메라별 잔차는 각각 적용한다. 샘플↔QPC↔100ns 변환의 정수 반올림은 아래 합성 검증에서 최대 1샘플을 허용했다.

Stop은 `Sstop = max(C, S0) + B`, `Nstop = Sstop + O`를 예약한다. 입력과 출력은 **동일한 `requestStop` 원자값**을 읽는다. 출력은 `Nstop - O`에서 끊고 입력은 보정 좌표 Nstop까지 받는다. 입력 지연만큼 뒤에 도착하는 raw 데이터를 받은 다음 최종 길이 `Nstop - O0`를 확정한다. 종료/장치 실패처럼 다음 콜백이 없는 경우는 기존 일반 테이크처럼 마지막 확인된 입력 구간을 보존한다.

normal 테이크이므로 모델·저널의 **N0 필드에 O0를 기록**한다. `placementMode=normal`, `Pstart=P`이며 더빙 O0/스택 스키마로 전환하지 않는다. 따라서 기존 복구기는 Nstop−N0와 Pstart로 같은 길이와 배치를 복원한다. `take.placementSample()`·`session.elapsed()` 시그니처는 유지했다. 녹화 중 elapsed는 보정된 입력의 확인된 길이이며, 배치 완료 직후 커서는 P+최종 길이로 이동한다. 입력 지연 때문에 아직 도착하지 않은 샘플은 UI 길이에 미리 포함하지 않는다.

수정 목록 — 이 작업 트리 기준

| file:line | 변경 |
| --- | --- |
| `recorder/src/app/RecorderSession.cpp:275`, `:303` | P/가청 span 판정, 고정 스냅샷의 청취 공급자 연결, 준비 예외 시 수명주기 해제 |
| `recorder/src/record/TakeController.h:66` | 선택적 청취 공급자; 비어 있으면 기존 경로 |
| `recorder/src/record/TakeController.cpp:58`, `:72` | P부터 읽는 공급자와 ASIO 제출/정지/언더런 처리 |
| `recorder/src/record/TakeController.cpp:159`, `:192`, `:522` | 청취 카메라에 기존 anchored mapper 선택 |
| `recorder/src/record/TakeController.cpp:544`, `:664`, `:743`, `:889` | 출력 수명 관리, 정렬 보고, 일반 오디오 준비/최종화 연결 |
| `recorder/src/record/TakeController.cpp:957`, `:1039`, `:1080`, `:1194` | 프로필 잔차 검증, 공통 시작·정지 원점 예약, 청취 중 관측 세대 변경 처리 |
| `recorder/src/audio/RecorderAudioEngine.h:40`, `:49`, `:80` | 일반 녹화의 입력 보정 설정, 합성 장치 지연 인자, 공통 정지 요청 읽기 |
| `recorder/src/audio/RecorderAudioEngine.cpp:73`, `:403`, `:605`, `:826`, `:848` | native 입력 보정 재사용, 합성 지연 유지, 정지 공유, 정렬 telemetry |
| `recorder/src/record/DubbingController.h:77`, `recorder/src/record/DubbingController.cpp:38`, `:496` | 단일 import용 공급자를 가청 타임라인 믹스에도 재사용; 무음 꼬리 지원 |
| `recorder/tests/TakeControllerTests.cpp:36`, `:87`, `:210` | 기존 video double/synthetic fixture에 출력 관측, 장치 지연·레이트·복수 마이크 지원과 새 테스트 연결 |
| `recorder/tests/TimelineRecordingTests.h:108` | 새로운 회귀 테스트 16개 |

검증 결과

- 기준 HEAD: 지정한 configure 및 Recorder/RecorderTests/RecorderProbe Release 빌드 성공. **전체 707 passed / 0 failed**, 종료 코드 0. `build/listen-baseline-configure.log`, `build/listen-baseline-build.log`, `build/listen-baseline-tests.log`.
- 수정 후 configure 및 **세 타깃 Release 빌드 성공**, 종료 코드 0. `build/listen-final-configure.log`, `build/listen-final-build.log`.
- 테이크 전용: **37 passed / 0 failed** = 기존 21 + 신규 16. `build/listen-targeted-tests-6.log`.
- 전체 회귀: **723 passed / 0 failed** = 기준선 707 + 신규 16, 종료 코드 0. `RecorderTests: all suites passed`. `build/listen-final-tests.log`.
- 기존 프로브의 헤드리스 시나리오 **1회 PASS**, 종료 코드 0. `build/listen-epoch-probe.json`, `build/listen-epoch-probe.log`. 합성 클록 + 실제 anchored mapper/CFR + 16×16 CPU MPEG4/AAC로 **60프레임 인코딩/60프레임 재디코딩**, 유효 오디오 **48,000샘플**, 잘못된 타임스탬프/세대 사례 3개 거부. 새 전체 녹화의 실제 NVENC/캡처보드 결과를 뜻하지 않는다.

실행 명령은 다음과 같다. cmake는 PATH에 없어 `C:/Users/claude/tools/cmake/bin/cmake.exe`를 사용했다. Windows 자식 프로세스 환경의 PATH/Path 중복을 피하도록 환경 키를 대문자로 정규화해서 실행했다. 지정된 `-m` 병렬 빌드가 성공했으며 단일 노드 대체 옵션은 필요하지 않았다.

```text
cmake --preset local
cmake --build --preset local-release --target Recorder --target RecorderTests --target RecorderProbe -- -m -v:m -nologo
RecorderTests.exe --suite take-lifecycle
RecorderTests.exe --suite all
RecorderProbe.exe dubbing --synthetic-epoch-regression --project-dir build/listen-epoch-probe-output --report build/listen-epoch-probe.json
```

신규 테스트는 실제 `openSynthetic()` native 입력/ASIO 출력 경로와 기존 일반 테이크 저장·배치·복구를 사용한다. 세션의 장치 없는 배치 구성 접근점으로 실제 `configurePlacement()`를 호출하고, 카메라 장치/NVENC 스트림만 lifecycle double로 대체한다. 물리 카메라 준비를 요구하는 `RecorderSession::record()` 버튼 진입 전체는 실장치 검증 대상으로 남긴다.

| 검증 | 확인 내용 |
| --- | --- |
| 혼합·선택 | import 2개 + 기존 mic의 출력 PCM을 독립적으로 계산한 원본 샘플과 비교; mute/solo, input monitoring, 한 번의 덮어쓰기/undo |
| 정렬 | 8kHz·48kHz, B=128, Lin=173, Lout=287, Rin=−11, Rout=23. **I=162, O=310**. 양 채널 WAV 1,601샘플을 모두 정확히 비교; 출력의 첫/마지막 부분 콜백도 비교 |
| 카메라 | 스트림에 전달된 실제 O0와 1ms 카메라 잔차로 기존 mapper를 구동. 예상 타임라인 좌표와 차이 ≤1샘플. 실제 광학/노출 측정은 아님 |
| 일반 경로 보존 | 빈 타임라인, 클립 끝의 P, 전부 mute, P 이후 소스 gap; 입력 지연을 지정해도 보정 모드/추가 보고 없이 기존 배치·WAV 좌표 유지 |
| 녹화 탭 | import가 있어도 출력 없음, 끝 4037에 배치 |
| 정지·길이 | P의 초기 공백 → 뒤의 오디오 → 끝 이후 무음, Stop까지 계속 녹화; 기본 시작/세션 Stop, 파일 마무리 전 커서 갱신 |
| 무장·종료 | mic06/mic08 동시 기록, sparse stereo 채널 보존, 마이크 없는 카메라 녹화, 준비 실패 후 재시도, 다음 콜백 없는 armed 종료 |
| 실패·복구 | ASIO 리셋, 지연 변경, 의도적 render worker 정지로 실제 큐 언더런. partial normal 테이크와 WAV prefix, 잠금 해제, 재스캔의 P/길이 보존 |
| 시작 경계 | native 샘플/QPC는 연속인 클록 관측 세대 변경에서도 O0를 유지하고 계속 녹화 |

실장치 확인 절차

1. 새 테스트 프로젝트를 저장하고 ASIO 출력 쌍/헤드폰과 무장 마이크를 확인한다. 카메라는 가능하면 native 1080p60으로 설정한다. 일치하는 기존 보정 프로필을 선택한다. 드라이버·버퍼·입출력 채널이나 카메라 모드를 바꿨다면 프로필 전체 키가 달라지는 점을 확인한다.
2. 60초 이상의 완성 오디오를 불러온다. 타임라인 15초에 재생헤드를 놓고 **일반 녹화**를 시작한다. 15초부터 들리는지, 두 카메라 라이브 화면과 마이크 기록이 계속되는지 확인한다. 입력 모니터링을 켜고 꺼서 기존처럼 섞이는지도 확인한다.
3. 기존 마이크 클립/두 번째 import를 추가하고 각각 mute/solo를 바꿔 다시 시작한다. 선택한 믹스가 들리는지 확인한다. 시작 P를 오디오 앞의 공백에도 놓아 공백 뒤에 소리가 들어오는지 확인한다.
4. 오디오 끝보다 5초 이상 더 녹화한다. 끝에서 자동 정지되지 않고 무음이 되는지, Stop 후 타임라인 출력이 멈추는지 확인한다. 새 클립은 P부터이고 마지막 커서는 P+녹화 길이여야 한다. 실행취소 한 번으로 대상 트랙의 덮어쓰기가 복원되고 import는 그대로인지 확인한다.
5. 같은 프로젝트에서 녹화 탭으로 바꿔 녹화한다. 타임라인 소리 없이 끝에 추가되는지 확인한다. 타임라인에서 P를 모든 가청 오디오 뒤로 옮기거나 전부 mute한 경우도 기존 경로인지 확인한다.
6. **오디오 ±5ms 확인:** 1초 간격의 짧은 펄스가 든 오디오를 준비하고, 선택한 ASIO 출력을 별도 무장 입력으로 루프백해 60초 이상 녹화한다. 피드백을 막기 위해 그 루프백 입력의 모니터링은 끈다. 배치된 원본 mic WAV 펄스를 원래 타임라인 펄스와 비교한다. 48kHz에서 허용 차이는 **240샘플**이다. 초반/중간/끝의 오차를 각각 기록한다.
7. **영상 ±1프레임 확인:** 동일 출력 펄스로 동작하는 동기 LED 등, 소리와 발광의 관계를 알고 있는 신호를 두 카메라가 촬영하게 한다. 60fps에서 목표는 **16.7ms**다. MP4 실제 프레임과 타임라인 펄스의 첫/중간/마지막 위치를 비교한다. 사람의 박수 반응 시간만으로 보정 오차를 측정하지 않는다. 장시간 녹화를 쓰면 실제 작업 길이에서도 반복한다.
8. 테스트 프로젝트 복사본에서만 ASIO 중단/재연결을 한 번 재현한다. 오류 후 기존 원본이 남는지, 프로젝트 재열기/복구 후 P와 보존된 길이가 유지되는지 확인한다. 보고 자료는 해당 `media/takes/<take-id>/take.json`, 원본 WAV/MP4, journal과 보정 프로필이다.

남은 위험과 검증 한계

- 합성 샘플 정렬이 실제 DAC·ADC·FlexASIO·캡처보드 지연을 측정한 것은 아니다. 보고 지연의 정확도, 카메라 timestamp 기준, 노출/신호 처리 지연과 프로필 잔차에 따라 물리 오차가 달라진다. **실장치 ±5ms/±16.7ms 충족은 미확인**이다.
- 프로필이 없거나 전체 키가 다르면 장치 보고값과 잔차 0을 사용한다. 저장된 정렬 telemetry로 적용량을 확인할 수 있다. 카메라 30fps 입력의 시간 해상도/반복 프레임과 긴 녹화에서의 독립 장치 클록 드리프트는 실측이 필요하다.
- UI 길이는 입력 지연을 반영한 확인 완료 구간이므로, 현재 들리는 시점보다 입력 지연/콜백 표시 갱신만큼 뒤에 보일 수 있다. 클립 배치와 저장된 샘플의 동기화는 이 표시 지연과 별개다. UI 코드는 바꾸지 않았다.
- 소스 준비 실패는 소리 없이 녹화를 몰래 계속하는 대신 기존 partialFailure 경로로 반환한다. 실제 긴 import의 캐시 재생성 시간, 저장 장치 부하, NVENC 부하 아래에서의 언더런 빈도는 이번 합성 테스트로 단정하지 않는다.
- 새 실패 테스트의 카메라 파일은 명시적인 lifecycle double이다. WAV와 저널/복구는 제품 경로이며, 실제 녹화 MP4의 화질·프레임 선택·전원 차단 복구는 실장치에서 확인해야 한다.

`RecorderSession::addMarker` 본문이 HEAD와 동일함을 별도로 비교했다. `ui/*`, ProductIdentity/버전/릴리스 노트/site는 수정하지 않았다. 커밋·push·release.py 실행과 프로세스 감시/kill 스크립트 작성은 하지 않았다. 커밋은 사용자가 한다.
