# 앤큐 웹리모트 v1 설계 (2026-10-24)

폰·태블릿 브라우저로 앤큐를 조작하는 웹 리모컨. 앱 설치 없음.

## gom 결정 (바꾸지 말 것)
- 앱이 아니라 웹. **앤큐 프로그램 안의 웹 서버만** 쓴다. 바깥 서버·중계·인터넷 없음.
- 같은 와이파이에서 앤큐가 띄운 **QR + 주소**로 연결. 주소 고정은 안 함(IP가 바뀌면 QR을 다시 찍는다).
- 주 사용 기기는 **폰과 태블릿(아이패드)**, 터치하기 편한 화면. 화면 = **시안 4 'PC 그대로'**(바탕화면 `웹리모트\04 PC 그대로.png`).
- PC 메뉴: 위쪽 메뉴 줄 **'도움말' 바로 오른쪽에 '모바일 연동'**(gom 10/24 "외부기기 연동 말고 모바일 연동으로 바꾸자") → `웹리모트 켜기`(체크) · `QR 코드·주소 보기` · (구분선) · `QR 새로 만들기`. 창 제목도 `모바일 연동`. 창 = **B안 세로형**(gom 10/24 "세로형으로 만들어줘" — `웹리모트\10 PC 외부기기 연동 창 후보.png` 오른쪽 그림: 위 가운데 QR, 그 아래 `웹리모트` 꺼짐|켜짐 · `주소`+`복사` · `네트워크` · `연결된 기기 N` · 오른쪽 아래 `QR 새로 만들기`).
- **메인 큐 리스트 안 큐 순서**를 웹에서 끌어 바꾸면 PC 앱에도 적용(PC 큐 표에서 줄을 끌어 놓은 것과 같은 결과·같은 되돌리기).
- **쇼 모드·편집 모드를 웹에서도 고른다**(PC 위쪽 토글과 같은 것). 쇼 모드에선 순서 바꾸기 잠금(PC와 같음).
- **첫 실행 안내(gom 10/24)**: 이 기능이 들어간 판을 처음 실행할 때 한 번만(새로 설치·업데이트 모두, 이 PC 설정에 '봤음' 기록) 문구 그대로: `휴대폰과 태블릿 등에서 앤큐를 컨트롤 할 수 있는 기능이 추가 되었습니다. 메뉴 창에서 "모바일 연동"을 클릭해서 진행해보세요! (공유기가 없으면 불가능합니다)` 버튼 `확인` 하나. 업데이트로 '업데이트 완료' 안내 창이 뜨는 경우엔 그 창 안에 이 문구를 같이 넣어 창이 두 번 뜨지 않게.
- 목록 줄을 누르면 **다음 큐로 지정만**(재생 안 함). 클로드 기본값, gom이 바꿀 수 있음.
- 카트 칸을 누르면 PC 카트 클릭과 같이 실행.

## 연결
- 기본 포트 **49820**(쓰고 있으면 49821~49829). 모든 IPv4 주소에서 받는다. 시험용 환경 변수 `ENQUEUE_REMOTE_LOOPBACK=1`이면 127.0.0.1만(윈도우 방화벽 창이 안 뜸 — 시험은 반드시 이걸로).
- **공유기 IP만 쓴다(gom 10/24 "공유기 아이피가 아니면 '공유기가 필요합니다' 문구")**: IP 후보 = 사설 IPv4(10.0.0.0/8 · 172.16.0.0/12 · 192.168.0.0/16)만. 하나도 없으면(모뎀 직결 공인 IP, 169.254 자동 주소, 연결 없음) PC 창의 QR·주소 자리에 **`공유기가 필요합니다`**를 띄우고 QR은 만들지 않는다.
- QR 내용 = `http://<고른 IP>:<포트>/#k=<키>`. 키 = PC마다 무작위 32바이트(base64url), 이 PC 설정(AppSettings)에 저장. **QR 새로 만들기 = 키 교체** → 연결된 기기 모두 끊김(예전 키로 다시 못 들어옴). IP·네트워크 선택이 바뀌면 주소 부분만 바뀌고 키는 그대로.
- 웹 페이지는 `location.hash`의 `k`를 읽어 `ws://<같은 host>/ws?k=<키>`로 연결한다. 키가 틀리면 서버가 `bye(reason:"key")`를 보내고 닫는다. URL의 `#k=`는 지우지 않는다(홈 화면에 추가해도 키가 남게).
- HTTP 경로: `/` = index.html, `/app.css`, `/app.js`, `/icons/<이름>.svg` — 전부 exe 안에 내장. 그 밖은 404. 응답 머리 `Cache-Control: no-store`(업데이트 뒤 옛 파일 안 쓰게).
- 웹소켓 메시지는 JSON 텍스트 한 덩어리씩.

