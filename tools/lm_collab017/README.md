# LiveMix 0.17.5 세 PC 장시간 시험 (10/29)

`soak.py`는 운영 방 서버를 사용하는 여러 PC 실행기다. 제품 소스는 수정하지 않는다.
실행 중 창·알림을 만들지 않고, 원격은 SSH 비대화형 세션만 사용한다.
Python·측정 도구·브라우저 프로세스는 BelowNormal, 브라우저는 headless/소리 끔/GPU 사용 안 함이다.
Chromium이 일부 자식의 우선순위를 올리는 동작도 Windows Job의 BelowNormal 제한으로 막는다.
이 Job에는 종료 동작을 넣지 않으며 프로세스 정리는 기록된 PID로만 한다.
실제 오디오 장치·GPU 부하 시험은 아니다. gom PC의 영상 생성 프로세스는 건드리지 않는다.

```powershell
# HEAD 동기화 + ProbeV2.cpp만 복사. rb.py의 가져오기를 재사용하되
# 스냅샷 커밋/로그인 화면 예약 작업은 사용하지 않는다.
python tools/lm_collab017/build_probe.py
python tools/lm_collab017/test_soak.py -v
python tools/lm_collab017/soak.py prepare

# 요청된 짧은 검증. 출력은 매번 새 폴더여야 한다.
python tools/lm_collab017/soak.py run --cases W3-roomc,W2-relay --seconds 30 --output tools/lm_collab017/artifacts/short-01
python tools/lm_collab017/soak.py cleanup
```

600초 본 시험은 실행자가 별도로 선택한다. 이 구현 검증에서는 실행하지 않았다.

```powershell
python tools/lm_collab017/soak.py prepare
python tools/lm_collab017/soak.py run --seconds 600 --output tools/lm_collab017/artifacts/soak-600-01   # --cases 기본값 = DEFAULT_CASES
python tools/lm_collab017/soak.py cleanup
```

`,`는 순서, `+`는 동시에 실행할 독립된 방이다. 각 방의 방장 코드를 읽는 즉시 Native를 시작하고,
초대 파일이 준비되면 웹을 시작한다. 모든 참가자가 READY가 된 뒤 측정 시작 파일을 전달한다.
각 참가자가 정해진 시간을 측정하는 동안 계속 5초 간격으로 통계를 기록한다. 모두 MEASURED가 된 뒤에야
연결을 해제한다. 웹 캡처는 약 1MB씩 순서대로 올리며, 마지막 flush도 연결 해제 전에 완료한다.
한 방 실패가 다음 방을 막지 않는다. 종료 코드 0은 모든 수치 판정 통과, 1은 시험 실패, 2는 실행 준비 오류다.

| 케이스 | 구성 | 조건 |
| --- | --- | --- |
| W3-roomc | roomc 방장 / local Native / gompc 웹 | 48k, 방장 WAV 4파일 검사 |
| W3-gompc44 | gompc 방장 / local Native(같은 공유기) / roomc 웹 | 44.1k 방 |
| N3 | roomc 방장 / local Native / gompc Native | Native 세 명 |
| W2-relay | roomc 방장 / local 웹 | 웹 TURN 강제, 실제 선택 후보로 확인 |
| W3-churn | roomc 방장 / local Native / gompc 웹 | 측정 240초에 웹 퇴장, 300초에 같은 초대로 재입장 요청 |
| RTT-native | roomc 방장 / gompc 반사 Native | 방장 콜백 시계로 표시음 왕복 |
| RTT-web | gompc 방장 / roomc 반사 웹 | 방장 콜백 시계로 표시음 왕복 |
| W3-office-out | gompc 방장 / roomc Native / roomc 웹 | 사무실 방장에 바깥 LiveMix — 0.17.6 이상 참가자만 연결 |
| N2-punch | roomc 방장(UPnP 끔) / gompc Native | 펀칭 방 |

