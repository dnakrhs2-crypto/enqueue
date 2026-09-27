# LiveMix 0.11.0 — Windows audio devices + "Send to OBS" (with the LiveMix OBS plugin) Implementation Plan

> **For agentic workers:** executed as Astra (codex) rounds with Claude reviewing, building and verifying between rounds
> (the operator's standing rule for LiveMix). Steps use checkbox (`- [ ]`) syntax for tracking. Read the whole
> "Decisions" and "Global Constraints" sections before any task - they are part of every task's requirements.

**Goal:** LiveMix 0.11.0 runs on ordinary Windows audio devices (USB mic, headset, onboard) as well as ASIO, and its
master can be sent to OBS Studio through a LiveMix OBS plugin (an OBS audio source and an OBS audio filter in one DLL)
that LiveMix installs by itself, with sample-rate drift corrected so a 2-4 hour stream never clicks or drifts.

**Architecture:** LiveMix writes its master (after the master chain) into a lock-free shared-memory ring from the audio
callback. The OBS plugin (C, libobs only) reads the ring on its own clock and runs an adaptive-ratio resampler (ASRC)
steered by a PI controller on the ring fill. The same ASRC + controller (one C module shared by both projects) also
feeds a separate Windows monitor output device when the mic and the headphones are different devices.

**Tech Stack:** C++17 + JUCE 8.0.15 (LiveMix, local checkout `C:\Users\claude\JUCE`), C17 + libobs from OBS 31.1.1
(plugin, official obs-plugintemplate build system in `obs-plugin/`), Win32 (file mapping, MMDevice, ShellExecuteEx),
Inno Setup, Python test tools.

**Spec:** this document ("Decisions" below) + the feasibility review
`C:\Users\claude\tools\claude_harness\codex_reviews\lm_obs_wasapi_consult_OUT.md` (OBS 32.1.2 / JUCE source facts).

**Working tree:** `C:\Users\claude\gocue-obs` (branch `livemix-obs` from main 9553704). LiveMix build =
`tools\claude_harness\build_obs.cmd` (preset `local`, `build\vs2022`). Plugin build = `obs-plugin\` preset `windows-x64`
(`.deps\` already downloaded: OBS 31.1.1 sources + obs-deps 2025-07-11, libobs built; output
`obs-plugin\build_x64\RelWithDebInfo\livemix-obs.dll`). **The sandbox has no network: never run the plugin
configure step that downloads; build with `cmake --build --preset windows-x64` only.**

---

## Decisions (operator-approved 2026-09-27 - do not change)

1. OBS side: **both** an independent audio source and an audio filter, in one DLL `livemix-obs.dll`.
2. A mic and a monitor on **different** Windows devices get drift correction (ASRC) - long runs must not click.
3. Everything ships together as **LiveMix 0.11.0** (no separate early release).
4. The LiveMix installer bundles the plugin and installs it (optional task, checked). Pressing "OBS로 보내기" when the
   plugin is missing or older installs it automatically; when the folder is not writable, one UAC prompt and retry.

Design choices made by Claude (binding for the implementer):

- Device types offered: `ASIO` (as today), `Windows Audio` (shared, label `윈도우 오디오`),
  `Windows Audio (Low Latency Mode)` (`윈도우 오디오 (저지연)`), `Windows Audio (Exclusive Mode)` (`윈도우 오디오 (독점)`).
  DirectSound is never listed. Only types JUCE reports as available are offered; ASIO only when the build has it.
- Windows types: one input device (required) + one output device or **none** (`없음 (OBS로만 보내기)`); outputs are the
  pair 1-2 only (like Enqueue's non-ASIO policy). ASIO keeps one device for both and up to 64 channels.
- Same physical device (capture and render endpoints share a `PKEY_Device_ContainerId`) -> one JUCE duplex device
  (lowest latency). Different or unknown -> **split mode**: the JUCE device is opened input-only and drives the graph;
  a second JUCE `AudioIODevice` (output-only, same type) is the monitor, fed from an SPSC FIFO through the shared ASRC.
- Session format **version 4**: `device {type, input, output, bufferSize, sampleRate}` and `master.sendToObs`.
  Files v1-v3 load as `type ASIO, input = output = name`, `sendToObs = false`. Older LiveMix refuses v4 (existing policy).
- "OBS로 보내기" is per session, default OFF, a toggle on the master card with a status next to it.
- Shared memory names (one LiveMix per Windows session - the app is single-instance):
  `Local\LiveMix.ObsAudio.v1` (audio ring, created by LiveMix only) and `Local\LiveMix.ObsAudio.v1.Readers`
  (reader presence, created by whichever side comes first). Both are created with an explicit security descriptor
  `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<current user SID>)S:(ML;;NW;;;ME)` so an elevated OBS and a normal LiveMix
  (and the reverse) can open them. The reader never writes into the audio ring.
- OBS plugin ids: source `livemix_master_source` (`LiveMix 마스터`), filter `livemix_master_filter`
  (`LiveMix 마스터 받기`, property `방식`: `바꾸기` default / `섞기`). Disconnected -> silence (filter in replace mode
  outputs silence too; the operator disables the filter to hear the parent again).
- ASRC: our own C module (no allocation after init, no locks): 32-tap Kaiser-windowed sinc (beta 8.6), 512 phases with
  linear phase interpolation, cutoff `0.45 * min(fsIn, fsOut)`, variable ratio per call. PI controller on the fill:
  target 30 ms (OBS) / `2 x output period + input period + 3 ms` (monitor), low-pass 1 s, correction clamp +-1000 ppm,
  slew <= 20 ppm per second. A change of 100 ppm is 0.17 cent - inaudible.
- Plugin install locations: OBS 33+ -> `%ProgramData%\obs-studio\plugins\livemix-obs\livemix-obs.dll` +
  `...\livemix-obs\data\locale\*.ini`; OBS <= 32 or not found -> legacy `...\livemix-obs\bin\64bit\livemix-obs.dll` +
  `...\livemix-obs\data\locale\*.ini`. Never both layouts at once (installing one removes the other's DLL).
  OBS version = file version of `<install>\bin\64bit\obs64.exe`, install dir from `HKLM\SOFTWARE\OBS Studio` (default
  value), falling back to `C:\Program Files\obs-studio`. Plugin version = its VERSIONINFO (the template writes one).
- A loaded (locked) older DLL is replaced by rename-then-copy (`livemix-obs.dll` -> `livemix-obs.dll.old-<n>`, copy the
  new one, delete `*.old-*` next time OBS is not running). If rename fails -> "OBS를 종료한 뒤 다시 눌러 주세요".
- One install code path for three callers: the button (in-process), the installer (`LiveMix.exe --install-obs-plugin`
  run hidden at the end of setup), the UAC fallback (`runas` of the same command). With that flag LiveMix does the
  install, writes a result line to `%TEMP%\LiveMix\obs-install-result.txt`, and exits without UI - it must be allowed
  to run while another LiveMix instance is open (`moreThanOneInstanceAllowed()` true for that flag only).

## Global Constraints

- UI text is Korean; code, comments and commit messages English. No "VST" in product names.
- Real-time rule: no allocation, lock, logging, file or kernel wait in the LiveMix audio callback, the monitor output
  callback, or the OBS `filter_audio` callback. The OBS source worker thread may block (it is not OBS's audio thread).
- Windows x64 only. MSVC warnings stay at the project's level; the plugin builds warning-free under the template flags.
- `livemix/shared/*` (protocol header + ASRC + drift controller) is plain C17, MIT-licensed header comment, compiled
  into LiveMix (C++ via `extern "C"`) and into the plugin. `obs-plugin/` carries GPL-2.0-or-later (template LICENSE).
- The plugin is built against OBS 31.1.1 (template buildspec) so OBS 31.1+ loads it (the loader refuses newer
  major.minor). It uses libobs only: `ENABLE_FRONTEND_API=OFF`, `ENABLE_QT=OFF`.
- Tests never write to real user data: not `%APPDATA%\LiveMix`, not `Documents\LiveMix`, not the real
  `%ProgramData%\obs-studio`, not a real OBS profile. Paths are injectable; tests use `juce::File::createTempFile` /
  temp dirs. A test that needs OBS uses the portable copy described in Task 7.
- Any new `.cmd` / `.bat` is pure ASCII with CRLF line ends.
- Keep ASIO behaviour identical for existing users: same device picking, same 64-channel routing, same buttons.

## Review Focus (the failure modes most likely to bite a real user - each has a test in the owning task)

1. Mic and headphones on different devices for 4 hours -> no click, latency stable (Task 1 ASRC soak simulation at
   +-200 ppm; Task 3 split-mode offline test; Task 8 real soak).
2. LiveMix's graph lock busy for a block -> the OBS stream still advances by exactly that many frames of silence
   (Task 4 engine test).
3. OBS started before LiveMix, LiveMix restarted, sample rate switched 48k -> 44.1k while OBS keeps running -> the
   plugin reconnects and resyncs without a burst or a stuck silence (Task 6 core tests with the fake writer).
4. An old `.livemix` (v1-v3) opens on the same ASIO device; saving writes v4; v4 is refused by the old reader
   (Task 2 session tests).
5. "OBS로 보내기" pressed while OBS runs with an older plugin loaded, or the plugin folder is read-only, or OBS is not
   installed, or OBS is 33 -> the right file layout, a clear Korean message, never a half-installed pair of layouts
   (Task 5 installer tests on temp roots).

---

### Task 1: Shared C module - protocol header, ASRC, drift controller

**Files:**
- Create: `livemix/shared/lm_obs_protocol.h` (ring + readers layout, names, constants, inline read/write helpers)
- Create: `livemix/shared/lm_asrc.h`, `livemix/shared/lm_asrc.c` (resampler + PI controller)
- Modify: `CMakeLists.txt` (add `livemix/shared/lm_asrc.c` to `LiveMixCore`, include dir `livemix/shared`)
- Test: `tests/LiveMixAsrcTests.cpp` (category "LiveMix"), registered like the other LiveMix tests

**Interfaces (produced):**
```c
/* lm_obs_protocol.h */
#define LM_OBS_RING_NAME    L"Local\\LiveMix.ObsAudio.v1"
#define LM_OBS_READERS_NAME L"Local\\LiveMix.ObsAudio.v1.Readers"
#define LM_OBS_MAGIC        0x424F4D4Cu   /* "LMOB" */
#define LM_OBS_PROTOCOL_MAJOR 1
#define LM_OBS_CHANNELS     2
#define LM_OBS_CAPACITY_FRAMES 32768u     /* power of two: 682 ms at 48 kHz */
#define LM_OBS_MAX_READERS  16
typedef struct lm_obs_ring_header {        /* 256 bytes, every 64-bit field 8-byte aligned */
    uint32_t magic, protocol_major, protocol_minor, header_bytes;
    uint32_t mapping_bytes, data_offset, channels, capacity_frames;
    volatile int64_t epoch;          /* +1 on every (re)start, format change, send ON, discontinuity */
    volatile int64_t sample_rate;    /* Hz */
    volatile int64_t write_frames;   /* frames written in this epoch; stored with release after the PCM copy */
    volatile int64_t heartbeat_qpc;  /* QueryPerformanceCounter of the last audio callback */
    volatile int64_t qpc_frequency;
    volatile int64_t send_enabled;   /* 0 / 1 */
    volatile int64_t writer_pid;
    volatile int64_t silent_frames;  /* diagnostics: zero frames written for busy-graph blocks */
    uint8_t reserved[256 - 32 - 8 * 8];
} lm_obs_ring_header;                /* PCM follows at data_offset: float32 interleaved [capacity_frames][2] */
typedef struct lm_obs_reader_slot { volatile int64_t pid, heartbeat_qpc, kind /*1 source 2 filter*/, fill_us; } lm_obs_reader_slot;
typedef struct lm_obs_readers { uint32_t magic, protocol_major, slot_count, reserved; lm_obs_reader_slot slot[LM_OBS_MAX_READERS]; } lm_obs_readers;

