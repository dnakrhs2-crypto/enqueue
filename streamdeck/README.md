# LiveMix Stream Deck 1.0.0

Windows release `com.gomtwigim.livemix`, manifest version `1.0.0.0`
(npm package `1.0.0`). Requires LiveMix 0.6.0+ on Windows 10/11 x64.
This package implements eight actions, a shared LiveMix connection and a
hardware-free test harness. Node 24 and Stream Deck 7.1+ are the runtime baseline.
It does not install, launch or change LiveMix or Stream Deck profiles.

## Build and test

From this directory (use `npm.cmd` in PowerShell when `.ps1` shims are blocked):

```powershell
npm.cmd run typecheck
npm.cmd run build
npm.cmd test
npm.cmd run validate
npm.cmd run pack -- -f
```

The dependencies in this worktree are already installed; retain them and the
existing lockfile. Do not run npm install or rewrite package-lock.json for
this release. Its preview package metadata is intentionally retained under that
constraint; package.json, manifest and runtime hello advertise 1.0.0.
All direct dependencies use exact versions.
The test script builds the actual distributable first, compiles the TypeScript
tests with `tsconfig.test.json`, then runs `node:test` serially. No physical Stream
Deck, Stream Deck application or LiveMix executable is needed by the fake tests.

If the CLI cannot check online schemas, its documented offline validation option is:

```powershell
npm.cmd run validate -- --no-update-check
```

See [VALIDATION.md](VALIDATION.md) for actual command results and the remaining
physical-device validation. `npm.cmd run pack -- -f` replaces
`dist/com.gomtwigim.livemix.streamDeckPlugin` with the validated package.

## Install / 설치

1. Install LiveMix 0.6.0+ and Stream Deck 7.1+ on the same Windows 10/11 x64 PC,
   and run them as the same Windows user.
2. Open `dist/com.gomtwigim.livemix.streamDeckPlugin` and complete Stream Deck's
   installation dialog. Use the same UUID to upgrade an existing 0.9.1 installation.
3. In LiveMix, choose 설정 → 설정... (Settings). Under 외부 제어 (Stream Deck),
   enable 외부 제어 사용. External Control is off by default.
4. Expand LiveMix in the Stream Deck action list, place keys and select channels
   in the property inspector. For Mobile, pair it with the Windows desktop app.
   FX Send Amount goes on a Stream Deck + dial; FX Send ± goes on keys.

LiveMix's own interface is Korean; the plugin supports Korean and English.
ASIO is needed for sound, but all controls can be tested while audio is stopped.
The client reported successful 0.9.1 use with real LiveMix 0.6.0 on Mobile;
the new artwork and physical Stream Deck + remain separate visual/device checks.

## Actions

| Action | Input and display |
|---|---|
| Microphone On/Off | Toggle / ON / OFF; original switch state with mute-group and stopped-audio badges. |
| All Microphones | Toggle turns everything OFF if any microphone is ON, otherwise ON. Explicit all ON/OFF modes. Shows all/some/none ON and the count; no command with zero microphones. |
| Microphone / FX Mute Group | Toggle / Mute / Unmute the LiveMix latch. Shows membership count; a zero-member group remains operable. Membership is edited in LiveMix. |
| Plugin Group | Toggle / ON / OFF an existing numbered slot. Deleted slots show Group missing. Deleting earlier groups shifts later numbers. |
| FX Send Amount (Encoder only) | Rotate adjusts amount by 1% (default) or 5% per tick, clamped to 0–100%. Down only records the press; release within 600 ms without rotation toggles pre/post when enabled. Rotating while pressed changes amount only. Long press, tap and long touch issue no command. |
| FX Send ± / FX 보내는 양 ± (Keypad, including Mobile) | Key down increases (default), decreases or sets the send amount. Steps: 1%, 5% (default), 10%; set target: 0–100% (default 50%). Amount-only CAS preserves pre/post, clamps to 0–100%, and recomputes once on an explicit conflict. Shows the current percentage, channel → FX title and +5 / −5 / =50 mode badge. |
| Status | Connection / session and dirty `*` / audio display. Key down only requests a snapshot, at most once per two seconds per key. |

All keypad releases are inert. The PI saves changes immediately and preserves
selected channel/FX/slot values while offline. Offline send keys and encoders show
`—`; encoders also disable the bar. Amount and pre/post commands update only their respective fields.

## Package structure

- `src/plugin.ts`: registers eight actions and makes one SDK connection; owns one
  LiveMix TCP connection and one command queue for every visible key/device/PI.
- `src/livemix/`: bounded discovery reads, lease validation, loopback NDJSON,
  handshake/heartbeat/reconnect, strict wire validation, state reducer, bindings
  and per-target command serialization. Ack results never patch the state cache.
- `src/actions/mic.ts`: microphone toggle/ON/OFF, context lifecycle, settings
  migration and PI bridge using the SDK 2.x `ui.action` and
  `ui.sendToPropertyInspector` APIs. Key release sends no command.