★10/29 실측 함정: 이 PC는 roomc 사설망(192.168.1.0/24)을 'gn' 터널로 간다 → 웹과 방장을 local·roomc에 나눠 두면
사설 주소끼리 터널로 붙을 수 있어 실제 인터넷 경로가 아니다(중계 강제만 예외, `load_cases`가 경고). 사무실 공유기는
바깥에서 먼저 온 흐름을 막아, 0.17.5 이하 참가자는 사무실 방장에게 바깥에서 못 들어온다. 크롬 가짜 마이크 파일은
실행보다 길게(되감기 틈 = 끊김), 웹 목소리는 시계 차가 1,500ppm 넘을 수 있어 끊김은 `continuity()['compensated']`로 본다.

`--case-file extra.json`은 아래 형태의 케이스 사전을 추가하거나 덮어쓴다. PC는 local/roomc/gompc,
방은 총 2~3명, 웹은 최대 한 명이다. 짧은 churn 시험은 `churnAt`/`churnGap`을 명시적으로 줄여야 한다.

```json
{
  "Quick-churn": {
    "host": "roomc", "native": ["gompc"], "web": "local",
    "wireRate": 48000, "churnAt": 8, "churnGap": 6
  }
}
```

`prepare`는 endpoint 의존 파일 23개와 Probe만 보낸다. `tests/livemix017/r6`에서는 HTTP 도우미 두 파일만,
`livemix/webjoin`에서는 실제 클라이언트의 JS 파일만 읽어 복사한다. 설치본과 SHA256이 같으면 보내지 않는다.
패키지/브라우저 버전과 실제 headless 페이지 열기를 확인한다. 외부 패키지를 자동 설치하지 않는다.
원격 방화벽은 고정 Probe 경로의 `LiveMixSoak` 그룹만 만들고 Chrome 규칙은 만들지 않는다.

local Probe는 방화벽 허용 경로
`C:/Users/claude/gocue/build/vs2022/LiveMixCollabProbe_artefacts/Release/LiveMixCollabProbe.exe`에서만 실행한다.
처음 교체할 때만 `.exe.bak_0175`를 만들고 이후 백업은 보존한다. `restore` 또는 `cleanup`으로 복원한다.
작업 폴더 밖 쓰기가 금지되거나 Chromium이 `TargetClosedError`로 차단되는 샌드박스에서는 local 준비가 실패한다.
이 경우 `prepare/run/cleanup`을 일반 PowerShell에서 실행해야 한다. 샌드박스 설정을 약화시키거나 다른 실행 경로로 우회하지 않는다.

결과는 `<output>/<case>/<role>@<pc>/result.json`, `timeline.json`, `endpoint.log` 및 하위 Probe/웹 로그에 모인다.
요약은 `summary.json`과 한국어 `summary.md`다. CPU는 한 코어=100% 및 전체 논리 코어 비율을 기록한다.
Native RTT/버퍼/손실은 제품 snapshot 값이며, 웹 개인 손실률은 누적 lost/(lost+received)이고 별도 이름으로 표시한다.
웹 ICE RTT와 표시음 왕복은 다른 측정값이다. 없는 수치는 미측정으로 남긴다.

끊김 판정은 기존 10ms 분석 그대로다. 레벨 저하·무음·고레벨·위상 점프를 모두 보존하고 분당 횟수만 추가했다.
위상 점프가 연속 검출된 길이는 동일 길이의 무음을 뜻하지 않는다. `phaseJumpWindows`와 `quietWindows`를 함께 확인한다.
churn의 웹 부재도 실제 수치로 남기며, 남은 사람은 부재·재입장을 포함한 전체 측정 구간을 따로 검사한다.

초대·토큰·TURN 주소/자격 증명은 요약/로그에 쓰지 않는다. 초대는 제어 파일로 전달하고 종료 시 삭제한다.
프로세스 정리는 `owned.json`에 기록한 PID와 생성 시각을 확인한 뒤 그 PID의 트리만 종료한다.
`run`은 수집 후 raw capture/녹음을 삭제한다. `--keep-audio`면 각 PC 실행 폴더에 남기며,
이 경우에도 `cleanup`은 원격 실행 폴더 전체와 방화벽 그룹을 정리한다. 원격 venv는 보존한다.

현재 검증 기록과 제한은 [OUT-soak.txt](OUT-soak.txt),
빌드 동일성은 `artifacts/build-verification.json`, 분석 동등성은 `artifacts/analysis-equivalence.json`을 본다.

