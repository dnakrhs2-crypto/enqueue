# Round 4 / 1.0.0 validation — 2026-09-08

Implemented in this worktree on `livemix-streamdeck`. Plugin/package versions:
**1.0.0.0 / 1.0.0**; runtime hello: **1.0.0**. Installed Node **24.13.0**,
SDK **2.1.2** and CLI **1.9.0** were reused. Commands ran from
`C:\Users\claude\gocue-sd\streamdeck` using the Windows `npm.cmd` shim.

| Exact command | Actual result |
|---|---|
| `npm.cmd run typecheck` | Exit 0; source and test TypeScript checks passed. |
| `npm.cmd run build` | Exit 0; generated release assets/localizations and Rollup bundle; Rollup reported **2.5 s** for the final standalone build. |
| `npm.cmd test` | Exit 0; **118 tests, 118 passed, 0 failed, 0 cancelled, 0 skipped, 0 todo**, **143592.4581 ms**. Builds the distributable before testing. |
| `npm.cmd run validate` | Exit 0; asset verifier passed, then Elgato **Validation successful**. |
| `npm.cmd run pack -- -f` | Exit 0; **Successfully packaged plugin**, **128 files**, **267.7 KiB unpacked**. |
| `npm.cmd run media` | Exit 0; five standalone HTML compositions generated. |

Package: **[dist/com.gomtwigim.livemix.streamDeckPlugin](dist/com.gomtwigim.livemix.streamDeckPlugin)**,
**150,369 bytes (146.8 KiB)**. SHA-256:
`42faada79cd9a8f35965e002e79dcc960a2b032f29c2fa13dabe177bbfd5a909`.
The ZIP was reopened after packing: version 1.0.0.0, eight actions,
and **every archived file byte-matches its tested build input**. Development
directories, design sources, tests, maps, logs and discovery files are absent.
The installed CLI's normal packaging/DRM preparation path was retained.

## Assets, rendering and translations

- **25 manifest image references / 50 files** exist in both required sizes:
  plugin PNG 256/512; category SVG 28/56; eight action-list SVGs 20/40;
  fourteen state-image references 72/144; encoder SVG 72/144.
- **118 packaged images** byte-match generated sources in `design/`, including
  mixed, muted, group 1–5, send and connection variants. All category/action-list
  graphics use only white/none with transparent backgrounds and a 24-unit
  viewBox. Key/encoder graphics use a 144-unit viewBox.
- The red #E5302D ON brand and the app's #35D07F / #3A3F47 / #FF5A5F /
  #15171B / #4C8DFF palette are shared by generated and runtime artwork.
  The PNG logo was visually inspected. The geometric PNG generator and
  256/512 HTML/SVG alternatives require no added dependencies.
- Existing renderer assertions now also check distinct mic/FX group images and
  exact static/runtime SVG agreement. Original ON/OFF under mute, localized
  labels, missing/unknown feedback and every existing rolling ≤10 calls/s
  assertion remain green. No action command semantics changed.
- ko/en translation keys, nonempty values, action/state names and encoder
  trigger descriptions remain complete. The plugin PI continues to use only
  bundled files and its local host WebSocket; no external URLs were added.

## Marketplace materials

The [submission package](../docs/marketplace/livemix-streamdeck/README.md)
contains English/Korean listings, 1.0.0 notes, all eight reviewer action tests,
the Korean Maker Console checklist, media export instructions and five HTMLs.
The English description is **1,039 characters** (within 250–1,500);
the Korean description is **625 characters**. Support is the requested
`https://곰튀김.com/livemix/#streamdeck-support`.

Chromium opened all five HTMLs at DPR 1: app icon **288×288**; thumbnail and
three galleries **1920×960**. Checked canvas dimensions, self-contained
resources, English captions, SVG text bounds, page bounds and footer overlap;
all passed. Thumbnail and gallery QA captures were visually inspected.
QA captures and the layout report stay in ignored `.test-build/`; submission
deliverables are HTML for Claude's final PNG exports. Initial Chromium QA
needed a local `--no-sandbox` browser flag because renderer initialization
timed out in this managed environment; it opened only these local compositions.
This does not alter plugin runtime or packaging.

