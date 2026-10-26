# R11 실행·검토 안내

작업 폴더는 `C:\Users\claude\gocue-webrec-r11`이다. Node 24.13.0, Python 3.12.8,
Playwright 1.59.0, Pillow 12.3.0을 사용한다. npm 설치, C++ 빌드, 외부 CDN은 없다.
제품 파일은 `livemix/webjoin/index.html`, `join.css`, `app.js`뿐이다.
R6의 `guest-client.js`, `signaling.js`, `audio/`는 수정하지 않는다.

## 구현

- fragment를 R6 `parseInvite`로 읽고 `inspect`가 성공한 뒤 `<닉> 님의 합방`과 입력 화면을 연다.
  조회만으로 마이크를 열거나 입장을 요청하지 않는다.
- 닉네임을 입력하고 마이크 목록을 열면 `prepareMicrophone`의 반환 목록으로 선택한다.
  다른 장치를 고르면 그 ID로 다시 준비한다. 목록을 건너뛴 경우 ‘들어가기’에서 기본 마이크를 준비한다.
  준비가 성공한 뒤에만 사용자의 ‘들어가기’에서 `requestJoin`을 호출한다.
- pending/connecting은 같은 대기 화면이다. 취소는 `cancel`과 `dispose` 후 다시 inspect하여 입력 화면으로
  돌아간다. 자동 재입장은 없다. connected에서 자기 ID를 제외한 사람별 A안 줄과 마이크 버튼·미터를 보인다.
- 나가기는 `leave`와 `dispose` 후 화면을 정리하고 로고만 남긴다. 원격 ended/rejected/error는
  연결 화면을 정리하고 승인된 오류 한 줄을 표시한다. pagehide에서도 `dispose`를 호출한다.
- 사람 줄을 만들기 전에 `setPeerVolume(id, 기억값 / 100)`을 부른다. 기존 줄은 유지하므로
  다른 사람의 입퇴장이나 roster 갱신이 남은 슬라이더의 키보드 포커스를 옮기지 않는다.
  `connecting`인 사람에게 ‘연결됨’을 먼저 표시하지 않는다.
- 로컬 키는 `livemix.webjoin.nickname`, `livemix.webjoin.volume.<닉네임>`이다.
  닉네임은 문자열, 음량은 정수 퍼센트 문자열이다. ID가 바뀌어도 닉네임이 같으면 기억한다.
  localStorage getter·읽기·쓰기를 각각 try/catch 안에서 처리한다. 읽을 수 없거나 값이 잘못되면
  닉네임은 빈칸, 음량은 100%다. 쓰기 실패 중에도 현재 슬라이더와 R6 음량 호출은 동작한다.
- 화면 문구는 §10과 시안의 로고·방 제목 및 실제 닉네임/장치명뿐이다. 오류는 code/message를 노출하지 않고
  모두 `초대 주소가 바뀌었거나 방이 닫혔습니다.`로 표시한다. 별도 오류 설명은 만들지 않았다.

시안의 다크 토큰, 간격, 420px 한 열, 44px 슬라이더 조작 영역을 사용한다. 슬라이더는 실제 native range에
시안의 트랙·손잡이를 겹쳐 그리며, 키보드와 스크린리더의 range 의미를 유지한다. 마이크 목록은 같은
다크 부품으로 그린 combobox/listbox다. Enter/Space로 열기·선택, 방향키/Home/End, Escape/Tab 닫기를 지원한다.
들어가기 버튼의 시안 색 `#4c8dff`/흰 글자는 계산 대비 3.20:1이다. 자동 첨부 지침의 4.5:1에 못 미치지만,
확정 시안 우선 지시에 따라 색은 그대로 유지했다. 색 변경 여부는 클로드 검토 사항이다.

계약 차이: §5의 권한 시점을 ‘들어가기’ 외에 **마이크 목록을 여는 명시적 사용자 동작**까지 적용했다.
R6가 권한 성공 뒤 장치 목록을 반환하므로, 입장 요청 전에 이름을 보고 장치를 고를 수 있게 하기 위해서다.
마이크 권한 없는 요청, 자동 권한 요청, 새 버튼/문구는 추가하지 않았다.

## 브라우저 없는 검사

```powershell
node tests/livemix017/r11/unit.mjs tests/livemix017/r11/artifacts/unit-result.json
if ($LASTEXITCODE) { throw 'R11 unit failed' }
node --check livemix/webjoin/app.js
node --check tests/livemix017/r11/fake-guest-client.js
python -m py_compile tests/livemix017/r11/run_browser.py
```

Node는 11개 사례를 검사한다. fragment API, 기본값, 닉네임/음량 재개방, 0/100%, storage 접근/읽기/쓰기 실패,
잘못된 기억값, 독립 닉네임 키, R6 닉네임 제한, 승인 오류 문구, 외부 자원 없는 마크업을 포함한다.
CTest 등록은 `tests/livemix017/R11.cmake`의 `LiveMixWebJoinUiTests`다. R11에 C++ 대상은 없다.

## 실제 페이지의 UI 검사와 비교 캡처

```powershell
python tests/livemix017/r11/run_browser.py --channel chrome --output tests/livemix017/r11/artifacts/chrome
if ($LASTEXITCODE) { throw 'R11 Chrome UI failed' }
python tests/livemix017/r11/run_browser.py --channel msedge --output tests/livemix017/r11/artifacts/edge
if ($LASTEXITCODE) { throw 'R11 Edge UI failed' }
```