---

# 기존 LiveMix 0.17 R9 종단 시험

작업 폴더: `C:\Users\claude\gocue-webrec-r9`, 시작 `c87792da`.
앱 코드를 수정하지 않는 검사 도구다. 기존 CollabProbe의 `--host` / `--join` / `--role measure|reflect`는 그대로다.
새 모드는 `LiveMixCollabProbe --v2 CONFIG.json`, 독립 파일 디코드는 `--decode INPUT OUTPUT.f32`다.

Python 3.12.8, NumPy 2.3.5, Playwright 1.59.0 환경에서 작성했다.
누락된 패키지만 `python -m pip install -r tools/lm_collab017/requirements.txt`로 준비한다.
기본 Chromium이 없으면 `python -m playwright install chromium`, 설치된 Chrome/Edge는 `--channel chrome` / `--channel msedge`로 선택한다.
출력 폴더는 **새 경로**여야 한다. 기존 실행 폴더를 덮어쓰거나 삭제하지 않는다.

실측 인계는 [OUT.txt](OUT.txt), 숫자를 보존한 요약은
[`tests/livemix017/r9/e2e-results.json`](../../tests/livemix017/r9/e2e-results.json),
빌드·회귀 결과는 [`verification.json`](../../tests/livemix017/r9/verification.json)이다.
최초 실행의 원본 `result.json`은 보존했다. 주파수 드리프트를 반영한 최종 사후 분석은 같은 폴더의
`analysis-current.json`이며, 처음 잘못 검출한 음량 저하는 최종 결함 집계에서 제외했다.

## 구성과 실행 경계

| 파일 | 역할 |
| --- | --- |
| `ProbeV2.cpp` | 실제 `MixEngine::renderBlock`, `CollabSession`, UDP/WebRTC, `MixEngine::getRecorder()`를 사용한다. 장치를 열지 않는다 |
| `run.py` | 프로세스별 임시 프로필·링, R2 방 서버, 네 가지 조합과 별도 v1 호환, 입퇴장, 음량, 녹음, CPU 실행 |
| `browser.py`, `browser.mjs`, `observer.js` | R6 `createGuestClient()`를 실제 헤드리스 브라우저에서 사용. 가짜 WAV 마이크와 실제 Worklet 출력 수집 |
| `audio.py` | 사람별 톤, L/R 누설, 레벨·위상 끊김, 표시음 왕복, PCM24 시작 위치 검사 |
| `recordings.py` | WAV PCM24와 MP3 실제 파일 검사, 모든 part의 길이·합·원음·표시음 비교 |
| `endpoint.py` | 이 PC와 roomc에서 각각 실행하는 실제 두 PC 측정 |
| `regression.py` | 수정하지 않은 `LiveMixTransportProbeTests`를 실제 VST2/VST3 fixture로 실행. opt-in 생략은 통과로 세지 않음 |

모든 자식은 `CREATE_NO_WINDOW`, 브라우저는 headless + `chromium_sandbox=True` + `--mute-audio`다.
추가 Worklet 출력도 항상 0이다. Native는 JUCE 메시지 스레드만 초기화하고 AudioIODevice/앱 창을 만들지 않는다.
프로세스 종료는 시작한 `Popen` 핸들/PID만 사용한다. 이름으로 종료하지 않는다.
사용자 설정·문서·녹음 폴더는 사용하지 않으며, 실행 폴더 안 `profile/`과 `recordings/`만 사용한다.
R6 코드는 import로 소비하고 바이트코드도 R6 폴더에 쓰지 않는다.

각 Native의 `environment.json`에 다음 환경을 남긴다. 앱과 OBS 플러그인의 계약 §8 이름과 같다.

```text
LIVEMIX_ALLOW_MULTI=1
LIVEMIX_COLLAB_LOOPBACK=1
LIVEMIX_COLLAB_SERVER=http://127.0.0.1:<owned-server-port>
LIVEMIX_OBS_RING=Local\LiveMixR9.<uuid>
LIVEMIX_OBS_PEER_RING=Local\LiveMixR9.<uuid>.Peer
LIVEMIX_OBS_PEER2_RING=Local\LiveMixR9.<uuid>.Peer2
```