- `src/actions/base.ts`: the mic lifecycle/PI/binding pattern shared by the seven
  additional actions. `mute-group.ts` shares the microphone/FX latch behavior.
- `src/ui/`: finite SVG templates, safe display text, bundled ko/en strings and a
  shared rolling call history per key/encoder. The 10/s budget includes `showAlert`.
- `com.gomtwigim.livemix.sdPlugin/`: manifest, local PI, local translations and
  release icons. `bin/plugin.js` and its module marker are Rollup
  build outputs. The PI only uses its host WebSocket; names use text nodes.
- `src/ui/artwork.ts`: shared vector geometry and exact LiveMix colors for
  runtime keys, static images and media. `design/` contains generated SVG/PNG
  sources and 256/512 PNG-ready brand HTML.
- `tools/generate-assets.mjs`, `tools/actions.mjs`, `tools/locales.mjs`:
  reproducible 1x/2x icons and complete ko/en strings. `tools/brand-png.mjs`
  exports the geometric ON logo without extra dependencies.
- `tools/verify-assets.mjs`: checks every manifest image in both sizes,
  white monochrome menu icons, design/output equality and locale completeness;
  `npm run validate` runs it before the Elgato CLI.
- `tools/generate-marketplace.mjs`: `npm.cmd run media` writes five standalone
  HTML compositions to [the submission folder](../docs/marketplace/livemix-streamdeck/README.md).
  Claude exports those HTMLs as PNG with Chromium. Release history is in [CHANGELOG.md](CHANGELOG.md).
- `tests/fake-host.mjs`: actual `ws` server and built SDK child process, official
  registration arguments, Mobile and Stream Deck + device/key/dial/settings events, context output recording,
  separate PI registration and relay, deadlines and cleanup.
- `tests/fake-livemix-server.mjs`: actual TCP server and atomic discovery in a
  temporary APPDATA directory. Only the SDK child receives the APPDATA override;
  in-process connection tests inject the discovery path through the constructor.
- `tests/fixtures/control/`: normalized C++ transcripts with precise source notes.
  The C++ tests construct JSON objects dynamically; their generated envelopes are
  transcribed with fixed test UUIDs/token, not claimed as literal copied captures.

The authoritative wire implementation is `../livemix/src/ControlProtocol.{h,cpp}`
and `ControlServer.cpp`; the design is
`../docs/superpowers/specs/2026-09-08-livemix-streamdeck-design.md`.
Structure and Rollup plugin order follow the locally installed CLI 1.9.0 template.
The SDK 2.x API review also used Elgato's
[upgrade guide](https://docs.elgato.com/streamdeck/sdk/releases/upgrading/v2/),
[current source](https://github.com/elgatosf/streamdeck/tree/main/packages/plugin/src/plugin)
and [host WebSocket contract](https://docs.elgato.com/streamdeck/sdk/references/websocket/plugin/).

## Behavior and boundaries

Discovery is always `%APPDATA%\LiveMix\control\discovery.json`, at most 8 KiB,
with no configurable remote endpoint. The parent is watched and disconnected or
disabled states are polled each second. A one-second lease recheck also runs while
connected: it expires unchanged heartbeats and handles lost/coalesced Windows
watch notifications without trusting an indefinitely cached ready/disabled file.
Stale/future timestamps require a newly observed heartbeat. HelloAck must match
the discovery instance, and a full snapshot must follow before any key can act.

Original UUIDs stay in action settings. Exact, unique name fallback resolves only
at an initial/new instance/session boundary; it does not overwrite the UUID.
Deleting a resolved target keeps it missing until a new boundary or an explicit
target selection. This also holds across profile hide/show and reconnects to the
same session. An origin rename updates the saved fallback name; a temporary
fallback rename does not. The display alias is never a fallback name.

Pending key intents are limited to two unsent intents per target and expire at
500 ms. Dial ticks coalesce across contexts on the same session/channel/FX target
until a 50 ms gap, with at most 50 absolute unsent ticks and a 500 ms lifetime.
Each target has at most one command in flight and waits for canonical
state to reach its ack revision. Computed values carry `ifRevision`. Disconnect,
timeout and session changes discard old input; reconnect only negotiates and
receives state. Runtime logs contain no token, raw JSON or channel names.
Computed key toggles and dial amounts retry an explicit revision conflict at most
twice against refreshed state. FX Send ± keys retry once and share the dial's
send-target queue. A pre/post press never retries a conflict.

The [Marketplace package](../docs/marketplace/livemix-streamdeck/README.md)
contains copy, reviewer instructions, media and the client's submission checklist.
Keep automatic publication disabled until the Marketplace-processed package,
real dial demonstration and public support page have been checked.
Fake tests do not establish physical key readability,
Chromium font/layout correctness or actual Stream Deck application installation.
