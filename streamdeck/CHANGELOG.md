# LiveMix Stream Deck changelog

## 1.2.0 — 2026-09-30

### 한국어

- 키 액션 8개를 Stream Deck 다중 동작에서 사용할 수 있게 했습니다. 일반 목록에는 보이지만 다중 동작 목록에는 없다는 사용자 보고에 따라, 처음 설계에서 명시적으로 꺼 둔 지원 옵션을 수정했습니다.
- FX 보내는 양 다이얼은 제외합니다. 회전이 본체인 다이얼 동작은 순서대로 실행하는 다중 동작에 맞지 않고, 키로 쓰는 몫은 이미 'FX 보내는 양 ±' 키가 맡고 있습니다. Key Logic 지원은 9개 모두 계속 꺼져 있습니다.
- 다중 동작 인스턴스는 키 상태·이미지·제목을 보내지 않으며, 실패 알림 `showAlert`만 기존 초당 10회 호출 예산을 사용합니다. 일반 키 표시와 기존 명령·바인딩·큐 동작은 유지합니다.
- 키 출력의 호출 기록을 20ms 더 유지해 짧은 전달 지연으로 호스트 수신 시각의 초당 10회 예산을 넘는 경계 문제를 보정했습니다.
- PI의 동작(mode) 설정을 그대로 실행합니다. 토글·뮤트·증감·지정 동작은 키의 두 상태와 1:1로 대응하지 않아 호스트의 `userDesiredState`를 사용하지 않습니다. 다중 동작 PI의 기존 안내문에 한국어/영어 설명을 덧붙였습니다.
- 실제 배포물을 실행하는 가짜 호스트 테스트로 무출력, 상태 갱신, 명령 실행, 실패 알림, 일반 키와의 공존 및 ko/en PI 안내를 검증합니다.

### English

- Enabled all eight Keypad actions in Stream Deck Multi Actions. A user report that actions appeared only in the ordinary list exposed the original design's explicit opt-out.
- FX Send Amount remains excluded: a dial whose point is rotation does not fit a sequence of steps, and the existing FX Send ± key already covers keys. Key Logic support remains disabled for all nine actions.
- Multi Action instances send no key state, image or title. Failure alerts still use `showAlert` and the existing 10 calls/second budget. Ordinary rendering, commands, bindings and queue behavior are preserved.
- Key output retains its call history for an extra 20 ms to protect the 10 calls/second boundary against short host-delivery delays.
- The PI Mode setting remains authoritative. Toggle, mute, increase/decrease and set modes do not map one-to-one to two key states, so the host's `userDesiredState` is ignored. The Korean/English PI appends an explanation to its existing notes.
- Built-plugin fake-host tests cover silent output, state refresh, command execution, failure alerts, ordinary keys alongside Multi Actions and localized PI guidance.

## 1.1.0 — 2026-09-09

### 한국어

- 플러그인 그룹 (전체 마이크) 키를 추가했습니다. 같은 번호의 그룹 1–5를 가진 모든 마이크에서 한꺼번에 토글하거나 ON/OFF로 설정합니다. 토글은 하나라도 ON이면 전부 OFF, 전부 OFF이면 전부 ON입니다.
- 그룹 번호와 켜진 그룹 수/전체 수, 모두 ON·모두 OFF·일부 ON을 표시합니다. 설정에는 그룹·동작·표시 이름만 있으며 그룹 선택에 해당 마이크 수를 표시합니다.
- 새 키에는 LiveMix 0.10.0+가 필요합니다. 이전 LiveMix에서는 업데이트 안내를 표시하고 명령을 보내지 않습니다. 기존 여덟 동작은 LiveMix 0.6.0+에서 계속 사용할 수 있습니다.

### English

- Added Plugin Group (All Mics): toggle or explicitly set ON/OFF for numbered group 1–5 across every microphone that has it. Toggle turns all OFF if any are ON, otherwise all ON.
- Shows the group digit, ON/total count and All ON, All OFF or Some ON. Setup exposes group, mode and display name; group choices show the microphone count.
- The new key requires LiveMix 0.10.0+. Older LiveMix versions show an update message and receive no command. The existing eight actions still require only LiveMix 0.6.0+.

## 1.0.0 — 2026-09-08

### 한국어

- 최초 정식 릴리스. 빨간 ON 브랜드 타일과 마이크·그룹·FX·상태 아이콘을 완성했습니다. 정적 SVG와 실행 중 키가 같은 도형과 LiveMix 팔레트를 사용합니다.
- 마이크 뮤트그룹과 FX 뮤트그룹을 서로 다른 실루엣으로 표시합니다. 원래 ON/OFF, 일부 ON, 그룹 번호, 보내는 양과 연결·오디오 상태 표시를 유지합니다.
- 영어·한국어 Marketplace 소개, 릴리스 노트, 심사 안내, 고객 제출 절차와 HTML 미디어 5종을 제공합니다.
- LiveMix 0.6.0+ / Stream Deck 7.1+ / Windows 10/11 x64. 외부 제어는 기본 OFF이며 LiveMix 설정에서 켜야 합니다. ASIO는 오디오 처리에 필요합니다.

### English

- First public release, with the red ON brand tile and a complete microphone, group, FX and status icon family. Static and runtime keys share vector geometry and the LiveMix palette.
- Distinct microphone and FX mute-group silhouettes retain original ON/OFF, mixed state, group number, send amount, connection and stopped-audio feedback.
- English/Korean Marketplace copy, release and review notes, submission instructions and five HTML media compositions.
- Requires LiveMix 0.6.0+, Stream Deck 7.1+ and Windows 10/11 x64. External Control is off by default; enable it in LiveMix settings. ASIO is needed for audio processing.

## 0.9.1 — 2026-09-08

### 한국어

- Mobile을 포함한 키패드에 FX 보내는 양 ±를 추가했습니다. 1/5/10% 증가·감소, 0–100% 값 지정, 현재 비율·모드 배지를 제공합니다.
- 키와 다이얼이 같은 FX 보내기 대기열을 공유하며, 보내는 양 조절은 프리/포스트를 바꾸지 않습니다.
- 고객이 실제 LiveMix 0.6.0과 Stream Deck Mobile에서 동작을 확인했다고 보고했습니다.

### English

- Added FX Send ± for keypad devices, including Mobile: 1/5/10% increase/decrease, a 0–100% set target and current-value/mode feedback.
- Keys and dials share the same send queue; amount changes preserve Pre/Post.
- The client reported successful use with real LiveMix 0.6.0 and Stream Deck Mobile.

## 0.9.0 — 2026-09-08

### 한국어

- 시험판: 마이크·전체 마이크 ON/OFF, 마이크/FX 뮤트그룹, 번호별 플러그인 그룹, Stream Deck + FX 다이얼, 상태 키를 구현했습니다.
- 자동 연결·재연결, ko/en 설정 UI, 미연결·대상 없음 표시와 로컬 제어 테스트를 제공합니다.
- 채널은 저장 UUID를 우선하며 세션 전환 시 정확히 같은 이름이 하나뿐인 경우에만 선택적 연결을 허용합니다. 플러그인 그룹은 번호 슬롯이므로 앞 그룹 삭제 후 선택 번호를 확인해야 합니다.

### English

- Preview with microphone/all-microphone switches, microphone/FX mute groups, numbered plugin groups, Stream Deck + FX dials and a status key.
- Automatic local connection/reconnection, English/Korean setup UI, offline/missing-target feedback and a local control test harness.
- Saved channel UUIDs take priority; optional cross-session fallback needs one exact matching name. Plugin groups use numbered slots; recheck selection after deleting earlier groups.
