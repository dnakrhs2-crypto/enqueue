R6 실행·연결 안내

이 디렉터리는 시험 전용이다. GitHub Pages 배포 대상에 포함하지 않는다. R11 화면 파일은 없다.
모든 명령의 작업 폴더는 `C:\Users\claude\gocue-webrec-r6`다. Node 24.13.0, Python 3.12.8,
Playwright 1.59.0에서 준비했다. npm 설치나 외부 JS 의존성은 없다.

```powershell
node tests/livemix017/r6/unit.mjs tests/livemix017/r6/unit-result.json
python tests/livemix017/r6/run_http.py
```

`run_http.py`는 수정하지 않은 R2 `livemix_collab.py --serve`를 빈 localhost 포트에서 시작하고
실제 JS Signaling으로 inspect/request/accepted/signal/leave/cancel/reject를 검사한다.
허용·비허용 Origin의 CORS 응답도 확인한다. 종료 때 자기가 시작한 서버만 정리한다.

검사 도구 구성·빌드·가져오기:

```powershell
python C:/Users/claude/tools/build_server/rb.py --fetch build/vs2022/LiveMixWebGuestProbe_artefacts/Release C:/Users/claude/gocue-webrec-r6 -- '& ./tests/livemix017/r6/build_probe.ps1'
```

현재 rb.py는 `argparse.REMAINDER`를 사용하므로 `--fetch`를 작업 폴더 **앞**에 놓는다.
`build_probe.ps1`은 `--preset local` 구성 뒤 기존 R1 `LiveMixWebGuestProbe`만 빌드한다.
R6 제품·시험 파일은 C++ 대상에 추가하지 않는다.

브라우저 시험은 아래 명령을 샌드박스 밖에서 실행한다. 창을 표시하지 않는다.

```powershell
python tests/livemix017/r6/run_browser.py --channel chrome --output tests/livemix017/r6/artifacts/chrome
python tests/livemix017/r6/run_browser.py --channel msedge --output tests/livemix017/r6/artifacts/edge
```

채널 인자를 생략하면 Playwright Chromium을 사용한다. `chromium_sandbox=True`를 유지한다.
`new_page` 실패를 재시도하거나 sandbox 옵션을 낮추지 않는다. 10/26 이 샌드박스에서는
Chromium 147.0.7727.15가 `BrowserContext.new_page: TargetClosedError`로 종료됐다.

실행기는 다음을 순서대로 검사하고 JSON으로 저장한다.

- 가짜 WAV 마이크 → 실제 AudioWorklet → Worker → mono16 코덱. 실제 track settings와 장치 rate 기록.
- 44.1/48kHz 방마다 120초, +550/−370ppm의 origin 두 개. Worker의 실제 수신 검증·stereo24 디코드와
  Worklet의 실제 두 재생기·ASRC를 통과한다. 출력 뒤 별도 시험 Worklet이 L/R PCM을 채집한다.
- L/R별 다른 톤, 100/50/0%의 한 사람 음량, 다른 사람·상향 불변, 마이크 OFF의 0 샘플과 진행 시계,
  roster 제거와 남은 사람 연속 재생, 창 스레드를 2초 점유해도 직접 오디오 포트가 계속 재생되는지 검사.
- 실제 R2 HTTP와 브라우저 방장 PC로 공개 `createGuestClient()` 흐름을 검사한다. 자동 채널 이전,
  강제 main 중계, 방장의 정상 연결 종료를 각각 실행한다. main 중계의 2초 적체 후 폐기도 검사한다.
- 수정하지 않은 R1 `tests/livemix017/r1/run_browser.py`를 호출해 roomc 네이티브 검사 도구와
  실제 DataChannel에서 방향당 64개 고정 PCM 패킷을 검증한다. 실행기가 SDP/ICE를 중계한다.

R1 검사 도구에는 임의 PCM/roster/origin 둘 입력 옵션이 없다. 따라서 네이티브 검사 도구 시험은
기존 고정 바이트 fixture로 두고, 두 사람·음량·드리프트는 위 브라우저 오디오 주입 시험에서 검사한다.
`--connection-only`는 HTTP/PC 시험만 실행하며, `--skip-probe`는 네이티브 검증이 빠진 부분 실행이다.
전체 통과 증거로 이 두 선택지를 사용하지 않는다. 기본 실행은 약 5~6분이다.

출력은 `artifacts/` 아래에 있고 git에서 제외한다. `unit-result.json`과 OUT의 숫자는 이 PC에서
실행한 결과다. 모의 재생 버퍼·시작 지연은 실제 마이크부터 스피커까지의 종단 지연이 아니다.

R11 연결

`guest-client.js` exports는 계약의 `parseInvite(hash)`, `createGuestClient({onState,onRoster,onMeter,onError})`
두 개다. 반환 객체의 `inspect`, `prepareMicrophone`, `requestJoin`, `setMicEnabled`, `setPeerVolume`,
`cancel`, `leave`, `dispose`를 사용한다. 마이크 준비 성공 후에만 `requestJoin`을 부른다.
마이크 장치 선택은 입장 전 준비 단계에서 한다. 닉네임 저장·문구·DOM은 R11이 맡는다.

시험 전용 의존성 주입은 `createGuestClient({..., testing:{apiBase,iceServers:[],forceRelay,onDiagnostics,onAudioNode}})`다.
URL/query/localStorage로 설정할 수 없고 제품 UI에서는 전달하지 않는다. `onDiagnostics`에는 실제 마이크
설정·AudioContext rate, 채널 협상값·경로, 사람별 재생 통계와 전송 폐기 수만 전달하며 토큰은 없다.
제품 객체에도 최신 마이크/전송/오디오 진단만 유지한다. 모든 사용자 오류 콜백은 `{code}`만 받는다.

메시지 포트·풀

Window에는 PC·AudioContext와 UI 콜백이 있다. Worker에 HTTP·토큰·cursor·packet codec·전송을,
Worklet에 캡처 ASRC와 최대 두 개의 독립 재생기·ASRC·gain·스테레오 합산을 둔다.
Worker↔Worklet은 이전된 MessageChannel로 직접 통신한다. Worklet의 초기화/메시지 처리에서
typed view를 만들고 render에서는 준비된 view·메시지 객체·transfer list를 재사용한다.
MessagePort의 구조화 복사/BigInt 값 처리에 드는 JS 런타임 내부 비용까지 0이라고 주장하지 않는다.

- capture: 120 mono Float32 × 8 = 3,840B. 전송 버퍼: mono16 288B × 8 = 2,304B.
- playback 전송: stereo Float32 960B × 사람당 ceil(rate×0.2/120), 두 사람. 48kHz 153,600B,
  44.1kHz 142,080B. 각 재생기도 동일 크기의 재정렬 슬롯을 별도로 선할당한다.
- 초기 재생 목표 20ms. 고갈 시 fade/재버퍼, 목표는 패킷 단위로 올라가며 최대 120ms다.
- main 중계 수신: 1200B × 16 = 19,200B. 전송/폐기 뒤 반드시 recycleTransport로 반환한다.
- 전송은 SCTP bufferedAmount와 대기 중인 lease를 합쳐 20ms 예산을 검사하고,
  Worklet에서 생성한 createdAtMs가 20ms를 넘은 오디오를 Worker와 main 양쪽에서 버린다.

채널 이전 시점은 [WebRTC 표준](https://w3c.github.io/webrtc-pc/#rtcdatachannel)의 이전 절차를 확인했고,
`ondatachannel` 안에서 send/listener 설치 전 즉시 시도한다. 실제 성공 경로는 실행기 JSON으로 확인한다.