## Validation boundaries and handoff

The client reports **0.9.1 + real LiveMix 0.6.0 on Stream Deck Mobile works**.
That report is accepted as prior real-device evidence, not represented as
new physical 1.0.0 testing. Remaining client/Claude release checks:

- Final PNG export and 1.0.0 icon/font inspection in actual Stream Deck/Mobile.
- Real Stream Deck + dial/touch verification and a **1920×1080 MP4**
  demonstration with English captions (below 250 MB, internal target ≤50 MB).
- Claude's public installation/support sections, with an English support contact.
  The GitHub releases URL could not be fetched by this environment's web tool;
  public 0.6.0 installer availability is included in the client checklist.
- Maker organization spelling matching Author **Gomtwigim**, available Console
  tags, Maker Agreement, submission with **automatic publication disabled**,
  and a Marketplace-processed package install/upgrade check before manual publication.

No account action, Marketplace submission or publication was performed.
Changes are confined to `streamdeck/` and
`docs/marketplace/livemix-streamdeck/`. No dependency installation or changes
to node_modules, C++, CMake, installer, site or Git metadata were made.
The lockfile SHA-256 is unchanged:
`6062a47265ac17f33ac578f2cb4e0943e181ad1600ad89fa00ce50decf4f718e`.
Its preview root version metadata is retained per the explicit no-lockfile-edit
constraint; the release manifests and runtime use 1.0.0.

Earlier validation records follow and describe their respective preview builds.

---

# Round 3C validation — 2026-09-08

Added `com.gomtwigim.livemix.fx-send-step` for Keypad devices, including
Stream Deck Mobile. Plugin/package versions are **0.9.1.0 / 0.9.1-0**.
The existing installed dependencies were used. Commands ran from
`C:\Users\claude\gocue-sd\streamdeck` with the Windows `npm.cmd` shim.

| Exact command | Actual final result |
|---|---|
| `npm.cmd run typecheck` | Exit 0; both source and test TypeScript checks passed. |
| `npm.cmd run build` | Exit 0; generated assets/localizations and Rollup bundle; Rollup reported 3.3 s. |
| `npm.cmd test` | Exit 0; **118 tests, 118 passed, 0 failed, 0 cancelled, 0 skipped, 0 todo**, **143770.9465 ms**. Includes 22 new SDK/fake-host/fake-LiveMix tests and one settings-validation test; existing action/PI/manifest coverage also includes the new action. |
| `npm.cmd run validate` | Exit 0; `Validation successful`. |
| `npm.cmd run pack -- -f` | Exit 0; `Successfully packaged plugin`; **64 files**, **204.2 KiB unpacked**. |

Package: **`dist/com.gomtwigim.livemix.streamDeckPlugin`**, **90,213 bytes
(88.1 KiB)**. SHA-256:
`66b180e3942d2fa23a300622079f69c1f2b6c7e5d7c96a98690bcfff29185651`.
The archive was reopened: version 0.9.1.0, eight actions, a single Keypad state
for the new action, and a bundle identical to the tested build. Development
directories, logs, source maps and discovery files are absent.

Key down increases (default), decreases or sets the selected channel/FX send.
Increase/decrease use 1%, 5% (default) or 10%; set uses an integer 0–100% target
(default 50%). Values are computed from canonical state at dispatch, clamped to
0–1 and sent as `setSend { channelId, fxId, amount }` with `ifRevision`.
`pre` is never sent. One explicit revision conflict permits one recomputation;
a second conflict alerts without another retry. Key release is inert.

Keys and dials share the same session/channel/FX queue: one active command,
ACK plus canonical-state barrier, at most two waiting key intents, and 500 ms
waiting-intent expiry. Displaced/expired inputs alert; disconnect clears input
without replay. Tests verify both key/key and key/dial synchronization.

The key shows the current percentage, a send lamp/bar, a +5 / −5 / =50 mode
badge, and the selected channel → FX title. Offline/missing states show `—`
with the existing status icons/titles, never a zero-percent substitute. Mute
and stopped-audio cues remain visible. The shared renderer retains its rolling
10-call/key/second budget, including alerts and reappearance.

