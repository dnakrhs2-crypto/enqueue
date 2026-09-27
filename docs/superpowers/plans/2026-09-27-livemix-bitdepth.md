# LiveMix 0.11.1 — bit depth (show it everywhere, choose it where Windows lets the app) Implementation Plan

> **For agentic workers:** executed as Astra (codex) rounds with Claude reviewing, building and verifying between rounds
> (the operator's standing rule for LiveMix). Read "Facts", "JUCE API" and "Decisions" before any task - they are part
> of every task's requirements.

**Goal:** the operator sees the real bit depth of the running audio device for every device type, and can choose it
in Windows exclusive mode (the only mode where the application decides it).

**Architecture:** a small patch in the local JUCE checkout (done by Claude, committed) reports the WASAPI formats and
takes a process-wide exclusive-mode preference. LiveMix stores the choice with the device (`MixDevice::sampleFormat`),
applies it before every device open, derives one `DeviceFormat` summary for the UI, and shows it in the settings
dialog and the top bar.

**Tech Stack:** C++17 + JUCE 8.0.15 (patched local checkout `C:\Users\claude\JUCE`), LiveMix sources in `livemix/src`,
tests in `tests/` (one `EnqueueTests` executable).

**Working tree:** `C:\Users\claude\gocue-obs`, branch `livemix-bitdepth` (from main 6de9c1b = LiveMix 0.11.0).

---

## Facts (verified by Claude on this PC with the patched JUCE, 2026-09-27)

- **ASIO**: the driver fixes the sample type of every channel (`ASIOSampleType`); a host cannot choose it. JUCE's
  `getCurrentBitDepth()` for ASIO = the driver's bits (16 / 24 / 32 - 32 for both int32 and float32; JUCE does not say
  which).
- **Windows shared ("윈도우 오디오") and low-latency ("저지연")**: LiveMix exchanges 32-bit float with the Windows audio
  engine; the hardware runs at the endpoint's *Default Format* chosen in the Windows Sound settings (Recording /
  Playback tab -> device -> Properties -> Advanced). Only the operator changes it there. In shared mode Windows also
  converts the sample rate when LiveMix runs at a different rate than that setting.
- **Windows exclusive ("독점")**: the application picks the format. JUCE's own order: 32 float, 32 int, 24 (in 4 bytes),
  24 (3 bytes), 20, 16. Probe on this PC: the HDMI capture card accepts 16-bit only; the NVIDIA HDMI output and the
  S/PDIF output accept 16 and 24 (automatic picks 24). A refused choice falls back to JUCE's order and still opens.
- LiveMix's processing and the OBS path are 32-bit float already - nothing changes there.

## JUCE API (patched checkout, commits b6a1f02 + 0b5ea27; declared in `juce_AudioIODeviceType.h` under `JUCE_WINDOWS && JUCE_WASAPI`)

```cpp
namespace juce {
struct WasapiFormatInfo
{
    int streamBits = 0;          // valid bits of the stream opened with WASAPI; 0 = that direction is not open
    bool streamIsFloat = false;
    int deviceBits = 0;          // the endpoint's shared-mode format (Windows Sound setting); 0 = unknown
    bool deviceIsFloat = false;
    double deviceSampleRate = 0;
    enum ExclusiveFormat { exclusiveInt16 = 1, exclusiveInt24 = 2, exclusiveInt32 = 4, exclusiveFloat32 = 8 };
    int exclusiveFormats = 0;    // exclusive mode, while open: the formats the driver accepts at the stream's rate/layout
};
bool getWasapiFormatInfo (AudioIODevice&, WasapiFormatInfo& input, WasapiFormatInfo& output); // false: not a WASAPI device (ASIO, test fakes)
void setWasapiExclusivePreferredFormat (int bits, bool isFloat);  // process-wide; read when a WASAPI device is created and at every open()
void getWasapiExclusivePreferredFormat (int& bits, bool& isFloat);
}
```

- In shared / low-latency mode `streamBits` is the engine's mix format (normally 32 float): show `deviceBits` there.
- A WASAPI device's `getCurrentBitDepth()` now returns the open stream's bits (output first, else input; 32 when closed).
- `AudioDeviceManager::setAudioDeviceSetup` does not reopen an unchanged setup. `MixEngine::openDevice` already closes
  the device first, so a new choice takes effect there. **The JUCE checkout is outside the worktree: never edit it.**

