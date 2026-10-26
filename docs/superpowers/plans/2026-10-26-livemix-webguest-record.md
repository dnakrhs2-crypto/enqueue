# LiveMix 0.17.0 — 합방 웹 참여(최대 3명) + 녹음 · 구현 계획

> **실행 방식:** 아스트라(codex) 회차 구현 + 클로드가 회차 사이 빌드·시험·검토(LiveMix 상시 규칙). 각 회차는
> 설계서의 "결정"과 아래 "설계 확정"·"전역 제약"을 전부 지킨다. 단계 표시는 `- [ ]`.

**설계서(대표 결정 — 바꾸지 말 것):** `docs/superpowers/specs/2026-10-26-livemix-webguest-record.md`
**기술 설계(아스트라 자문, 근거·파일:줄):** `C:\Users\claude\tools\claude_harness\lm_collab\webrec_design_OUT.md` — 이 계획과
다른 곳은 **이 계획이 우선**(아래 '설계 확정'에서 바꾼 곳).
**작업 폴더:** `C:\Users\claude\gocue-webrec`(브랜치 `livemix-webguest-record`, main 769d1b7d = LiveMix 0.16.0).

## 설계 확정 (아스트라 기술 설계 + 클로드 조정, 2026-10-26)

1. **구조(아스트라 설계 그대로):** 네트워크 연결(PeerLink)과 발화자 스트림(RemoteStream)을 분리. 방장 UDP 소켓 하나에 최대 두 링크.
   전달 패킷은 방장 재생 버퍼 **전에** 원래 `firstFrame/seq/epoch`를 유지해 분기. 사람마다 `CollabPlayout` 하나. 패킷 v2 = v1 36바이트
   공통 영역 + 12바이트(48바이트), v1 형식·검사는 그대로 보존하고 capability 협상 뒤 v2로 전환. 구버전이 하나라도 있으면 2명 호환 모드.
   방 서버 모델 `members/reservations/pendingWebRequests/inviteGeneration/events/revision`, 수락 시점에 자리 원자 예약.
2. **OBS:** 자리 1 = 기존 소스 ID `livemix_peer_source` + 링 `Local\LiveMix.ObsAudio.v1.Peer`(그대로), 자리 2 = `livemix_peer2_source` +
   `...Peer2`. 메뉴 이름 'LiveMix 합방 상대 1/2'. 닉네임은 별도 메타데이터 공유 메모리(`…Peer.Metadata`/`…Peer2.Metadata`)로 보내
   OBS 소스 속성에 '현재 참가자', 사용자 소스 이름은 안 바꿈. 플러그인 버전 0.17.0.
3. **웹 연결(아스트라 설계 그대로):** libdatachannel v0.24.6 + Mbed TLS 3.6.7 정적(`/MT`), `NO_MEDIA/NO_WEBSOCKET/NO_EXAMPLES/NO_TESTS`,
   `USE_MBEDTLS=ON`, 하위 libjuice b89c792·usrsctp fec583d·plog 94899e0 소스를 `third_party`에(원본 묶음은 클로드가 받아 둠 —
   `C:\Users\claude\AppData\Local\Temp\claude\C--Users-claude--local-bin\b3b9a98c-c3de-4465-9066-c9e910ec58a6\scratchpad\dl_ldc\`,
   Mbed TLS 해시 a7e8bcbe… 공식 일치). libdatachannel은 `if(NOT TARGET MbedTLS::MbedTLS)`일 때만 find_package → 미리 INTERFACE 대상을
   만들어 연결. 데이터 채널 `ordered:false, maxRetransmits:0`(실제 협상값 확인), TURN 없음.
   패킷: 웹→방장 mono PCM16 120프레임(48+240B), 방장→웹 stereo PCM24 120프레임(48+720B). 웹→방장→다른 LiveMix는 같은 mono16을 원
   발화자 ID로 전달. 방장의 웹용 믹스 = 내 마스터 + 다른 LiveMix 참가자 재생 결과(헤드폰 음량 전).
4. **★클로드 조정 — 웹 페이지 위치(gom 원문 15):** 페이지는 **곰튀김.com `/livemix/join/`** = 저장소 `site/livemix/join/`
   (GitHub Pages, 앱과 같은 배포). 아스트라 설계의 유튜브다운 정적 경로·COOP/COEP·SharedArrayBuffer는 **쓰지 않는다**(GitHub Pages는
   헤더를 못 정함). 대신: AudioWorklet ↔ Worker는 `MessageChannel` 포트를 넘겨 직접 연결(메인 스레드를 거치지 않음), 데이터 채널은 Worker로
   넘길 수 있으면 넘기고 아니면 메인 스레드 중계. 시그널링 API(유튜브다운.com)는 **`Content-Type: text/plain`의 POST**(사전 요청 없는 단순
   요청)로 부르고, 서버는 `Origin`이 `https://xn--jb0byyo90f.com`일 때만 `Access-Control-Allow-Origin`을 붙인다(→ `web_server.py`
   변경 불필요). 웹 요청은 IPv6여도 막지 않는다(네이티브 주소 교환 규칙과 분리).
