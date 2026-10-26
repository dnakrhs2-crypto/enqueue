# LiveMix 0.17.0 R0 — 구현 계약

기준: 브랜치 `livemix-webguest-record`, 시작 HEAD `a03d904b77706efe3daf0da05dd2f8b3e9aca77f`.
설계서 [대표 결정](2026-10-26-livemix-webguest-record.md)과
[구현 계획의 설계 확정](../plans/2026-10-26-livemix-webguest-record.md)이 우선한다.
아스트라 기술 설계 `C:/Users/claude/tools/claude_harness/lm_collab/webrec_design_OUT.md`는 근거이며,
그 안의 유튜브다운 정적 페이지·COOP/COEP·SharedArrayBuffer·단일 합본 녹음 제안은 적용하지 않는다.
화면 기준은 `C:/Users/claude/tools/claude_harness/lm_webrec/mock/`의 네 HTML/PNG와 `webrec.css`다.
시안의 옛 URL만 최종 대표 결정의 곰튀김.com으로 대체한다. R0는 기능 구현이나 버전 상승을 하지 않는다.

이 문서의 수치·식별자·함수 모양은 후속 회차의 연결 기준이다. 계획에 없던 세부 선택은
끝의 **클로드 확인 필요**에 모았다. 변경이 필요하면 자기 회차에서 상대 회차 파일까지 고치지 말고
계약 변경을 먼저 인계한다. 경로는 저장소 루트 기준이며 `{h,cpp}`는 두 파일 모두를 뜻한다.

## 1. 정체성, 방 모드, 자리

- 방 최대 3명(방장 포함), 웹 최대 1명, 방장은 LiveMix. 각 참가자는 방장과만 연결한다.
- `session`: 방마다 서버가 발급하는 64비트 값. JSON은 소문자 16자리 hex 문자열, 패킷은 LE u64.
  JS는 `BigInt`/`DataView.getBigUint64`로 다룬다. JSON 숫자로 변환하지 않는다.
- `participantId`: 방 안 u32, 0은 없음, 방장은 1, 서버가 입장 예약 때 2부터 증가시킨다.
  방 수명 안에서 퇴장한 ID를 다른 사람에게 재사용하지 않는다. `inst`·닉네임·IP·자리 번호와 다르다.
- `connectionId`: 네이티브 create/join 호출자가 각 입장 시도마다 만든 128비트 난수의 소문자 32자리 hex.
  같은 요청 재시도에는 같은 값을 쓴다. 같은 설정 프로필의 여러 앱도 서로 다른 값을 쓴다.
  같은 ID의 재시도는 기존 예약을 반환하며, 퇴장 후 새 입장은 새 ID/participantId다.
- `connectionGeneration`: 서버가 승인한 연결의 u32 세대(0 금지). 새 연결마다 증가하며
  웹 HTTP 이벤트·SDP·ICE·C++ 콜백·Worker 메시지에 넣는다. 같은 세대에서 ICE 재협상/자동 재접속을 추가하지 않는다.
- `streamEpoch`: 발화자별 u32, 처음 1. 같은 사람의 장치 재시작/샘플 위치 초기화 때 증가한다.
  `seq/firstFrame`은 epoch 안의 값이다. 오디오 패킷만 보고 epoch를 채택하지 않고 방장의 roster로 확정한다.
- 서버 `revision`은 서버의 멤버·예약·모드 변경 번호, 패킷 `rosterRevision`은 방장이 배포한 재생 목록 번호다.
  둘은 별도 u32(처음 1)이며 서로 비교하지 않는다. 후자는 스트림 epoch/지연 정보 변경에도 증가한다.
  0은 미확정, u32 제어 세대는 방 안에서 되감지 않는다. 오디오 seq만 mod 2^32 비교한다.
- OBS 자리는 **각 LiveMix에서 자기 자신을 뺀 두 자리**다. 배열 첨자는 0/1, 화면·메타데이터 slot은 1/2.
  참가자의 방장은 처음 자리 1, 나머지는 빈자리 순이다. 다른 사람이 나가도 남은 사람을 옮기지 않는다.
  퇴장 자리는 무음, 재사용 때 그 자리의 이전 재생 큐·epoch·메타데이터만 교체한다.
- `roomMode`: JSON `"legacy2"` / `"v2"`, 바이너리 0 / 1. 구버전이 있으면 최대 2명, 웹 불가.
  새 호스트 혼자이면 v2 가능, 새 네이티브 둘이면 v2, 새 네이티브 하나+웹도 v2다.
  구버전이 3명/웹 방에 늦게 들어오면 거절한다. 기존 사람을 내보내거나 방을 자동 축소하지 않는다.
  2명 호환 방에서 구버전이 나가면 남은 새 방장은 다시 v2 초대를 만들 수 있다.

## 2. 패킷 계약

구현 소유 R3. 상수/논리 타입은 `livemix/src/CollabContracts.h`의
`gocue::livemix::collab::contracts`에 있다. 기존 `collab::Kind/Header`와 다른 이름공간이므로
R0 헤더를 넣는 것만으로 v1 동작이 바뀌지 않는다. 구조체 통째로 `memcpy`하지 않는다.

### 2.1 v2 머리와 오디오

모든 정수 LE, 한 UDP datagram/한 DataChannel binary message = 정확히 한 패킷.
전체 길이 `48 + payloadBytes`, 최대 1200바이트. 뒤에 패딩·다른 패킷을 붙이지 않는다.

| 오프셋 | 바이트 | 필드 | 규칙 |
|---:|---:|---|---|
| 0 | 4 | magic | `0x42434d4c`, 바이트 `4c 4d 43 42` (`LMCB`) |
| 4 | 1 | version | 2; v1은 여전히 1 |
| 5 | 1 | kind | 아래 표 |
| 6 | 2 | flags | 제어 0, stereo PCM24 1, mono PCM16 2 |
| 8 | 8 | session | 방 session |
| 16 | 4 | origin | v1 sender 자리. 원 발화자의 participantId |
| 20 | 4 | seq | 오디오 순번, 제어는 요청 번호/응답 echo |
| 24 | 8 | firstFrame | 원 발화자 방 샘플레이트 기준 시작 프레임 |
| 32 | 2 | frames | 오디오 120, 제어 0 |
| 34 | 2 | payloadBytes | 머리 제외 길이 |
| 36 | 4 | linkSender | 이 링크에서 실제 송신한 사람 |
| 40 | 4 | streamEpoch | 오디오는 원 발화자 epoch, 제어는 0(재시작 요청 제외) |
| 44 | 4 | rosterRevision | 이 수신자가 적용할 목록 번호 |

`flags`는 독립 on/off 비트의 합이 아니라 형식 값이다. 3 또는 알 수 없는 비트는 거절한다.
`flags=1`: signed stereo PCM24 LE, 프레임마다 L 3B/R 3B, 720B → 전체 768B.
`flags=2`: signed mono PCM16 LE, 프레임당 2B, 240B → 전체 288B.
웹 모노는 네이티브 재생/OBS에서 L=R로 확장하며 네이티브 간 전달 payload는 바꾸지 않는다.
방 샘플레이트는 44100 또는 48000, 방장 결정이며 패킷마다 실지 않는다.
오디오는 `firstFrame % 120 == 0`, seq/firstFrame은 송신자가 120프레임 생성할 때마다 진행한다.
큐 폐기로 패킷을 안 보냈어도 시계를 되감지 않는다. epoch 시작은 seq=0/firstFrame=0.
정수 변환은 기존 PCM24의 finite/clamp/round 규칙을 유지한다. PCM16은 같은 규칙에
배율 32768, 범위 [-32768,32767]; 디코드는 32768로 나눈다. 전송 PCM에 MP3용 디더를 넣지 않는다.

### 2.2 Kind, capability

| Kind 값 | 이름 | v2 payload / 사용 |
|---:|---|---|
| 1 | audio | 위 형식, 오디오 재전송 없음 |
| 2/3 | hello/welcome | **v1 전용**. v2에서 거절 |
| 4/5 | ping/pong | u64 단조시계 ns 8B, pong은 seq/payload echo |
| 6 | bye | 0B. 참가자는 자기 퇴장, 방장은 방 종료 |
| 7 | probe | **v1 전용**. 기존 nonce 16B 그대로 |
| 8 | punch | 0B. 네이티브 주소 열기; 웹에 보내지 않음 |
| 9/10 | offer/offerAck | 16B, 아래 협상 |
| 11/12 | roster/rosterAck | 전체 목록 / 빈 payload ACK |
| 13 | streamRestart | 12B: deviceRate u32, inputLatencyUs u32, outputLatencyUs u32 |

일반 제어는 flags=0, frames=0, firstFrame=0, origin=linkSender=제어 송신자,
streamEpoch=0. `streamRestart`만 머리에 **새** epoch를 넣는다.
`rosterAck`는 받은 roster의 seq와 rosterRevision을 echo한다. bye도 다른 사람의 ID로 보낼 수 없다.
스트림 재시작 요청은 roster에 채택될 때까지 재전송하며, 방장은 중복 요청에 같은 roster를 다시 보낸다.

| JSON capability 이름 | 비트 | 의미 |
|---|---:|---|
| `packet_v2` | 0 / `0x01` | 48B 머리·v2 codec |
| `roster_ack` | 1 / `0x02` | roster revision과 ACK |
| `star_3` | 2 / `0x04` | 세 사람 방·origin/linkSender 분리 |
| `pcm16_mono` | 3 / `0x08` | 웹 모노 패킷 수신/전달 |
| `web_guest` | 4 / `0x10` | 웹 참여/mix-minus |

0.17 정상 구현은 다섯 개(`0x1f`) 모두 광고한다. 네이티브 v2 필수 교집합은 `0x07`,
웹이 있는 방은 네이티브 전원과 웹 모두 `0x1f`가 필요하다. JSON은 이름 배열(중복 없음, 최대 5),
binary offer는 u32 mask다. 알 수 없는 이름/비트는 협상 교집합에서 제외한다.
`appVersion`은 표시 및 0.17.0 최소 버전 검사(숫자 major/minor/patch 비교), 기능 판정은 교집합이다.
버전 문자열만 보고 v2 패킷을 보내지 않는다.

### 2.3 v1 보존과 전환 순서

v1의 36B 머리, Kind 1..8, flags=0, 정확한 payload 길이 검사를 그대로 둔다.
hello/welcome payload는 `u8 nickBytes + UTF8 nick(0..64B) + u32 inputLatencyUs + u32 outputLatencyUs
+ u8 versionBytes + UTF8 appVersion(0..32B) + u32 deviceRate + u32 wireRate`(18..114B).
여기에 capability 등을 덧붙이지 않는다. 기존 probe는 session/sender/seq/firstFrame/frames=0,
payload 16B nonce다. v1 stereo PCM24는 36+720=756B 그대로다.

1. create/join 확장 응답에 v2 지원·participantId·연결 세대를 받는다. 새 서버 응답이 없거나
   상대/서버가 legacy이면 기존 v1 handshake/오디오 경로를 유지한다.
2. 양쪽 신형인 네이티브 링크도 먼저 **v1 hello → v1 welcome**을 교환한다.
   이때 양쪽 sender는 서버 할당 participantId를 사용한다. 순수 legacy/구버전 상대의
   임의 sender를 강제로 1 등으로 바꾸지 않고 기존 v1 바인딩을 보존한다.
3. 방장만 v2 offer를 시작한다. payload @0 capabilities u32(교집합), @4 wireRate u32,
   @8 peerParticipantId u32(수신자의 ID), @12 connectionGeneration u32.
   머리 origin/linkSender는 방장, seq는 협상 번호, rosterRevision=현재 방장 revision.
   이미 성공한 v1 주소 바인딩 및 서버 예약과 일치할 때만 이 offer를 읽는다.
4. 참가자는 offerAck로 seq·capabilities·wireRate·generation을 echo하고 payload의
   peerParticipantId는 방장 ID로 바꾼다. 머리 origin/linkSender는 자기 ID.
   offer가 거듭 와도 같은 ACK를 보낸다. ACK 전에는 기존 v1 재생을 유지한다.
5. 방장은 ACK 뒤 roster를 보낸다. 참가자는 roster 설치 후 ACK하고 v2 송신으로 전환한다.
   방장은 그 링크의 roster ACK 뒤 v2 오디오를 송신한다. 각 방향 최초 v2 활성화 뒤 v1 오디오는 폐기한다.
   v1/v2를 같은 재생기에 동시에 밀어 넣지 않는다. 협상 제어 중복은 여전히 응답한다.