## Decisions (binding)

1. **Model.** `MixDevice::sampleFormat` (`juce::String`, added as the LAST member so every existing aggregate
   initialiser stays valid): `""` = 자동 (JUCE's order: the highest format the device accepts), `"int16"`, `"int24"`,
   `"int32"`, `"float32"`. Stored in the session file's `device` object (key `sampleFormat`; the session version stays
   4 - LiveMix 0.11.0 ignores keys it does not know) and in `LiveMixSettings`' last device. Missing / unknown /
   non-string values read as `""` without failing the load. The choice is kept whatever the type (it acts only in
   exclusive mode), so going back to exclusive restores it.
2. **Engine - applying the choice.** Every open path calls `juce::setWasapiExclusivePreferredFormat` with the device's
   choice before the device is created/opened: `initialise` (saved device; the fallbacks use 자동), `openDevice`,
   `restartDevice`, `setBufferSize`, `openSessionDevice`, and the rollback inside `openDevice` (restore the PREVIOUS
   device's choice before reopening it). `getOpenDevice()` returns the choice in force (`openedDevice.sampleFormat`),
   so every "reopen the current device with one thing changed" path keeps it. `openSessionDevice`: a different choice
   makes the running device "not the session's" only for the exclusive type (reopen); for the other types it only
   records the choice (no reopen). Every place that compares or copies `MixDevice` fields includes the new one
   (e.g. `MixDocument` device dirty check near `MixDocument.cpp:541`, `LiveMixSettings::getLastDevice/setLastDevice`).
   The monitor device of split mode follows the same process-wide choice (nothing extra to do, but report it - 3).
3. **Engine - what runs.** `MixEngine::DeviceFormat MixEngine::getDeviceFormat() const` (message thread, cheap - the UI
   polls it):
   ```cpp
   struct DeviceFormat
   {
       enum class Kind { none, asio, windowsShared, windowsExclusive };   // windowsShared = shared and low-latency
       Kind kind = Kind::none;
       int inputBits = 0;   bool inputFloat = false;    // 0 = unknown / not open
       int outputBits = 0;  bool outputFloat = false;   // the device's output, or split mode's monitor device; 0 = none / unknown
       double inputDeviceRate = 0, outputDeviceRate = 0;  // windowsShared: the Windows setting's rate (0 = unknown)
       int inputAccepted = 0, outputAccepted = 0;       // windowsExclusive: juce::WasapiFormatInfo::exclusive* bits
       bool inputRefused = false, outputRefused = false; // windowsExclusive with a choice: that direction opened in another format
   };
   ```
   - asio: both directions = `getCurrentBitDepth()` of the running device (never "float").
   - windowsShared: bits / float / rate from `WasapiFormatInfo::device*` of each open direction.
   - windowsExclusive: bits / float from `stream*`, accepted = `exclusiveFormats`, refused = a choice is set and the
     direction's (bits, float) differs from it.
   - Split mode: the output side comes from `MonitorOutput`'s own device (add a small accessor there).
   - A device `getWasapiFormatInfo` does not know (the test fakes) while a Windows type runs: exclusive -> bits from
     `getCurrentBitDepth()`, accepted 0, refused false; shared -> 0 (unknown).
   - Put the mapping from (kind, `WasapiFormatInfo` in/out, choice, ASIO bits) to `DeviceFormat` in a pure function the
     tests call with synthetic values.
4. **Settings dialog.** A new row captioned `비트뎁스` under the sample-rate / buffer row, per type:
   - ASIO: text `24비트 · ASIO 드라이버가 정합니다`; when the device has a control panel add
     ` (바꿀 수 있는 장치는 ASIO 제어판에서)`. Not running: `장치가 열려 있지 않습니다`.
   - 윈도우 오디오 / 저지연: text like `입력 16비트 · 48 kHz, 출력 24비트 · 48 kHz (윈도우 소리 설정의 '기본 형식')` - only
     the directions that are open, `알 수 없음` for an unknown one - and a button `윈도우 소리 설정...` that opens the
     classic Sound dialog on the Recording tab (`control.exe mmsys.cpl,,1`, via `juce::ChildProcess` or
     `juce::File::startAsProcess`; tests must never click it), with the hint
     `녹음/재생 탭 → 장치 더블클릭 → 고급 → 기본 형식에서 바꿉니다.` When a direction's Windows rate differs from the rate
     LiveMix runs at, add `윈도우가 44.1 kHz → 48 kHz로 변환 중` for it.
   - 독점: a combo (component ID `device-bitdepth`) with `자동 (장치가 받는 가장 높은 형식)`, `16비트`, `24비트`, `32비트`,
     `32비트 부동소수점`. While an exclusive device runs, an item no open direction accepts is disabled and suffixed
     ` — 이 장치 지원 안 함`; an item only one direction accepts gets ` — 입력만` / ` — 출력만`. Below it: `지금: 입력 16비트,
     출력 24비트` and, when a direction was refused, a note in the warning colour, e.g.
     `입력 장치가 24비트를 받지 않아 16비트로 열었습니다.` (no alert window - the device did open). Picking an item reopens
     the device with that choice through `engine.openDevice` (same error handling as the other device rows).
   - The row fits the existing 560 px content with no overlap or clipping for every type (the existing UI test checks
     that visible children stay inside the content); `updateContentSize` grows by the row; long texts wrap.
   - The row's texts refresh on the dialog's existing 500 ms timer (only this row - never rebuild the combos there):
     when the operator changes the Windows *Default Format*, Windows restarts the stream and JUCE reopens the device,
     and the new format must show without reopening the settings.
5. **Top bar.** Running: `48.0 kHz · 24비트 · 256 샘플  10.7 ms` - the input's bits from `DeviceFormat` (omit the bits
   part when unknown). It must fit the status label in all three bar layouts at the minimum window width - measure the
   text; if it does not fit, give the label more of the row (without taking the device combo below its minimum) or
   drop the word `샘플`, never clip. Tooltip on the status label with the full detail (input and output bits, and
   `(윈도우 설정)` / `(ASIO 드라이버)` / `(독점)`).
