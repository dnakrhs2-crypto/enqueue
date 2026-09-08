# LiveMix Stream Deck changelog

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