5. **★클로드 조정 — 녹음 사람별 파일(gom 원문 16):** 합방 중 녹음 시작이면 폴더 하나(`LiveMix 녹음\2026-10-26_14-05-33\`)에
   `A_<내 닉네임>`, `B_<합방 상대 1 닉네임>`, `C_<합방 상대 2 닉네임>`, `D_전체` (자리가 비었으면 `B_합방상대1`처럼), 확장자 .wav/.mp3.
   2명이면 A·B·D. 합방이 아니면 `LiveMix 녹음\LiveMix_2026-10-26_14-05-33.wav` 파일 하나. 모든 파일 같은 시작 샘플·같은 길이,
   D = A+B+C(1배 합, 정수 변환 때 포화 + 초과 수 기록). 녹음 중 합방이 새로 시작되면 다음 녹음부터 사람별.
   **(10/26 R0 검토 뒤 클로드 확정 — 계약 §7·§11)** 합방 중 시작이면 A·B·C·D를 모두 열고(빈 자리는 무음) 정상 멈춤 때 소리가
   없었던 자리 파일은 지운다(→ 2명이면 A·B·D, 녹음 중 들어온 세 번째 사람도 C 파일), 시작 때 빈 자리였던 파일은 처음 들어온 사람
   닉네임으로 이름을 바꾼다. 모든 파일 스테레오(웹은 좌우 같은 소리 — 자리 주인이 바뀌어도 파일을 안 나눔). 합방 아닌 파일 하나는
   내 마스터 + 상대 소리 합(녹음 중 합방이 시작돼도 합방 소리가 빠지지 않게).
   `Recorder::push(master, remote1, remote2, n)` — 상대 재생기는 한 콜백에 한 번만 pull(`CollabAudio::renderSources`).
6. **★클로드 결정 — 강제 종료 시 보존 범위(아스트라 질문 가):** "마지막 체크포인트(약 1초)까지 재생 가능"(종료 직전 메모리에 있던 소리까지는
   약속 안 함). WAV는 쓰기 스레드에서 약 1초마다 `writer->flush()` + 파일 flush, 4GiB−1MiB 전에 다음 파일(`…_2`)로 프레임 경계 분할.
7. **녹음 엔진(아스트라 설계 그대로):** `livemix/src/recording/`(Recorder·RecordingQueue·WavSink·MfMp3Sink·RecordingRecovery), 전용 쓰기
   스레드, 오디오 콜백은 선할당 FIFO 복사·atomic만, MP3 = `CLSID_MP3ACMCodecWrapper` IMFTransform 직접(출력 먼저, 16비트 입력, TPDF
   디더, 32/44.1/48 외는 `lm_asrc`로 48k, drain), MF는 MP3 시작 때만 확인(없으면 WAV는 계속). 저장 실패·FIFO 초과 = 방송은 계속·녹음만
   오류 종료(조용히 버리고 정상 표시 금지). 장치 샘플레이트가 바뀌면 파일 묶음 전체를 닫고 새 묶음으로 이어서.
8. **방 서버 작업본은 작업 폴더 안 `server/ytdown/`**(R0에서 운영 원본 `C:\Users\claude\Desktop\유튜브다운\livemix_collab.py`·
   `test_livemix_collab.py`를 복사하고 원본 SHA-256을 `server/ytdown/ORIGIN.txt`에). 아스트라는 작업본만 고친다. 운영 반영은 클로드.
9. 그 밖의 미정 사항은 아스트라 설계 '(나)' 표의 권장값 그대로(참가자 ID·자리 재사용·구버전 늦은 입장 거절·초대 재발급·대기 요청 만료
   60초·예약 15초·합산 포화·파일 충돌 번호·세션 바꿔도 녹음 계속).

## 전역 제약 (모든 회차)
- **새로 넣는 오디오 경로(녹음·합방·웹 믹스)는 오디오 콜백 안에서 할당·잠금·블로킹·파일·네트워크·MF 호출 금지.** 기존 엔진의 비차단
  `ScopedTryLock`은 그대로 둔다. 그래프 try-lock 실패 경로에서도 내 소리는 무음·상대 소리는 유지하며 녹음·웹 믹스 샘플 수는 진행.
- 2명 합방(0.15.x/0.16.x와의 연결 포함) 평소 동작 회귀 금지. 기존 시험 기대값을 바꿔야 하면 이유를 보고.
- 화면 글자는 설계서·시안(`C:\Users\claude\tools\claude_harness\lm_webrec\mock\`)에 있는 말과 작동에 꼭 필요한 글자(입력칸·버튼·오류)만.
- **빌드·시험은 `python C:\Users\claude\tools\build_server\rb.py C:\Users\claude\gocue-webrec -- <명령>`** (작업 폴더 스냅샷을 빌드 서버에서,
  CMake 프리셋 `local`, 빌드 폴더 `build\vs2022`, 시험 실행 파일 `EnqueueTests`, 합방 시험 필터 `LiveMix Collab`). 옵션은 작업 폴더 앞에
  (`rb.py --fetch <경로> <폴더> -- …`), 명령 안에 `exit` 금지. 이 PC에서 직접 cmake/ctest 금지.
- 시험은 원래 요구와 사람이 평소 쓰는 흐름 위주. 1초 안쪽 우연 겹침·이론상 경로 대비 코드·시험은 만들지 않는다.
- 커밋은 회차마다 작은 단위로(메시지 영어 한 줄 + 본문), 클로드가 검토 뒤 합친다.

## 회차 (아스트라 설계의 R0~R13, 세션 수는 아스트라 추정)
| 회차 | 목적·소유 | 선행·동시 | 세션 |
|---|---|---|---:|
| R0 | 계약 고정: `docs/superpowers/specs/2026-10-26-livemix-webguest-record-contracts.md`(패킷 v2·capability·roster·시그널링 JSON·Snapshot·Recorder 인터페이스·OBS 메타데이터·파일 소유표), 공통 타입 헤더, 묶음별 CMake·시험 등록 조각, 서버 작업본 반입·원본 해시, 기존 합방 시험 기준 수집 | 단독 | 1 |
| R1 | libdatachannel·Mbed TLS `third_party` + `cmake/LiveMixWebRTC.cmake` + `WebGuestTransport.*` + 브라우저와 비신뢰성 채널 송수신 시험 | R0 뒤, R2·R5와 동시 | 3~4 |
| R2 | 방 서버(`server/ytdown/livemix_collab.py`·시험) + CORS(곰튀김 출처) + `CollabRoom.*` 확장 + `WebGuestSignaling.*` | R0 뒤, R1·R5와 동시 | 3~4 |
| R5 | `livemix/src/recording/*` + `livemix/tools/RecorderProbe.cpp` + 시험 | R0 뒤, R1·R2와 동시 | 3~4 |
| R3 | 네이티브 v2·별 연결: `CollabProtocol.*`·`CollabTransport.*`·roster | R2 연동 뒤, R6·R7과 동시 | 3~4 |
| R6 | 웹 오디오 엔진 `site/livemix/join/` 의 audio·worker·worklet·packet 코드 + 시험 진입점 | R1/R2 연동, R3·R7과 동시 | 3~4 |
| R7 | OBS 두 자리·메타데이터 `obs-plugin/src/*`·locale·`lm_obs_metadata.h`·작성기 | R0 뒤, R3·R6과 동시 | 1~2 |
| R4 | 사람별 오디오 `CollabAudio.*`·`CollabPlayout.*`·`CollabSender.*` | R3 뒤 | 2~3 |
| R8 | 앱 통합 `MixEngine.*`·`CollabSession.*`·`Main.cpp`·`LiveMixSettings.*`·공통 CMake | R1~R7 뒤 단독 | 2~3 |
| R9 | 자동 종단 도구 `tools/lm_collab017/*`·`CollabProbe.cpp` 확장·파일 검사기 | R8 뒤, R10·R11과 동시 | 2~3 |
| R10 | 앱 화면(시안 확정됨): `MasterCard.*`(녹음 버튼 A안)·`CollabWindow.*`·`MainComponent.*`·새 녹음 창 | R8 뒤, R9·R11과 동시 | 2~3 |
| R11 | 웹 화면(시안 확정됨): `site/livemix/join/index.html`·CSS·접속 흐름 + **상대방 음량**(gom 10/26 원문 18, 시안 ③ 사람 목록 칸 아래, 기기에 기억) | R2/R6 뒤, R9·R10과 동시 | 1~2 |
| R12 | 최종 통합·실물 결과 수정 — 실물 시험 PC = **이 PC ↔ roomc**(gom 10/26 "다른 pc랑 연결테스트가 필요하면 roomc pc 쓰면됨": 강남 망, SSH `rbuild.py` 연결 그대로, 빌드 서버 작업과 겹치지 않게) + 이 PC·roomc 브라우저 | 단독 | 2~3 |
| R13 | 배포 준비: 버전 0.17.0(앱·OBS)·설치기·release 도구(OBS 빌드도 rb.py)·노트·서버 배포 명세 | 단독, 클로드 공개 배포 | 1 |

검토: 묶음마다 회차 실행기 검토 1바퀴, 필요하면 수정 확인 2바퀴째(상한 2바퀴). 마지막 검토 뒤 고친 것은 배포 전 아스트라 검증.