The ko/en PI shares live channel/FX lists with the dial. It exposes the three
localized modes and steps only as applicable; the target field appears only
in set mode. Tests execute all eight shipped PI views and verify defaults,
field visibility, saves, target bounds and offline selection retention.

Files changed:

- Added `src/actions/fx-send-step.ts`; extended `src/actions/base.ts`,
  `src/livemix/bindings.ts`, `src/ui/key-renderer.ts` and `src/plugin.ts`.
  Updated only the hello client version in `src/livemix/connection.ts`.
- Updated `tools/{actions,generate-assets,locales}.mjs`; regenerated
  `src/ui/strings.ts`, the plugin manifest, `ko.json`, `en.json`, `ui/strings.js`,
  and six new 1x/2x placeholder SVGs in `imgs/actions/fx-send-step/`.
- Updated plugin `ui/{inspector.html,inspector.js}` and `package.json`.
- Added `tests/fx-send-step.test.ts`; extended `tests/{actions,bindings,inspector,rendering}.test.ts`
  and updated the fake host's version in `tests/fake-host.mjs`.
- Updated `README.md` and this report. Build/package outputs were regenerated.

All edits are under `streamdeck/`. The lockfile SHA-256 remains
`6062a47265ac17f33ac578f2cb4e0943e181ad1600ad89fa00ce50decf4f718e`.
No dependency installation or edits to node_modules, C++, CMake, installer,
site or Git metadata were performed. Verification uses the built SDK plugin,
fake Mobile host and fake LiveMix TCP server; no new physical-device result is
claimed for this build.

---

# Round 3 (3A + 3B) validation — 2026-09-08

Implemented in `streamdeck/` on `livemix-streamdeck`, using the installed SDK
2.1.2 and CLI 1.9.0. No dependency installation ran. All commands below ran in
`C:\Users\claude\gocue-sd\streamdeck`. PowerShell blocks the `npm.ps1` shim,
so the same npm scripts were invoked with `npm.cmd`.

| Exact command | Actual final result |
|---|---|
| `npm.cmd run typecheck` | Exit 0; source and test TypeScript checks passed. |
| `npm.cmd run build` | Exit 0; generated assets/localizations and Rollup bundle; Rollup reported 3 s. |
| `npm.cmd test` | Exit 0; build and test compilation followed by **95 tests, 95 passed, 0 failed, 0 cancelled, 0 skipped, 0 todo** in **101110.049 ms**. Includes all 52 existing tests and 43 new tests. |
| `npm.cmd run validate` | Exit 0; `Validation successful`. |
| `npm.cmd run pack` | Exit 0; `Successfully packaged plugin`; **58 files**, **195.9 KiB unpacked**. |

Package: **`dist/com.gomtwigim.livemix.streamDeckPlugin`**, version **0.9.0.1**,
**85,353 bytes (83.4 KiB)**. The ZIP was independently opened after packaging:
58 files, seven manifest actions and the FX-send layout are present; development
directories, logs, source maps and discovery files are absent.

## Action behavior verified

| Action | Result |
|---|---|
| Microphone On/Off | Existing toggle/ON/OFF behavior and all prior fake-host/connection/binding tests remain green. |
| All Microphones | Any ON → all OFF; all OFF → all ON. Explicit all ON/OFF modes. All/some/none labels and counts, zero-channel inhibition, shared-target serialization and revision-conflict recomputation. Mute latches stay unchanged. |
| Microphone Mute Group | Toggle/mute/unmute; state 1 is red/muted, state 0 unmuted. Live member count, including an operable zero-member latch. |
| FX Mute Group | Same latch behavior with FX membership; send amount/pre and other groups are preserved. |
| Plugin Group | Existing numbered channel slot; toggle/ON/OFF; state is inverse of wire `off`. Accent ON/red OFF, channel/group title, live slot lists and missing-group rendering. |
| FX Send Amount | Encoder only. Rotate adds ticks × 1% (default) or 5%, clamped to 0–100%, using amount-only CAS. Down records a press; release within 600 ms without any intervening rotation toggles pre only when enabled. Press+rotate changes amount only. Long press, tap and long touch send no command. |
| Status | Connection/session+dirty marker/audio display; key down only requests state, at most once per two seconds per key, including hide/show. |