6. offer/roster/streamRestart는 **200ms마다** 미확인 대상에만 재전송한다. 오디오는 재전송하지 않는다.
   최초 연결은 기존 `connectTimeoutMs=10000` 범위에서 끝낸다. 협상 실패 시 해당 새 링크를 실패 처리하며
   이미 연결된 다른 링크의 재생/펀칭은 계속한다.

웹은 HTTP 수락으로 v2/capability/ID가 확정되므로 v1 hello와 UDP offer 단계를 거치지 않는다.
DataChannel open 뒤 방장이 동일한 v2 roster를 보내고 웹이 ACK한 뒤 오디오를 시작한다.

### 2.4 roster 바이너리와 검증

roster는 방장만 송신하며, JSON 대신 아래 완전한 스냅샷을 한 패킷으로 보낸다.
앞 8B: wireRate u32 @0, memberCount u8 @4(1..3), roomMode u8 @5, reserved u16 @6(0).
이후 participantId 오름차순의 가변 길이 entry를 memberCount개 연속으로 둔다.

| entry 내 오프셋 | 형식 | 내용 |
|---:|---|---|
| 0/4/8 | u32 각 | participantId / streamEpoch / capabilities mask |
| 12/16/20 | u32 각 | deviceRate / inputLatencyUs / outputLatencyUs |
| 24 | u8 | kind: native=1, web=2 |
| 25 | u8 | state: connecting=1, connected=2 |
| 26 | u16 | nicknameBytes, 1..160 |
| 28 | u8 | appVersionBytes, 0..32(웹은 0 가능) |
| 29 | u8 | reserved=0 |
| 30 | bytes | nickname UTF-8, 이어서 appVersion UTF-8; NUL 없음 |

최대 payload 8+3×(30+160+32)=674B. nickname은 제어문자 제거/trim 후 1..40 Unicode scalar,
UTF-8 160B 이하. 잘린 UTF-8, 중복 ID, 자기 ID/방장 누락, 웹 둘, 잘못된 epoch·예약 ID,
남거나 부족한 payload는 거절한다. 전체 목록에서 빠진 사람만 퇴장이다.
같은 revision 재수신은 재설치하지 않고 다시 ACK, 낮은 revision은 무시, 높은 revision은
전체 검증 후 교체한다. 새 사람/epoch만 재생기를 초기화하며 변함없는 사람의 버퍼는 유지한다.
ACK payload는 0B, seq/revision은 받은 값. ACK 유실 시 방장 재전송에 다시 응답한다.

방장은 링크별 마지막 ACK 목록을 기억한다. 새 사람/새 epoch는 그 링크가 해당 roster를 ACK해야
송신한다. ACK를 기다리는 중에도 **변함없는 사람**은 그 링크가 ACK한 revision으로 계속 보낸다.
이때 전달 패킷의 rosterRevision은 수신자 기준 값이며 원본과 같을 필요가 없다.
새 목록 전에 도착한 오디오는 저장해 나중에 재생하지 않고 폐기한다.

| 수신점 | 필수 검사 |
|---|---|
| 방장 UDP | session, 예약/연결 participantId, 성공한 hello의 IP+port, origin=linkSender=그 링크 ID, roster의 kind/epoch/PCM 형식 |
| 네이티브 참가자 | 성공한 방장 주소 IP+port, linkSender=방장 ID, origin=roster의 자기 외 사람, 그 사람 epoch/형식 |
| 방장 WebRTC | 수락된 PeerConnection과 connectionGeneration, origin=linkSender=그 웹 ID, mono16만, 현재 epoch |
| 웹 | 현재 PeerConnection/generation, linkSender=origin=방장 ID, stereo24 mix-minus만, 방장 믹스 epoch |

공통: 알려진 Kind/flags, 정확한 크기·frames·firstFrame 정렬·session을 먼저 검사한다.
현재 설치 revision보다 높은 오디오는 거절한다. 이전 revision 오디오는 현재에도 같은 origin/epoch/형식이
존재할 때만 수용한다. 이전 epoch/퇴장 ID/자기 origin은 폐기한다. 더 큰 epoch가 오디오로 먼저 와도
전환하지 않는다. 참가자가 다른 사람 origin을 주장하거나 방장 이외 주소에서 전달받으면 거절한다.
주소 재바인딩은 새로운 연결 승인/handshake 경로에서만 한다. CORS나 UDP 주소 검사를 암호 인증으로 설명하지 않는다.

재시작은 자기 링크의 `streamRestart`(현재 epoch+1) → 방장 roster 갱신 → ACK 후 새 epoch 오디오 순이다.
방장 자체 장치 재시작은 로컬 스트림 및 웹 mix-minus 시계도 새 epoch로 바꾸어 roster로 알린다.
웹 mix-minus는 방장 origin/epoch를 사용하되 별도 seq/firstFrame을 가진다(웹 수신자에게만 있는 한 스트림).
로컬 장치 rate가 바뀌어도 이미 연 방의 wireRate는 유지하며 ASRC를 다시 준비한다.

전달은 수신 검증 직후, **방장 CollabPlayout 투입 전** 분기한다. origin/seq/firstFrame/epoch/flags/payload는
유지하고 linkSender만 방장으로, rosterRevision은 수신자 ACK 상태로 바꾼다. 자기 링크에 반송하지 않는다.
내 송신 FIFO는 송신 스레드 하나가 한 번 drain하여 링크에 복제한다. UDP 수신·WebRTC 수신은 별도
SPSC 입력을 사용한다. 전송 큐 포화는 그 링크의 오래된 오디오 폐기/통계 증가이며 다른 링크를 막지 않는다.

## 3. 방 서버 HTTP API

R2는 `server/ytdown/` 작업본만 수정한다. prefix는 `/api/livemix/collab/`, 운영 API base는
`https://xn--2j1bo5s81gjmag88b.com/api/livemix/collab/`다. 오디오는 HTTP 서버에 보내지 않는다.
모든 본문은 UTF-8 JSON object, bool을 정수로 받지 않는다. 아래 `?`는 선택 필드,
표에 없는 필드는 필수다. 성공은 HTTP 200, 오류는 `{"error":"<code>"}`.

### 3.1 기존 5개 API: 확장 선택을 하지 않은 요청은 그대로

`apiVersion`이 없으면 legacy 요청이다. 확장 키/토큰/빈 배열을 응답에 추가하지 않는다.
현재 `json.dumps(data, ensure_ascii=False).encode("utf-8")`의 공백·키 삽입 순서·UTF-8 방식,
Content-Type `application/json; charset=utf-8`, Content-Length, Cache-Control `no-store`를 보존한다.
아래는 기존 소스의 실제 응답 순서다. 운영 원본과 같은 시계/난수 입력이면 응답 body 바이트가 같아야 한다.

| POST suffix | 요청 | 응답(키 순서 포함) |
|---|---|---|
| create | nick, inst, port, lan?=[], upnp?={}, stun? | `{"code":C,"session":S,"hostToken":H,"publicIp":IP,"nonce":N}` |
| confirm | code, hostToken, reachable bool, reason?="", punch?=false | `{"ok":true}` 또는 펀칭 유지 시 `{"ok":true,"punch":true}` |
| heartbeat | code, hostToken, connected bool | `{"ok":true}` 또는 `{"ok":true,"guest":{"candidates":[Address...]}}` |
| join | code, nick, inst, stun?, port? | `{"session":S,"hostNick":N,"candidates":[Address...],"sameNetwork":bool}` |
| close | code, hostToken | `{"ok":true}` |

표의 JSON은 설명용 compact 표기다. 실제 legacy serializer에는 현재의 `: ` / `, ` 공백이 남는다.
`code`는 정확한 ASCII 숫자 4자리, `inst`는 `[A-Za-z0-9-]{1,64}`, nick은 기존 제어문자
U+0000..001F/U+007F..009F 제거 후 trim, 1..40글자. create port는 정수 1024..65535,
join port/stun.port는 1..65535, stun `{ip:IPv4,port:int}`이며 미지정/null은 없음.
lan은 최대 8원소 배열, RFC1918 IPv4만 기존 순서로 채택한다. upnp `{found?:bool,mapped?:bool,externalIp?:string}`.
legacy의 reason/externalIp에는 현재 별도 문자열 상한이 없으므로 전체 **16KiB body 상한만** 그대로 적용한다.
새 상한을 구 요청에 소급하지 않는다. Content-Length 1..16384 외/잘못된 JSON/object 외는 400 bad.
네이티브 public IP는 기존 X-Forwarded-For 첫 항목 또는 소켓 주소의 IPv4 검사 그대로다.

legacy 유지 사항: 최대 500방, 방장 heartbeat 10초, 45초 무소식 시 닫힘, 닫힌 코드 600초 보관,
nonce 16바이트/hostToken 16바이트 난수의 hex32, session 8바이트 hex16, 펀칭 주소 반환 30초.
LAN 후보 우선순위·STUN 후보 중복 제거·probe 3회(0/150/300ms)·confirmed 실패/펀칭 유지 조건도 같다.
legacy-only 방의 같은 inst 재입장 및 connected 동작은 반입한 기존 시험을 유지한다.
신형 방의 자리는 예약 모델로 관리하되 구 앱에 돌려주는 JSON 형식은 위 형식이다.

### 3.2 v2 공통 형식과 네이티브 확장

새 클라이언트는 **매 요청에 `apiVersion:2`**를 붙인다. 다른 값은 400 bad.
create/join은 기존 필드에 다음을 추가한다.

```json
{
  "apiVersion": 2,
  "appVersion": "0.17.0",
  "protocols": [1, 2],
  "capabilities": ["packet_v2", "roster_ack", "star_3", "pcm16_mono", "web_guest"],
  "connectionId": "00112233445566778899aabbccddeeff",
  "wireRate": 48000
}
```

protocols는 중복 없는 [1] 또는 [1,2], appVersion은 ASCII 최대 32B, wireRate는 44100/48000.
join의 wireRate는 선호일 뿐 응답의 방 wireRate를 따른다. v2 reason 최대 256B,
externalIp 최대 45B, nick은 40 scalar/160B 제한도 검사한다. v2 네이티브 body 상한도 16KiB.
확장 응답의 공통 `Room` 필드는 다음과 같다(토큰은 Room/members에 넣지 않는다).

```text
Room = { apiVersion:2, code:string4, session:hex16, roomMode:"legacy2"|"v2",
         wireRate:u32, revision:u32, capabilities:[name...],
         nativeState:"checking"|"reachable"|"punch"|"unavailable",
         members:[Member...], reservations:[Reservation...] }
Member = { participantId:u32, kind:"native"|"web", nick:string,
           appVersion:string, protocols:[int...], capabilities:[name...],
           connectionGeneration:u32, streamEpoch:u32,
           state:"connecting"|"connected" }
Reservation = { participantId:u32, kind:"native"|"web", connectionGeneration:u32,
                expiresAtMs:unix_ms }
Address = { ip:IPv4_string, port:int }
```

members에는 방장과 아직 연결 중인 승인된 사람도 들어가며 최대 3개다. reservations는 이 멤버의
연결 전 예약만 별도 기록(최대 2), 인원 계산 때 같은 사람을 두 번 더하지 않는다.
pendingWebRequests는 자리를 소비하지 않는다. `expiresAtMs`는 UTC Unix 밀리초의 안전한 JSON 정수다.
서버 Room은 목록의 정체성과 승인 상태, UDP roster는 오디오 epoch와 재생 목록의 최종 기준이다.

| suffix | v2 추가 요청 | v2 성공 응답 |
|---|---|---|
| create | 위 공통 필드 | 기존 create 필드 + Room + `participantId:1, connectionGeneration:1, streamEpoch:1` |
| confirm | 기존 필드 + apiVersion | `ok:true, punch:bool` + Room |
| heartbeat | 기존 code/hostToken/connected + apiVersion, `memberStates:[{participantId,connectionGeneration,state,streamEpoch}]` | `ok:true` + Room + `peers:[{participantId,connectionId,candidates:[Address...]}]` |
| join | 위 공통 필드 | 기존 join 필드 + Room + `participantId,connectionGeneration,streamEpoch`(자기 예약) |
| close | apiVersion | `ok:true,apiVersion:2` |