두 상대 링은 `lm_obs_protocol.h`의 실제 공유 메모리 ABI로 읽는다. OBS 프로그램/플러그인을 띄우는 시험은 아니다.
`getPeerRingNames()`의 실제 해석 결과도 JSON에 남긴다. 생산 링 이름이면 새 검사 모드는 실행을 거절한다.
웹 초대는 버리는 `host/invite.txt` 제어 파일로만 전달하고 정상 종료 때 지운다. 관리/연결 토큰은 결과 JSON에 싣지 않는다.

## 구성·빌드

모든 명령은 위 작업 폴더에서 PowerShell로 실행한다. `rb.py`의 `--fetch` / `--local`은 작업 폴더 **앞**에 놓는다.

```powershell
python C:/Users/claude/tools/build_server/rb.py --fetch build/vs2022/LiveMixCollabProbe_artefacts/Release --fetch build/vs2022/tests/EnqueueTests_artefacts/Release C:/Users/claude/gocue-webrec-r9 -- '& ./tools/lm_collab017/build.ps1'
```

`build.ps1`은 매번 `--preset local` 구성부터 하고 `LiveMix EnqueueTests LiveMixCollabProbe`를 빌드한다.
CMake의 stderr 경고를 PowerShell 예외로 바꾸지 않고 각 명령의 종료 코드를 확인한다.
roomc 빌드 자리가 30분 이상 막힌 경우에만 같은 명령의 `--fetch ...` 대신 `--local`을 사용한다.
이 PC의 소리 행렬·CPU 측정은 아래처럼 `rb.py --local`로 실행해야 한다. roomc에서 실행한 CPU를 5600X 결과로 쓰지 않는다.

## 한 PC 전체 실행

```powershell
python C:/Users/claude/tools/build_server/rb.py --local C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/run.py --output tools/lm_collab017/artifacts/full-01 --seconds 8; if ($LASTEXITCODE) { throw ''R9: see summary.json'' }'
```

전체 실행은 순차 실행이다. 서로 다른 사용자의 앱을 건드리지 않는다. `summary.json`의 각 case가 `passed`여야 전체 통과다.
`failed`, `blocked`, 누락된 숫자는 통과로 바꾸지 않는다. 종료 코드 0=전체 통과, 1=실패/차단이다.

| case | 실제 구성/관측 |
| --- | --- |
| `nnn` | 방장 + Native 2명. 모든 모니터·상대 링, 세 번째 참가자의 퇴장·재입장, 남은 사람의 연속 재생·자리 유지 |
| `nnw` | 방장 + Native + 실제 R6 웹. 웹의 퇴장·새 ID 재입장, 남은 Native 경로 유지 |
| `nw` | 방장 + 실제 R6 웹. 양방향·자기 소리 없음·재입장 |
| `nn` | 신형 둘. v2와 사람별 모니터 음량 |
| `v1` | 신형 방장 + 기존 `CollabRoom::join`/v1 `CollabTransport`만 쓰는 0.16.0 흉내 참가자. 확장 HTTP와 v2 configure를 호출하지 않음 |
| `native-rtt`, `web-rtt` | 별도의 두 사람 방. 참가자가 들은 PCM을 반사하고 방장 시계에서 표시음 왕복을 측정 |
| `record-44100`, `record-48000`, `record-96000` | Native 3명 + WAV 44.1/48/96kHz. 48kHz에서 MP3 128/256/320, 96kHz 장치에서 MP3 320의 48kHz 변환도 검사 |
| `record-late-seat` | 대기방에서 녹음을 먼저 시작. 뒤에 온 B의 이름 변경, 끝까지 빈 C 제거 |
| `record-split` | 실제 엔진 녹음, 전체 WAV 300,000바이트 한도. 모든 part를 이어 붙여 프레임 누락·중복·D 합 비교 |
| `record-web` | 대기방 녹음 후 Native와 웹 입장. 웹 파일의 L=R 및 실제 세 사람 A/B/C/D 검사 |
| `checkpoint-wav`, `checkpoint-mp3` | 엔진 녹음 체크포인트 뒤 자기 호스트 PID만 강제 종료. 마지막 완료 길이까지 파일 열림·MP3 실제 디코드 확인 |