/* writer side (LiveMix audio thread) */
void lm_obs_write(lm_obs_ring_header* h, float* ring, const float* left, const float* right, uint32_t frames);
void lm_obs_write_silence(lm_obs_ring_header* h, float* ring, uint32_t frames);
/* reader side: copies up to max_frames from *read_pos; returns frames copied, or -1 on overrun / epoch change */
int32_t lm_obs_read(const lm_obs_ring_header* h, const float* ring, int64_t epoch, int64_t* read_pos, float* dst_interleaved, uint32_t max_frames);
```
```c
/* lm_asrc.h */
typedef struct lm_asrc lm_asrc;                  /* caller-allocated: lm_asrc_size(channels) bytes */
size_t lm_asrc_size(int channels);
void   lm_asrc_init(lm_asrc* s, int channels, double fs_in, double fs_out);
void   lm_asrc_reset(lm_asrc* s);
void   lm_asrc_set_correction_ppm(lm_asrc* s, double ppm);   /* input consumed per output = fs_in/fs_out*(1+ppm/1e6) */
/* produces exactly out_frames unless input runs out; returns frames produced, *in_used = input frames consumed */
int    lm_asrc_process(lm_asrc* s, const float* in, int in_frames, int* in_used, float* out, int out_frames);
double lm_asrc_latency_input_frames(const lm_asrc* s);      /* delay held inside the filter, in input frames */