채널을 생략하면 Playwright Chromium을 사용한다. 언제나 `headless=True`, `chromium_sandbox=True`이며
실행/new_page를 재시도하지 않는다. 2026-10-26 샌드박스 실행은 Chromium 147.0.7727.15의
`BrowserContext.new_page: TargetClosedError`로 중단됐다. 결과는 `artifacts/browser/result.json`이다.
브라우저 사례는 **실행 0개**, 제품 페이지 캡처도 **0장**이다. 이 상태를 통과로 처리하지 않는다.

실행기는 localhost에서 제품 페이지 자체를 연다. Playwright route로 `guest-client.js`만 가짜 모듈로 바꾸고,
fragment 파서는 실제 R6 모듈을 사용한다. 제품 코드에는 query/localStorage/global 시험 스위치가 없다.
30개 사례의 검사 범위는 다음과 같다.

- inspect → 권한 → 장치 선택 → 요청 순서, 권한 거절/마이크 없음/사용 중이면 요청 0회.
- 취소·재입장, 2명/3명 roster·ID 기준 자기 제외, 사람별 음량의 ID/gain 호출,
  0/100%와 pointer/키보드, 다시 연 페이지에서 다른 ID에 닉네임 기억 적용.
- 마이크 ON/OFF·미터, 나가기·원격 종료·pagehide 정리, storage getter/read/write 실패.
- ① 입력 ② 대기 ③ 연결 ④ 오류의 화면 글자/접근성 문구 허용 목록 및 금지 도메인.
- 폰 390×760, PC 960×600, 320px 폭의 가로 스크롤/중앙 정렬/44px 조작 영역.
- 40자 긴 닉네임, 공백 없는 이름, 긴 장치명과 12개 장치 목록. 방 최대 인원에 따라 상대는 2명이다.
- CSS zoom 1/1.25/1.5로 레이아웃 배율을 검사한다. 이것은 Windows 실제 배율 및 브라우저 UI 배율을
  바꾼 실물 검증과 구분한다. 스크린리더의 실제 음성 출력도 클로드 수동 확인 대상이다.

캡처 파일은 아래 이름으로 생성된다(예시는 Chrome 출력 폴더).

```text
artifacts/chrome/actual/{phone,pc}-{entry,waiting,connected,error}.png
artifacts/chrome/reference/{phone,pc}-{entry,waiting,connected,error}.png
artifacts/chrome/compare/{phone,pc}-{entry,waiting,connected,error}.png
artifacts/chrome/compare/index.html
```

actual/reference는 각각 390×760 또는 960×600이며 compare는 같은 두 장을 **왼쪽 시안 / 오른쪽 실제**로
나란히 붙인다(780×760 또는 1920×600). 비교용 화면은 시안 HTML에서 `.web`만 가져와 원래 CSS로 그린다.
보드 제목·설명·기기 프레임은 제외한다. PC 대기/연결/오류는 같은 시안 `.web`을 PC 크기에 놓은 것이다.
①은 마이크 준비 후 장치명이 표시된 입력 화면이다. 폰은 휴대폰 마이크, PC는 Shure MV7이다.
확정 시안 PNG 원본도 `reference/approved-board.png`로 보존한다.
현재 이 원본만 존재하며 실제/비교 캡처는 샌드박스 밖 실행 성공 후 생성된다.

시안 위치가 달라지면 `--mock <webguest.html 절대경로>`를 지정한다. 원래 common/tokens/components의
상대 디렉터리 구조가 함께 있어야 한다. 서버는 이 5개 시안 파일만 별도 읽기 경로로 제공한다.
결과는 R11 소유 디렉터리 안에만 쓸 수 있다. `artifacts/`, `__pycache__/`는 Git에서 제외한다.

## 실제 오디오/연결 전체 검사 (클로드, 샌드박스 밖)

R11의 가짜 클라이언트 시험은 실제 오디오/연결 성공 증거가 아니다. 기존 R6 전체 실행기를 그대로 재사용한다.
이 작업 폴더에는 네이티브 검사 도구가 없으므로, 기존에 빌드한 `LiveMixWebGuestProbe.exe` 경로를 `$probe`에 넣는다.
R11 작업에서 새 C++ 빌드는 하지 않는다.

```powershell
$probe = 'C:/path/to/LiveMixWebGuestProbe.exe'
python tests/livemix017/r6/run_browser.py --channel chrome --probe $probe --output tests/livemix017/r11/artifacts/r6-chrome
if ($LASTEXITCODE) { throw 'R6 Chrome full run failed' }
python tests/livemix017/r6/run_browser.py --channel msedge --probe $probe --output tests/livemix017/r11/artifacts/r6-edge
if ($LASTEXITCODE) { throw 'R6 Edge full run failed' }
```

`--connection-only`/`--skip-probe`/드리프트 단축 없이 실행한다. R6 README의 44.1/48kHz 각 120초 오디오,
사람별 음량·좌우 분리, Worker/Worklet, HTTP/WebRTC, R1 네이티브 probe까지 포함한다.
이 명령은 R6의 공개 API 검증이며 R11 화면 검증은 위 R11 실행기로 함께 확인한다.