필요한 범위만 실행할 수도 있다. 동일 출력 이름을 재사용하지 않는다.

```powershell
python C:/Users/claude/tools/build_server/rb.py --local C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/run.py --case nnn --output tools/lm_collab017/artifacts/nnn-01'
python C:/Users/claude/tools/build_server/rb.py --local C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/run.py --suite recordings --seconds 20 --output tools/lm_collab017/artifacts/record-cpu-01'
python C:/Users/claude/tools/build_server/rb.py --local C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/run.py --suite rtt --seconds 60 --output tools/lm_collab017/artifacts/rtt-01'
python C:/Users/claude/tools/build_server/rb.py --local C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/run.py --suite matrix --wire-rate 44100 --output tools/lm_collab017/artifacts/matrix-44100-01'
```

### 샌드박스 브라우저 제한

2026-10-26 이 샌드박스에서 Chromium `new_page`가 **2.235초 뒤 TargetClosedError**로 종료됐다.
증거는 `tests/livemix017/r9/browser-preflight.json`이다. 옵션을 낮추거나 재시도하지 않았다.
같은 샌드박스에서 Native/녹음만 계속 측정하려면 실제 차단 결과를 명시적으로 전달한다.

```powershell
python C:/Users/claude/tools/build_server/rb.py --local C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/run.py --browser-blocked tests/livemix017/r9/browser-preflight.json --output tools/lm_collab017/artifacts/sandbox-01'
```

이 경우 웹 case 네 개는 `blocked`이며 전체 성공으로 집계하지 않는다.
클로드는 **샌드박스 밖에서 `--browser-blocked` 없이** 전체 명령을 돌린다. Chrome을 쓸 때만 `--channel chrome`을 추가한다.
별도 최초 브라우저 검사 명령은 `python tools/lm_collab017/browser.py --output <새 폴더>`다.

## 숫자와 판정

Native 장치는 고해상도 타이머로 대신한다. 기본 48kHz/128프레임, MMCSS `Pro Audio`, 플러그인 없는 stereo 입력 한 채널이다.
호스트 L/R 700/1100Hz, Native 1은 1700/2300Hz, Native 2는 2900/3100Hz, 웹 mono는 3700Hz다.
각 사람은 500ms마다 2ms Hann 표시음(6000/8000/10000/12000Hz)도 보낸다. 기존 구형 측정 모드의 1kHz+3kHz 표식은 보존했다.

