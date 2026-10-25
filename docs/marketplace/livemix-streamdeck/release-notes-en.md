# LiveMix 1.3.0

- Added FX On/Off to toggle one FX channel or explicitly set it ON/OFF, including in Multi Actions. Send amounts and the FX mute group remain independent.
- The FX lamp shows ON/OFF, original switch state during group mute and stopped audio. Setup offers FX channel, Mode, same-name fallback and Display name, with saved selections preserved offline.
- This key requires LiveMix 0.16.0+ with External Control enabled. Older versions show an update message and send no FX switch command. Plugin Group (All Mics) still requires 0.10.0+; the other eight actions still support 0.6.0+.

# LiveMix 1.1.0

- Added Plugin Group (All Mics), a key that switches a numbered plugin group (1–5) across every microphone that has it. Toggle turns all OFF if any are ON, otherwise all ON; explicit ON and OFF modes are also available.
- The key shows the group number, All ON / All OFF / Some ON and the ON/total count. Setup includes group, mode and display name, with microphone counts in the group list.
- This new key requires LiveMix 0.10.0+ with External Control enabled. Older versions show an update message for this key. The other eight actions still support LiveMix 0.6.0+.

# LiveMix 1.0.0

Initial public release for Stream Deck.

- Control individual microphones and all microphones, microphone and FX mute groups, and numbered plugin groups.
- Adjust FX sends with keys on Stream Deck hardware and Stream Deck Mobile. On Stream Deck +, rotate a dial to change the amount and short press and release to toggle Pre/Post.
- New LiveMix ON branding and distinct action icons, with ON/OFF, mixed, group mute, offline, missing-target and stopped-audio feedback.
- English and Korean plugin interfaces, automatic reconnection and optional exact, unique name matching across sessions.

Requires LiveMix 0.6.0+ with External Control enabled and Stream Deck 7.1+ on Windows 10/11 x64. LiveMix's own interface is Korean. ASIO hardware is needed for audio, not for testing the controls. Dial controls require Stream Deck +. Audio meters are not included.