heartbeat의 memberStates는 방장 외 승인된 사람의 **완전한** 상태 목록(최대 2), state는
`connecting|connected|left`. 서버가 발급하지 않은 ID/generation은 409 generation.
아직 방장이 보지 못한 예약을 배열에서 빠졌다는 이유로 지우지 않는다. 삭제는 명시적 left 또는 예약 만료다.
기존 connected는 구 경로용으로 보내되 v2 인원 판단에 단일 bool을 쓰지 않는다.
streamEpoch는 방장 roster에 반영된 값을 알리는 용도이고 서버가 독자 증가시키지 않는다.
connected 갱신 시 예약을 해제하되 멤버는 유지한다. peers의 connectionId/주소는 방장에게만 공개한다.
join 응답에는 다른 참가자의 IP/token이 없으며 방장 candidates만 있다.

native 참가를 받을 수 있는 신형 방장은 빈자리 또는 미연결 native 예약이 있는 동안 기존
`punchHeartbeatMs=1000` 주기로 새 예약/후보를 조회한다. 이미 다른 한 명과 connected라는 이유로
이 조회/두 번째 링크 펀칭을 중단하지 않는다. 정원이 찬 뒤 정상 heartbeat는 10초다.
native bye/연결 끊김을 확인하면 방장은 해당 사람의 left heartbeat를 즉시 큐에 넣고
roster에서 그 사람만 제거한다. 한 사람 퇴장이 다른 예약을 덮어쓰지 않는다.

신형 confirm에서 UDP 불가여도 방을 웹용으로 유지한다(`nativeState=unavailable`).
reachable/punch 규칙은 기존과 같고 native join은 이 상태에서 409 native_unavailable.
WebRTC ICE 가능 여부는 별도이며 UPnP 성공/실패로 추정하지 않는다.
구 confirm 요청에는 종전 닫힘 동작을 보존한다.

v2/legacy 혼합에서 구 join 요청은 종전 응답 모양으로 최대 두 사람만 예약한다.
서버가 신형이어도 방장이 구 앱이면 신형 join 응답 `roomMode=legacy2`이며 UDP는 v1이다.
구 서버가 확장 필드를 무시하고 기존 응답만 보내도 신형 CollabRoom은 이를 v1로 읽는다.
웹/3명 방의 늦은 구 join은 HTTP 409 `full`(구 앱이 이해하는 기존 코드),
확장 요청의 호환 불일치는 409 `incompatible`이다.

### 3.3 웹 토큰과 요청/응답

모든 `web/*` 요청은 apiVersion=2, code를 가진다. 토큰은 body에만 실으며 query/header/cookie에 넣지 않는다.

| 토큰 | 형식/발급 | 허용 범위·폐기 |
|---|---|---|
| hostToken | 기존 hex32 유지, create 응답 | 방장 관리/이벤트; 웹으로 전달 금지, 방 종료 때 폐기 |
| inviteToken | 256비트 난수 hex64, web/invite | 초대 정보 조회·request만, 재발급/방 종료 때 폐기 |
| requestToken | 별도 256비트 hex64, web/request | 자기 요청 상태 events/대기 취소만; SDP·ICE 권한 없음 |
| connectionToken | 별도 256비트 hex64, accept | **역할별 두 개**: 방장용/웹용. 해당 연결 generation의 signal/events/leave만 |

방장용 connectionToken은 accept 응답, 웹용은 해당 requestToken의 accepted 이벤트에만 나온다.
양쪽 토큰을 하나로 공유하지 않는다. requestId는 서버 난수 hex32, inviteGeneration은 u32(처음 1).
수락 때 connectionGeneration을 발급하며 재발급과는 별개다. 연결 종료/예약 만료/방 종료 때
두 connectionToken을 모두 폐기한다. accepted 이후 requestToken은 accepted 결과 확인용으로만 남기고
새 시그널을 공개하지 않는다. 토큰은 로그·UI Snapshot·OBS 메타데이터에 기록하지 않는다.

| suffix | 요청 JSON (apiVersion,code 외) | 성공 응답 JSON |
|---|---|---|
| web/invite | `{hostToken}` | `{apiVersion:2,inviteToken,inviteGeneration,hostNick,session,wireRate,revision}` |
| web/request (조회) | `{inviteToken,inviteGeneration,action:"inspect"}` | `{apiVersion:2,hostNick,session,wireRate,roomMode,revision}` |
| web/request (입장) | `{inviteToken,inviteGeneration,action:"join",nick,connectionId,protocols:[2],capabilities:[name...]}` | `{apiVersion:2,requestId,requestToken,state:"pending",expiresAtMs,hostNick,session,cursor}` |
| web/accept | `{hostToken,requestId}` | `{apiVersion:2,requestId,state:"accepted",participantId,streamEpoch,connectionGeneration,connectionToken,reservationExpiresAtMs,room:Room}` |
| web/reject | `{hostToken,requestId}` | `{ok:true,apiVersion:2,requestId,state:"rejected"}` |
| web/signal | `{connectionToken,connectionGeneration,signalId,signal:Signal}` | `{ok:true,apiVersion:2,signalId}` |
| web/events | `{hostToken\|requestToken\|connectionToken,cursor,connectionGeneration?}` | `{apiVersion:2,cursor,events:[Event...]}` |
| web/kick | `{hostToken,participantId,connectionGeneration}` | `{ok:true,apiVersion:2}` |
| web/leave (대기 취소) | `{requestToken}` | `{ok:true,apiVersion:2}` |
| web/leave (연결 퇴장) | `{connectionToken,connectionGeneration}` | `{ok:true,apiVersion:2}` |

`inspect`는 들어가기 전 시안의 “<방장> 님의 합방”을 표시하기 위한 읽기이며 pending/예약을 만들지 않는다.
`action:join`은 닉네임·마이크 권한 성공 후 사용자의 들어가기 동작에서만 호출한다.
web/invite는 매 호출 새 주소를 만든다. 이전 초대와 그 세대 **미수락 요청만** invalidated,
이미 수락된 예약/연결은 유지한다. 방장이 웹을 내보내려면 별도 kick을 사용한다.
request 재시도는 같은 connectionId/초대 세대이면 같은 pending 결과(같은 requestToken)를 반환한다.
accept/reject는 방별 잠금 안에서 pending 여부·만료·전체 정원·웹 1명·버전을 한 번에 검사한다.
pending은 60초, 수락 예약은 15초다. pending은 최대 8, 웹 연결/예약은 합쳐 최대 1.
수락됐던 요청에 같은 accept를 재시도하면 같은 연결 결과를 돌려주며 예약 시간을 연장하지 않는다.
종료된 요청에는 409 request_state, 만료된 것은 410 expired다.

```text
Signal = { type:"offer"|"answer", sdp:string }
       | { type:"candidate", candidate:string, sdpMid:string|null,
           sdpMLineIndex:int|null }
       | { type:"endOfCandidates" }
       | { type:"connected" }      // 방장만: DataChannel 및 roster 준비 완료
Event  = { id:cursor, type:string, data:object }
```