- 연결: 요청 시작부터 실제 `connected`까지 `connectMs`. 웹은 실제 마이크 준비·수락·DataChannel·roster 흐름이다.
- 행렬: 1초 Hann 스펙트럼에서 **각 구간의 ±5Hz peak를 먼저 구하고** 중앙값을 낸다. 기대한 톤은 0.084..0.156(0.12의 70..130%), 없어야 할 톤 <0.0024, stereo 역채널 분리 ≥35dB.
- 끊김: 기존 Probe와 같은 10ms 창. 톤 레벨이 기준 70..130% 밖, 무음 <10%, 정상 레벨 창 사이 위상 점프 >0.35rad. 창 수와 연속 사건 수·최장 길이를 모두 저장한다.
- 입퇴장: 3초 안정화 후 정상 구간을 판정하며, **남은 사람은 퇴장 전부터 재입장 후까지 전체 구간**을 검사한다. 전환 구간을 빼고 통과시키지 않는다.
- 음량: 각 상대를 100/50/0%로 변경. 비율 허용 오차 0.08. 다른 상대·다른 수신자·Native OBS 링의 원음 불변도 검사한다.
- 왕복: 방장 송신과 반사 수신의 표시음 위치를 같은 콜백 프레임 시계에서 뺀다. 중앙값·p95·최소·최대·각 표본을 저장한다. 500ms 주기의 혼동을 피하려고 유효 범위는 0..450ms다. 장치·스피커·헤드폰 지연은 포함하지 않는다.
- WAV: RIFF PCM24, 장치 rate, 실제 프레임 수·비트레이트·표시음. A의 상관으로 시작을 찾은 뒤 A/B/C 모두 같은 위치의 콜백 원음/실제 OBS 링과 **모든 정수 샘플을 비교**한다. D는 넓은 정수로 A+B+C를 합하고 PCM24 범위로 포화한 결과와 모든 샘플을 비교한다.
- MP3: 모든 MPEG 프레임의 rate·bitrate·stereo·완전한 끝을 확인하고 JUCE decoder로 전체 디코드한다. 동일 길이, encoder padding을 허용한 길이 오차 <150ms, 사람별 톤과 D의 같은 표시음 위치(5ms 이내)를 검사한다. 손실 압축 결과의 샘플 동등성을 주장하지 않는다.
- 분할: R5의 공개 `Recorder::TestHooks`로 작은 한도를 넣는다. 장치를 준비하기 전 idle `getRecorder()` 인스턴스를 동일 타입으로 재구성한 뒤 **엔진의 실제 renderBlock → Recorder::push 경로**를 사용한다. 앱 코드/4GB 기본 한도는 수정하지 않았다.
- CPU: 녹음 시작부터 멈춤 요청까지 **호스트 프로세스 전체**의 `GetProcessTimes` 차이. `cpuPercentOneCore`는 코어 하나=100%, `cpuPercentMachine`은 논리 CPU 수로 나눈 값이다. 모델·논리 CPU 수·wall/CPU seconds·writer CPU도 남긴다. 프로브의 신호 생성·링 수집·상태 JSON 비용도 포함하며 UI·VST·실제 ASIO/WASAPI 비용은 포함하지 않는다. 별도 성능 임계값은 만들지 않았다.

`lateBlocks`는 검사 타이머가 블록 시간 이상 늦은 횟수다. `ringReadErrors`는 공유 메모리 수집 실패다.
둘을 앱의 네트워크 패킷 손실로 바꿔 부르지 않는다. 같은 PC의 다른 작업 부하는 통제하지 않으므로 측정 조건과 함께 읽는다.

## 결과 파일

```text
<output>/summary.json                         전체 case 성공/실패/차단, 정확한 실행 명령
<output>/<case>/result.json                   행렬·음량·끊김·왕복 또는 녹음 검사와 CPU
<output>/<case>/run.json                      분석 전 실행 상태, 단계의 시작/끝 프레임
<output>/<case>/<host|n1|n2|web>/result.json    실제 endpoint 메타데이터, recorder snapshot, 연결 숫자
<output>/<case>/<host|n1|n2>/environment.json  그 프로세스와 OBS가 사용할 링/방 서버 이름
<output>/<case>/<host|n1|n2>/capture.f32       LE float32 8ch: 내 마스터 L/R, 상대 모니터 L/R, 상대 링1 L/R, 링2 L/R
<output>/<case>/web/capture.f32               실제 Worklet 출력 LE float32 stereo
<output>/<case>/web-before/                  웹 재입장 전 별도 프로필/캡처
<output>/<case>/host/recordings/<take>/      실제 WAV/MP3 (검사 뒤에도 보존)
<output>/<case>/host/decoded-*.f32            MP3 독립 디코드 결과
```

결과를 지우는 명령은 실행기에 없다. raw capture는 메모리에 수집한 뒤 정상 종료 때 저장한다.
호스트 CPU 구간에 Python FFT/파일 사후 분석은 실행하지 않는다. 강제 종료 case는 체크포인트 파일 검사만 하고 raw capture를 기대하지 않는다.

## 회귀·분석기 시험

```powershell
python C:/Users/claude/tools/build_server/rb.py --local C:/Users/claude/gocue-webrec-r9 -- 'python tests/livemix017/r9/test_analysis.py; if ($LASTEXITCODE) { throw ''R9 detector tests failed'' }'
python C:/Users/claude/tools/build_server/rb.py --local C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/regression.py --output tools/lm_collab017/artifacts/regression-01; if ($LASTEXITCODE) { throw ''R9 regression failed'' }'
python C:/Users/claude/tools/build_server/rb.py --local C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/regression.py --suite collab --output tools/lm_collab017/artifacts/collab-check-01; if ($LASTEXITCODE) { throw ''R9 Collab regression failed'' }'
```