6. **Release files.** `LIVEMIX_VERSION` 0.11.1 in `CMakeLists.txt`; `docs/release-notes/livemix/0.11.1.html` in the
   style of `0.11.0.html` (Korean, short); one line in the `주요기능` list of `site/livemix/index.html`. Do not touch
   `installer/`, `tools/`, `obs-plugin/`, `livemix/shared/`.

## Review focus (inputs no happy-path test hits, most likely first)

1. A choice the device refuses (the capture card accepts 16 only; `int24` chosen) -> it opens in 16, the UI says so
   in words, no error dialog, the stored choice stays `int24`.
2. A failing `openDevice` with a new choice -> the previous device AND its previous choice come back (the JUCE
   preference reads back the old value).
3. A session saved with a choice, opened while the same device runs: exclusive with another choice -> reopened;
   shared -> not reopened, choice recorded.
4. A 0.11.0 session / settings file (no key), and a hand-edited one (`"sampleFormat": 24`, `"int20"`) -> 자동.
5. Split mode in exclusive: the monitor device reports its own format and refusal.

## Tasks (test-first; each ends with a build and the whole suite)

- **T1 model** - `MixSessionTests.cpp`: JSON round trip of the five values; missing key; unknown / non-string value;
  `LiveMixSettings` last-device round trip in a temp directory (never the real `%APPDATA%\LiveMix`).
- **T2 engine choice** - `MixEngineTests.cpp` with the fake types: `openDevice` with each choice -> JUCE preference
  read back; `getOpenDevice().sampleFormat`; `restartDevice` / `setBufferSize` keep it; a failing open with a new
  choice restores the previous choice (readback) and device; `openSessionDevice` reopen rule for the exclusive vs
  shared fake type (e.g. compare the current `AudioIODevice*` - `openDevice` recreates it).
- **T3 DeviceFormat** - the pure mapping with synthetic `WasapiFormatInfo` (accepted masks, refused flags, rate
  mismatch, split output, ASIO bits) + `getDeviceFormat()` with the fakes.
- **T4 settings UI** - extend the existing settings-dialog test (`MixEngineTests.cpp` ~1480): exclusive fake ->
  `device-bitdepth` visible with 5 items, picking `24비트` reopens with `int24` (engine + JUCE readback); ASIO and
  shared fakes -> combo hidden, text visible (shared: the Windows-settings button visible, never clicked); layout
  check for every type; `LIVEMIX_UI_SCREENSHOT_DIR` screenshots keep working.
- **T5 top bar + texts** - the status text builder (pure) and the Korean format texts; the fit rule of Decision 5.
- **T6 release files** - Decision 6.
