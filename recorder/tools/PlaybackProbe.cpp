#include "playback/VideoPlaybackEngine.h"
#include "playback/TimelineTransport.h"
#include "record/WavTrackWriter.h"
#include "record/Ffmpeg.h"
#include "diagnostics/CaptureTelemetry.h"
#include <juce_events/juce_events.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace gocue::recorder
{
namespace
{
struct Options
{
    std::map<juce::String, juce::String> values;
    bool stopToPlay = false;
    bool indexOnly = false;
    bool warm = false;
    juce::Array<juce::var> command;
    juce::String get(const char* key, const char* fallback = "") const
    { const auto it = values.find(key); return it == values.end() ? juce::String(fallback) : it->second; }
};
int number(const juce::String& s, int low, int high)
{
    if (s.isEmpty() || !s.containsOnly("0123456789")) throw std::invalid_argument("Expected nonnegative integer option");
    const auto value = s.getLargeIntValue();
    if (value < low || value > high) throw std::invalid_argument("Playback option out of range"); return static_cast<int>(value);
}
juce::File path(const juce::String& text) { return juce::File::getCurrentWorkingDirectory().getChildFile(text); }
void check(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
struct Window
{
    HWND window = nullptr;
    std::array<HWND, 2> hosts{};
    std::array<HWND, 2> placeholders{};
    std::array<bool, 2> gaps{{true, true}};
    bool closed = false;
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l)
    {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_CLOSE) { self->closed = true; return 0; }
        if (self && message == WM_SIZE)
        {
            const auto width = LOWORD(l), height = HIWORD(l);
            if (self->hosts[0]) MoveWindow(self->hosts[0], 0, 0, width / 2, height, TRUE);
            if (self->hosts[1]) MoveWindow(self->hosts[1], width / 2, 0, width - width / 2, height, TRUE);
            for (unsigned i = 0; i < 2; ++i) if (self->placeholders[i])
                MoveWindow(self->placeholders[i], 0, height / 2 - 12, width / 2, 24, TRUE);
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
    Window()
    {
        WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"RecorderR11PlaybackProbe"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Register playback probe window failed");
        window = CreateWindowExW(0, cls.lpszClassName, L"Recorder playback | cam1 / cam2", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 480, nullptr, nullptr, cls.hInstance, this);
        if (!window) throw std::runtime_error("Create playback probe window failed");
        for (unsigned i = 0; i < 2; ++i)
            hosts[i] = CreateWindowExW(0, L"STATIC", i == 0 ? L"cam1" : L"영상 없음", WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
                i * 620, 0, 620, 440, window, nullptr, cls.hInstance, nullptr);
        if (!hosts[0] || !hosts[1]) throw std::runtime_error("Create playback HWND hosts failed");
        for (unsigned i = 0; i < 2; ++i)
            placeholders[i] = CreateWindowExW(0, L"STATIC", L"영상 없음", WS_CHILD | WS_VISIBLE | SS_CENTER,
                0, 208, 620, 24, hosts[i], nullptr, cls.hInstance, nullptr);
    }
    ~Window() { if (window) DestroyWindow(window); }
    void pump()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { if (message.message == WM_QUIT) closed = true; TranslateMessage(&message); DispatchMessageW(&message); }
        if (closed) throw std::runtime_error("Playback probe cancelled by user");
    }
    void updateGaps(const VideoPlaybackEngine& engine)
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto gap = engine.displaySelection(i).gap;
            if (gaps[i] != gap) { gaps[i] = gap; ShowWindow(placeholders[i], gap ? SW_SHOWNOACTIVATE : SW_HIDE); }
        }
    }
};
Sample metadataDuration(const juce::File& file, std::uint32_t rate)
{
    struct Input { AVFormatContext* p = nullptr; ~Input() { avformat_close_input(&p); } } input;
    ffCheck(avformat_open_input(&input.p, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Read fixture duration");
    ffCheck(avformat_find_stream_info(input.p, nullptr), "Read fixture video metadata");
    const auto index = av_find_best_stream(input.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0); ffCheck(index, "Find fixture video");
    const auto* stream = input.p->streams[index];
    if (stream->duration <= 0 || stream->duration == AV_NOPTS_VALUE) throw std::invalid_argument("Fixture has no video duration");
    return av_rescale_q(stream->duration, stream->time_base, {1, static_cast<int>(rate)});
}
struct Fixture
{
    juce::File root;
    juce::Uuid take;
};
Fixture synthesizeWav(Sample length, std::uint32_t rate)
{
    Fixture fixture{juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("gocue-rec-r11-fixture-" + juce::Uuid().toString()), juce::Uuid()};
    WavTrackWriter::Config c; c.projectDirectory = fixture.root; c.takeId = fixture.take;
    c.sampleRate = rate; c.mics = 1; c.framesPerBlock = 4096;
    c.devices = {{"synthetic-r11", "440 Hz synthetic WAV, 0.10 peak", 1, 0, 0}};
    WavTrackWriter writer(c); check(writer.start());
    std::vector<std::int32_t> pcm(c.framesPerBlock);
    for (Sample at = 0; at < length;)
    {
        const auto frames = static_cast<std::uint32_t>((std::min)(Sample(c.framesPerBlock), length - at));
        for (std::uint32_t i = 0; i < frames; ++i)
            pcm[i] = static_cast<std::int32_t>(std::sin(6.283185307179586 * 440.0 * static_cast<double>(at + i) / rate) * 838860.0);
        while (writer.queueFrames() + frames > writer.queueCapacityFrames())
        { check(writer.status()); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        if (!writer.tryPush(pcm.data(), frames, static_cast<std::uint64_t>(at))) throw std::runtime_error("Synthetic WAV fixture queue failed");
        at += frames;
    }
    check(writer.stop(length, juce::Uuid())); return fixture;
}
double percentile(std::vector<double> values, double p)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end()); return values[static_cast<std::size_t>(std::ceil(values.size() * p)) - 1];
}
// QPC intervals make overlapping startup work and pre-marker warmup explicit.
// Missing endpoints stay null; an unfinished operation is never reported as 0ms.
juce::var interval(std::int64_t begin, std::int64_t end, std::int64_t marker, std::int64_t hz)
{
    auto result = jsonObject();
    jsonSet(result, "beginQpc", begin ? juce::var(begin) : juce::var());
    jsonSet(result, "endQpc", end ? juce::var(end) : juce::var());
    jsonSet(result, "durationMs", begin && end ? juce::var(1000.0 * (end - begin) / hz) : juce::var());
    jsonSet(result, "beginFromStopMs", begin && marker ? juce::var(1000.0 * (begin - marker) / hz) : juce::var());
    jsonSet(result, "endFromStopMs", end && marker ? juce::var(1000.0 * (end - marker) / hz) : juce::var());
    jsonSet(result, "completedBeforeStop", marker && end && end <= marker);
    return result;
}
struct ReportOnExit
{
    std::function<void()> capture;
    ~ReportOnExit() { try { capture(); } catch (...) {} }
};
juce::var seekPhases(const PlaybackSeekTiming& t, std::int64_t release, std::int64_t hz)
{
    auto result = jsonObject();
    const auto span = [&](const char* name, std::int64_t begin, std::int64_t end)
    {
        auto s = jsonObject();
        jsonSet(s, "beginQpc", begin ? juce::var(begin) : juce::var());
        jsonSet(s, "endQpc", end ? juce::var(end) : juce::var());
        jsonSet(s, "durationMs", begin && end ? juce::var(1000.0 * (end - begin) / hz) : juce::var());
        jsonSet(result, name, s);
    };
    jsonSet(result, "definition", "QPC endpoints; prefix completes when target AVFrame is received; GPU copy/draw completion is separate. Missing/cache-bypassed stages are null. DXGI receipt is successful Present return, not photon time.");
    span("releaseToRequest", release, t.requestedQpc);
    span("requestToWorker", t.requestedQpc, t.workerQpc);
    span("requestToDecoderSeek", t.requestedQpc, t.decode.seekBeginQpc);
    span("decoderSeekAndFlush", t.decode.seekBeginQpc, t.decode.seekEndQpc);
    span("prefixDecodeThroughTarget", t.decode.prefixBeginQpc, t.decode.prefixEndQpc);
    span("targetConversion", t.decode.convertBeginQpc, t.decode.gpuCompleteQpc);
    span("targetResources", t.decode.convertBeginQpc, t.decode.resourcesReadyQpc);
    span("targetGpuSubmission", t.decode.resourcesReadyQpc, t.decode.gpuSubmittedQpc);
    span("targetGpuCompletion", t.decode.gpuSubmittedQpc, t.decode.gpuCompleteQpc);
    span("requestToReady", t.requestedQpc, t.readyQpc);
    span("readyToDxgiReceipt", t.readyQpc, t.presentQpc);
    span("releaseToDxgiReceipt", release, t.presentQpc);
    return result;
}
}
int runPlaybackProbe(int argc, wchar_t** argv)
{
    auto report = jsonObject(); Options options;
    juce::File reportFile; std::string stage = "arguments"; bool beganPlayback = false;
    const auto hz = qpcFrequency();
    jsonSet(report, "result", "FAIL"); jsonSet(report, "probe", "playback");
    jsonSet(report, "ffmpegVersion", av_version_info()); jsonSet(report, "startedAt", utcNowIso8601());
    try
    {
        for (int i = 0; i < argc; ++i) options.command.add(juce::String(argv[i]));
        jsonSet(report, "command", options.command);
        const std::set<juce::String> valued{"--media-dir", "--report", "--seek-storm", "--asio-device", "--asio-outputs", "--buffer-size", "--sample-rate"};
        for (int i = 2; i < argc; ++i)
        {
            const juce::String arg(argv[i]);
            if (arg == "--stop-to-play") { options.stopToPlay = true; continue; }
            if (arg == "--index-only") { options.indexOnly = true; continue; }
            if (arg == "--warm") { options.warm = true; continue; }
            if (!valued.count(arg) || i + 1 == argc || options.values.count(arg)) throw std::invalid_argument("Unknown, duplicate or missing playback option");
            options.values[arg] = juce::String(argv[++i]);
            if (arg == "--report") reportFile = path(options.values[arg]);
        }
        if (options.get("--media-dir").isEmpty() || reportFile == juce::File()) throw std::invalid_argument("playback requires --media-dir DIR --report FILE");
        const auto directory = path(options.get("--media-dir")); const auto videoFile = directory.getChildFile("cam1.mp4");
        const auto storm = number(options.get("--seek-storm", "0"), 0, 10000);
        if (options.warm && options.indexOnly) throw std::invalid_argument("--warm requires playback; incompatible with --index-only");
        constexpr std::uint32_t rate = 48000;
        if (number(options.get("--sample-rate", "48000"), 8000, 768000) != rate)
            throw std::invalid_argument("Development playback probe/fixtures require 48000 Hz");
        const bool manifest = directory.getChildFile("take.json").existsAsFile();
        if (manifest)
        {
            const auto m = juce::JSON::parse(directory.getChildFile("take.json"));
            const auto fs = m["sampleRate"].toString();
            if (number(fs, 8000, 768000) != rate)
                throw std::invalid_argument("Development playback manifest must use 48000 Hz");
        }
        stage = "fixture";
        const auto lengthHint = metadataDuration(videoFile, rate);
        Fixture fixture;
        if (!manifest)
        {
            const auto wavs = directory.findChildFiles(juce::File::findFiles, true, "*.wav");
            if (!wavs.isEmpty()) throw std::invalid_argument("Existing WAV chunks require a complete schema-1 playback take.json; samples will not be guessed");
            fixture = synthesizeWav(lengthHint, rate);
        }
        jsonSet(report, "mediaDirectory", directory.getFullPathName()); jsonSet(report, "audioSource", manifest ? "manifest PCM24 WAV" : "synthetic PCM24 WAV via round-06 WavTrackWriter");
        if (!manifest) { jsonSet(report, "fixtureProject", fixture.root.getFullPathName()); jsonSet(report, "fixtureTakeId", fixture.take.toDashedString()); }
        jsonSet(report, "fixturePreparation", "Before measured stop marker; metadata duration read may warm OS cache; input MP4 unchanged");
        jsonSet(report, "measurementDefinition", "Synthetic Stop marker with finalized/renamed fixture already available -> successful DXGI Present and ASIO first PCM callback; audible time also estimated with driver output latency. No TakeController/finalize duration included.");
        jsonSet(report, "physicalDisplayAndDac", "UNAVAILABLE: Present success is not a photon timestamp; driver latency is not a measured DAC timestamp");
        jsonSet(report, "stopToPlayRequested", options.stopToPlay);
        jsonSet(report, "warm", options.warm); jsonSet(report, "sampleRate", rate);
        jsonSet(report, "finalizeMs", juce::var());
        if (options.indexOnly)
        {
            stage = "media-index"; const auto start = qpcNow(); MediaIndex index;
            const auto indexed = index.openVideo(videoFile, rate);
            const auto sources = manifest ? index.openWavManifest(directory) : index.openWavJournal(fixture.root, fixture.take);
            jsonSet(report, "videoPackets", indexed->packets.size()); jsonSet(report, "idrCount", indexed->idrs.size());
            jsonSet(report, "timelineSamples", indexed->length); jsonSet(report, "wavTracks", sources.size());
            jsonSet(report, "indexMs", 1000.0 * (qpcNow() - start) / hz);
            jsonSet(report, "stage", stage); jsonSet(report, "result", "PASS");
            jsonSet(report, "reason", "Offline final-file index validation only; no ASIO device, GPU decoder or playback opened");
            CaptureTelemetry::writeJson(reportFile, report); std::cout << juce::JSON::toString(report, false) << '\n'; return 0;
        }
        juce::ScopedJuceInitialiser_GUI runtime;
        Window window;
        auto output = makeAsioPlaybackOutput(); AudioOutputConfig config;
        config.sampleRate = rate; config.deviceIndex = number(options.get("--asio-device", "0"), 0, 1024);
        config.bufferFrames = number(options.get("--buffer-size", "0"), 0, 262144);
        const auto map = juce::StringArray::fromTokens(options.get("--asio-outputs", "1:2"), ":", "");
        if (map.size() < 1 || map.size() > 2) throw std::invalid_argument("--asio-outputs expects one-based L:R or mono channel");
        config.leftPhysical = number(map[0], 1, 1024) - 1;
        config.rightPhysical = map.size() == 2 ? number(map[1], 1, 1024) - 1 : config.leftPhysical;
        // Cold run includes device open/start. Warm run places its Stop marker
        // after the initial exact frame, codec, presenter and ASIO are prepared.
        auto stoppedAt = options.warm ? std::int64_t{0} : qpcNow();
        std::int64_t openBegin = 0, openEnd = 0, startBegin = 0, startEnd = 0, indexBegin = 0, indexEnd = 0;
        auto startup = jsonObject(); jsonSet(report, "startup", startup);
        const auto saveIntervals = [&]
        {
            jsonSet(report, "stopMarkerQpc", stoppedAt); jsonSet(report, "qpcFrequency", hz);
            jsonSet(startup, "indexBuild", interval(indexBegin, indexEnd, stoppedAt, hz));
            jsonSet(startup, "asioOpen", interval(openBegin, openEnd, stoppedAt, hz));
            jsonSet(startup, "asioStart", interval(startBegin, startEnd, stoppedAt, hz));
        };
        ReportOnExit startupOnExit{saveIntervals};
        stage = "device"; openBegin = qpcNow(); const auto info = output->open(config); openEnd = qpcNow();
        jsonSet(report, "asioDriver", info.driver); jsonSet(report, "sampleRate", info.sampleRate); jsonSet(report, "blockFrames", info.blockFrames);
        jsonSet(report, "outputLatencySamples", info.outputLatency); jsonSet(report, "leftPhysicalChannel", info.leftPhysical + 1); jsonSet(report, "rightPhysicalChannel", info.rightPhysical + 1);
        stage = "media-index"; indexBegin = qpcNow();
        MediaIndex index;
        const auto video = index.openVideo(videoFile, rate);
        const auto audioSources = manifest ? index.openWavManifest(directory) : index.openWavJournal(fixture.root, fixture.take);
        std::vector<PlaybackVideoClip> videoClips;
        const auto addVideo = [&](unsigned camera, std::shared_ptr<const VideoIndex> source)
        {
            PlaybackVideoClip c; c.camera = camera; c.source = std::move(source); c.mapping.clipId = newId();
            c.mapping.assetId = newId(); c.mapping.mediaGeneration = static_cast<Sample>(c.source->generation); c.mapping.lengthSamples = c.source->length;
            videoClips.push_back(std::move(c));
        };
        addVideo(0, video);
        if (directory.getChildFile("cam2.mp4").existsAsFile()) addVideo(1, index.openVideo(directory.getChildFile("cam2.mp4"), rate));
        const unsigned cameras = static_cast<unsigned>(videoClips.size());
        Sample timelineEnd = video->length;
        for (const auto& c : videoClips) timelineEnd = (std::max)(timelineEnd, c.mapping.lengthSamples);
        std::vector<PlaybackAudioTrack> tracks;
        for (const auto& source : audioSources)
        {
            PlaybackAudioTrack track; track.trackId = source->trackId;
            PlaybackAudioClip c; c.source = source; c.mapping.clipId = newId(); c.mapping.trackId = track.trackId;
            c.mapping.mediaGeneration = static_cast<Sample>(source->generation); c.mapping.lengthSamples = source->length;
            timelineEnd = (std::max)(timelineEnd, source->length);
            if (source->length) track.clips.push_back(std::move(c)); tracks.push_back(std::move(track));
        }
        jsonSet(report, "timelineSamples", timelineEnd); jsonSet(report, "videoPackets", video->packets.size()); jsonSet(report, "idrCount", video->idrs.size());
        indexEnd = qpcNow(); jsonSet(report, "indexMs", 1000.0 * (indexEnd - indexBegin) / hz);
        TimelineAudioRenderer audio(rate, info.blockFrames); audio.setPlan(std::move(tracks), timelineEnd);
        VideoPlaybackEngine engine; engine.prepare(std::move(videoClips));
        TimelineTransport transport(rate, hz, audio.queue(), timelineEnd);
        // Callback barrier must run before transport/PCM destructors, including all
        // startup exceptions.
        struct CloseOutput { IAudioOutput& output; ~CloseOutput() { output.close(); } } close{*output};
        bool savedFinal = false;
        const auto saveState = [&]
        {
            if (savedFinal) return;
            jsonSet(report, "video", engine.telemetry()); jsonSet(report, "transport", transport.telemetry());
            jsonSet(report, "underruns", transport.snapshot().underruns);
        };
        ReportOnExit stateOnExit{saveState}; // also captures failed seek before worker/queue teardown
        stage = "playback"; startBegin = qpcNow(); output->start(transport); startEnd = qpcNow();
        for (unsigned camera = 0; camera < cameras; ++camera) engine.attachPlaybackView(camera, window.hosts[camera]);
        auto pump = [&]
        {
            window.pump(); transport.service(audio, engine, *output, qpcNow()); check(transport.status());
            window.updateGaps(engine);
            const HANDLE events[]{transport.wakeHandle()};
            const auto waited = MsgWaitForMultipleObjectsEx(1, events, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (waited == WAIT_FAILED) throw std::runtime_error("Wait for playback coordinator event failed");
        };
        if (options.warm)
        {
            stage = "warmup"; transport.stop(); const auto gen = transport.generation();
            const auto until = qpcNow() + hz * 15;
            bool prepared = false;
            while (qpcNow() < until)
            {
                pump(); prepared = transport.snapshot().generation == gen && transport.snapshot().state == TransportState::stopped;
                for (unsigned camera = 0; camera < cameras; ++camera)
                    prepared &= engine.seekTiming(camera).generation == gen && engine.seekTiming(camera).presentQpc != 0;
                if (prepared) break;
            }
            if (!prepared) throw std::runtime_error("Warm device/decoder/presenter preparation timed out");
            jsonSet(report, "warmPreparationVideo", engine.telemetry());
            jsonSet(report, "warmPreparationTransport", transport.telemetry());
            stoppedAt = qpcNow();
        }
        stage = "playback"; saveIntervals();
        jsonSet(report, "measurementDefinition", options.warm
            ? "Warm Stop marker -> new stop/play generation with indexed media, open codecs, initialized presenters and running silent ASIO; finalization excluded. Warm preparation intervals have negative offsets."
            : "Cold Stop marker -> ASIO open, finalized-file indexing, codec creation, ASIO start, first successful DXGI Present and first PCM block; finalization excluded. Unlike r11, ASIO open is inside this marker.");
        jsonSet(startup, "intervalDefinition", "Durations are QPC intervals and may overlap; do not sum parallel camera/audio work. IDR-to-target includes seek, codec flush, compressed decode and GPU conversion.");
        transport.stop(); transport.play();
        const auto initialGeneration = transport.generation();
        std::array<std::int64_t, 2> firstVideo{}; TransportSnapshot firstAudio;
        const auto deadline = stoppedAt + hz * 15;
        while (qpcNow() < deadline)
        {
            pump();
            for (unsigned camera = 0; camera < cameras; ++camera)
            {
                const auto p = engine.lastPresentation(camera);
                if (!firstVideo[camera] && p.generation == initialGeneration && p.qpc >= stoppedAt) firstVideo[camera] = p.qpc;
            }
            firstAudio = transport.snapshot();
            if (firstAudio.generation == initialGeneration && firstAudio.firstBlockQpc >= stoppedAt && firstVideo[0] && (cameras == 1 || firstVideo[1])) break;
        }
        if (firstAudio.generation != initialGeneration || firstAudio.firstBlockQpc < stoppedAt || !firstVideo[0] || (cameras == 2 && !firstVideo[1]))
            throw std::runtime_error("First synchronized playback timed out");
        beganPlayback = true;
        const auto videoMs = 1000.0 * ((std::max)(firstVideo[0], firstVideo[1]) - stoppedAt) / hz;
        const auto blockMs = 1000.0 * (firstAudio.firstBlockQpc - stoppedAt) / hz;
        const auto audibleMs = 1000.0 * (firstAudio.firstAudibleQpc - stoppedAt) / hz;
        jsonSet(report, "firstVideoPresentMs", videoMs); jsonSet(report, "firstAudioBlockOutputMs", blockMs);
        jsonSet(report, "firstAudioAudibleEstimateMs", audibleMs); jsonSet(report, "stopToPlayMs", (std::max)(videoMs, audibleMs));
        jsonSet(report, "indexToPlayMs", 1000.0 * ((std::max)({firstVideo[0], firstVideo[1], firstAudio.firstAudibleQpc}) - indexBegin) / hz);
        const auto startupLimit = options.warm ? 150 : 2000;
        jsonSet(report, "stopToPlayLimitMs", startupLimit);
        juce::Array<juce::var> startupCameras;
        const auto videoState = engine.telemetry();
        for (unsigned camera = 0; camera < cameras; ++camera)
        {
            const auto timing = engine.seekTiming(camera); const auto c = videoState["cameras"][static_cast<int>(camera)];
            auto phases = jsonObject(); jsonSet(phases, "camera", camera);
            jsonSet(phases, "decoderCreation", interval(timing.decoderBeginQpc, timing.decoderEndQpc, stoppedAt, hz));
            jsonSet(phases, "decoderReused", !timing.decoderBeginQpc);
            jsonSet(phases, "idrToTargetDecode", interval(timing.decodeBeginQpc, timing.decodeEndQpc, stoppedAt, hz));
            jsonSet(phases, "frameCache", timing.cacheHit ? "hit" : "miss");
            jsonSet(phases, "presenterInitialisation", interval(static_cast<juce::int64>(c["presenterInitBeginQpc"]),
                static_cast<juce::int64>(c["presenterInitEndQpc"]), stoppedAt, hz));
            jsonSet(phases, "readyToFirstPresent", interval(timing.readyQpc, firstVideo[camera], stoppedAt, hz));
            jsonSet(phases, "stopToFirstPresent", interval(stoppedAt, firstVideo[camera], stoppedAt, hz));
            jsonSet(phases, "seek", c["seek"]); startupCameras.add(phases);
            jsonSet(phases, "seekPhases", seekPhases(timing, stoppedAt, hz));
        }
        jsonSet(startup, "cameras", startupCameras); jsonSet(startup, "transport", transport.telemetry());
        jsonSet(startup, "stopToFirstAudioBlock", interval(stoppedAt, firstAudio.firstBlockQpc, stoppedAt, hz));
        jsonSet(startup, "asioStartToFirstAudioBlock", interval(startEnd, firstAudio.firstBlockQpc, stoppedAt, hz));
        const auto runningUntil = qpcNow() + hz; while (qpcNow() < runningUntil) pump();
        std::vector<double> seekTimes; auto seeks = juce::var(juce::Array<juce::var>{});
        std::vector<double> hitTimes, missTimes;
        jsonSet(report, "seeks", seeks); jsonSet(report, "seekRequestedCount", storm);
        double cold = 0; std::uint64_t random = 0x11c0ffee;
        unsigned cacheHits = 0, cacheMisses = 0, timeouts = 0;
        const auto summariseSeeks = [&]
        {
            jsonSet(report, "seekCount", seekTimes.size()); jsonSet(report, "seekTimeoutCount", timeouts);
            jsonSet(report, "seekCacheHits", cacheHits); jsonSet(report, "seekCacheMisses", cacheMisses);
            if (!storm) return;
            jsonSet(report, "seekP50Ms", seekTimes.empty() ? juce::var() : juce::var(percentile(seekTimes, .50)));
            jsonSet(report, "seekP95Ms", seekTimes.empty() ? juce::var() : juce::var(percentile(seekTimes, .95)));
            jsonSet(report, "seekMaxMs", seekTimes.empty() ? juce::var() : juce::var(*std::max_element(seekTimes.begin(), seekTimes.end())));
            jsonSet(report, "coldSeekMs", cold ? juce::var(cold) : juce::var());
            const auto distribution = [&](const std::vector<double>& times)
            {
                auto s = jsonObject(); jsonSet(s, "count", times.size());
                jsonSet(s, "p50Ms", times.empty() ? juce::var() : juce::var(percentile(times, .50)));
                jsonSet(s, "p95Ms", times.empty() ? juce::var() : juce::var(percentile(times, .95)));
                jsonSet(s, "maxMs", times.empty() ? juce::var() : juce::var(*std::max_element(times.begin(), times.end())));
                return s;
            };
            jsonSet(report, "seekCacheHitLatency", distribution(hitTimes));
            jsonSet(report, "seekCacheMissLatency", distribution(missTimes));
        };
        ReportOnExit seekSummaryOnExit{summariseSeeks};
        jsonSet(report, "seekP95LimitMs", 250); jsonSet(report, "coldSeekLimitMs", 500);
        jsonSet(report, "seekCacheHitP50LimitMs", 50);
        jsonSet(report, "seekTimeoutMs", 10000);
        jsonSet(report, "seekDefinition", "Release -> FIRST exact containing-frame DXGI receipt in every active camera; codecs retained, immutable resident frame cache validated/rebound per generation; all-camera hit versus any-camera miss. Cold = first distant frame-cache miss, NOT device/OS-cache cold. OS cache is not flushed. Repeat targets exercise hits without changing the denominator.");
        if (storm) transport.pause();
        const auto beforeStorm = engine.telemetry();
        jsonSet(report, "videoBeforeSeekStorm", beforeStorm); // counters above this boundary include the one-second playback run
        const auto coldAnchor = video->frameAt(video->length * 3 / 4);
        const auto nextIdr = std::upper_bound(video->idrs.begin(), video->idrs.end(), coldAnchor);
        const auto coldPacket = nextIdr == video->idrs.end() ? video->packets.size() - 1 : *nextIdr - 1;
        jsonSet(report, "coldSeekDefinition", "First distant target is the last sample of a GOP near 75% of the source, exercising the full IDR prefix with open codecs and OS cache unchanged.");
        Sample previousSeek = 0;
        for (int i = 0; i < storm; ++i)
        {
            stage = "seek-storm";
            // Seven coalesced drag requests plus immediate exact release. Every
            // tenth release repeats the resident target to expose real cache hits.
            for (int drag = 0; drag < 7; ++drag)
            {
                random = random * 6364136223846793005ull + 1;
                transport.scrub(static_cast<Sample>((random >> 1) % static_cast<std::uint64_t>(video->length)), false, qpcNow());
            }
            random = random * 6364136223846793005ull + 1;
            const auto target = i == 0 ? video->packets[coldPacket].endSample - 1 : i % 10 == 9 ? previousSeek
                : static_cast<Sample>((random >> 1) % static_cast<std::uint64_t>(video->length));
            const auto release = qpcNow(); transport.scrub(target, true, release); const auto gen = transport.generation();
            auto attempt = jsonObject(); jsonSet(attempt, "sample", target); jsonSet(attempt, "generation", gen);
            jsonSet(attempt, "releaseQpc", release); jsonSet(attempt, "result", "pending");
            jsonSet(attempt, "firstLargeColdSeek", i == 0); seeks.getArray()->add(attempt);
            std::int64_t exactAt = 0;
            std::array<std::int64_t, 2> receipts{};
            while (qpcNow() - release < hz * 10)
            {
                pump(); bool exact = true;
                for (unsigned camera = 0; camera < cameras; ++camera)
                {
                    // A lane beyond its final clip is an explicit gap, not a
                    // frame that can ever produce an exact receipt.
                    const auto selection = engine.displaySelection(camera);
                    if (selection.generation == gen && selection.gap) continue;
                    const auto p = engine.lastPresentation(camera);
                    if (!receipts[camera] && p.generation == gen && p.begin <= target && target < p.end && p.qpc >= release)
                        receipts[camera] = p.qpc;
                    exact &= receipts[camera] != 0;
                    if (receipts[camera]) exactAt = (std::max)(exactAt, receipts[camera]);
                }
                if (exact) break;
                exactAt = 0;
            }
            jsonSet(attempt, "video", engine.telemetry()); jsonSet(attempt, "transport", transport.telemetry());
            juce::Array<juce::var> phases;
            for (unsigned camera = 0; camera < cameras; ++camera)
            {
                auto detail = seekPhases(engine.seekTiming(camera), release, hz);
                jsonSet(detail, "camera", camera); phases.add(detail);
            }
            jsonSet(attempt, "phases", phases);
            if (!exactAt)
            {
                ++timeouts; jsonSet(attempt, "result", "timeout"); jsonSet(attempt, "releaseToExactPresentMs", juce::var());
                throw std::runtime_error("Exact seek presentation timed out; inspect seeks[].video/transport phase and generation");
            }
            bool hit = true;
            for (unsigned camera = 0; camera < cameras; ++camera)
            {
                const auto s = engine.displaySelection(camera); if (s.gap) continue;
                const auto t = engine.seekTiming(camera); hit &= t.generation == gen && t.cacheKnown && t.cacheHit;
            }
            if (hit) ++cacheHits; else ++cacheMisses;
            const auto ms = 1000.0 * (exactAt - release) / hz; seekTimes.push_back(ms); if (!i) cold = ms;
            (hit ? hitTimes : missTimes).push_back(ms);
            jsonSet(attempt, "result", "presented"); jsonSet(attempt, "releaseToExactPresentMs", ms);
            jsonSet(attempt, "decodedFrameCache", hit ? "hit" : "miss"); previousSeek = target;
            summariseSeeks();
        }
        summariseSeeks();
        const auto final = transport.snapshot(); jsonSet(report, "underruns", final.underruns); jsonSet(report, "video", engine.telemetry());
        juce::Array<juce::var> deltas; const auto afterStorm = engine.telemetry();
        for (unsigned camera = 0; camera < cameras; ++camera)
        {
            auto delta = jsonObject(); jsonSet(delta, "camera", camera);
            for (const auto* key : {"lateDisplaySelections", "staleDiscarded", "cancelledTargetDecodes", "cancelledPrefetchDecodes"})
                jsonSet(delta, key, static_cast<juce::int64>(afterStorm["cameras"][static_cast<int>(camera)][key])
                    - static_cast<juce::int64>(beforeStorm["cameras"][static_cast<int>(camera)][key]));
            deltas.add(delta);
        }
        jsonSet(report, "seekStormCounterDelta", deltas);
        jsonSet(report, "audibleCursorSample", TimelineTransport::audibleCursor(final, rate, hz, qpcNow()));
        saveState(); savedFinal = true;
        output->close(); audio.stopWorker(); engine.stop();
        const bool startupMet = (std::max)(videoMs, audibleMs) <= startupLimit;
        const bool hitsMet = hitTimes.empty() || percentile(hitTimes, .50) <= 50;
        jsonSet(report, "seekCacheHitTargetMet", hitTimes.empty() ? juce::var() : juce::var(hitsMet));
        const bool seekMet = !storm || (seekTimes.size() == static_cast<std::size_t>(storm)
            && percentile(seekTimes, .95) <= 250 && cold <= 500 && hitsMet);
        jsonSet(report, "stopToPlayTargetMet", startupMet); jsonSet(report, "seekTargetMet", storm ? juce::var(seekMet) : juce::var());
        const bool pass = (!(options.stopToPlay || !storm) || startupMet) && !final.underruns && seekMet;
        jsonSet(report, "result", pass ? "PASS" : "FAIL");
        jsonSet(report, "reason", pass ? "Observed finalized-fixture playback/seek software timing targets met; physical A/V gate remains unverified"
                                       : "Observed latency or underrun target missed; inspect measurements");
    }
    catch (const std::invalid_argument& e) { jsonSet(report, "result", "FAIL"); jsonSet(report, "reason", e.what()); }
    catch (const std::exception& e)
    {
        const bool unavailable = !beganPlayback && (stage == "device" || stage == "playback" || stage == "warmup");
        jsonSet(report, "result", unavailable ? "UNAVAILABLE" : "FAIL"); jsonSet(report, "reason", e.what());
    }
    jsonSet(report, "stage", stage);
    try { if (reportFile != juce::File()) CaptureTelemetry::writeJson(reportFile, report); }
    catch (const std::exception& e) { std::cerr << "Playback report write failed: " << e.what() << '\n'; return 1; }
    std::cout << juce::JSON::toString(report, false) << '\n';
    return report["result"].toString() == "PASS" ? 0 : report["result"].toString() == "UNAVAILABLE" ? 2 : 1;
}
}
