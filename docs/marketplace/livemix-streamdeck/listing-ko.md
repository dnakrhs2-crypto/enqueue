# LiveMix — 한국어 소개

| 항목 | 값 |
|---|---|
| 제품 이름 | LiveMix |
| 제품 종류 | Stream Deck plugin |
| 카테고리 | Audio |
| 가격 | 무료 |
| 제작자 | Gomtwigim — 제출 전에 Maker 조직 이름과 맞추기 |
| 버전 | 1.0.0 (manifest 1.0.0.0) |
| 태그 후보 | LiveMix, microphone, audio, streaming, effects |
| 지원 URL | https://곰튀김.com/livemix/#streamdeck-support |

## 설명

방송 중 Stream Deck에서 LiveMix의 마이크와 FX를 조작하세요. 개별 마이크나 전체 마이크를 켜고 끄고, 마이크·FX 뮤트그룹과 번호별 플러그인 그룹을 바꿀 수 있습니다. LiveMix 창을 앞으로 가져오지 않아도 됩니다.

Stream Deck 하드웨어와 Stream Deck Mobile의 키로 FX 보내는 양을 늘리거나 줄이고, 원하는 값으로 설정하세요. Stream Deck +에서는 다이얼을 돌려 세밀하게 조절하고 짧게 눌렀다 놓아 프리/포스트를 바꿀 수 있습니다. 키는 ON, OFF, 일부 ON, 뮤트 상태와 함께 미연결·채널 없음도 알려 줍니다. 앱을 다시 실행하면 자동으로 연결됩니다.

같은 Windows 10/11 x64 PC에서 LiveMix 0.6.0 이상과 Stream Deck 7.1 이상이 필요합니다. LiveMix 설정에서 ‘외부 제어 사용’을 켜세요. 실제 소리를 처리하려면 지원되는 ASIO 장치가 필요하지만, 오디오가 멈춰 있어도 제어 기능을 시험할 수 있습니다. Mobile 키는 Windows의 Stream Deck 앱을 통해 작동하며 다이얼은 Stream Deck +가 필요합니다. 플러그인은 한국어·영어를 지원하고 LiveMix 본체 UI는 현재 한국어입니다.

## 요구사항

- Windows 10/11 x64, LiveMix 0.6.0+, Stream Deck 데스크톱 앱 7.1+. 두 앱을 같은 Windows 사용자로 실행합니다.
- LiveMix의 설정 → 외부 제어 (Stream Deck) → 외부 제어 사용을 켭니다. 기본값은 꺼짐입니다.
- 키: Stream Deck 하드웨어 또는 Stream Deck Mobile. Mobile 단독으로 LiveMix를 실행하는 기능은 아닙니다.
- 다이얼: Stream Deck +. ASIO 장치는 실제 오디오 처리에만 필요합니다.
- 마이크·FX 채널, 뮤트그룹 소속과 플러그인 그룹은 LiveMix에서 먼저 만듭니다.

## 키와 다이얼

| 액션 | 기능 |
|---|---|
| 마이크 ON/OFF | 선택한 마이크의 토글 / ON / OFF. 그룹 뮤트 중에도 원래 스위치 상태를 표시합니다. |
| 전체 마이크 | 토글 / 모두 ON / 모두 OFF. 켜진 마이크 수와 일부 ON 상태를 표시합니다. |
| 마이크 뮤트그룹 | 소속된 마이크의 그룹 뮤트 / 해제 / 토글. |
| FX 뮤트그룹 | 소속된 FX 리턴의 그룹 뮤트 / 해제 / 토글. |
| 플러그인 그룹 | 마이크의 기존 번호 슬롯 1–5 중 하나를 ON / OFF / 토글. |
| FX 보내는 양 ± | 키로 보내는 양 증가 / 감소 / 지정 값 설정. 현재 값과 설정된 단계·목표를 표시합니다. |
| 상태 | 연결 / 세션 / 오디오 상태 표시. 누르면 상태를 새로 읽습니다. |
| FX 보내는 양 — 다이얼 | 회전으로 1% 또는 5%씩 조절. 짧게 눌렀다 놓으면 프리/포스트 전환(설정에서 끌 수 있음). |

[LiveMix 다운로드](https://github.com/dnakrhs2-crypto/livemix/releases) · [설치·지원](https://곰튀김.com/livemix/#streamdeck-support).

오디오 미터와 플러그인을 통한 오디오 전송은 제공하지 않습니다. 플러그인 그룹은 각각 독립된 번호 슬롯이며, 앞 그룹을 삭제하면 뒤 번호가 당겨집니다. 연결은 이 PC 안에서 처리하며 플러그인용 계정·클라우드 서비스·사용 통계 전송을 추가하지 않습니다.

Marketplace 기본 제출문은 [영문 listing](listing-en.md)을 사용하고, 한국어 입력란이 제공되면 위 설명을 추가합니다.
