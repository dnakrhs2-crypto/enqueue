# Reviewer guide — LiveMix 1.0.0

Plugin UUID: `com.gomtwigim.livemix`. Manifest: `1.0.0.0`.
Windows 10/11 x64; Stream Deck 7.1+; LiveMix 0.6.0+.

## Install and connect

1. Install the Windows Stream Deck desktop app (7.1 or later). Use Stream Deck hardware or pair Stream Deck Mobile with this PC for key tests. The FX Send Amount action needs a **Stream Deck + dial**.
2. Open [LiveMix releases](https://github.com/dnakrhs2-crypto/livemix/releases), find **LiveMix 0.6.0** (or a later LiveMix app release), expand Assets and download its Windows installer. Select the LiveMix installer, not a Stream Deck plugin archive or source-code ZIP. Run the installer and launch LiveMix. No paid service or plugin account is needed.
3. Run both desktop apps as the same Windows user. LiveMix's own UI is **Korean**; the translations below identify the Korean controls and do not imply an English LiveMix UI.
4. In LiveMix, open **설정** (Settings; choose **설정...** in that menu). Under **외부 제어 (Stream Deck)** (External control), turn on **외부 제어 사용** (Enable external control). It is off by default. No port or token needs to be entered.
5. Install the supplied `.streamDeckPlugin` by opening it in Windows and following Stream Deck's installation dialog. For a Marketplace-processed test, install the processed package supplied by Maker Console. In Stream Deck's action list, expand **LiveMix**.
6. Drag **Status** onto a key. With External Control enabled, its default Connection display should read **Connected**. Drag other actions to keys/dials and configure them in the property inspector beneath the device canvas. Changes save immediately.

**An ASIO device is needed only to hear audio, not to test these controls.** With no ASIO device selected, LiveMix can show **오디오 멈춤** (Audio stopped); microphone/group/send settings still change and synchronize. The key pause badge reports stopped audio separately from a working control connection. No VST installation is required to test group slot switches or send percentages; use an effect and ASIO device if evaluating the audible result.

## Prepare a disposable session

Use a new or disposable LiveMix session so the checks do not alter a working mix.

1. Use **+ 마이크 채널 추가** (Add microphone channel) to create three microphones. Double-click their names and use **Host**, **Guest** and **Spare**. English user-defined names make comparison easier; the app controls remain Korean.
2. Open the **FX** panel and use **+ FX 채널** (Add FX channel). Rename one FX to **Hall**. Each microphone card's **이펙터** section exposes its send amount and **프리 / 포스트** (Pre / Post).
3. Enable the **뮤트그룹** (Mute group) chip on Host and Spare. In the FX panel, enable the **뮤트그룹** chip for Hall. These chips assign membership; they are separate from the active group mute switch.
4. On Host, choose **플러그인 그룹** (Plugin groups) and **+ 그룹 추가** (Add group) twice. The numbered slots 1 and 2 can be switched even with no effect members. To verify audible bypass, add your own effect and assign it to a group.

## Exercise every action

The action names below assume Stream Deck is set to English. Korean equivalents are included in the plugin. The seven keypad actions can be tested sequentially on a six-key Mobile layout.

| Action | Test and expected result |
|---|---|
| Microphone On/Off | Select Host. Press to alternate ON/OFF and compare the Host lamp in LiveMix. Try explicit ON and OFF modes. Change Host in LiveMix and verify the key follows. Apply the microphone mute group while Host is ON: the key becomes red with **Mute group / Originally ON**; switch Host OFF and it must say **Originally OFF** while the group remains muted. |
| All Microphones | With all three OFF, Toggle turns all ON; another press turns all OFF. Manually turn on Host and Guest: the key shows **Some ON / 2/3**. Toggle now turns all OFF. Try explicit all ON/OFF. Group mute latches remain as set. |
| Microphone Mute Group | Toggle with Host and Spare assigned; the key becomes red **Muted / Targets: 2**, and both assigned microphone lamps show the group mute. Toggle again to unmute. Verify explicit Mute and Unmute modes. Original microphone ON/OFF switches are preserved. |
| FX Mute Group | With Hall assigned, toggle and compare the FX panel's mute indication; the key shows **Muted / Targets: 1**. Try Mute and Unmute. This controls assigned FX returns separately from microphone mute and preserves send amount and Pre/Post. |
| Plugin Group | Select Host and Group 1. Toggle: the numbered chip changes between blue ON and red OFF. Try explicit ON/OFF. Group 2 stays independent; selecting one does not turn off others. Delete the last group while its key is selected: it shows **Group missing**. Deleting earlier groups shifts later slot numbers, so recheck selection after edits. |
| FX Send ± | Select Host and Hall; clear group mutes for the ordinary green send indication. Increase by the default 5%; Decrease by 5%; try 1% and 10%; Set value to 50%. The key shows the current percentage and +5 / −5 / =50 badge. Compare LiveMix's send slider. Values stop at 0% and 100%; Pre/Post does not change. Two keys on this route stay synchronized. Releases do nothing. |
| Status | Test Display = Connection, Session and Audio. Session shows its name and an asterisk when modified. Audio shows stopped without ASIO; it is not a connection failure. Press to refresh (limited to once per two seconds per key). The action does not start audio. |
| FX Send Amount — Stream Deck + only | Place on a dial, select Host and Hall. Rotate both ways; default is 1% per tick, with a 5% option. Touch strip shows route, amount, Pre/Post and a bar. Values clamp at 0–100%. Short press and release within 600 ms without turning changes only Pre/Post when Press = Pre / Post. Rotate while held: amount changes, release does not toggle Pre/Post. A long press, touch or long touch sends no command. Press = Disabled prevents the short-press change. Compare a key on the same route. |

## Connection and missing-target checks

- Turn **외부 제어 사용** off: keys show **Control disabled**. Pressing a control alerts and changes nothing. Re-enable it and verify automatic recovery.
- Fully quit LiveMix (minimizing leaves it running in the tray): keys lose their active lamp and show offline/checking. Send keys and the dial show **—**, not 0%. Restart LiveMix; the plugin reconnects and reads fresh state without replaying disconnected presses.
- Delete a selected microphone or FX in the disposable session: show **Channel missing** / **FX channel missing** and reject commands. The selected name remains in the property inspector until a new selection is made.
- On a different session, a saved channel ID is preferred; optional name fallback works only for one exact matching name. Duplicate names require reselection. Display aliases do not affect binding.
- Set Stream Deck to Korean and English, reopen the inspector and verify translated labels, mode/state names and dial trigger descriptions. User-defined channel names stay unchanged.

## Validation supplied and remaining device evidence

The client reports successful **0.9.1 with real LiveMix 0.6.0 on Stream Deck Mobile**. Version 1.0.0 adds release artwork and submission materials; automated SDK/fake-host results are recorded in [VALIDATION.md](../../../streamdeck/VALIDATION.md). This is not a claim of physical Stream Deck + testing or Marketplace DRM verification. The Maker will supply a real device demonstration and verify the Marketplace-processed package before manual publication. The gallery uses clearly identified illustrations, not translated app screenshots or device photographs.

The plugin connects to LiveMix over the local PC's loopback interface. It has no external endpoints, CDN assets, analytics or automatic updater. Logs avoid discovery tokens and user channel names.

Support: [LiveMix Stream Deck support](https://곰튀김.com/livemix/#streamdeck-support). This is the customer support link; Elgato's Maker email is only for submission correspondence.
