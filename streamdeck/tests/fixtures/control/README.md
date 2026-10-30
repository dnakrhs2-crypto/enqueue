These are normalized transcripts of the C++ reference, not a new mock protocol.

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

LiveMix 0.16.0 adds the boolean `fx[].on` field (initially `true`) and the
`fxOn` helloAck capability after `pluginGroupsEverywhere`. These two fixtures
include them. Older projections omit `on`; the plugin defaults it to `true`
but rejects a present non-boolean value. Deltas omit unchanged fields.

LiveMix 0.18.0 adds `muteAll: { mic: false, fx: false }` to the initial
projection and the `muteAll` helloAck capability after `fxOn`. Older snapshots
omit the object; the plugin fills absent mic/FX keys with false. A present
non-object or non-boolean key is invalid. Deltas carry only changed keys and
merge independently of `muteGroups`.

The fake server loads these files, substitutes the live port/instance/token and
current document values, and sends the same envelopes. It implements hello,
ping, requestState and the mic, FX, mute-group, mute-all, plugin-group and send commands.
`fxOn:false` omits that capability; `legacyFx:true` also omits FX `on` fields
and defaults `fxOn` to false, serving a LiveMix 0.15.5-shaped snapshot.
`pluginGroupsEverywhere:false` independently omits that capability.
`muteAll:false` independently omits the new capability. `legacyMuteAll:true`
also omits the projection object and defaults the capability to false, serving
a LiveMix 0.17.7-shaped snapshot. `legacyFx:true` defaults `legacyMuteAll` to true.
`toggleMuteAll` / `setMuteAll` return `{ group, muted }`, preserve mute groups,
and use the normal revision/delta path; identical sets return `changed:false`.