## 서버 → 웹
| t | 언제 | 내용 |
|---|---|---|
| `hello` | 연결 직후 1번 | `proto:1, server:"<설치 id>", pc:"<컴퓨터 이름>", project:"<프로젝트 이름>", app:"<앤큐 버전>"` |
| `lists` | 연결 직후 + 탭·큐 구조·이름·번호·순서·재생 헤드·모드·활성 탭이 바뀔 때 | `gen:N, active:"<탭 id>", mode:"edit"\|"show", tabs:[{id,name,kind:"list"\|"cart",cols,rows}], cues:{"<탭 id>":[{id,no,name,kind,depth,parent,len,armed,color,slot}]}, playhead:{"<탭 id>":"<큐 id>"\|null}` — `cues`는 표에 보이는 순서(그룹 자식 포함, depth로 들여쓰기), `len`은 PC 길이 칸 글자 그대로(`"0:12.0"`, `"∞"`, `""`), `slot`은 카트 칸 번호(리스트면 null) |
| `state` | 재생·대기 중이면 초당 10번, 아니면 바뀔 때 + 1초마다 | `seq:N, mode, next:{id,no,name,len}\|null, playing:[{id,no,name,remain,elapsed,dur,progress,st:"play"\|"pause"\|"fade",order,fade?,fadeLeft?}], waits:[{cue,no,name,kind:"pre"\|"post"\|"wait",remain,startId}], counts:{playing,paused,waiting}, panic:bool, goLocked:bool` (시간은 초, 소수; `fade`·`fadeLeft` = 정지 페이드 진행 0..1·남은 초, 정지 페이드 중일 때만 있음, 웹 '페이드아웃' 진행바용, 0.17.5) |
| `ack` | 요청마다 | `id, ok:bool, msg?:"<짧은 한국어 이유>"` — ok는 '요청을 받아 처리함'. 실제 재생 여부는 state로 본다 |
| `bye` | 닫기 전 | `reason:"key"\|"off"\|"newkey"` |

## 웹 → 서버
모든 요청에 연결 안에서 1씩 늘어나는 `id`. 서버는 연결마다 처리한 마지막 id를 기억해 **같은 id는 다시 실행하지 않는다**(ack만). 웹은 **응답 없는 요청을 자동으로 다시 보내지 않고, 끊긴 동안 누른 것은 버린다.**

