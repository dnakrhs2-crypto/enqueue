# LiveMix 0.15.0 — 2명 오디오 합방 구현 계획

> **실행 방식:** 아스트라(codex) 회차 구현 + 클로드가 회차 사이 빌드·시험·검토(LiveMix 상시 규칙). 각 회차는 아래
> "결정"과 "전역 제약"을 전부 지킨다. 단계 표시는 `- [ ]`.

**목표:** 서로 다른 집의 LiveMix 사용자 2명이 방 코드로 연결해, 각자의 마스터 소리(스테레오)를 최대한 짧은 지연으로
주고받는다. 상대 소리는 내 헤드폰(마스터 출력 쌍)과 OBS의 별도 소스 'LiveMix 합방 상대'로 들어가고, 내 OBS 'LiveMix'
소스는 계속 내 소리만 담는다. 방장 PC의 공유기 포트가 바깥에서 안 열리면 방 코드 대신 그 사실과 원인을 알린다.

**구조:** 소리는 두 PC가 UDP로 직접 주고받는다(48 kHz·스테레오·PCM 24비트·2.5 ms 패킷). 방장 PC가 UDP 포트를 열고
(UPnP 자동 + 수동 포워딩도 인정), 유튜브다운.com 서버가 방 코드 ↔ 방장 주소를 알려 주고, 방을 만들 때 서버가 바깥에서
그 포트로 시험 신호를 보내 실제로 들어오는지 확인한다. 받는 쪽은 사람별 재생 버퍼(지터 흡수) + 기존 `lm_asrc`/`lm_drift`
(장치 샘플레이트 차이 보정)로 내 장치 속도에 맞춘다.

**기술:** C++17 + JUCE 8.0.15(LiveMix), Winsock UDP, miniupnpc 2.3.3(BSD-3, `third_party/miniupnpc`에 넣어 둠),
C17 + libobs 31.1.1(OBS 플러그인), Python 3.12(유튜브다운 서버 모듈).

**근거 자료:** 타당성 검토 `C:\Users\claude\tools\claude_harness\codex_reviews\lm_collab_audio_consult_OUT.md`,
메모리 `project_livemix_collab_audio_feasibility.md`.

