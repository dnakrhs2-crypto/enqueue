# LiveMix — English listing

| Field | Value |
|---|---|
| Product name | LiveMix |
| Product type | Stream Deck plugin |
| Category | Audio |
| Price | Free |
| Maker / manifest Author | Gomtwigim — match the client's Maker organization before submission |
| Version | 1.0.0 (manifest 1.0.0.0) |
| Suggested tags | LiveMix, microphone, audio, streaming, effects; select relevant available Console tags |
| Support URL | https://곰튀김.com/livemix/#streamdeck-support |

## Description

Control LiveMix microphones and FX from Stream Deck while you stream. Toggle a microphone or all microphones, mute assigned microphone and FX groups, and switch numbered plugin groups without bringing LiveMix to the front.

Adjust FX send amounts with keys on Stream Deck hardware or Stream Deck Mobile. With Stream Deck +, turn a dial to fine-tune the send and short press and release to switch between Pre and Post. Key faces show ON, OFF, mixed and muted states, with clear feedback when LiveMix is offline or a selected channel is missing. The plugin reconnects automatically after either app restarts.

Requires LiveMix 0.6.0 or later and Stream Deck 7.1 or later on the same Windows 10/11 x64 PC. Enable External Control in LiveMix settings before use. A supported ASIO device is needed for audio processing, but not to test the controls. Keys support Stream Deck Mobile through the Windows desktop app; dials require Stream Deck +. The plugin includes English and Korean interfaces. LiveMix itself currently uses a Korean interface.

## Requirements

- Windows 10/11 x64; LiveMix 0.6.0+ and Stream Deck desktop app 7.1+ running as the same Windows user.
- In LiveMix: 설정 (Settings) → 외부 제어 (Stream Deck) → 외부 제어 사용 (Enable external control). This is off by default.
- Stream Deck hardware or Stream Deck Mobile for keys. Mobile connects through the Windows Stream Deck desktop app; it does not run LiveMix independently.
- Stream Deck + for the FX Send Amount dial action.
- A supported ASIO audio device for sound. Control/state testing works while audio is stopped.
- Create microphone/FX channels and plugin groups in LiveMix before selecting them in the plugin.

## Key and dial actions

| Action | What it does |
|---|---|
| Microphone On/Off | Toggle, ON or OFF for a selected microphone; shows its original switch state during group mute. |
| All Microphones | Toggle or explicitly turn all microphone switches ON/OFF; shows how many are ON. |
| Microphone Mute Group | Toggle, mute or unmute microphones assigned to the LiveMix mute group. |
| FX Mute Group | Toggle, mute or unmute assigned FX returns. |
| Plugin Group | Toggle, ON or OFF for an existing numbered group (1–5) on a microphone. |
| FX Send ± | Increase, decrease or set a send amount; shows its current percentage and the configured step/target. |
| Status | Show connection, session or audio state; press to refresh. |
| FX Send Amount — dial | Rotate in 1% or 5% steps; short press and release toggles Pre/Post when enabled. |

## Support and scope

[LiveMix downloads](https://github.com/dnakrhs2-crypto/livemix/releases) · [Setup and support](https://곰튀김.com/livemix/#streamdeck-support).

Controls and state feedback only; no audio meters or audio streaming through the plugin. Plugin groups are independent numbered slots, not exclusive presets. Deleting an earlier group shifts later group numbers. The connection stays on the local PC; no account, cloud service or telemetry is added by this plugin.

Copy only the three paragraphs under Description into the Console description field (1,039 characters including spaces and paragraph breaks). The remaining sections supply requirement/action details and additional links. English is the primary submission copy.
