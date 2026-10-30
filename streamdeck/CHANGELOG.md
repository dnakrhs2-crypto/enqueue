# LiveMix Stream Deck changelog

## 1.4.0 - 2026-10-30

### 한국어

- 마이크 전체 뮤트와 FX 전체 뮤트 키를 추가했습니다. LiveMix 위쪽 마이크 뮤트·FX 뮤트 버튼처럼 뮤트그룹 지정 없이 모든 해당 채널을 토글 / 뮤트 / 해제합니다. 키를 뗄 때는 명령을 보내지 않습니다.
- 전체 뮤트와 뮤트그룹은 독립적입니다. 전체 뮤트를 해제해도 원래 뮤트된 그룹은 유지하며 개별 ON/OFF, 보내는 양과 프리/포스트는 바꾸지 않습니다. 기존 마이크·FX·보내기 키와 다이얼도 전체 뮤트를 그룹 뮤트와 같은 방식으로 표시합니다.
- 새 키는 그룹 괄호 없는 마이크·FX 그림, 뮤트 시 빨간색과 사선, 기존 뮤트/해제 문구, 전체 채널 수와 오디오 멈춤 배지를 표시합니다. 설정에는 동작과 표시 이름만 제공하며 다중 동작에서는 설정한 동작만 실행하고 키 그림·상태·제목을 보내지 않습니다.
- 두 새 키에는 LiveMix 0.18.0+의 `muteAll` 기능이 필요합니다. 이전 버전에서는 기존 업데이트 안내와 입력 실패 알림만 표시하고 명령을 보내지 않습니다. `muteAll`이 없는 이전 상태도 읽으며 기존 열 액션의 요구 버전은 유지합니다.
- 실제 SDK를 사용하는 가짜 호스트·LiveMix 테스트에 명령, 독립성, 구버전 호환, ko/en 설정과 정적/실시간 그림 검증을 추가했습니다. 실제 LiveMix 검사 도구는 마이크 전체 뮤트를 두 번 토글해 원래 상태로 복구하고, 미지원 버전은 건너뜁니다.

### English

- Added Mute All Mics and Mute All FX. Toggle, Mute or Unmute every matching channel, like LiveMix's top bar switches, without assigning mute-group marks. Key release sends nothing.
- All-mute and mute groups are independent. Releasing all-mute preserves any muted group and leaves individual ON/OFF switches, send amounts and pre/post intact. Existing mic, FX, send keys and send dials show all-mute just like group mute.
- New keys use plain mic/FX glyphs without group brackets, red with a slash when muted, the existing muted/unmuted labels, total target count and stopped-audio badge. Setup exposes only Mode and Display name. Multi Actions execute Mode without key artwork, state or title.
- These two keys require LiveMix 0.18.0+ and `muteAll`. Older versions show the existing update message and alert on input without commands. Snapshots without `muteAll` still parse; requirements for the existing ten actions are unchanged.
- Extended real-SDK fake-host/LiveMix tests for commands, independence, compatibility, ko/en PI and static/runtime artwork. The real-app checker toggles mic all-mute twice and restores its original state, or skips unsupported versions.

## 1.3.0 — 2026-10-25

### 한국어

- FX ON/OFF 키를 추가했습니다. 선택한 FX 채널 하나를 토글하거나 ON/OFF로 설정하며, 키를 뗄 때는 명령을 보내지 않습니다. 보내는 양과 FX 뮤트그룹은 그대로 유지합니다.
- 마이크 키와 같은 램프에 FX 글자를 표시합니다. ON은 파랑(마이크 키의 초록과 구분), OFF는 회색이며, 뮤트그룹 적용 중에는 빨간 램프와 원래 ON/OFF를 표시합니다. 키 상태는 개별 스위치를 따르고 오디오 멈춤 배지도 유지합니다.
- 설정에는 FX 채널, 동작, 같은 이름 연결, 표시 이름을 제공합니다. UUID 우선 연결, 이름 변경 저장, 새 세션의 유일한 같은 이름 연결과 오프라인 선택 보존을 지원합니다. 다중 동작에서는 설정한 동작을 실행하고 키 그림·상태·제목을 보내지 않습니다.
- 새 키에는 LiveMix 0.16.0+의 `fxOn` 기능이 필요합니다. 이전 버전에서는 업데이트 안내와 입력 실패 알림만 표시합니다. `on`이 없는 이전 FX 상태도 읽으며 기존 아홉 액션의 요구 버전은 유지합니다.
- 실제 SDK를 실행하는 가짜 호스트·LiveMix 테스트와 ko/en PI·렌더링·프로토콜 검증을 추가했습니다. 실제 LiveMix 검사 도구는 FX를 두 번 토글해 원상 복구하며, 미지원 버전이나 FX가 없는 세션은 건너뜁니다.

### English

- Added FX On/Off: toggle one selected FX channel or explicitly set it ON/OFF. Key release sends nothing. Send amounts and the FX mute group remain independent.
- Reuses the microphone lamp with an FX glyph: blue ON (so it never reads as a green microphone key), gray OFF, or red group-muted with Originally ON/OFF. Key state follows the individual switch, with the existing stopped-audio badge.
- Setup offers FX channel, Mode, same-name fallback and Display name. UUID binding, rename persistence, unique matching names across sessions and offline selections follow the existing FX binding rules. Multi Actions execute the configured Mode without key artwork, state or title.
- Requires LiveMix 0.16.0+ and its `fxOn` capability for this key. Older versions show the update message and alert on input without a command. Legacy FX snapshots without `on` still parse; requirements for the existing nine actions are unchanged.
- Added real-SDK fake-host/LiveMix tests and ko/en PI, rendering and protocol checks. The real-app checker toggles an FX twice and restores its switch, or skips when unsupported or no FX exists.

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
