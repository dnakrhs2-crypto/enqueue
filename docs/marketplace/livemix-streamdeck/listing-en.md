# LiveMix — English listing

| Field | Value |
|---|---|
| Product name | LiveMix |
| Product type | Stream Deck plugin |
| Category | Audio |
| Price | Free |
| Maker / manifest Author | Gomtwigim — match the client's Maker organization before submission |
| Version | 1.4.0 (manifest 1.4.0.0) |
| Suggested tags | LiveMix, microphone, audio, streaming, effects; select relevant available Console tags |
| Support URL | https://곰튀김.com/livemix/#streamdeck-support |

## Description

Control LiveMix microphones and FX from Stream Deck while you stream. Toggle one microphone, one FX channel or all microphones, mute assigned groups or every mic/FX channel without group marks, and switch numbered plugin groups on one mic or every mic at once without bringing LiveMix to the front.

Adjust FX send amounts with keys on Stream Deck hardware or Stream Deck Mobile. With Stream Deck +, turn a dial to fine-tune the send and short press and release to switch between Pre and Post. Key faces show ON, OFF, mixed and muted states, including all-mute, with clear feedback when LiveMix is offline or a selected channel is missing. All-mute is independent of mute groups and individual switches, so releasing it preserves any muted group. The plugin reconnects automatically after either app restarts.

Requires LiveMix 0.6.0 or later and Stream Deck 7.1 or later on the same Windows 10/11 x64 PC. Mute All Mics and Mute All FX require LiveMix 0.18.0+; FX On/Off requires 0.16.0+; Plugin Group (All Mics) requires 0.10.0+; the other eight actions still support 0.6.0+. Enable External Control in LiveMix settings before use. A supported ASIO device is needed for audio processing, but not to test the controls. Keys support Stream Deck Mobile through the Windows desktop app; dials require Stream Deck +. The plugin includes English and Korean interfaces. LiveMix itself currently uses a Korean interface.

## Requirements

- Windows 10/11 x64; LiveMix 0.6.0+ and Stream Deck desktop app 7.1+ running as the same Windows user.
- Mute All Mics and Mute All FX require LiveMix 0.18.0+; FX On/Off requires LiveMix 0.16.0+; Plugin Group (All Mics) requires LiveMix 0.10.0+. On older versions the affected key shows an update message and sends no command. The other eight actions require 0.6.0+.
- In LiveMix: 설정 (Settings) → 외부 제어 (Stream Deck) → 외부 제어 사용 (Enable external control). This is off by default.
- Stream Deck hardware or Stream Deck Mobile for keys. Mobile connects through the Windows Stream Deck desktop app; it does not run LiveMix independently.
- Stream Deck + for the FX Send Amount dial action.
- A supported ASIO audio device for sound. Control/state testing works while audio is stopped.
- Create microphone/FX channels and plugin groups in LiveMix before selecting them in the plugin.

## Key and dial actions

| Action | What it does |
|---|---|
| Microphone On/Off | Toggle, ON or OFF for a selected microphone; shows its original switch state during group mute or all-mute. |
| FX On/Off | LiveMix 0.16.0+. Toggle, ON or OFF for one FX channel, keeping send amounts. Shows the individual switch during FX group mute or all-mute. Available in Multi Actions. |
| All Microphones | Toggle or explicitly turn all microphone switches ON/OFF; shows how many are ON. |
| Microphone Mute Group | Toggle, mute or unmute microphones assigned to the LiveMix mute group. |
| FX Mute Group | Toggle, mute or unmute assigned FX returns. |
| Mute All Mics | LiveMix 0.18.0+. Toggle, mute or unmute every microphone like the top bar Mic Mute switch, without group marks. Shows total count and mute state. Mode and Display name settings; Multi Actions supported. |
| Mute All FX | LiveMix 0.18.0+. Toggle, mute or unmute every FX channel like the top bar FX Mute switch, without group marks. Shows total count and mute state. Mode and Display name settings; Multi Actions supported. |
| Plugin Group | Toggle, ON or OFF for an existing numbered group (1–5) on a microphone. |
| Plugin Group (All Mics) | LiveMix 0.10.0+. Same numbered group across every mic that has it: any ON → all OFF, all OFF → all ON; explicit ON/OFF modes. Shows group digit, All/Some ON or All OFF and ON/total count. |
| FX Send ± | Increase, decrease or set a send amount; shows its current percentage and the configured step/target. |
| Status | Show connection, session or audio state; press to refresh. |
| FX Send Amount — dial | Rotate in 1% or 5% steps; short press and release toggles Pre/Post when enabled. |

## Support and scope

[LiveMix downloads](https://github.com/dnakrhs2-crypto/livemix/releases) · [Setup and support](https://곰튀김.com/livemix/#streamdeck-support).

Controls and state feedback only; no audio meters or audio streaming through the plugin. Plugin groups are independent numbered slots, not exclusive presets. Deleting an earlier group shifts later group numbers. The connection stays on the local PC; no account, cloud service or telemetry is added by this plugin.

Copy only the three paragraphs under Description into the Console description field. The remaining sections supply requirement/action details and additional links. English is the primary submission copy.