signalId는 송신 역할별 u32 증가 번호(처음 1); 동일 signalId 재전송은 동일 결과이며 중복 이벤트를 만들지 않는다.
offer는 방장 역할 토큰만, answer는 웹 역할 토큰만 허용한다. SDP UTF-8 최대 65536B,
candidate 최대 2048B, sdpMid 최대 64B, sdpMLineIndex는 0..65535 또는 null,
각 방향 candidate 최대 64개, SDP는 방향당 하나(같은 signalId 재시도 제외).
web/* body는 최대 131072B. SDP/ICE는 수락된 연결 토큰으로만 보내고 해당 상대에게만 전달한다.
connected는 방장이 roster ACK까지 받은 뒤 보내며 예약 → connected로 바꾼다.

상태 전이: `pending → accepted/connecting → connected → left|kicked|closed`;
pending은 rejected/cancelled/expired/invalidated/closed로도 끝난다. accepted 예약 만료는 expired.
ICE 실패는 방장/웹이 leave하여 자리 해제한다. WebGuestTransport도 닫고 이 세대의 늦은 콜백을 무시한다.
방 닫힘/45초 만료는 전체 연결·예약·초대를 끝내고 모든 poll을 깨운다.

### 3.4 이벤트, cursor, 롱폴

cursor는 방별 단조 증가 **10진 문자열**(u64; 최초 `"0"`). 이벤트 id도 같은 형식이다.
자기 권한으로 볼 수 있는 `id > cursor`를 오름차순 최대 32개 반환한다. 32개가 차면
마지막 반환 id, 그 외에는 검사한 현재 방 event id를 응답 cursor로 쓴다. 따라서 보이지 않는
다른 역할의 이벤트 때문에 같은 응답을 무한 반복하지 않는다. 응답을 처리한 뒤 다음 cursor를 저장한다.
네트워크 재시도는 마지막 처리 완료 cursor를 재사용하며 중복 id는 한 번만 처리한다.
최초 web/request 응답 cursor는 그 요청의 pending 생성 시점이며 accepted 이벤트를 건너뛰지 않는다.

이벤트가 없으면 **서버 최대 20초** 대기 후 `events:[]`와 cursor를 반환한다.
클라이언트 HTTP timeout은 30000ms, 역할별 poll 하나. WebGuestSignaling의 poll 전용 스레드는
CollabSession의 heartbeat/leave/UPnP worker를 점유하지 않는다. 서버 전역 `_lock`으로 대기하지 않고
방별 Condition에서 대기하며 HTTP 쓰기도 잠금 밖이다.

| event.type | data | 대상 |
|---|---|---|
| snapshot | `room:Room,pendingWebRequests:[{requestId,nick,expiresAtMs}]` | 방장 최초 cursor=0; 대기 요청의 토큰 없음 |
| request | `requestId,nick,expiresAtMs` | 방장 |
| requestState | `requestId,state` | 방장 + 그 requestToken |
| accepted | `requestId,participantId,streamEpoch,connectionGeneration,connectionToken,reservationExpiresAtMs,room:Room` | 그 requestToken만, 웹용 연결 토큰 |
| signal | `connectionGeneration,signalId,from:"host"|"web",signal:Signal` | 반대 역할의 connectionToken |
| room | `room:Room` | 방장 + 승인된 웹 연결 |
| ended | `requestId?,participantId?,connectionGeneration?,reason:"left"|"kicked"|"expired"|"closed"|"invalidated"|"rejected"|"cancelled"` | 방장 + 관련 요청/연결 |

hostToken poll에는 SDP/ICE를 섞지 않는다. 방장은 연결용 poll을 따로 사용한다.
이미 pending으로 기다리던 poll은 폐기 시 ended를 받을 수 있다. 폐기된 자격으로 새 호출은
410 expired/closed로 끝난다. terminal 토큰을 시그널 권한으로 되살리지 않는다.
events는 현재 방/요청 수명 동안 cursor 재읽기가 가능하게 보관하고, 기존 closed 보관 600초 뒤 제거한다.
오디오/미터/heartbeat마다 이벤트를 만들지 않으며 Room 변경 때만 생성한다. 별도 영속 이벤트 저장소는 만들지 않는다.

### 3.5 오류와 CORS

| HTTP | error | 의미 |
|---:|---|---|
| 400 | bad | JSON/길이/필드 형식/알 수 없는 action/apiVersion |
| 400 | ipv4 | 기존 native 주소 교환 규칙 위반(웹에는 적용 안 함) |
| 403 | host | 기존 hostToken 오류 |
| 403 | token | request/connection 토큰 오류 또는 잘못된 역할 |
| 404 | path / room / request | 경로 / 방 / 요청 없음 |
| 409 | full | 정원/예약 포함 정원 초과; 구 앱의 늦은 입장도 이것 |
| 409 | web_full / incompatible | 웹 자리 이미 사용 / v2 기능 조건 부족 |
| 409 | request_state / generation | 끝난 요청 전이 / 현재 연결 세대 불일치 |
| 409 | native_unavailable | 신형 웹용 방의 native UDP 불가 |
| 410 | closed / invite / expired | 방 닫힘 / 옛 초대 / 요청·예약·연결 만료 |
| 425 | checking | 기존 방장 검사 중 |
| 429 | requests | 대기 요청 8개 상한 |
| 503 | busy | 기존 500방 제한 |

legacy 요청이 사용하던 오류의 status/body/순서를 유지한다. 새 웹 오류를 기존 구 앱 경로에
일괄 대입하지 않는다. 오류 코드는 내부 API 데이터이며 그대로 화면 문구로 노출하지 않는다.

브라우저 요청은 `fetch(api, {method:"POST",headers:{"Content-Type":"text/plain"},
body:JSON.stringify(body),credentials:"omit"})`다. application/json, Authorization,
사용자 정의 header를 추가하지 않는다. **OPTIONS 사전 요청은 필요하지 않으며 web_server.py 변경도 없다.**
모든 성공/오류 응답에서 요청 `Origin`이 정확히 `https://xn--jb0byyo90f.com`일 때만
`Access-Control-Allow-Origin: https://xn--jb0byyo90f.com`을 붙인다.
와일드카드, 접미사 비교, 유니코드 도메인 비교, http, 다른 포트, `null`은 허용하지 않는다.
허용 Origin에는 `Vary: Origin`도 붙일 수 있다. Origin 없음/다른 값에는 ACAO와 credentials 허용 헤더를
붙이지 않는다. Origin 없는 기존 네이티브 응답은 기존 헤더 그대로다.
웹 요청은 IPv4/IPv6 모두 받고 `_public_ip()` IPv4 강제를 공유하지 않는다.
CORS는 브라우저 읽기 정책이며 역할별 토큰 검사를 대체하지 않는다.

## 4. WebRTC와 초대 주소

R1은 libdatachannel v0.24.6 + Mbed TLS 3.6.7을 정적으로 연결한다.
반입 완료 소스/해시는 `third_party/README.txt`가 기준이다. `NO_MEDIA/NO_WEBSOCKET/NO_EXAMPLES/NO_TESTS=ON`,
`USE_MBEDTLS=ON`, `USE_NICE=OFF`, `BUILD_SHARED_LIBS/BUILD_SHARED_DEPS_LIBS=OFF`,
Release `/MT`, Debug `/MTd`. `MbedTLS::MbedTLS` INTERFACE wrapper를 먼저 만들고 libdatachannel을 추가한다.
R0는 이 CMake 연결을 하지 않는다.

- **방장이 offerer**다. 수락 결과에 연결된 PeerConnection 하나와 DataChannel 하나를 만들고 offer를 게시한다.
  웹은 answerer이며 main thread에서 RTCPeerConnection을 만들고 `datachannel` 이벤트로 채널을 받는다.
- label `livemix-audio-v2`, protocol `livemix-pcm-v2`, `ordered:false`, `maxRetransmits:0`,
  `maxPacketLifeTime` 미지정, `negotiated:false`(in-band DCEP). SCTP stream id를 고정하지 않는다.
  양쪽은 실제 열린 채널의 label/protocol/ordered/maxRetransmits를 검사한다. 다른 신뢰성 설정은 연결 실패다.
- ICE는 **trickle 사용**. STUN은 `stun:stun.l.google.com:19302`, `stun:stun.cloudflare.com:3478` 두 개.
  기존 native STUN 기본과 같다. TURN 없음. SDP는 application/SCTP만, audio/video track·Opus 없음.
  remoteDescription 전에 받은 candidate는 제어 스레드에 최대 64개 보관 후 적용한다.
  수집 끝은 `endOfCandidates` 한 번. 재시도는 같은 signalId로 HTTP 전송만 반복한다.
- 테스트 `LIVEMIX_COLLAB_LOOPBACK=1`/주입 `loopbackOnly`에서는 public STUN을 쓰지 않고 host candidate로 시험한다.
  웹의 public 페이지가 임의 쿼리로 외부 API base/STUN을 바꾸는 기능은 넣지 않는다.
- 웹 전송은 §2 v2 바이너리 그대로, 120프레임 mono16 상향/stereo24 하향.
  DataChannel의 신뢰성 없음은 SCTP 혼잡 제어/내부 대기가 없다는 뜻이 아니다.
  권장 송신 적체 한도는 오디오 **20ms**: 방향 packetBytes × ceil(wireRate×0.020/120).
  bufferedAmount/앱 대기가 그 한도를 넘으면 오래된 오디오를 폐기하고 카운터를 올린다.
  작은 제어 패킷은 별도로 우선 송신/ACK 재시도하며 송신에서 오디오 콜백을 기다리게 하지 않는다.
- 웹 하향은 방장 장치 시계에서 `내 마스터 + 웹 이외 native 상대의 재생 결과` 1배 합,
  헤드폰 volume/on 적용 전, 웹 자신의 소리는 제외한다. PCM24 변환 때 포화/초과 수 기록.
- 연결 generation은 모든 HTTP signal/event, PC 콜백, JS 내부 메시지의 필수 값이다.
  닫힌/교체된 generation은 데이터가 올바른 모양이어도 버린다. 한 세대의 close가 다른 세대를 닫지 않는다.

초대 URL은 아래 fragment 파라미터를 고정한다. code/token은 API 본문에서만 전송하고 페이지 경로/query에는 넣지 않는다.

```text
표시·복사: https://곰튀김.com/livemix/join/#v=2&code=4827&invite=<hex64>&generation=1
QR payload: https://xn--jb0byyo90f.com/livemix/join/#v=2&code=4827&invite=<hex64>&generation=1
```

URL을 열 때만 v=2, code ASCII4, invite hex64, generation 양의 u32를 파싱한다.
hostToken/requestToken/connectionToken은 주소에 넣지 않는다. 재발급은 invite/generation만 바꾸며
방 코드는 유지한다. QR은 기존 `EnqueueQrCode`를 재사용하고 메시지 스레드에서 만든다.
API 도메인은 브라우저의 뒤쪽 통신용이며 주소창·초대 주소·QR에 표시하지 않는다.

채널 이전 지원 여부는 실제 브라우저 기능 검사로 결정한다. 표준상 이전 전 send() 호출 여부도
제한에 관계하므로 받자마자 이전을 시도한다. [WebRTC 표준의 DataChannel 이전](https://w3c.github.io/webrtc-pc/#rtcdatachannel)
R1/R6는 실제 Chrome/Edge에서 협상값과 경로를 기록해야 하며 R0는 해당 실행 성공을 주장하지 않는다.

## 5. 웹 파일과 스레드 경계

사이트 root는 `site/livemix/join/`. 외부 CDN·별도 시험용 버즈 페이지를 추가하지 않는다.
파일 import는 상대 URL, JS는 ES module, Worker는 `{type:"module"}`.

| 파일(이 prefix 아래) | 소유 | 역할 |
|---|---|---|
| `index.html` | R11 | 확정 시안 DOM, app.js 로드 |
| `join.css` | R11 | 한 열 최대 420px, 모바일/PC, 시안 토큰·배치 |
| `app.js` | R11 | 닉네임/권한/들어가기/대기/마이크 ON·OFF/나가기 UI |
| `guest-client.js` | R6 | UI용 API, PC·오디오 수명, Worker/Worklet 배선, 신호 전달 |
| `signaling.js` | R6 | §3 text/plain fetch, 토큰/cursor/generation, 롱폴 |
| `audio/worker.js` | R6 | packet codec·수신 시각·전송 큐·메트릭·시그널링 작업 |
| `audio/worklet.js` | R6 | `livemix-guest` AudioWorkletProcessor, mono 캡처/스테레오 재생 |
| `audio/packet.js` | R6 | §2 LE codec, 형식 검증/roster ACK |
| `audio/playout.js` | R6 | 재정렬·late 폐기·PLC·재버퍼·fade·독립 clock |
| `audio/asrc.js` | R6 | lm_asrc의 32탭/512위상 JS 이식 |
| `audio/buffers.js` | R6 | 선할당 buffer pool, 120프레임 packet 경계 FIFO |

R6는 DOM/문구/CSS를 소유하지 않는다. R11은 오디오 알고리즘·codec을 복제하지 않고 다음 API만 쓴다.

```js
// guest-client.js — R6 exports. All methods run from the window thread.
export function parseInvite(hash) {} // -> {code, inviteToken, inviteGeneration}
export function createGuestClient({ onState, onRoster, onMeter, onError }) {}
// client.inspect(invite) -> Promise<{hostNick, wireRate, roomMode}>
// client.prepareMicrophone(deviceId = null) -> Promise<MediaDeviceInfo[]>
// client.requestJoin(invite, nickname) -> Promise<void>
// client.setMicEnabled(enabled) -> void
// client.cancel() / client.leave() -> Promise<void>
// client.dispose() -> Promise<void> (tracks, PC, ports, context, worker close)
// onState({state, generation})
//   state = idle|ready|pending|connecting|connected|rejected|ended|error
// onRoster({members:[{participantId,nick,kind,state}], selfId})
// onMeter({peak})    onError({code})  // codes are data, not new screen text
```

마이크 권한은 사용자의 들어가기 동작에서 요청하고 성공 뒤에만 pending을 만든다.
getUserMedia는 audio mono, echoCancellation/noiseSuppression/autoGainControl=false,
선택 deviceId를 사용하며 실제 track settings와 AudioContext.sampleRate를 진단 결과에 남긴다.
마이크 OFF는 캡처 값을 0으로 만들어 상향 시계를 계속 진행한다. 하향 재생은 유지한다.
AudioContext/PC는 Window에 두고 stereo 출력 노드의 channelCount=2,
channelCountMode=`explicit`, channelInterpretation=`discrete`를 사용한다.
Worklet은 process()의 실제 배열 길이를 사용한다. 128을 네트워크 packet 크기로 바꾸지 않는다.

main이 MessageChannel 하나를 만들고 port1을 Worklet node.port를 통해, port2를 Worker에 **이전**한다.
이후 Worklet↔Worker audio 메시지는 직접 전달한다. SharedArrayBuffer/Atomics.wait/교차 출처 격리에 의존하지 않는다.
초기화 메시지 `audioPort`에는 `generation,wireRate,deviceRate,port`가 있고 준비 단계에서 방향별
고정 ArrayBuffer pool을 넘긴다. Worklet process()에는 동적 배열·정렬·블로킹·네트워크 호출이 없다.

| 직접 port 메시지 | 필드 | 흐름 |
|---|---|---|
| capture | `generation,streamEpoch,firstFrame,frames,buffer` | Worklet→Worker, 방 rate mono Float32, frames=120 |
| playback | `generation,streamEpoch,seq,firstFrame,frames,arrivalNs,buffer` | Worker→Worklet, 방 rate stereo Float32 interleaved, frames=120 |
| recycle | `generation,pool:"capture"|"playback",buffer` | 소비자→생산자, 버퍼 소유권 반환 |
| configure | `generation,wireRate,deviceRate,streamEpoch,rosterRevision` | Worker→Worklet, 제어 경로에서 반영 |
| mic | `generation,enabled` | Worker→Worklet |
| stats | `generation,peak,underruns,late,dropped,bufferMs,driftPpm` | Worklet→Worker, UI 주기 집계 |
| close | `generation` | 해당 세대 처리 종료 |

firstFrame/arrivalNs는 JS 내부 BigInt, buffer는 transfer list로 이전한다. Float32 buffer의
할당/TypedArray view 준비는 초기화·메시지 수신에서 하고 process()는 준비된 버퍼를 쓴다.
capture pool은 권장 20ms, playback은 최대 200ms 분량, 초기 목표 재생 버퍼 20ms다.
pool 고갈은 블로킹/추가 할당 대신 해당 오디오 폐기·계수 증가다. 늦은 데이터는 sample 위치로 버린다.
이 권장값은 실측 조정 대상이며 지연 보장값이 아니다. SharedArrayBuffer 대체로 메인 오디오 중계를 넣지 않는다.

DataChannel 경로는 두 가지이며 codec/Worklet 코드는 공유한다.

1. 이전 가능: ondatachannel 직후 `worker.postMessage({type:"channel",generation,channel},[channel])`.
   Worker가 binaryType=`arraybuffer`, onmessage/send/bufferedAmount를 담당한다.
2. 이전 불가(DataCloneError/미지원): main에 채널을 유지하고 별도 MessageChannel로 Worker와 연결한다.
   main은 `{type:"packet",generation,buffer}` 수신/송신·bufferedAmount 보고만 담당한다.
   packet codec/재생/ASRC는 Worker/Worklet 그대로이며 main이 오디오를 혼합하지 않는다.
   send 전에도 동일 적체 한도를 검사한다. 이전 성공 뒤 main에서 같은 채널을 다시 사용하지 않는다.

fallback의 Worker→main 전송 버퍼도 20ms 분량의 고정 pool에서 빌린다.
main이 send 또는 폐기 뒤 `{type:"recycleTransport",generation,buffer}`로 돌려줘야 재사용한다.
따라서 main이 바빠도 MessagePort 내부에 무한히 오디오를 게시하지 않는다. 패킷 메시지에
`createdAtMs:Date.now()`를 함께 보내고 main은 20ms보다 오래 대기한 오디오를 송신하지 않고 반환한다.
오디오 재전송 ACK가 아니라 프로세스 내부 버퍼 소유권 반환이며, 제어 패킷의 §2 ACK와 구분한다.

Worker의 signaling 작업은 main에 `{type:"remoteDescription"|"remoteCandidate",generation,...}`로 보내
PC에 적용하고, main의 local description/candidate는 반대 방향으로 게시한다.
RTCDataChannel을 Worker로 이전해도 RTCPeerConnection은 main에 남는다.

## 6. C++ 공개 인터페이스 초안

아래는 각 소유 회차가 해당 헤더에 구현할 공개 계약이다. R0에서 기존 헤더를 선행 변경하지 않는다.
`c`는 `gocue::livemix::collab::contracts` 별칭이다. JUCE 문자열은 제어 스레드에서만 다룬다.
기존 공개 함수는 기존 시험/호출자를 위해 유지하고 새 overload/함수를 추가한다.

### 6.1 CollabSession / Snapshot — R8, 화면 소비 R10

```cpp
// namespace gocue::livemix::collab
class CollabSession : public juce::ChangeBroadcaster /* existing Timer unchanged */ {
public:
    // Existing State, createRoom/joinRoom/leave/rebuffer/setMonitor/setRoute remain.
    struct PersonSnapshot {
        c::ParticipantId participantId = 0;
        int slot = 0; // self=0, others=1/2; array order is not identity
        bool isSelf = false, isHost = false;
        c::ParticipantKind kind = c::ParticipantKind::none;
        c::ConnectionState state = c::ConnectionState::empty;
        c::StreamEpoch streamEpoch = 0;
        juce::String nickname, appVersion;
        double rttMs=0, estimatedOneWayMs=0, bufferMs=0, lossPercent=0;
        float peakLeft=0, peakRight=0;
    };
    struct WebRequestSnapshot {
        juce::String requestId, nickname;
        int64_t expiresAtMs = 0;
        bool canAccept = false, canReject = false;
    };
    struct Snapshot {
        State state = State::off;
        bool isHost = false;
        juce::String code, myNick, reason, message;
        uint64_t session = 0;
        c::ParticipantId selfId = 0;
        uint32_t wireRate = 48000, capabilities = 0;
        c::RoomMode roomMode = c::RoomMode::legacy2;
        c::Revision roomRevision = 0, rosterRevision = 0;
        bool nativeAvailable = false, signalingConnected = false;
        std::array<PersonSnapshot, 3> people {};
        size_t personCount = 0;
        std::array<WebRequestSnapshot, 8> webRequests {};
        size_t webRequestCount = 0;
        juce::String webInviteUrl, webInviteQrUrl;
        uint32_t inviteGeneration = 0;
        bool canCreateWebInvite = false;
        c::ParticipantId kickableWebId = 0;
        float monitorVolume = 1.0f;
        bool monitorOn = true;
        CollabAudio::Route route = CollabAudio::Route::master;
        int routeFirst = 0;
        // Legacy UI/test projections of slot 1, not an additional stream:
        juce::String peerNick;
        double rttMs=0, estimatedOneWayMs=0, bufferMs=0, lossPercent=0;
    };
    void createWebInvite(); // also used by "새 주소"
    void acceptWebRequest(const juce::String& requestId);
    void rejectWebRequest(const juce::String& requestId);
    void kickWebGuest(c::ParticipantId);
    Snapshot getSnapshot() const;
};
```

공개 명령/스냅샷/ChangeBroadcaster 알림은 메시지 스레드다. 명령은 비동기 큐에 넣고 즉시 반환한다.
성공/실패는 Snapshot으로 알린다. people에는 자기 자신 포함 최대 3명, webRequests에는 pending만 있다.
토큰은 Snapshot에 노출하지 않는다(초대 URL의 inviteToken만 예외).
일부 상대 퇴장이 전체 disconnected를 뜻하지 않는다. 다른 연결/스트림 상태는 유지한다.

### 6.2 CollabAudio / 네이티브 연결 경계 — R4 / R3

```cpp
// Additions to CollabAudio. All views use the prepared device rate.
CollabAudio(const juce::String& peer1Mapping, const juce::String& peer2Mapping);
c::RenderedSources renderSources(int frames) noexcept;
void renderMonitor(const c::RenderedSources&, float* const* outputs,
                   int numOutputs, int masterFirst) noexcept;