**작업 폴더:** `C:\Users\claude\gocue-collab`(브랜치 `livemix-collab`, main 582bec6 = LiveMix 0.14.1에서 분기).
LiveMix 빌드 = 회차 실행기(`dev_cycle.py`, 프리셋 `local`, `build\vs2022`). 플러그인 빌드 =
`obs-plugin\` 에서 `cmake --build --preset windows-x64`(구성·`.deps`는 클로드가 해 둠 — **샌드박스엔 네트워크가 없으니
구성(configure)은 다시 돌리지 말 것**).

---

## 결정 (gom 승인 2026-10-15 — 바꾸지 말 것)

1. **2명만.** 방장 1 + 참가자 1. 3명 이상·압축 코덱·둘 다 포트가 막혔을 때의 연결·외부 중계 서버는 이번 범위 밖.
2. **방장 = 포트가 바깥에서 열린 쪽.** 방을 만들 때 확인하고, 안 열리면 방 코드 대신
   "이 PC는 공유기 포트가 열리지 않아 방장을 할 수 없습니다. 상대방이 방을 만들어 주세요." + 원인 한 줄.
   참가자는 포트가 안 열려도 된다(참가자가 먼저 보내므로 응답이 돌아온다).
3. **OBS 사람별 소스:** 기존 'LiveMix'(내 소리) + 새 'LiveMix 합방 상대'(상대 소리). 헤드폰 음량은 LiveMix, 방송 음량은
   OBS에서 따로.
4. **소리는 우리 서버를 지나가지 않는다.** 서버는 방 코드·주소 교환·시험 신호만.
5. 실측(두 집 실제 회선)은 gom이 나중에. 이번엔 개발 → 검토 → 배포.

## 전역 제약

- **오디오 콜백 규칙:** 할당·잠금 대기·소켓 호출·로그 금지. 콜백은 SPSC 큐에 복사 + Win32 이벤트 `SetEvent`만.
  모든 버퍼·ASRC·큐는 연결 준비(메시지 스레드)에서 미리 할당.
- **전송 형식(고정):** 48000 Hz, 2채널, PCM 24비트 리틀엔디언 교차(interleaved), 패킷당 120프레임(2.5 ms),
  UDP 페이로드 최대 1200바이트. 장치가 48 kHz가 아니면 **보내는 쪽**이 `lm_asrc` 고정 비율로 48 kHz로 바꾸고,
  **받는 쪽**이 48 kHz → 내 장치 속도로 바꾸면서 `lm_drift`로 샘플레이트 차이를 보정한다
  (`lm_asrc`는 내림 변환 때 차단 주파수를 `0.45*min(1,fs_out/fs_in)`로 낮춘다 — `livemix/shared/lm_asrc.c:58`).
- **패킷 머리(36바이트, 리틀엔디언 — 서버 파이썬도 같은 배치로 만든다):**
  `u32 magic=0x42434D4C`(바이트 4C 4D 43 42 = "LMCB") · `u8 version=1` · `u8 kind` · `u16 flags=0` ·
  `u64 session` · `u32 sender` · `u32 seq` · `u64 firstFrame` · `u16 frames` · `u16 payloadBytes`.
  kind: 1 audio · 2 hello · 3 welcome · 4 ping · 5 pong · 6 bye · 7 probe.
  probe = 서버가 보내는 시험 신호: session·sender·seq·firstFrame·frames = 0, payloadBytes = 16, 페이로드 = 16바이트 nonce.
  파이썬: `struct.pack('<IBBHQIIQHH', 0x42434D4C, 1, 7, 0, 0, 0, 0, 0, 0, 16) + nonce`.
- **포트:** 방장은 UDP `collabPort`(설정, 기본 49731)에 IPv4로 묶는다. 참가자는 임시 포트(0).
- **서버 주소:** 기본 `https://xn--2j1bo5s81gjmag88b.com`(유튜브다운.com). 시험용 환경 변수
  `LIVEMIX_COLLAB_SERVER`(예: `http://127.0.0.1:8899`)로 바꿀 수 있다. `LIVEMIX_COLLAB_NO_UPNP=1`이면 UPnP를 건너뛴다(시험용).
- **믹스마이너스:** 받은 상대 소리는 절대 다시 보내지 않는다. 보내는 소리 = 마스터 체인 뒤(현재 `obsSender.write`와 같은
  지점 `MixEngine.cpp` 772), 상대 소리를 더하기 전.
- **기존 OBS 'LiveMix' 링(`Local\LiveMix.ObsAudio.v1`)은 계속 내 소리만.** 상대 소리는 새 링
  `Local\LiveMix.ObsAudio.v1.Peer`(읽는 쪽 표시 `...Peer.Readers` — `ObsSender(name)`·플러그인 `ring_name`의 기본 규칙)에
  **헤드폰 음량·ON/OFF 적용 전** 값으로 쓴다. 상대가 없을 땐 무음을 쓴다(시간축 유지). 이 링은 마스터의 'OBS로 보내기'가
  켜져 있을 때 같이 켜진다.
- **헤드폰:** 상대 소리 × 헤드폰 음량(0~100%, 기본 100%) × ON/OFF(기본 ON, 5 ms 램프)를 마스터 출력 쌍
  (`master.outputFirst`)에 더한다. 분리 모니터(입력·출력 다른 장치)면 모니터 단계에 더한다. 그래프 잠금 실패 블록에서도
  상대 소리와 합방 시간축은 끊기지 않는다(공통 마무리 단계).