typedef struct lm_drift { double target, kp, ki, integ, lp, lp_alpha, ppm, max_ppm, slew_per_s; int primed; } lm_drift;
void   lm_drift_init(lm_drift* d, double target_frames, double update_hz);
double lm_drift_update(lm_drift* d, double fill_frames, double dt_s);   /* returns the ppm to apply */
```

- [ ] **Step 1: Write the failing tests** (`tests/LiveMixAsrcTests.cpp`):
  - ring: write 3 blocks of 480 frames of a ramp, read them back in 7 uneven reads -> identical samples; a reader
    that falls `capacity` behind gets -1 (overrun) and resyncs to `write_frames - target`; an epoch change returns -1.
  - silence: `lm_obs_write_silence(480)` advances `write_frames` by 480 and the frames read are 0.0f.
  - ASRC quality: 1 kHz sine at 48 kHz, ratio 1.0 and 48000->44100 and 44100->48000: after 4096 frames of warm-up, the
    error against an ideal sine (phase-fitted) is below -80 dBFS; DC gain within 0.01 dB.
  - ASRC ratio: with +200 ppm correction, over 48,000 output frames the input used is 48,009.6 +-1.
  - drift loop (simulated clocks, no audio device): a producer at 48000*(1+e) and a consumer at 48000 pulling 480
    frames every 10 ms, `e` in {0, +200, -200, +1000, -1000} ppm, 2 simulated hours: after 60 s the fill stays within
    target +-5 ms, never underruns, never overruns; `e` switching +100 -> -100 ppm at 30 min recovers within 60 s.
  - no allocation: process 10,000 blocks with a counting allocator hook -> zero allocations after init.
- [ ] **Step 2: Run** `EnqueueTests.exe` (hidden desktop runner) -> the new tests fail to link / fail.
- [ ] **Step 3: Implement** the three files (plain C17; `lm_obs_*` inline helpers use `InterlockedExchange64` /
  `_ReadWriteBarrier` + `MemoryBarrier` for release/acquire; PCM copy handles the wrap).
- [ ] **Step 4: Run** the whole suite -> all pass (baseline count + new).
- [ ] **Step 5: Commit** `LiveMix: shared OBS ring protocol, ASRC and drift controller (C)`.

### Task 2: Session v4 - device model and sendToObs

**Files:**
- Modify: `livemix/src/MixModel.h` (MixDevice, MixMaster, `currentVersion = 4`), `livemix/src/MixModel.cpp`
  (JSON read/write, sanitise, migration), `livemix/src/MixDocument.h/.cpp` (`setDeviceInfo(const MixDevice&)`,
  `setSendToObs(bool)` with notifyValue + dirty like `setMasterOutput`)
- Test: `tests/MixSessionTests.cpp`, `tests/MixDocumentTests.cpp`

**Interfaces (produced):**
```cpp
struct MixDevice
{
    juce::String type = "ASIO";   // JUCE AudioIODeviceType name
    juce::String input;           // ASIO: the device; Windows types: capture endpoint name (required)
    juce::String output;          // ASIO: same as input; Windows types: render endpoint name, "" = none (OBS only)
    int bufferSize = 256;         // Windows shared mode: informational (the engine period)
    double sampleRate = 48000.0;
    bool isAsio() const noexcept { return type.containsIgnoreCase ("ASIO"); }
};
struct MixMaster { std::vector<PluginSlotState> chain; int outputFirst = 0; bool sendToObs = false; };
```
JSON v4: `"device": {"type","input","output","bufferSize","sampleRate"}`, `"master": {..., "sendToObs": false}`.

- [ ] **Step 1: Failing tests**: v3 file with `device.name = "Focusrite USB ASIO"` loads as type ASIO,
  input = output = that name, sendToObs false; v4 round-trip keeps type/input/output/"" output/sendToObs; `sanitise`
  forces output = input for ASIO; a v5 file is refused with the existing "newer LiveMix" message; `setSendToObs`
  marks the document dirty and notifies once.
- [ ] **Step 2-4:** implement, run the suite, all pass.
- [ ] **Step 5: Commit** `LiveMix: session v4 (device type, input/output, sendToObs)`.

### Task 3: Engine - Windows devices, input-only, split mode with the monitor ASRC

**Files:**
- Create: `livemix/src/AudioBackends.h/.cpp` (type list + Korean labels, endpoint ContainerId lookup via
  IMMDeviceEnumerator, `sameContainer(inputName, outputName)`), `livemix/src/MonitorOutput.h/.cpp` (second JUCE
  device, SPSC FIFO, ASRC + drift, own callback)
- Modify: `livemix/src/MixEngine.h/.cpp` (replace ASIO-only `initialise/openDevice/openSessionDevice/restartDevice/
  setBufferSize` with `MixDevice` based ones; outputs 1-2 for Windows types; split mode: renderBlock writes the device
  output pair into a staging buffer that is pushed to MonitorOutput), `livemix/src/LiveMixSettings.h/.cpp`
  (`lastDevice` JSON; one-time migration from the old `audioDeviceState` ASIO XML)
- Test: `tests/MixEngineTests.cpp`, new `tests/LiveMixMonitorTests.cpp`

**Interfaces (produced):**
```cpp
juce::String MixEngine::openDevice (const MixDevice& wanted);   // "" on success; rollback to what ran before on failure
MixDevice    MixEngine::getOpenDevice() const;                   // what really runs (type, names, rate, buffer)
bool         MixEngine::isSplitMonitor() const noexcept;         // Windows input + different output device
juce::StringArray AudioBackends::availableTypes (juce::AudioDeviceManager&);   // ASIO first, then the 3 Windows types
juce::String AudioBackends::label (const juce::String& typeName);             // "ASIO", "윈도우 오디오", ...
```
- [ ] **Step 1: Failing tests**: offline split-mode test - MonitorOutput driven by a fake pull at 48000*(1+300ppm)
  while the graph renders at 48000 for 30 simulated minutes: no FIFO under/overrun after 60 s, gain unity (1 kHz sine
  level within 0.05 dB); input-only engine renders with 0 outputs (meters and loudness still move); `openDevice`
  rollback when the new device fails (use a fake `AudioIODeviceType` registered in the test manager); Windows type caps
  the output pair to 1-2 and sanitises a session asking for 5-6.
- [ ] **Step 2-4:** implement, run the suite, all pass.
- [ ] **Step 5: Commit** `LiveMix: Windows audio devices, output none, split monitor with drift correction`.

### Task 4: UI - device settings, top bar, messages; OBS sender and master card

**Files:**
- Create: `livemix/src/ObsSender.h/.cpp` (creates both mappings with the security descriptor on the message thread;
  audio-thread `write(L, R, n)` / `writeSilence(n)`; `readerState()` -> none / connected / stale using the readers
  mapping and `OpenProcess` liveness)
- Modify: `MixEngine` (after the master chain + loudness: `obsSender.write`; the TryLock-failed early return writes
  silence of `numSamples`; `audioDeviceAboutToStart` bumps the epoch and publishes the rate), `MixDocument` wiring,
  `ui/MasterCard.h/.cpp` (toggle `OBS로 보내기` + status label in the column, compact stack and strip forms),
  `ui/SettingsDialog.cpp` (장치 종류 / 입력(마이크) / 출력(모니터) with `없음 (OBS로만 보내기)` / 샘플레이트 /
  버퍼 (저지연·독점 only) / ASIO 제어판 only for ASIO), `ui/TopBar.cpp` (caption = type label, combo = devices of the
  current type), `ui/MainComponent.cpp` + `Main.cpp` (generalised startup / status / safe-mode texts:
  `오디오 장치를 열지 못했습니다`, `오디오 멈춤 - 설정에서 오디오 장치를 확인하세요`)
- Test: `tests/MixEngineTests.cpp` (busy-lock silence), new `tests/LiveMixObsSenderTests.cpp`

Status texts next to the toggle: `OBS 연결됨` (green), `OBS 대기 중` (dim), `OBS 플러그인 설치 필요` (orange),
`OBS를 다시 시작하세요` (orange), `오디오 멈춤` (red when the device is not running).

- [ ] **Step 1: Failing tests**: engine with a held graph lock renders 3 blocks -> `write_frames` advanced by the
  three block sizes, samples 0.0f; sender OFF -> `send_enabled = 0`, no writes; ON again -> epoch +1; a reader slot
  with a fresh heartbeat and a live pid -> connected, a dead pid -> none; mapping names and SD applied (open the
  mapping from the test with `OpenFileMappingW(FILE_MAP_READ)`).
- [ ] **Step 2-4:** implement, run the suite, all pass. Claude verifies the UI on this PC (FlexASIO = ASIO path,
  WASAPI capture card input + S/PDIF / NVIDIA HDMI outputs = split mode).
- [ ] **Step 5: Commit** `LiveMix: device settings for ASIO and Windows audio; Send to OBS on the master card`.

### Task 5: Plugin installer inside LiveMix

**Files:**
- Create: `livemix/src/ObsPluginInstaller.h/.cpp` (detect OBS dir + version, target layout, compare VERSIONINFO,
  rename-then-copy, cleanup of `*.old-*`, `needsElevation`, result reporting), `Main.cpp` (`--install-obs-plugin`
  mode, single-instance bypass for that flag only), MasterCard/MainComponent (install on toggle when needed, messages;
  `needsElevation` -> `ShellExecuteExW` verb `runas` on `LiveMix.exe --install-obs-plugin` (wide strings: the install
  path may contain Korean), wait for it on a background thread (never block the message thread), then read
  `%TEMP%\LiveMix\obs-install-result.txt` and show the result; the user declining UAC -> `설치를 취소했습니다`)
- Test: `tests/LiveMixObsInstallerTests.cpp` (all on temp roots: fake ProgramData, fake OBS dir with a copied
  `obs64.exe`-like file carrying a VERSIONINFO - use any small exe from the build with a known version, or inject the
  version through the test hook)

**Interfaces (produced):**
```cpp
struct ObsPluginInstaller
{
    struct Roots { juce::File programData, obsInstallDir, bundledPlugin; std::function<bool()> isObsRunning; };
    enum class Result { alreadyCurrent, installed, installedRestartObs, needsElevation, obsBusyCloseIt, noBundledFiles, failed };
    static Roots systemRoots();                          // real paths (never used by tests)
    static int  obsMajorVersion (const Roots&);          // 0 = OBS not found
    static bool isInstalledAndCurrent (const Roots&);
    static Result install (const Roots&, juce::String& message);   // Korean message for the UI
};
```
- [ ] **Step 1: Failing tests**: fresh install on "OBS 32" root -> legacy layout files present, new layout absent;
  on "OBS 33" -> new layout, and a pre-existing legacy DLL is removed; same version present -> alreadyCurrent (no
  write); older DLL "loaded" (file held open with share-delete by the test) -> renamed + new copy, Result
  installedRestartObs; rename impossible (held without share-delete) -> obsBusyCloseIt; read-only target dir ->
  needsElevation; OBS not found -> legacy layout, message says OBS를 찾지 못했다; `*.old-*` removed when not running.
- [ ] **Step 2-4:** implement, run the suite, all pass.
- [ ] **Step 5: Commit** `LiveMix: install or update the OBS plugin from Send to OBS (and --install-obs-plugin)`.

### Task 6: The OBS plugin

**Files (all under `obs-plugin/`):**
- Modify: `CMakeLists.txt` (sources below + `../livemix/shared/lm_asrc.c`, include `../livemix/shared`, option
  `LIVEMIX_OBS_TESTS` building `livemix-obs-core-tests.exe` without libobs), `buildspec.json` (already renamed),
  `data/locale/ko-KR.ini`, `data/locale/en-US.ini`
- Create: `src/plugin-main.c` (register both), `src/receiver.h/.c` (connection manager thread shared by instances:
  opens/validates the mapping every 1 s, publishes an atomic pointer; per-instance `lm_receiver` with read position,
  local FIFO, ASRC, drift, fade state, stats), `src/livemix-source.c` (worker: sleeps to `T0 + sent/Fout`, pulls N =
  10 ms, outputs at OBS's rate with timestamp `T0 + sent*1e9/Fout`), `src/livemix-filter.c` (replace / mix, exactly
  `audio->frames` per call, channel mapping for mono / stereo / 5.1 / 7.1 OBS layouts), `tests/core_tests.c`
- Stats: every 10 s at LOG_INFO `[livemix-obs] fill=..ms ppm=.. under=.. over=.. resync=.. epoch=..` (worker only).

- [ ] **Step 1: Failing core tests** (no libobs; fake writer in-process on a private mapping name): connect after the
  writer starts; writer restarts (epoch+1) -> resync with a 10 ms fade, no burst; rate 48000 -> 44100 mid-stream ->
  new ASRC ratio, fill back to target within 10 s; writer stalls 3 s -> silence with fade, then recovery; overrun
  (reader paused 1 s) -> resync; +-1000 ppm steady for 2 simulated hours -> zero under/overrun after lock.
- [ ] **Step 2-4:** implement, `cmake --build --preset windows-x64` (no configure), run core tests, all pass.
- [ ] **Step 5: Commit** `LiveMix OBS plugin: LiveMix master source and filter with drift correction`.

### Task 7: Test tools and the real-OBS check

**Files:**
- Create: `tools/obs_test/fake_writer.py` (writes the real ring name with a sine / impulse train at
  `rate*(1+ppm/1e6)` paced by QPC; options `--ppm --rate --restart-every --stall`), `tools/obs_test/portable_obs.py`
  (copies `C:\Program Files\obs-studio` to `%LOCALAPPDATA%\LiveMixObsTest\obs` once, `portable_mode.txt`, a scene
  collection with the source + a mic-less filter host, websocket on port 4466 without auth, loads the plugin from
  that copy's legacy `obs-plugins\64bit` + data folder), `tools/obs_test/record_and_check.py` (websocket: start/stop
  recording to PCM in MKV, then numpy: sine phase continuity, impulse spacing drift, report discontinuities),
  `tools/obs_test/README.md`
- [ ] Steps: build tools, run a 5-minute check at 0 / +200 / -200 ppm and with a LiveMix restart -> zero
  discontinuities; the 4-hour soak (+150 ppm) is started by Claude with `tools\detach_run.py` (Task 8).
- [ ] Commit `LiveMix OBS: fake writer, portable OBS test instance and recording checker`.

### Task 8: Installer, release plumbing, site, notes, review

**Files:**
- Modify: `installer/LiveMix.iss` (payload `{app}\obs-plugin\livemix-obs\...`; task `obsplugin` checked:
  `[Run] {app}\LiveMix.exe --install-obs-plugin` hidden, waituntilterminated; `CloseApplicationsFilter=LiveMix.exe`
  so setup never closes OBS), `tools/release.py` (build the plugin with the existing `.deps`, stage the DLL + locale
  into the installer source dir, fail the release when the DLL is missing), `CMakeLists.txt` `LIVEMIX_VERSION 0.11.0`,
  `site/livemix/index.html` (features + an "OBS로 보내기" how-to), `docs/release-notes/livemix/0.11.0.html`
- [ ] Steps: dry-run the installer on this PC into a temp dir, check the plugin lands in the legacy layout for OBS
  32.1.2 and OBS starts with `[livemix-obs]` in its log; Astra read-only review of the whole branch; fix; 4-hour soak;
  release with the usual wrapper.
- [ ] Commit `LiveMix 0.11.0: installer, release plumbing, site and notes`.
