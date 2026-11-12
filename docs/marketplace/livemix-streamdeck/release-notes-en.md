# LiveMix 1.5.0

- Added Plugin Group Windows to open or close the plugin windows of a numbered plugin group, like LiveMix's Plugin group 1–5 windows hotkeys. Choose all mics or one mic, the group and Open/Close, Open or Close, including in Multi Actions.
- The windows come to the front even while LiveMix is minimized, in the tray or behind a game or OBS. Open/Close closes them when one is in use or in front, otherwise opens them or brings them to the front.
- A group without plugins shows an alert on press; a missing group or microphone shows the existing missing feedback.
- This key requires LiveMix 0.22.0+ with External Control enabled. Older versions show an update message and send no window command. Existing action requirements are unchanged.

# LiveMix 1.4.0

- Added Mute All Mics and Mute All FX, matching LiveMix's top bar mute switches without mute-group marks. Choose Toggle, Mute or Unmute, including in Multi Actions.
- All-mute is independent of mute groups and individual ON/OFF switches. Releasing it restores the previous group-muted state. Mic, FX and send controls show all-mute with the existing muted feedback.
- New keys show plain mic/FX glyphs, red slash when muted, total channel count and stopped audio. Setup offers Mode and Display name.
- These two keys require LiveMix 0.18.0+ with External Control enabled. Older versions show an update message and send no all-mute commands. Existing action requirements are unchanged.

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