- **에코 제거·자동 음량·잡음 제거 없음**(헤드폰 전제, 스테레오 보존).
- **설정(앱 단위, 세션 파일에 넣지 않음):** `collabNick`(문자열), `collabPort`(정수 49731), `collabMonitorVolume`(0~1, 1),
  `collabMonitorOn`(참). 세션·프리셋을 열어도 합방이 저절로 시작되지 않는다.
- **화면 글자:** gom 문구 + 작동에 꼭 필요한 글자(입력칸·버튼·상태·오류)만. 설명·안내 문장을 지어내지 않는다.
- **보안 장치는 넣지 않는다**(내부 규칙: 공격자 시나리오 제외). 단 다른 세션 번호(session)의 패킷은 무시한다(엉뚱한 소리 섞임 방지).
- **시험 전용 스위치:** `LIVEMIX_ALLOW_MULTI=1`이면 LiveMix를 여러 개 띄울 수 있다(같은 PC 두 대 시험용, 평소엔 지금처럼 하나).
- 코드 주석·식별자는 영어(주변 코드와 같게), 화면 글자는 한국어(`ko()` 매크로 규칙 그대로).
- 버전: LiveMix **0.15.0**, OBS 플러그인 **0.15.0**(설치기·자동 설치가 새 DLL을 깔게).

## 검토 초점 (시험이 직접 안 덮는, 실제 사용에서 걸릴 만한 것)

1. 와이파이처럼 도착 간격이 들쭉날쭉할 때: 처음 버퍼가 너무 작게 잡혀 계속 끊기는지 → 끊김이 나면 버퍼가 빨리 늘고, 안정되면 천천히 줄어야 한다.
2. 몇 시간 연결: 두 장치 샘플레이트 차이로 버퍼가 한쪽으로 쌓이거나 비지 않아야 한다(±1000 ppm까지).
3. 방장이 창을 닫거나 앱을 끔 / 참가자가 나감: 상대 쪽은 '연결 끊김'을 보여 주고 소리는 페이드로 끝나며, 다시 들어오면 이어진다.
4. 세션을 바꾸거나 장치를 바꿔도(샘플레이트 변경 포함) 합방이 이어지거나, 끊기면 끊겼다고 보인다 — 조용히 무음이면 안 된다.
5. 같은 집(같은 공유기)의 두 PC: 공인 주소가 같으면 LAN 주소로 붙는다.

---

## 파일 구성

