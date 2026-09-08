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

The fake server loads these files, substitutes the live port/instance/token and
current document values, and sends the same envelopes. It implements only hello,
ping, requestState and the two mic commands needed for Round 2A.
