These are normalized transcripts of the C++ reference, not a new mock protocol.

The master-group extension adds `state.master.pluginGroups` (empty in the initial
fixture), `masterPluginGroups` capability, and `setMasterPluginGroupOff {index, off}`.
The fake server enables this capability with `masterPluginGroups:true`; its default
keeps earlier capability configurations for the existing action tests. Master group
deltas replace the whole list. A missing master object on older servers defaults
to an empty list, and the new action requires the capability before sending commands.

The aux extension (LiveMix 0.24.1) adds `state.auxes: [{id, name, on}]` in card
order, the final `auxOn` capability, and `toggleAux {auxId}` / `setAuxOn {auxId, on}`.
The fake server enables two aux channels and version `0.24.1` with `auxOn:true`;
the default omits both the field and capability, preserving earlier versions.
Both commands return `{on}`; identical sets return `changed:false`, and missing
targets return `AUX_NOT_FOUND`. Name/ON changes use partial `changes.auxes` entries;
additions, removals and reordering use a full state. Older snapshots default a
missing `auxes` field to an empty list, and the action requires `auxOn` before sending.

`tests/ControlServerTests.cpp` constructs JSON with `Client::hello`, `command`,
`toggle`, `ping`, and `wire` rather than containing literal JSON transcript files.
The fixtures transcribe those objects with fixed non-null JUCE-format UUIDs,
an example 32-byte token, and the revisions asserted in `orderingAndState()`.
The initial projection uses `Fixture`'s `Control test` session, one ON microphone,
dirty session, stopped audio and the default reverb send. The default Korean
channel/FX names exercise byte splitting. Only runtime UUIDs and the token are
substituted; this is not a captured user session.

- `hello.json`: `Client::hello` (client description is the C++ test description).
- `hello-ack.json`, `initial-state.json`: `authenticate()` and
  `ControlProtocol.cpp::encode(HelloAck/State)` / `toVar(Projection)`.
- `ordering.json`: `orderingAndState()` first two toggles, their ack revisions,
  empty coalesced delta, then the requestState ack/snapshot exchange.
- `errors.json`: `handshake()` error cases and
  `ControlProtocol.cpp::encode(ErrorResponse/ServerStatus)`.
- `plugin-windows.json`: LiveMix 0.22.0 `TogglePluginGroupWindows` /
  `SetPluginGroupWindows` arguments and `PluginGroupWindowsResult` in
  `ControlProtocol.{h,cpp}` / `ControlDispatcher.cpp`; three microphones with
  one plugin each in group 1, initially closed. This is a normalized command/result
  sequence, with the current instance/session and request ID added by the fake server.

LiveMix 0.16.0 adds the boolean `fx[].on` field (initially `true`) and the
`fxOn` helloAck capability after `pluginGroupsEverywhere`. These two fixtures
include them. Older projections omit `on`; the plugin defaults it to `true`
but rejects a present non-boolean value. Deltas omit unchanged fields.

LiveMix 0.18.0 adds `muteAll: { mic: false, fx: false }` to the initial
projection and the `muteAll` helloAck capability after `fxOn`. Older snapshots
omit the object; the plugin fills absent mic/FX keys with false. A present
non-object or non-boolean key is invalid. Deltas carry only changed keys and
merge independently of `muteGroups`.

LiveMix 0.22.0 adds the `pluginGroupWindows` capability. Window commands return
`{ index, open, count }` with `changed:false` and the current revision, without
publishing window visibility in a snapshot or delta. Opening counts targeted
plugins; closing counts editors actually closed, so an already closed group
returns zero. `errors.json` includes missing microphone/group, empty group
(`PLUGIN_GROUP_EMPTY`, not retryable) and loading-session (`SERVER_BUSY`, retryable).

The fake server loads these files, substitutes the live port/instance/token and
current document values, and sends the same envelopes. It implements hello,
ping, requestState and the mic, FX, mute-group, mute-all, plugin-group and send commands.
It also implements toggle/set plugin-group windows. Its `pluginWindows` map holds
test-only plugin counts and open-editor counts by `channelId/index`; a group has
one plugin by default. Set its count to zero to model an empty group. These values
never enter the projection. `behavior.sessionLoading` returns `SERVER_BUSY`.
`pluginGroupWindows:false` omits the capability and rejects the commands.
The capability defaults to `muteAll`, retaining legacy fake-server configurations.
`fxOn:false` omits that capability; `legacyFx:true` also omits FX `on` fields
and defaults `fxOn` to false, serving a LiveMix 0.15.5-shaped snapshot.
`pluginGroupsEverywhere:false` independently omits that capability.
`muteAll:false` independently omits the new capability. `legacyMuteAll:true`
also omits the projection object and defaults the capability to false, serving
a LiveMix 0.17.7-shaped snapshot. `legacyFx:true` defaults `legacyMuteAll` to true.
`toggleMuteAll` / `setMuteAll` return `{ group, muted }`, preserve mute groups,
and use the normal revision/delta path; identical sets return `changed:false`.