| 파일 | 책임 | 회차 |
|---|---|---|
| `livemix/src/CollabProtocol.h/.cpp` | 패킷 머리 인코드/디코드, PCM24 팩/언팩, 제어 페이로드(hello/welcome/ping/pong/bye) | R1 |
| `livemix/src/CollabPlayout.h/.cpp` | 받는 쪽: 프레임 번호 기준 재생 버퍼(순서 바뀜·중복·늦음·손실), 손실 은닉, 지터 목표(처음 측정·끊김 시 증가·안정 시 감소), `lm_asrc`+`lm_drift`로 장치 속도 출력, 통계 | R1 |
| `livemix/src/CollabSender.h/.cpp` | 보내는 쪽: 콜백 → SPSC → 송신 스레드, 장치 속도 → 48 kHz 고정 변환, 120프레임 패킷화 | R1 |
| `livemix/src/CollabTransport.h/.cpp` | UDP 소켓·수신/송신 스레드, 역할(방장/참가자), hello/welcome/bye, 후보 주소 여러 개 시도, ping RTT, 시험 신호(probe) 수신 확인, 상태·통계 | R1 |
| `livemix/src/CollabAudio.h/.cpp` | 엔진 쪽 창구: `pushLocal`/`pushLocalSilence`, `renderRemote`(헤드폰 합산 + 상대 OBS 링), 헤드폰 음량·ON/OFF, 합방 시간축 | R1 |
| `livemix/src/MixEngine.*` 수정 | 위 창구 연결, 공통 마무리 단계 | R1 |
| `livemix/src/Main.cpp` 수정 | `LIVEMIX_ALLOW_MULTI` | R1 |
| `tests/LiveMixCollab*Tests.cpp` | R1 시험 | R1 |
| `obs-plugin/src/*` | 연결 2개(마스터·상대), 소스 `livemix_peer_source` 'LiveMix 합방 상대', 버전 0.15.0 | R1c |
| `C:\Users\claude\Desktop\유튜브다운\livemix_collab.py` + `test_livemix_collab.py` | 방 서버(메모리 방, 4자리 코드, 시험 신호 UDP 발송) | R2a |
| `tools\claude_harness\lm_collab\patch_ytdown_lmcollab.py` | web_server.py 연결 패치(클로드가 적용·재시작) | R2a |
| `livemix/src/CollabRoom.h/.cpp` | 방 서버 HTTP 클라이언트 | R2b |
| `livemix/src/CollabUpnp.h/.cpp` | miniupnpc 감싸기(찾기·외부 IP·매핑·해제), 시험용 인터페이스 | R2b |
| `livemix/src/CollabHostCheck.h/.cpp` | 방장 가능 판정·원인 분류(순수 함수) + 문구 | R2b |
| `livemix/src/CollabSession.h/.cpp` | 메시지 스레드 조율(방 만들기·참가·나가기·재연결·상태) | R2b |
| `CMakeLists.txt`, `tests/CMakeLists.txt` | 새 파일·miniupnpc 정적 라이브러리 | R1·R2b |
| `livemix/src/ui/CollabWindow.*`, `TopBar.*`, `MainComponent.*`, `LiveMixSettings.*` | 합방 창(시안 확정 뒤), 탑바 버튼, 설정 | R3 |
| `installer/LiveMix.iss`, `site/livemix/`, `docs/release-notes/livemix/0.15.0.html` | 배포물 | R3 |

---

## 회차

### R1 — LiveMix 핵심(소리 주고받기·엔진 연결) [아스트라]
- [ ] 패킷 형식(위 머리) 인코드/디코드 + 시험(왕복, 잘못된 magic·길이·version 거부)
- [ ] 재생 버퍼: 프레임 번호로 자리 찾기, 순서 바뀜·중복·늦은 패킷(재생 지점 지난 것) 버림, 손실 은닉(짧은 페이드 — 클릭 없음),
      처음 1초 도착 간격으로 목표 정함(최소 5 ms·최대 80 ms), 끊김마다 +2.5 ms(최대 120 ms), 30초 안정 시 −1.25 ms(측정 바닥까지),
      `rebuffer()`로 다시 측정 + 시험
- [ ] 샘플레이트 차이: ±50/150/500/1000 ppm에서 모의 1시간 동안 채움이 목표 ±2 ms 안 + 시험
- [ ] 속도 변환: 44.1 kHz 장치 → 전송 48 kHz → 96 kHz 장치에서 1 kHz 사인 주파수 유지·접힘 없음 + 시험
- [ ] 스테레오 보존(L만 펄스 → L만) + 시험
- [ ] 송신 스레드: 콜백 SPSC + `SetEvent`, 120프레임 패킷, 그래프 잠금 실패 블록은 같은 길이 무음 + 시험
- [ ] 전송: 127.0.0.1 실제 UDP로 방장/참가자 hello→welcome, 다른 session 무시, ping RTT, bye, 후보 여러 개 중 응답 온 주소 채택,
      시험 신호 nonce 수신 확인 + 시험
- [ ] 엔진: 공통 마무리 단계(잠금 실패 블록 포함), 헤드폰 합산(음량·ON/OFF 램프), 상대 OBS 링(음량 적용 전), 기존 'LiveMix' 링은
      내 소리만, 합방 시간축이 매 콜백 진행 + 시험(오프라인 renderBlock 두 엔진을 가짜 전송으로 이어 믹스마이너스 확인)
- [ ] `LIVEMIX_ALLOW_MULTI`
- [ ] 전체 시험 통과, 커밋