실제 VST2/VST3 fixture 기본 위치는 `C:/Users/claude/tools/claude_harness/transport_probe/build/TransportProbe_artefacts/Release`다.
다르면 `--fixture`를 준다. 기존 `tests/LiveMixTransportProbeTests.cpp`는 수정하지 않았다.
`--suite collab`은 기존 native 합방 시험만 선택하며 창 배치 시험을 제외한다. 기존 engine 시험은 한 프로세스에 두 고유 링을
직접 만들므로 이 시험에서만 peer override를 비우고, master base는 시험 전용 이름을 유지한다.
실패 항목만 재현하려면 `--filter ''LiveMix Collab Session''`처럼 지정한다. `rb.py` 바깥 인용부호 안에서 작은따옴표는 두 번 쓴다.
`tests/livemix017/R9.cmake`는 `LiveMixR9Analysis`만 추가한다. 전체 GUI CTest를 이 PC 기본 바탕화면에서 실행하지 않는다.

## 이 PC ↔ roomc: R12 명령

두 PC에 같은 브랜치의 Probe를 빌드한다. R12가 실제 LAN 주소를 아래 `<이PC-IP>`에 넣고, 동시에 돌아가는 빌드가 끝난 뒤 측정한다.
방 서버는 host 명령이 소유하여 띄우고 종료한다. 자동 방화벽 변경은 하지 않는다.

이 PC에서 호스트(2명 왕복 60초):

```powershell
python C:/Users/claude/tools/build_server/rb.py --local C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/endpoint.py host --bind <이PC-IP> --rtt --seconds 60 --output tools/lm_collab017/artifacts/lan-host-01'
```

출력의 `PROBE_CODE 1234`를 사용해 roomc에서 참가자(방장보다 5초 길게 유지):

```powershell
python C:/Users/claude/tools/build_server/rb.py C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/endpoint.py native --server http://<이PC-IP>:8897/api/livemix/collab/ --code 1234 --reflect --seconds 65 --output tools/lm_collab017/artifacts/lan-guest-01'
```

세 사람 행렬+녹음은 호스트에서 `--rtt` 대신 `--people 3 --record wav --seconds 60`을 사용하고,
roomc에 `--reflect` 없는 Native를 `--voice 1` / `--voice 2`, 서로 다른 출력 폴더로 두 개 띄운다.
웹 조합은 호스트에 `--web`을 추가하고, 출력에 안내한 `host/invite.txt`를 roomc의 **버리는 폴더**로 복사하여 다음 명령을 쓴다.

```powershell
python C:/Users/claude/tools/build_server/rb.py C:/Users/claude/gocue-webrec-r9 -- 'python tools/lm_collab017/endpoint.py web --server http://<이PC-IP>:8897/api/livemix/collab/ --invite-file <버리는폴더>/invite.txt --seconds 65 --output tools/lm_collab017/artifacts/lan-web-01'
```

웹 왕복만 재려면 호스트 `--web --rtt`, 웹 참가자 `--reflect`다. 두 Native 중 구버전 흉내는 참가자 `--legacy`다.
두 PC용은 endpoint 각각의 `result.json`을 모아 판정한다. 한쪽 성공만으로 상대 PC도 성공했다고 하지 않는다.
endpoint 모드는 정상 연결 상태의 측정이며, 입퇴장·모든 사람 음량 조작은 한 PC `run.py`가 담당한다.
기존 20분 왕복 측정 명령도 유지한다:

```powershell
# 각각 rb.py를 통해 두 PC에서 실행. 오디오 장치/창 없음.
./build/vs2022/LiveMixCollabProbe_artefacts/Release/LiveMixCollabProbe.exe --host --seconds 1200 --out <새폴더>/host.json
./build/vs2022/LiveMixCollabProbe_artefacts/Release/LiveMixCollabProbe.exe --join 1234 --seconds 1205 --out <새폴더>/guest.json
```

앱 결함이 나오면 해당 case의 재현 명령과 JSON 수치만 인계한다. 이 디렉터리에서 앱/부품 코드를 고치지 않는다.
