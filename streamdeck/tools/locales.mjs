// Single source for bundled runtime strings, manifest translations and local PI strings.
export const locales = {
  en: {
    action: "Microphone On/Off", tooltip: "Toggle a LiveMix microphone or explicitly set it on or off.",
    connected: "Connected", disconnected: "LiveMix offline", disabled: "Control disabled", checking: "Checking connection",
    version: "Update required", missing: "Channel missing", duplicate: "Duplicate name — select again", audioStopped: "Audio stopped",
    muted: "Mute group", originalOn: "Originally ON", originalOff: "Originally OFF", microphone: "Microphone", mode: "Mode", toggle: "Toggle",
    fallback: "Match the same name when changing sessions", shortTitle: "Display name", choose: "Select a microphone",
    offlineHelp: "Start LiveMix and enable Settings → External control (Stream Deck).",
    disabledHelp: "In LiveMix, enable Settings → External control (Stream Deck).",
    failure: "The command failed. Check the connection and selected microphone.", delayed: "Input delayed", invalidSettings: "Select the microphone and mode again."
  },
  ko: {
    action: "마이크 ON/OFF", tooltip: "LiveMix 마이크를 토글하거나 ON/OFF로 설정합니다.",
    connected: "연결됨", disconnected: "LiveMix 미연결", disabled: "제어 꺼짐", checking: "연결 확인 중",
    version: "업데이트 필요", missing: "채널 없음", duplicate: "이름 중복 — 다시 선택", audioStopped: "오디오 멈춤",
    muted: "뮤트그룹", originalOn: "원래 ON", originalOff: "원래 OFF", microphone: "마이크", mode: "동작", toggle: "토글",
    fallback: "세션을 바꿀 때 같은 이름 연결", shortTitle: "표시 이름", choose: "마이크를 선택하세요",
    offlineHelp: "LiveMix를 실행하고 설정 → 외부 제어 (Stream Deck)를 켜세요.",
    disabledHelp: "LiveMix 설정 → 외부 제어 (Stream Deck)를 켜세요.",
    failure: "조작에 실패했습니다. 연결과 선택한 마이크를 확인하세요.", delayed: "입력 지연", invalidSettings: "마이크와 동작을 다시 선택하세요."
  }
};
