# LiveMix -> OBS test tools

Everything here runs against a **portable copy of OBS** (`%LOCALAPPDATA%\LiveMixObsTest\obs`, own config, websocket
port 4466 without a password) started on an invisible desktop, so the real OBS on the PC is never touched.

| Script | What it does |
|---|---|
| `portable_obs.py setup / start / stop / log` | copy OBS once, write the test profile (FLAC-in-MKV recordings to `...\LiveMixObsTest\rec`), start it with the plugin from `obs-plugin\build_x64\rundir\RelWithDebInfo`, stop only the OBS it started |
| `fake_writer.py` | pretends to be LiveMix: a 997 Hz tone at a chosen clock error (`--ppm`), restarts, rate changes, stalls |
| `record_and_check.py` | adds the source (or a filter on a capture host), records, and checks every 10 ms block for clicks, gaps, skips and repeats |
| `run_matrix.py` | the short real-OBS checks (+-200 ppm, filter, restart + stall, 44.1 -> 48 kHz) |
| `soak.py --isolated NAME --port P --hours H` | long run on a second, isolated test OBS (own folder, port, ring and plugin copy) in 30-minute segments; start it with `tools\detach_run.py` so it outlives the session |

## Rings
- By default the test OBS and the fake writer use the **test ring** `Local\LiveMix.ObsAudio.test`, never the real one,
  so a LiveMix that is live on the same PC is never disturbed.
- `LMOBS_TEST_RING=real` makes the test OBS read the real ring - for an end-to-end check with a real LiveMix build.
  Do not run `fake_writer.py` with it while a LiveMix is sending.
- `LMOBS_TEST_ROOT`, `LMOBS_TEST_PORT`, `LMOBS_TEST_PLUGIN_DIR`, `LMOBS_TEST_PRIORITY` select a second isolated OBS
  (what `soak.py --isolated` sets up).