| t | 내용 | 앤큐에서 하는 일(PC와 같은 함수) |
|---|---|---|
| `go` | `id, down:bool, touch:<누름 번호>` | pointerdown=down, pointerup·pointercancel·연결 끊김=up. 키보드·MIDI와 **같은 공용 눌림 추적**(InputActivationTracker)·더블 GO·requireKeyUp |
| `cmd` | `id, cmd:"pause"\|"fade"\|"panic"` | PC 명령(transport.pauseToggle / transport.fadeOutSelected / 전체 페이드 정지)과 같은 대상 규칙. panic은 누를 때마다 하나 → PC 단조 시계로 받은 시각을 **PanicGestureGate**에(500ms 안 두 번 = 즉시 정지) |
| `cue` | `id, op:"pause"\|"resume"\|"stop", cue, order` | PC 활성 큐 카드의 일시정지·재개·×와 같은 함수 |
| `wait` | `id, cue, kind, startId` | PC 대기 카드 ×와 같은 cancelWait |
| `fire` | `id, cue` | PC 카트 클릭과 같은 fire |
| `stopcue` | `id, cue` | PC 카트 오른쪽 클릭과 같은 정지 |
| `tab` | `id, tab` | 활성 탭 바꾸기(PC 화면도 바뀜) |
| `playhead` | `id, tab, cue` | 다음 큐로 지정(재생 헤드 이동, 재생 안 함) |
| `move` | `id, tab, cue, before:"<큐 id>"\|null, parent:"<그룹 id>"\|null` | 큐 순서 바꾸기 = PC 큐 표 줄 끌기와 같은 함수·되돌리기. `before`=이 큐 앞에(null이면 parent의 맨 끝, parent null이면 리스트 맨 끝). 쇼 모드면 `ok:false, msg:"쇼 모드"` |
| `mode` | `id, show:bool` | PC 편집/쇼 토글을 그 상태로 맞춤(토글이 아니라 지정) |
| `ping` | `id` | ack만(다시 붙었는지 확인) |
| `hi` | `id, device:"<기기 · 브라우저>"` | 연결 직후 1번. PC '모바일 연동' 창의 연결된 기기 목록에 쓰는 이름(예: `iPhone · 사파리`, `iPad · 사파리`, `Android · 크롬`). 아이패드 사파리는 맥처럼 자신을 밝히므로 웹이 `navigator.maxTouchPoints`까지 보고 정한다 |

- 설정 창·파일 선택 같은 모달이 PC에 떠 있으면 MIDI처럼 일반 실행은 막고(`ok:false`), 전체 페이드 정지와 GO 떼기는 받는다.
- 앤큐 창이 뒤에 있어도 동작한다.

## 늦게 도착한 조작 막기 (검토 1바퀴 지적 반영, 10/24)
- 서버는 `hello`·`state`에 `now`(서버 단조 시계 ms, 정수)를 넣는다.
- 웹은 받을 때마다 `서버 now − 내 performance.now()`를 기록하고, 최근 20초 안의 값 중 **가장 큰 값**을 시계 차이로 쓴다(전달이 가장 빨랐던 표본).
- 웹은 조작 요청(`go` down·`cmd`(pause·fade)·`cue`·`wait`·`fire`·`stopcue`·`tab`·`playhead`·`move`·`mode`)에 `st` = 사람이 누른 순간의 `performance.now() + 시계 차이`를 넣는다. `go` up·`cmd panic`·`ping`·`hi`에는 넣지 않는다(떼기는 늘 처리, 전체 정지는 늦어도 실행).
- 서버는 `st`가 있고 `서버 now − st > 1500`이면 실행하지 않고 `ack ok:false, msg:"늦게 도착해 취소됨"`. `go` down이 취소되면 같은 touch의 up은 눌림이 없으니 그냥 ack. `st`가 없으면(시계 차이를 아직 모름 등) 기존대로 실행.

## 웹 화면 규칙
- 연결 전·다시 연결 중·state를 받기 전엔 GO와 버튼을 잠그고 위에 `다시 연결 중`. 화면이 숨었다 보이면(visibilitychange·pageshow) ping으로 확인하고 state를 받으면 푼다.
- 화면 글자 = 데이터(큐 번호·이름·탭 이름·PC 이름·프로젝트 이름·시간) + 앤큐 기존 이름(`GO` `일시정지` `재개` `페이드아웃` `전체 페이드 정지` `다음 큐` `활성 큐` `재생 중` `대기` `프리웨이트` `포스트웨이트` `편집 모드` `쇼 모드` `길이`) + 연결 상태(`연결됨` `다시 연결 중` `연결 끊김`) + 오류(`QR을 다시 찍어 주세요`). 설명 문구를 지어 넣지 않는다. 줄표(—, –) 금지.
- 누르는 것 최소 48×48 CSS px. GO는 pointerdown에 바로 보낸다. 전체 페이드 정지는 GO와 떨어진 빨강.
- 글꼴은 기기 기본(시스템) 글꼴, 숫자는 고정폭 + tabular-nums. 아이콘은 `/icons/*.svg`(Phosphor, MIT). 외부 주소 요청 없음.