void pushWebMix(c::AudioSourceView master, const c::RenderedSources&, int frames) noexcept;
void receiveV2(const c::HeaderV2&, const uint8_t* payload, uint64_t arrivalNs) noexcept;
void applyRoster(const RosterSnapshot&, c::ParticipantId selfId); // lifecycle, R3 type
ObsSender& getPeerObs(unsigned slotIndex) noexcept; // 0/1; existing no-arg returns 0
CollabPlayout::Stats getStats(unsigned slotIndex) const noexcept;
void drainWebMix(const CollabSender::Sink&); // one network sender, not audio callback
```

renderSources는 콜백당 **한 번** 두 재생기를 pull하고 두 OBS 링에 헤드폰 전 원음을 기록한다.
반환 view는 다음 renderSources/prepare까지 유효, frames는 prepare의 maxBlock 이하,
빈 자리는 null=무음, 웹은 채널 수 1이지만 L/R view가 같은 mono sample을 가리킨다.
기존 renderRemote는 renderSources→renderMonitor wrapper로 남긴다. R8의 새 경로에서는
renderSources 뒤에 이 wrapper를 다시 부르지 않는다.
pushWebMix는 선할당 FIFO만 채운다. 세션 종료 때 그 FIFO의 transport 소비자를 해제한다.
두 자리 생성자는 R8이 해석한 두 링 이름을 받는다. 기존 한 자리 생성자/시험에서는
자리 2 링을 임의로 생산 기본 이름에 연결하지 않고 비활성으로 둔다.
drainWebMix의 기존 Sink는 PCM24 payload와 seq/firstFrame/frames를 전달하는 생산자 경계다.
R8의 네트워크 송신 어댑터가 현재 session/방장 origin/linkSender/epoch/ACK revision을 넣은
HeaderV2로 직렬화하여 WebGuestTransport::sendAudio에 보낸다. v1 packet을 채널에 보내지 않는다.

R3는 `CollabRoster.{h,cpp}`에 다음 제어 자료형을 제공한다.

```cpp
struct RosterMember {
    c::ParticipantId participantId = 0;
    c::StreamEpoch streamEpoch = 1;
    c::ParticipantKind kind = c::ParticipantKind::native;
    c::ConnectionState state = c::ConnectionState::connecting;
    uint32_t capabilities=0, deviceRate=48000, inputLatencyUs=0, outputLatencyUs=0;
    juce::String nickname, appVersion;
};
struct RosterSnapshot {
    uint64_t session = 0;
    c::Revision revision = 0;
    uint32_t wireRate = 48000;
    c::RoomMode mode = c::RoomMode::v2;
    std::array<RosterMember, 3> members {};
    size_t memberCount = 0;
};
// Additions in CollabTransport (R3). Existing v1 methods remain usable.
struct V2Peer {
    c::ParticipantId participantId = 0;
    c::ConnectionGeneration generation = 0;
    std::vector<Address> candidates;
};
struct V2Callbacks {
    std::function<void(const c::HeaderV2&, const uint8_t*, uint64_t)> onAudio;
    std::function<void(const RosterSnapshot&)> onRoster;
    std::function<void(c::ParticipantId, State)> onPeerState;
};
void configureV2(c::ParticipantId self, const RosterSnapshot&,
                 const std::vector<V2Peer>&, V2Callbacks); // lifecycle thread