All keypad releases are inert. Every new key action uses the mic lifecycle
pattern, shared connection/state/bindings/queue and budgeted key renderer.
Computed key intentions wait for ACK plus canonical state, retain at most two
unsent intentions for 500 ms, and visibly fail displaced/expired input.

Dial tests run the built plugin via the real SDK, real fake-host WebSocket and
fake-LiveMix TCP server. `fake-plus` uses installed `DeviceType.StreamDeckPlus`
(7). Tests cover signed ticks, saturation/no-op ACKs, both steps, independent
amount/pre preservation, coalescing across encoders, both sides of the ACK/state
barrier, 50 absolute unsent ticks/500 ms limits, up to two explicit conflict
retries, non-retried press conflicts, input cancellation on timeout/EOF/session
change/disappearance, simultaneous channel/FX renames and session fallback,
offline em dash/disabled bar, mute/audio statuses and rolling feedback budget.

The shipped PI script is executed for all seven action views in Korean and
English. Tests verify relevant fields, live channel/FX/slot/count lists, duplicate
name labels, safe text nodes, stale/context rejection, notes, immediate saves and
offline selection retention. Manifest/controller/state/trigger translations,
1x/2x asset references, exact 200×100 layout rectangles and key/encoder rendering
budgets are also checked. All fake-host ≤10-call assertions remain active.

## Files changed

- Added `src/actions/{all-mics,mic-mute-group,fx-mute-group,plugin-group,fx-send,status}.ts`,
  with shared `base.ts` and `mute-group.ts`; registered all seven in `src/plugin.ts`.
- Extended `src/livemix/{protocol,connection,bindings,command-queue}.ts` for typed
  commands/ACKs, independent FX binding, computed intentions and dial accumulation.
  Retained the shared state store and the existing mic action.
- Extended `src/ui/key-renderer.ts`; added `src/ui/feedback.ts`; regenerated
  `src/ui/strings.ts` from the expanded `tools/locales.mjs`.
- Updated the plugin manifest, ko/en JSON, `ui/{inspector.html,inspector.js,inspector.css,strings.js}`;
  added `layouts/fx-send.json` and 38 placeholder SVG assets under the six new
  `imgs/actions/` directories.
- Extended `tools/generate-assets.mjs`; added the shared `tools/actions.mjs` catalogue.
- Added `tests/{action-helpers,actions,dial,status}.ts`; extended fake host/server,
  binding, connection, rendering and inspector tests. Updated `README.md` and this report.

## Implementation choices and validation limits

No requested scope was deferred. The six new actions share the mic's lifecycle
pattern through a base class, and the two mute actions share a small helper.
Computed keys, like dial amounts, retry only explicit revision conflicts, at
most twice; pre/post presses never retry. Rotational limits count absolute ticks
so opposite-direction events cannot create an unbounded unsent backlog.

The fake host now fences key/PI disappearance with WebSocket ping/pong before
enforcing no further output. This permits messages already in transit while
still rejecting output after the plugin has processed disappearance. Initial
expanded-run failures were title-update timing and these teardown races; no
budget, deadline or closed-context assertion was removed.

The lockfile SHA-256 remains
`6062a47265ac17f33ac578f2cb4e0943e181ad1600ad89fa00ce50decf4f718e`, matching
Round 2A and the committed blob. Changes are confined to `streamdeck/`; no
node_modules, C++, CMake, installer, site or Git metadata changes were made.
The package contains placeholder artwork as requested. Round 3 was verified
against the fake LiveMix server; real LiveMix, Stream Deck App installation and
physical Stream Deck + dial/touch testing remain for the client/maintainer.

---

# Round 2A follow-up validation — 2026-09-08

Environment: Windows, Node `v24.13.0`, npm `11.6.2`, installed
`@elgato/streamdeck 2.1.2` and local `@elgato/cli 1.9.0`.
Claude's installed dependencies and lockfile were reused. No installation ran.