### R1c — OBS 플러그인 상대 소스 [아스트라, R1과 동시]
- [ ] `plugin-main.c`: 연결 2개(마스터 = 지금 그대로·`LIVEMIX_OBS_RING` 시험 변수 유지, 상대 = `Local\LiveMix.ObsAudio.v1.Peer`,
      시험 변수 `LIVEMIX_OBS_PEER_RING`)
- [ ] 소스 `livemix_peer_source`, 이름 `LiveMix 합방 상대`(ko-KR) / `LiveMix Collab Partner`(en-US),
      상태 `연결됨` / `합방 상대 기다리는 중`. 마스터 소스·필터 동작 불변
- [ ] 버전 0.15.0, 핵심 시험(`livemix-obs-core-tests`) 통과, 빌드 통과

### R2a — 방 서버 [아스트라, R1과 동시]
- [ ] `livemix_collab.py`: `POST /api/livemix/collab/{create,confirm,heartbeat,join,close}` (아래 R2a 지시문의 규약)
- [ ] 시험 신호: 방 만들 때 방장 공인 IPv4:포트로 probe 3번(0·150·300 ms) — 요청 처리를 막지 않는 스레드
- [ ] 공인 주소 = `X-Forwarded-For` 첫 값, 없으면 연결 주소
- [ ] `test_livemix_collab.py`(가짜 요청 + 127.0.0.1 UDP 수신으로 probe 바이트 확인)
- [ ] 단독 실행 `python livemix_collab.py --serve 127.0.0.1:8899`(클로드의 로컬 종단 시험용)
- [ ] web_server.py 패치 스크립트(기준 문자열 count==1, 적용은 클로드)

### R2b — 방 만들기·참가 흐름 [아스트라, R1 뒤]
- [ ] miniupnpc 정적 라이브러리 CMake 연결, `CollabUpnp`(찾기 2초 제한, 외부 IP, UDP 매핑 설명 'LiveMix', 30분마다 갱신, 나갈 때 해제)
- [ ] `CollabHostCheck`: (UPnP 찾음·매핑 성공·외부 IP, 서버가 본 공인 IP, 시험 신호 받음) → 가능/원인 3가지 + 문구 + 시험
- [ ] `CollabRoom`: HTTP JSON(백그라운드 스레드), 오류 → 사용자 문구
- [ ] `CollabSession`: 방 만들기(포트 묶기 → UPnP → create → probe 대기 2.5초 → confirm) / 참가(join → 후보로 hello) /
      heartbeat 10초 / 나가기(bye·close·매핑 해제) / 연결 끊김 3초 → '다시 연결하는 중', 참가자는 계속 hello / 상태 알림 + 시험(가짜 방·UPnP)

### R3 — 화면·배포물 [아스트라, 시안 확정 뒤]
- [ ] 합방 창(gom이 고른 시안), 탑바 [합방] 버튼, 설정 저장, 'OBS로 보내기'와 상대 링 연동
- [ ] 설치기·사이트·노트 0.15.0, 플러그인 0.15.0 동봉

### 검토·검증 [클로드 + 아스트라]
- 아스트라 검토 최대 2바퀴(`--review-resume-key lm_collab_review`), 마지막 검토 뒤 수정분은 `--extra-round "gom 승인: 10/15 수정분 배포 전 검증"`
- 로컬 종단: 서버 단독 실행 + LiveMix 두 개(숨은 바탕화면·임시 프로필·`LIVEMIX_ALLOW_MULTI`·`LIVEMIX_COLLAB_SERVER`·`LIVEMIX_COLLAB_NO_UPNP`)
  → 방 만들기·참가·연결·통계·나가기, 지연·손실 주입(UDP 중계 도구), 장시간
- Vercel 디자인 검사(합방 창)
- 유튜브다운 서버 반영(백업·패치·Task Scheduler 재시작·전후 비교), LiveMix 0.15.0 배포