void updateRoster(const RosterSnapshot&); // host control thread
void updatePeers(const std::vector<V2Peer>&); // join/punch per link, not global
void receiveWebPacket(const c::HeaderV2&, const uint8_t*, uint64_t arrivalNs);
Stats getStats(c::ParticipantId linkedPeer) const; // physical native link stats
```

configureV2는 기존 startHost/startGuest 전에 설정하고 legacy 모드에서는 사용하지 않는다.
R3는 v1 audio adapter와 새 콜백 경계를 함께 제공해 R4가 오기 전에도 기존 시험이 빌드되게 한다.
R8은 onAudio를 CollabAudio::receiveV2에, onRoster를 세션 제어 작업에 연결한다.
transport가 보낸 포인터는 콜백 동안만 유효하며 보관하려면 정해진 입력 큐에 복사한다.
receiveWebPacket은 웹 수신 큐 전용 생산자 경계다. UDP 입력 큐와 공유하지 않는다.
R3의 송신 스레드가 웹 원본 패킷을 native로 전달하고 R4 재생 입력에도 배분한다.
R2의 서버 Member→RosterMember 변환은 R8이 맡아 R2가 미래의 R3 헤더를 요구하지 않는다.

### 6.3 Recorder — R5, 엔진/설정 연결 R8

공통 타입은 `livemix/src/recording/RecordingTypes.h`. namespace는 `gocue::livemix::recording`.

```cpp
class Recorder {
public:
    Recorder();
    ~Recorder(); // control thread; stop and finish before destruction
    bool prepareFormat(const DeviceFormat&); // while device callbacks are stopped
    bool start(const StartRequest&); // queues file opening; false = invalid/busy
    void requestStop() noexcept; // idempotent request; does not join or drain here
    void push(collab::contracts::AudioSourceView master,
              collab::contracts::AudioSourceView remote1,
              collab::contracts::AudioSourceView remote2, int n) noexcept;
    void noteSeatOccupant(unsigned slot, const TrackIdentity&); // control thread; slot 1/2, §7 rename
    Snapshot snapshot() const; // control/UI thread; copying strings allowed here
    void finish(); // non-audio shutdown: consume queued blocks, drain, close, join
};
```

State `idle → starting → recording → stopping → idle`; 시작/쓰기/FIFO/인코더 실패는 error.
start 반환 true는 요청 접수이며 실제 파일 열기·빈 머리 flush가 끝난 뒤 recording이다.
R10은 recording 전환을 확인한 뒤 창을 닫는다. 녹음 중 설정 변경은 불가.
Recorder는 앱 수명, 세션 파일 새로 만들기/열기로 파괴·정지하지 않는다.
각 오디오 블록은 formatEpoch/rate/firstFrame를 FIFO에 함께 싣는다. push가 샘플 위치를 유일하게 진행한다.
오디오 입력이 잠깐 중단된 실제 장치 교체 시간을 임의 무음으로 메우지 않는다.
prepareFormat는 callback 중지 상태에서 새 선할당 버퍼를 준비하고 이전 큐를 쓰기 스레드가 끝낸 뒤
전체 파일 묶음을 다음 part로 연다. FIFO 부족/저장 실패는 녹음만 error이며 방송은 계속한다.

MixEngine의 R8 공개 추가는 `recording::Recorder& getRecorder() noexcept`다.
`LiveMixSettings`는 `getRecordingOptions()/setRecordingOptions(const recording::Options&)`를 제공한다.
R10은 두 API를 사용하며 MixEngine·설정 파일을 직접 고치지 않는다.

### 6.4 WebGuestTransport — R1

```cpp
// namespace gocue::livemix::collab
class WebGuestTransport {
public:
    enum class State { off, connecting, open, closed, failed };
    struct Options { bool loopbackOnly = false; }; // §4 default STUN list
    struct Callbacks {
        std::function<void(uint32_t, const juce::String& type,
                           const juce::String& sdp)> onLocalDescription;
        std::function<void(uint32_t, const juce::String& candidate,
                           const juce::String& mid, int mLineIndex)> onLocalCandidate;
        std::function<void(uint32_t)> onGatheringComplete;
        std::function<void(uint32_t, State)> onState;
        std::function<void(uint32_t, const uint8_t*, size_t, uint64_t)> onPacket;
    };
    struct Snapshot {
        State state = State::off;
        uint32_t generation = 0;
        uint64_t sent=0, received=0, dropped=0, bufferedBytes=0;
        bool ordered = false;
        int maxRetransmits = 0;
    };
    WebGuestTransport();
    ~WebGuestTransport();
    void startAcceptedGuest(const c::WebConnection&, Options, Callbacks);
    void applyRemoteDescription(uint32_t generation, const juce::String& type,
                                const juce::String& sdp);
    void addRemoteCandidate(uint32_t generation, const juce::String& candidate,
                            const juce::String& mid, int mLineIndex);
    void endRemoteCandidates(uint32_t generation);
    bool sendAudio(uint32_t generation, const void* packet, size_t bytes);
    bool sendControl(uint32_t generation, const void* packet, size_t bytes);
    void close();
    Snapshot snapshot() const;
};
```

start/apply/add/close는 제어 스레드, send는 네트워크 송신 스레드만 호출한다.
onPacket은 libdatachannel 수신 스레드이며 데이터는 콜백 수명만 유효하다.
콜백은 MessageManager/HTTP/CollabPlayout를 직접 기다리지 않고 큐로 넘긴다.
onState의 open은 DataChannel 준비만 뜻하며 서버 connected 통지는 roster ACK 뒤다.
close 반환 후 객체를 참조하는 콜백을 끝내고, 세대가 오래된 콜백은 버린다.
R1은 CollabSession/MixEngine을 수정하지 않는다. R8이 세 구성요소를 배선한다.

### 6.5 WebGuestSignaling / CollabRoom — R2

```cpp
class WebGuestSignaling {
public:
    struct Invite {
        juce::String inviteToken, hostNick;
        uint32_t inviteGeneration = 0;
    };
    struct PendingRequest { juce::String requestId, nickname; int64_t expiresAtMs=0; };
    struct Accepted {
        c::WebConnection connection;
        juce::String requestId, connectionToken; // host-role token only
        int64_t reservationExpiresAtMs = 0;
    };
    struct Callbacks {
        std::function<void(const Invite&)> onInvite;
        std::function<void(const PendingRequest&)> onRequest;
        std::function<void(const juce::String& requestId)> onRequestEnded;
        std::function<void(const Accepted&)> onAccepted;
        std::function<void(uint32_t generation, const juce::var& signal)> onSignal;
        std::function<void(uint32_t generation, const juce::String& reason)> onEnded;
        std::function<void(int httpStatus, const juce::String& error)> onError;
    };
    explicit WebGuestSignaling(CollabRoom::RequestFunction = {}, juce::String server = {});
    ~WebGuestSignaling();
    void startHost(const juce::String& code, const juce::String& hostToken, Callbacks);
    void createInvite();
    void accept(const juce::String& requestId);
    void reject(const juce::String& requestId);
    void kick(c::ParticipantId, uint32_t generation);
    void sendSignal(uint32_t generation, const juce::var& signal);
    void leave(uint32_t generation);
    void stop();
};
```

공개 command는 자체 제어 큐에 넣는다. host/request 관리 poll과 연결 signal poll은 각각 전용 대기를 가지며
20초 poll이 accept/reject/kick/leave 전송을 막지 않는다. 콜백은 signaling 스레드에서 나오므로
R8은 세션 generation 검사 후 메시지/네트워크 큐에 전달한다. stop은 poll 취소·스레드 종료 뒤 반환한다.
토큰 수명은 R2 내부, R1에는 `WebConnection`만 전달한다.

CollabRoom 기존 create/confirm/heartbeat/join/close 서명은 그대로 두고 R2가 `createV2`,
`confirmV2`, `heartbeatV2`, `joinV2`, `closeV2` 및 `ClientInfo/RoomSnapshot/MemberState`를 추가한다.
각 입력·출력 필드는 §3.2 JSON과 1:1이며 CreatedV2/JoinedV2는 기존 Created/Joined의 필드도 갖는다.
legacy 응답 판독은 별도 유지하고, 네이티브 request 기본 timeout 8000ms를
web/events에 재사용하지 않는다. RequestFunction을 주입하는 기존 시험 경계는 보존한다.

## 7. 녹음 파일·분할·체크포인트·설정

구현 계획 **설계 확정 5·6·7**이 정본이다. R5는 포맷/쓰기, R8은 실제 소리와 수명 배선을 맡는다.

| 시작 조건 | 저장 결과 |
|---|---|
| 합방 없음 | `<선택 폴더>/LiveMix_2026-10-26_14-05-33.wav` 또는 .mp3 하나 = 내 마스터 + 상대 자리 1·2의 1배 합(D와 같은 계산, Track `mix`). 합방이 없으면 상대가 무음이라 내 마스터와 같고, 녹음 중 합방이 시작되면 그때부터 상대 소리도 이 파일에 들어간다 |
| 합방 중(대기방 포함) | `<선택 폴더>/2026-10-26_14-05-33/`에 `A_<내 닉>`, `B_<자리1>`, `C_<자리2>`, `D_전체` 넷을 시작 때 모두 연다 |
| 정상으로 멈춘 뒤 | 녹음 내내 소리가 없었던 자리 파일은 지운다 → 두 사람이면 A·B·D, 세 사람(중간에 들어온 사람 포함)이면 A·B·C·D |

기본 선택 폴더는 Windows 문서 Known Folder 아래 `LiveMix 녹음`(문서 위치를 하드코딩하지 않는다).
시각은 녹음 시작의 로컬 시각 `yyyy-MM-dd_HH-mm-ss`. 한 번의 녹음에 폴더 하나,
같은 설정 형식·MP3 비트레이트를 모든 파일에 적용한다. **모든 파일은 stereo**다. 웹 참가자 자리는 mono 원음을
L=R로 기록하고 D에도 L/R에 같은 값을 더한다(클로드 확정 10/26 — 자리 주인이 녹음 중 웹↔LiveMix로 바뀌어도 파일을
나누지 않게. 설계서 결정 16의 '웹 자리 파일 모노'를 대체한다).
상대 소리는 헤드폰 volume/on/출력 경로 전의 재생 결과, 내 소리는 마스터 처리 후 내 OBS와 같은 값이다.
모든 파일은 한 Recorder 블록 시계에서 시작하고 같은 프레임 구간/길이를 갖는다.

닉네임 파일 부분의 권장 정리 순서:

1. 서버의 40 scalar/160B nickname을 사용한다. C0/C1 제어문자를 제거하고 앞뒤 공백을 정리한다.
2. Windows 금지 문자 `< > : " / \\ | ? *` 각각을 `_`로 바꾸고 끝의 공백/점을 제거한다.
   Unicode 글자를 ASCII로 바꾸거나 별도 표시 이름을 만들지 않는다.
3. 빈 결과는 A=`내소리`, B=`합방상대1`, C=`합방상대2`. 비어 있는 자리도 이 기본 이름이다.
   A_/B_/C_ 접두어 때문에 CON/NUL 같은 원래 닉네임도 예약 파일명이 되지 않는다.
4. 파일 base는 녹음 시작 시 고정한다. 나중의 닉네임/입퇴장으로 이미 열린 파일 이름을 바꾸지 않는다.
5. 날짜 폴더 또는 솔로 파일이 이미 있으면 `2026-10-26_14-05-33 (2)` /
   `LiveMix_2026-10-26_14-05-33 (2).wav`처럼 번호를 증가시킨다. 기존 파일을 덮어쓰지 않는다.

파일 역할 집합(솔로 1개 / A·B·C·D)은 녹음 시작 때 고정한다(클로드 확정 10/26). 합방 없이 시작하면 중간에 합방이
생겨도 솔로 파일 하나 그대로이고(상대 소리는 그 파일에 합산) 다음 녹음부터 사람별이다. 합방에서 시작하면:

- B/C는 **자리**(로컬 OBS 자리 1/2) 기준이다. 비어 있는 동안·나간 뒤에는 무음으로 같은 길이를 유지하고, 녹음 중
  들어온 사람은 그 자리 파일에 이어서 기록한다(다른 파일과 줄이 맞는다). 사람을 다른 자리로 옮기지 않는다.
  누가 들어오고 나가도 나머지 파일·방송은 멈추지 않고 part도 나누지 않는다.
- '소리가 있었던 자리' = 녹음 중 그 자리의 view가 한 블록이라도 null이 아니었던 자리(참가자가 있었으나 조용했던 자리 포함).
  정상 멈춤(`requestStop`·`finish`)에서 모든 파일을 닫은 뒤 소리가 없었던 자리 파일은 모든 part를 지운다.
- 시작 때 빈 자리라 기본 이름(`B_합방상대1`/`C_합방상대2`)으로 연 파일은 정상 멈춤 때, 녹음 시작 뒤 그 자리에 처음 들어온
  사람의 닉네임(위 정리 규칙 그대로)으로 모든 part의 이름을 바꾼다. 바꿀 이름 중 하나라도 이미 있거나 바꾸기가 실패하면
  그 자리 파일은 모두 기본 이름 그대로 두고 오류로 만들지 않는다. 시작 때 사람이 있던 자리는 그 닉네임으로 열고 바꾸지 않는다.
- 강제 종료나 FIFO·저장 오류로 끝난 녹음에서는 지우기·이름 바꾸기를 하지 않는다(무음 자리 파일·기본 이름이 남을 수 있다).
- R8은 roster/자리 변경 때 제어 스레드에서 `Recorder::noteSeatOccupant(slot, TrackIdentity)`(slot 1/2)를 부른다.
  Recorder는 시작 때 비어 있던 자리마다 시작 뒤 처음 받은 사람만 이름 바꾸기에 쓴다.

D는 A+B+C의 1배 합, 별도 리미터·자동 감쇄 없음. 정수 변환에서만 포화하고 각 파일의
`clippedSamples`를 기록한다. WAV의 정수 샘플 비교 기준은 동일 프레임 A/B/C의 PCM24를 넓은 정수로
합한 뒤 PCM24 범위로 포화한 값과 D가 일치하는 것이다. 빠진 파일의 해당 원음도 D 계산에는 포함한다.
인코딩 전 PCM 양자화를 공통으로 수행해 파일별 반올림 때문에 D 비교가 흔들리지 않게 한다.
MP3는 손실 압축이므로 디코드 결과 샘플 동등성이 아닌 같은 입력 구간·시작 표식·길이로 검사한다.

WAV는 장치 sampleRate 그대로, PCM24. 파일 **전체 크기 4GiB−1MiB**를 넘기기 전에 프레임 경계에서
전체 묶음을 같이 분할한다(모노 파일이 덜 찼어도 함께). part1은 접미사 없음,
part2부터 `A_<닉>_2.wav`, `B_<닉>_2.wav`, `D_전체_2.wav` 및 솔로 `LiveMix_<시각>_2.wav`.
블록 중간 분할도 앞뒤 프레임에 누락·중복이 없어야 한다. RF64 자동 전환에 맡기지 않는다.
장치 sampleRate가 바뀌면 WAV/MP3 모두 이전 FIFO를 끝내고 전체 묶음을 다음 part로 닫아 연다.
파일명 part 번호는 크기/rate 분할에 공통 단조 증가 번호다.

체크포인트는 쓰기 스레드에서 약 **1000ms**마다 WAV writer->flush()로 머리를 갱신한 뒤
파일 stream flush(Windows FlushFileBuffers)하고 성공한 프레임 수를 snapshot에 게시한다.
시작 때 빈 WAV 머리와 flush 완료 후 recording으로 전환한다. 강제 종료 보장은 **마지막 완료
체크포인트까지 재생 가능**이며, FIFO/인코더/RAM에 남은 마지막 소리까지 약속하지 않는다.
MP3도 완성된 인코더 프레임을 쓰고 파일 flush로 체크포인트를 남긴다.
`RecordingRecovery`는 이 체크포인트의 완료 길이/종료 처리를 담당한다. 자동 재개,
시작 시 폴더 전체 탐색, 별도 백업/저널 서비스를 추가하지 않는다.