All commands ran from `C:\Users\claude\gocue-sd\streamdeck`, using `npm.cmd`
because PowerShell's execution policy blocks the `npm.ps1` shim.

| Exact command | Actual result |
|---|---|
| `npm.cmd run typecheck` | Exit 0. Both source and test TypeScript checks passed. |
| `npm.cmd run build` | Exit 0. Assets/strings generated; Rollup built `bin/plugin.js` in 2.3 s. |
| `npm.cmd test` | Exit 0. The build and test compilation ran, followed by the complete suite: **52 tests, 52 passed, 0 failed, 0 cancelled, 0 skipped, 0 todo**; 26295.0206 ms. |
| `npm.cmd run validate` | Exit 0. `√ Validation successful`. |

The [full command output](.test-build/round2a-validation-output.txt) includes every
test and exit code. The 52 tests include nine tests running the real SDK and built
plugin in a subprocess against the fake Stream Deck host.

## Reproduced failures and corrections

The saved Claude output was read first. An unmodified full run reproduced
**51 tests, 48 passed, 3 failed**.

1. **Offline title / alert test:** the plugin emitted `LiveMix\n미연결`.
   The test deleted the newline and compared `LiveMix미연결` with
   `LiveMix 미연결`, so it never reached the keyDown/alert assertion.
   The test now treats a status-title newline as a word boundary, as allowed by
   §4.4. A socket trace confirmed state 0, the disconnected image/title, and
   exactly one `showAlert` on `key-a` after explicit input.
2. **Muted OFF label test:** the red lamp is shared by muted ON and OFF.
   Immediately after `setState(0)`, the previous image still said `원래 ON`;
   the following `setImage` supplied `원래 OFF`. The test incorrectly
   assumed an atomic visual update. It now waits for state, color and the
   expected original-state label together. §4.4 explicitly requires
   state → image → title ordering. The existing Korean and English labels
   already implement the design; no renderer or string change was needed.
3. **PI test:** the getOptions/options exchange had completed on the real SDK
   plugin socket. The failure was its final key-title comparison:
   `Control\ndisabled` became `Controldisabled`. The same title normalization
   fixes it. The expanded test also verifies a plugin-originated rename through
   setSettings/didReceiveSettings on the separate PI socket, and an offline PI
   settings edit through the plugin's settings event and options reply.

Only `tests/mic.test.ts` and this report were edited as source/documentation.
Build outputs were regenerated by the required commands. Plugin source,
including Claude's `SendToPluginEvent<JsonValue, MicSettings>` fix, was retained.

## Installed SDK and EOF verification

The installed `dist/plugin/ui.js` / `ui.d.ts`, action declarations/runtime,
and action registration routing were inspected. SDK 2.1.2 tracks the current PI
action on propertyInspectorDidAppear; onSendToPlugin resolves the action by
context. `streamDeck.ui.sendToPropertyInspector` sends
`sendToPropertyInspector` on the plugin connection with that action's context.
The plugin uses these APIs correctly. Its renderer holds the SDK key action;
calling its `showAlert()` sends the proper key context.

The stopped-server test now verifies both visible keys and the alert context.
A new bare TCP EOF test leaves discovery at ready, holds the next handshake,
and verifies that both keys lose their green lamp and reject input with one
alert on the pressed key. A reconnect may legitimately show Checking before
the rolling render budget renews, so this case accepts disconnected or checking
feedback. It does not wait for a handshake timeout to prove loss of readiness.

All existing rolling **≤10 total calls/key/second** assertions remain enabled,
including alerts, rapid updates and reappearing keys. No rate limit, deadline
or failure assertion was removed.

The lockfile SHA-256 remained
`6062a47265ac17f33ac578f2cb4e0943e181ad1600ad89fa00ce50decf4f718e`.
No node_modules, C++, CMake, installer, site or git changes were made.
The local CLI's existing same-day schema-check cache was reused without a write.

Tests use real loopback sockets and the installed SDK. The PI DOM test uses a
minimal DOM; physical Stream Deck/App rendering and hardware remain separate
validation work.