MP3는 `CLSID_MP3ACMCodecWrapper`의 IMFTransform 직접 사용: 출력 형식 먼저, 입력 PCM16,
float→PCM16 직전 TPDF dither, 128/256/320kbps. 원본 장치가 32000/44100/48000이면 유지,
그 외는 전용 lm_asrc로 48000. 모든 파일이 stereo이므로 입력·출력 모두 2채널이다.
`AVG_BYTES_PER_SECOND`는 16000/32000/40000. NOTACCEPTING/NEED_MORE_INPUT을 처리하고
정상 stop은 EOS→drain→남은 출력→파일 close 순이다. MF 가용성은 MP3 시작 때만 확인하며
실패해도 WAV는 쓸 수 있다. [Microsoft MP3 인코더 형식](https://learn.microsoft.com/en-us/windows/win32/medfound/mp3-audio-encoder)
R5는 세 비트레이트의 stereo 실제 출력을 각각 확인한다.

오디오 콜백은 선할당 FIFO 복사·고정 버퍼 계산·atomic만 사용한다. 권장 큐는 준비된 장치 rate의
4초 분량. 파일·MF·네트워크·잠금·할당·join 없음. FIFO 초과/저장 실패는 해당 녹음 전체 error,
다른 파일만 정상 표시하며 계속하지 않는다. 이미 저장된 파일은 마무리 가능한 범위까지 닫는다.
세션 교체에도 Recorder는 살아 있고 앱 종료 때 입력 차단→진행 중 콜백 종료→FIFO 소비→drain/close→엔진 파괴다.

MixEngine 정상/graph try-lock 실패 경로 모두 아래 순서를 지킨다.

1. 내 마스터 결과(실패 경로는 무음)를 내 OBS/네이티브 송신에 전달.
2. `renderSources(n)` 한 번으로 상대별 원음과 상대 OBS 1/2 기록.
3. 같은 view로 `Recorder::push(master, remote1, remote2, n)`.
4. 방장 웹 연결이 있으면 `pushWebMix(master,sources,n)`(웹 자기 소리 제외).
5. `renderMonitor(sources,...)`에서만 공통 헤드폰 gain/on/route 적용.

기존 ScopedTryLock을 새 기능의 blocking lock으로 바꾸지 않는다. try-lock 실패해도
상대 재생·녹음·웹 믹스의 샘플 수는 진행한다.

설정은 `%APPDATA%/LiveMix/LiveMix.settings`에 다음 **세 키만 추가**한다.

| 키 | 저장값 | 기본값 |
|---|---|---|
| recordingFormat | 문자열 `wav` / `mp3` | wav |
| recordingMp3Kbps | 정수 128 / 256 / 320 | 320 |
| recordingDirectory | 절대 경로 문자열 | 비어 있으면 문서/LiveMix 녹음 |

collabNick/Port/Rate/MonitorVolume/MonitorOn/Route/RouteFirst/InstallId의 기존 키/기본값은 보존한다.
토큰·사람 ID·초대·녹음 진행 상태·파일 집합은 설정에 저장하지 않는다. 앱 재실행 때 자동 녹음하지 않는다.

## 8. OBS 링과 메타데이터 ABI

공통 파일 `livemix/shared/lm_obs_metadata.h`는 R0 고정, 작성기 `ObsMetadata.{h,cpp}`와
플러그인 읽기는 R7. 기존 `lm_obs_protocol.h`의 오디오 ABI(256B 머리, stereo float ring)는 그대로다.

| 대상 | 소스 ID / 메뉴 | 기본 링 | 메타데이터 |
|---|---|---|---|
| 내 소리 | 기존 LiveMix | `Local\LiveMix.ObsAudio.v1` | 추가 없음 |
| 상대 1 | `livemix_peer_source` / LiveMix 합방 상대 1 | `Local\LiveMix.ObsAudio.v1.Peer` | 링 이름 + `.Metadata` |
| 상대 2 | `livemix_peer2_source` / LiveMix 합방 상대 2 | `Local\LiveMix.ObsAudio.v1.Peer2` | 링 이름 + `.Metadata` |

기존 장면의 `livemix_peer_source`는 그대로 자리 1을 읽는다. 플러그인 버전은 R7에서 0.17.0.
닉네임은 속성의 `현재 참가자`에 표시하고 사용자가 지정한 OBS 소스 이름을 바꾸지 않는다.
빈 자리에도 오디오 무음을 기록하며 메타데이터 participantId/epoch/nick을 지운다.
마스터 OBS 보내기 또는 합방 열림일 때 상대 링 1/2를 사용 가능하게 하는 기존 정책을 함께 적용한다.

메타데이터는 Windows x64 LE **정확히 256B**, 정렬 8B. `#pragma pack(1)`을 쓰지 않는다.

| 오프셋 | 바이트 | 필드 |
|---:|---:|---|
| 0 | 4 | magic `0x444d4d4c` (`LMMD`) |
| 4 | 4 | version=1 (패킷 v2와 별개) |
| 8 | 4 | mapping_bytes=256 |
| 12 | 4 | slot=1 또는 2 |
| 16 | 8 | sequence: 게시 순번, 정렬된 int64, 쓰는 중 홀수/완료 짝수 |
| 24 | 8 | session |
| 32 | 4 | participant_id (빈자리 0) |
| 36 | 4 | stream_epoch |
| 40 | 4 | state: empty=0/connecting=1/connected=2/disconnected=3 |
| 44 | 4 | kind: none=0/native=1/web=2 |
| 48 | 4 | nickname_bytes 0..160 (NUL 제외) |
| 52 | 4 | writer_pid |
| 56 | 161 | nickname_utf8: UTF-8 + NUL, 나머지 0 |
| 217 | 39 | reserved=0 |

작성자는 제어 스레드 하나, 처음 전체 0 후 위 머리를 초기화한다. 기존 Windows 공유 메모리 acquire/release
규칙으로 sequence를 홀수 게시→내용 복사→다음 짝수 게시한다. 읽기는 짝수 sequence 확인→복사→
같은 짝수 재확인 후 사용한다. 이 읽기는 OBS 속성/제어 경로이며 audio callback을 기다리지 않는다.
writer_pid/해당 오디오 링의 기존 heartbeat 상태가 현재 writer와 맞지 않으면 이름을 현재 참가자로 쓰지 않는다.
새 닉네임을 위해 오디오 링 reserved를 쓰거나 읽기 전용 mapping에 Interlocked 쓰기를 하지 않는다.

R7 작성기의 공개 계약:

```cpp
// namespace gocue::livemix
class ObsMetadata {
public:
    bool open(const juce::String& resolvedPeerRing, unsigned slot); // slot 1/2
    void publish(uint64_t session, collab::contracts::ParticipantId,
                 collab::contracts::StreamEpoch, collab::contracts::ParticipantKind,
                 collab::contracts::ConnectionState, const juce::String& nickname);
    void clear();
    void close();
};
```

`publish/clear/open/close`는 제어 스레드. R8이 roster/자리 변경에 연결하고 R4의
audio callback은 이 객체나 문자열을 다루지 않는다.

시험 환경 변수의 **앱과 플러그인 공통 이름 해석**:

```text
base = nonempty LIVEMIX_OBS_RING, otherwise Local\LiveMix.ObsAudio.v1
peer1 = nonempty LIVEMIX_OBS_PEER_RING, otherwise base + .Peer
peer2 = nonempty LIVEMIX_OBS_PEER2_RING, otherwise base + .Peer2
peer1 metadata = resolved peer1 + .Metadata
peer2 metadata = resolved peer2 + .Metadata
each readers mapping = resolved audio ring + .Readers
```

기존 두 변수는 보존하고 자리 2용 LIVEMIX_OBS_PEER2_RING만 추가한다. 별도 metadata override는 없다.
명시적 peer override가 base 파생보다 우선하며 **기본 Peer 문자열에서 Peer2를 치환해 만들지 않는다**.
이름은 UTF-16 127 code unit 이하(현재 시험 override 길이 제한), 파생 suffix 공간까지 고려하여
시험 base는 104 이하로 정한다. 기본값이나 빈 override는 생산 링 규칙을 따른다.
R8은 앱, R7은 플러그인, R9는 두 쪽에 같은 resolved name을 주는 시험 도구를 맡는다.
`LIVEMIX_ALLOW_MULTI=1`, `LIVEMIX_COLLAB_LOOPBACK=1`, `LIVEMIX_COLLAB_SERVER` 기존 시험 환경도 보존한다.
웹 loopback API/ICE는 R6 시험 API 주입으로 구성하며 배포 UI에 시험 설정을 추가하지 않는다.

## 9. 파일 소유표 — R1~R11 겹침 없음

여기 적힌 파일/하위 디렉터리만 각 회차의 수정 범위다. 기존 파일은 수정, 없는 파일은 필요할 때 생성한다.
`tests/livemix017/Rn.cmake`는 R0가 빈 틀을 만들고 Rn에 인계한다.
`cmake/livemix017/Rn.cmake`는 해당 회차가 만드는 **빌드 조각**이다. test 조각과 역할이 다르다.
이 문서/공통 타입 변경은 R0 계약 변경으로 별도 직렬 처리한다. 다른 회차가 편의상 복제하지 않는다.

| 회차 | 제품·도구 파일 전체 범위 | 시험 파일 전체 범위 | 빌드 연결 |
|---|---|---|---|
| R1 | `CMakeLists.txt`; `cmake/LiveMixWebRTC.cmake`; `third_party/libdatachannel/**`, `third_party/mbedtls/**`, `third_party/README.txt`; `livemix/src/WebGuestTransport.{h,cpp}`; `livemix/tools/WebGuestProbe.cpp` | `tests/LiveMixWebGuestTransportTests.cpp`; `tests/livemix017/r1/` 전체(브라우저 채널 시험 자산/runner); `tests/livemix017/R1.cmake` | 루트의 의존성/optional round include를 **유일하게** 편집 |
| R2 | `server/ytdown/livemix_collab.py`, `server/ytdown/test_livemix_collab.py`; `livemix/src/CollabRoom.{h,cpp}`, `livemix/src/WebGuestSignaling.{h,cpp}` | `tests/LiveMixCollabRoomV2Tests.cpp`, `tests/LiveMixWebGuestSignalingTests.cpp`; `tests/livemix017/R2.cmake` | `cmake/livemix017/R2.cmake` |
| R3 | `livemix/src/CollabProtocol.{h,cpp}`, `livemix/src/CollabTransport.{h,cpp}`, `livemix/src/CollabRoster.{h,cpp}` | 기존 `tests/LiveMixCollabProtocolTests.cpp`, `tests/LiveMixCollabTransportTests.cpp`; 새 `tests/LiveMixCollabRosterTests.cpp`, `tests/LiveMixCollabV2Tests.cpp`; `tests/livemix017/R3.cmake` | `cmake/livemix017/R3.cmake` |
| R4 | `livemix/src/CollabAudio.{h,cpp}`, `livemix/src/CollabPlayout.{h,cpp}`, `livemix/src/CollabSender.{h,cpp}` | 기존 `tests/LiveMixCollabPlayoutTests.cpp`, `tests/LiveMixCollabSenderTests.cpp`; 새 `tests/LiveMixCollabSourcesTests.cpp`; `tests/livemix017/R4.cmake` | `cmake/livemix017/R4.cmake` |
| R5 | `livemix/src/recording/Recorder.{h,cpp}`, `RecordingQueue.{h,cpp}`, `WavSink.{h,cpp}`, `MfMp3Sink.{h,cpp}`, `RecordingRecovery.{h,cpp}`(모두 같은 recording/ 안); `livemix/tools/RecorderProbe.cpp` | `tests/LiveMixRecorderTests.cpp`, `tests/LiveMixWavSinkTests.cpp`, `tests/LiveMixMp3SinkTests.cpp`; `tests/livemix017/r5/` 전체(파일 fixture/검사); `tests/livemix017/R5.cmake` | `cmake/livemix017/R5.cmake` |
| R6 | §5의 `site/livemix/join/guest-client.js`, `signaling.js`, `audio/` 전체 | `tests/livemix017/r6/` 전체(JS codec/ASRC/브라우저 오디오 시험과 진입 HTML); `tests/livemix017/R6.cmake` | `cmake/livemix017/R6.cmake` |
| R7 | `obs-plugin/src/` 전체; `obs-plugin/data/locale/ko-KR.ini`, `en-US.ini`; `obs-plugin/CMakeLists.txt`, `obs-plugin/buildspec.json`; `livemix/src/ObsMetadata.{h,cpp}` | `tests/LiveMixObsMetadataTests.cpp`; `tests/livemix017/r7/` 전체(플러그인 2자리 검사); `tests/livemix017/R7.cmake` | `cmake/livemix017/R7.cmake` + OBS 자체 CMake |
| R8 | `livemix/src/MixEngine.{h,cpp}`, `CollabSession.{h,cpp}`, `Main.cpp`, `LiveMixSettings.{h,cpp}`(모두 livemix/src/ 안) | 기존 `tests/LiveMixCollabSessionTests.cpp`, `tests/LiveMixCollabEngineTests.cpp`, `tests/LiveMixCollabTestHelpers.h`; 새 `tests/LiveMixRecordingEngineTests.cpp`, `tests/LiveMixRecordingSettingsTests.cpp`; `tests/livemix017/R8.cmake` | `cmake/livemix017/R8.cmake` |
| R9 | `tools/lm_collab017/` 전체(오디오 행렬 runner·브라우저 구동·파일 검사·CPU 측정·fixture·README); `livemix/tools/CollabProbe.cpp` | 기존 `tests/LiveMixTransportProbeTests.cpp`; `tests/livemix017/r9/` 전체; `tests/livemix017/R9.cmake` | `cmake/livemix017/R9.cmake` |
| R10 | `livemix/src/ui/MasterCard.{h,cpp}`, `CollabWindow.{h,cpp}`, `MainComponent.{h,cpp}`, `RecordingWindow.{h,cpp}`(모두 ui/ 안) | 기존 `tests/MasterCardTests.cpp`, `tests/LiveMixCollabWindowTests.cpp`; 새 `tests/LiveMixRecordingWindowTests.cpp`; `tests/livemix017/R10.cmake` | `cmake/livemix017/R10.cmake` |
| R11 | `site/livemix/join/index.html`, `join.css`, `app.js` | `tests/livemix017/r11/` 전체(화면·접속 흐름 시험); `tests/livemix017/R11.cmake` | `cmake/livemix017/R11.cmake` |

R0 고정 파일: 이 계약, `livemix/src/CollabContracts.h`, `livemix/shared/lm_obs_metadata.h`,
`livemix/src/recording/RecordingTypes.h`, `tests/CMakeLists.txt`의 dispatcher,
`server/ytdown/ORIGIN.txt`, `server/ytdown/.gitattributes`, R0 기준 결과 보고서.
R7의 metadata 구현 범위에 공통 ABI 헤더는 포함하지 않는다. R5도 RecordingTypes를 소유하지 않는다.
시험의 새 fixture 디렉터리는 소유 회차만 생성하며 다른 회차는 링크/import로 소비한다.
기존 `livemix/shared/lm_asrc.{h,c}`, `lm_obs_protocol.h`, `ObsSender.{h,cpp}`,
`CollabHostCheck.*`, `CollabUpnp.*`는 본 회차표에서 변경하지 않는 소비 대상이다.

공유 파일 대신 사용할 연결점:

| 공유 수요 | 한 소유자 | 다른 회차가 사용하는 경계 |
|---|---|---|
| 루트 CMake 연결 | R1 | 아래 optional `cmake/livemix017/Rn.cmake` |
| tests/CMakeLists | R0 동결 | 이미 포함된 `tests/livemix017/Rn.cmake` |
| CollabSession / 방·자리 관리 | R8 | R1 transport, R2 room/signaling, R3 roster/packet, R4 audio API; R10 Snapshot |
| MixEngine / 콜백 | R8 | R4 renderSources/renderMonitor/pushWebMix, R5 Recorder |
| 앱 수명·종료 / 설정 | R8 | R5 start/stop/finish/Options, R10 getRecorder/getRecordingOptions |
| OBS metadata 정의 | R0 동결 | R7 writer/reader, R8 publish 호출; R4는 음성 링만 |
| 웹 audio와 화면 | R6 / R11 각각 독립 | guest-client.js API; index/CSS/app은 R11만 |
| 운영 Python/배포 | 클로드 | R2는 server/ytdown 작업본; 운영 경로/web_server.py 수정 금지 |

기존 LiveMixCore/LiveMixUI/LiveMixCollabProbe 대상 생성 뒤, `add_subdirectory(tests)` 앞의 R2..R11 빌드 조각
OPTIONAL include 반복은 클로드가 R0 단계에서 미리 넣었다(§11.3). R1은 그 반복 **바로 앞**에 LiveMixWebRTC.cmake 연결만 더한다.
각 조각은 존재하는 대상에 target_sources/target_link_libraries 또는 자기 probe 대상을 추가한다.
파일 경로는 `${PROJECT_SOURCE_DIR}`/`${CMAKE_CURRENT_LIST_DIR}`를 사용한다.
아직 생성되지 않은 다른 회차 .cpp를 미리 source 목록에 넣지 않는다.
R8의 “공통 CMake 통합”은 자기 조각 R8.cmake로 수행하므로 루트 파일을 다시 소유하지 않는다.
R10의 새 RecordingWindow.cpp도 R10.cmake에서 LiveMixUI에 추가한다.
새 probe 대상 이름은 R1=`LiveMixWebGuestProbe`, R5=`LiveMixRecorderProbe`로 한다.
R9는 기존 `LiveMixCollabProbe`를 확장한다. 별도 Recorder 앱의 probe 대상과 이름을 공유하지 않는다.
R13 버전/배포 연결은 R1 종료 뒤 별도 인계하며 R1~R11 소유가 겹치지 않는다.

```cmake
# Root wiring after all existing LiveMix targets, before tests. The loop is already in place (R0);
# R1 adds only the include line right above it:
include(cmake/LiveMixWebRTC.cmake)
foreach(_livemix017_round RANGE 2 11)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/livemix017/R${_livemix017_round}.cmake" OPTIONAL)
endforeach()
unset(_livemix017_round)

# An owning round's tests/livemix017/Rn.cmake, only when its file exists:
# target_sources(EnqueueTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/LiveMix...Tests.cpp")
```

실행 순서는 계획대로 R0 → (R1/R2/R5) → (R3/R6/R7), R4는 R3 뒤,
R8은 R1~R7 뒤 단독 → (R9/R10/R11). 최대 세 회차.
다른 회차의 아직 없는 구현을 대신 작성하거나 임시 stub을 상대 파일에 넣지 않는다.

## 10. 화면·시험·반입 기준

화면 글자는 시안/대표 확정 문구만 쓴다. 녹음 버튼은 A안 LUFS 바로 옆,
창은 420×304, 녹음 성공 후 닫고 다시 열어 `녹음 중지`.
`형식`, `비트레이트`, `저장 위치`, `변경`, `WAV`, `MP3`, `128/256/320 kbps`, `녹음 중`은 시안 그대로다.
합방 창은 440×800, 두 자리 행, 방장만 `웹 초대`/`주소 복사`/`새 주소`/
`들어오기 요청`/`수락`/`거절`/`내보내기`를 다룬다.
웹은 `닉네임`, `마이크`, `헤드폰을 쓰고 들어오세요.`, `들어가기`,
`방장이 수락하면 연결됩니다`, `취소`, `연결됨`, `마이크 ON/OFF`, `나가기`,
`초대 주소가 바뀌었거나 방이 닫혔습니다.`를 사용한다. 새 오류 코드에 맞춰 설명 문구를 임의로 늘리지 않는다.
시안 설명용 board 제목·프레임 설명은 실제 화면에 넣지 않는다.

R0 서버 원본은 다음 두 파일이며 byte 복사와 SHA-256/복사 시각은 `server/ytdown/ORIGIN.txt`에 기록한다.
scope .gitattributes의 `-text`는 checkout/스냅샷에서도 원본 줄바꿈을 보존하기 위한 것이다.

```text
C:\Users\claude\Desktop\유튜브다운\livemix_collab.py
C:\Users\claude\Desktop\유튜브다운\test_livemix_collab.py
```

모든 빌드/시험은 rb.py 경유. 옵션은 작업 폴더 앞, 명령 안 exit 금지,
실패는 `if ($LASTEXITCODE) { throw '...' }`. 직접 cmake/ctest 실행 금지.
기존 시험의 runner는 `tests/TestMain.cpp`: 첫 인자가 이름 부분 문자열 필터이며 출력의
passed/failed는 UnitTest assertion 수다(테스트 클래스 수와 구분한다).

```powershell
python C:\Users\claude\tools\build_server\rb.py C:\Users\claude\gocue-webrec -- '& ''C:/Users/claude/tools/cmake/bin/cmake.exe'' --build --preset local --target LiveMix EnqueueTests LiveMixCollabProbe; if ($LASTEXITCODE) { throw ''R0 build failed'' }'
python C:\Users\claude\tools\build_server\rb.py C:\Users\claude\gocue-webrec -- '& ./build/vs2022/tests/EnqueueTests_artefacts/Release/EnqueueTests.exe ''LiveMix Collab''; if ($LASTEXITCODE) { throw ''Collab tests failed'' }'
python C:\Users\claude\tools\build_server\rb.py C:\Users\claude\gocue-webrec -- 'Set-Location server/ytdown; python -m unittest test_livemix_collab -v; if ($LASTEXITCODE) { throw ''Server tests failed'' }'
```

첫 build가 configure되지 않은 원격 작업 폴더라면 같은 rb.py 경유로 `cmake --preset local`을 먼저 실행한다.
roomc 기본 PATH에 cmake가 없어 위 예시는 설치된 실행 파일의 절대 경로를 쓴다.
자동 복사되는 CMakeUserPresets.json에 configure preset local만 있고 build preset local이 없으면,
작업 폴더의 **Git 제외된** CMakeUserPresets.json에
`{"name":"local","configurePreset":"local","configuration":"Release"}` build preset을 추가한다.
루트 CMake나 본 저장소의 프리셋을 이 환경 준비 때문에 수정하지 않는다.
합방 필터의 기존 검증/기대값은 R0에서 바꾸지 않는다. R1..R11 시험은 자기 조각으로만 등록한다.
공통 헤더는 C++17, OBS 헤더는 C17/C++17 양쪽에서 크기/오프셋 static_assert가 컴파일되어야 한다.
빈/없는 시험 조각은 모두 허용하며, 기존 세 대상의 링크/시험 집합이 R0 이전과 같아야 한다.
결과 숫자는 `docs/superpowers/reports/2026-10-26-livemix-webguest-record-r0-baseline.txt`에 별도 기록한다.
R9 이후 실제 다른 PC 연결은 갱신된 설계/계획에 따라 **이 PC ↔ roomc**이며 빌드 작업과 겹치지 않게 한다.

## 11. 클로드 확인 결과 (10/26)

1. 녹음 시작 뒤 자리 추가/종류 교체: **R0 권장안 대신** §7의 확정안 — 합방 중 시작이면 A·B·C·D를 모두 열고
   (빈 자리는 무음), 정상 멈춤 때 소리가 없었던 자리 파일은 지우고 시작 때 빈 자리였던 파일은 처음 들어온 사람 닉네임으로
   이름을 바꾼다. 모든 파일 stereo(웹은 L=R)라 종류 교체로 part를 나누지 않는다. 솔로 파일은 상대 소리까지 합한다.
   이유: 녹음 중 들어온 사람(게스트 등장)도 자기 파일을 갖게 하고(gom 원문 16 "최대 3명이면 A.B.C.D"),
   녹음 중 시작된 합방 소리가 솔로 녹음에서 빠지지 않게(gom 원문 "녹음하면 합방 소리도 녹음되게") 하기 위해서다.
2. 계획에 없던 접합 세부값(§2의 Kind/flag/capability 번호·200ms 제어 재시도, §3의 apiVersion/action=inspect·상한·
   역할별 연결 토큰·cursor 형식, §4의 채널 이름/fragment/STUN·20ms 적체, §5 pool 상한, §7 파일명 정리/WAV 정수 합산
   순서/기본 WAV·320kbps, §8 metadata 256B/환경 변수 파생 규칙)은 **이 문서의 값으로 확정**한다.
   후속 회차는 이 값을 사용하며 변경이 필요하면 계약을 먼저 갱신한다.
3. 회차별 작업 폴더: rb.py는 작업 폴더마다 roomc의 같은 경로 하나를 덮어쓰므로, 동시에 도는 회차는 각자 작업 폴더
   (`gocue-webrec-rN`, 브랜치 `livemix-webguest-record-rN`)에서 빌드·시험한다. 아스트라 샌드박스는 공용 `.git`에 쓸 수 없으니
   회차는 커밋하지 않고, 클로드가 검토 뒤 소유 파일만 커밋해 본 브랜치에 합친다. 루트 CMake의 회차 빌드 조각 반복
   (`cmake/livemix017/R2..R11.cmake` OPTIONAL include)은 R0 단계에서 클로드가 미리 넣었다 — R1은 그 위에 WebRTC 연결만 더한다.
